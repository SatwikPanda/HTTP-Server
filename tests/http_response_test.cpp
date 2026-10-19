#include "http/framing.hpp"
#include "http/response_parser.hpp"
#include "http/serializer.hpp"
#include "test_support.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using http::BodyFraming;
using http::Method;
using http::ParseError;
using http::ParseEvent;
using namespace std::string_literals;

const std::string chunked_head = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";

// Scripted origin bytes exercise the production parser at every split, and with
// another response in the same read. Nothing fakes framing or message decisions.
void check_message(const std::string& wire, std::string_view body, BodyFraming framing,
                   Method method = Method::GET, http::RequestLimits limits = {}) {
    for (std::size_t split = 0; split <= wire.size(); ++split) {
        http::ResponseParser parser(method, limits);
        const auto first = parser.consume(std::string_view(wire).substr(0, split));
        CHECK(!parser.failed() && first.consumed == split);
        const auto rest = wire.substr(split) + (framing == BodyFraming::close_delimited ? "" : "NEXT RESPONSE");
        const auto second = parser.consume(rest);
        CHECK(!parser.failed() && first.consumed + second.consumed == wire.size());
        CHECK(parser.complete() == (framing != BodyFraming::close_delimited));
        CHECK(parser.finish_on_eof() && parser.complete());
        CHECK(parser.response().body == body && parser.framing() == framing);
        CHECK(parser.consume("ignored").consumed == 0 && parser.finish_on_eof());
    }
    http::ResponseParser bytewise(method, limits);
    for (char byte : wire) CHECK(bytewise.consume(std::string_view(&byte, 1)).consumed == 1 && !bytewise.failed());
    CHECK(bytewise.finish_on_eof() && bytewise.response().body == body);
}

void check_error(const std::string& wire, ParseError expected, http::RequestLimits limits = {}) {
    for (std::size_t split = 0; split <= wire.size(); ++split) {
        http::ResponseParser parser(Method::GET, limits);
        parser.consume(std::string_view(wire).substr(0, split));
        CHECK(parser.consume(std::string_view(wire).substr(split)).event == ParseEvent::error);
        if (parser.error() != expected) std::cerr << wire << ": " << http::to_string(parser.error()) << '\n';
        CHECK(parser.failed() && !parser.complete() && parser.error() == expected);
        CHECK(parser.consume("ignored").consumed == 0 && !parser.finish_on_eof());
        CHECK(parser.error() == expected); // EOF must preserve the original error.
        CHECK(parser.error_detail().operation == "HTTP response parser");
    }
}

void test_invalid_messages() {
    const std::vector<std::pair<std::string, ParseError>> lines = {
        {"HTTP/1.1 204", ParseError::malformed_status_line},
        {"HTTP/1.1 2000 OK", ParseError::malformed_status_line},
        {"HTTP/1.1  200 OK", ParseError::malformed_status_line},
        {"HTTP/1.1\t200 OK", ParseError::malformed_status_line},
        {"HTTP/1.1 099 Bad", ParseError::malformed_status_line},
        {"HTTP/1.1 600 Bad", ParseError::malformed_status_line},
        {"HTTP/1.1 200 O\0K"s, ParseError::malformed_status_line},
        {"HTTP/1.1 200 O\x7fK", ParseError::malformed_status_line},
        {"http/1.1 200 OK", ParseError::malformed_version},
        {"HTTP/1.2 200 OK", ParseError::unsupported_version},
        {"\r\nHTTP/1.1 200 OK", ParseError::malformed_status_line},
        {"HTTP/1.1 200 OK\r\nX : a", ParseError::malformed_header},
        {"HTTP/1.1 200 OK\r\nX: a\0b"s, ParseError::malformed_header},
        {"HTTP/1.1 200 OK\r\nX: a\r\n folded", ParseError::obsolete_line_folding}
    };
    for (const auto& [line, error] : lines) check_error(line + "\r\n\r\n", error);
    check_error("HTTP/1.1 200 OK\n\n", ParseError::bare_line_ending);
    check_error("HTTP/1.1 200 OK\rX", ParseError::bare_line_ending);
    const std::vector<std::pair<std::string, ParseError>> fields = {
        {"Content-Length: 1\r\nTransfer-Encoding: chunked", ParseError::ambiguous_framing},
        {"Content-Length:", ParseError::invalid_content_length},
        {"Content-Length: 1,", ParseError::invalid_content_length},
        {"Content-Length: ,1", ParseError::invalid_content_length},
        {"Content-Length: 1,,1", ParseError::invalid_content_length},
        {"Content-Length: 1\r\nContent-Length:", ParseError::invalid_content_length},
        {"Content-Length: +1", ParseError::invalid_content_length},
        {"Content-Length: -1", ParseError::invalid_content_length},
        {"Content-Length: 1x", ParseError::invalid_content_length},
        {"Content-Length: 1, 2", ParseError::conflicting_content_length},
        {"Content-Length: 18446744073709551616", ParseError::content_length_overflow},
        {"Transfer-Encoding:", ParseError::invalid_transfer_encoding},
        {"Transfer-Encoding: ,chunked", ParseError::invalid_transfer_encoding},
        {"Transfer-Encoding: chunked,", ParseError::invalid_transfer_encoding},
        {"Transfer-Encoding: chunked, chunked", ParseError::invalid_transfer_encoding},
        {"Transfer-Encoding: chunked\r\nTransfer-Encoding: chunked", ParseError::invalid_transfer_encoding},
        {"Transfer-Encoding: chunked; a=b", ParseError::invalid_transfer_encoding},
        {"Transfer-Encoding: chunked, gzip", ParseError::invalid_transfer_encoding},
        {"Transfer-Encoding: gzip, chunked", ParseError::unsupported_transfer_coding},
        {"Transfer-Encoding: gzip", ParseError::unsupported_transfer_coding}
    };
    for (const auto& [field, error] : fields) check_error("HTTP/1.1 200 OK\r\n" + field + "\r\n\r\n", error);
    check_error("HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n", ParseError::invalid_transfer_encoding);
    const std::vector<std::pair<std::string, ParseError>> chunks = {
        {"\r\n", ParseError::invalid_chunk_size}, {"+1\r\n", ParseError::invalid_chunk_size},
        {"0x1\r\n", ParseError::invalid_chunk_size}, {"z\r\n", ParseError::invalid_chunk_size},
        {"10000000000000000\r\n", ParseError::chunk_size_overflow},
        {"1\r\nx\r\nffffffffffffffff\r\n", ParseError::body_too_large},
        {"1\r\nx!", ParseError::malformed_chunk}, {"1\r\nx\r!", ParseError::malformed_chunk},
        {"1;\r\n", ParseError::invalid_chunk_extension}, {"1;=a\r\n", ParseError::invalid_chunk_extension},
        {"1;a=\r\n", ParseError::invalid_chunk_extension}, {"1;a=\"unclosed\r\n", ParseError::invalid_chunk_extension},
        {"1;a=\"closed\"junk\r\n", ParseError::invalid_chunk_extension},
        {"1;a=\0\r\n"s, ParseError::invalid_chunk_extension},
        {"0\r\n folded: x\r\n\r\n", ParseError::obsolete_line_folding},
        {"0\r\nNo-Colon\r\n\r\n", ParseError::malformed_header},
        {"0\r\nContent-Length: 0\r\n\r\n", ParseError::prohibited_trailer},
        {"0\r\nConnection: close\r\n\r\n", ParseError::prohibited_trailer},
        {"0\r\nCache-Control: public\r\n\r\n", ParseError::prohibited_trailer}
    };
    for (const auto& [chunk, error] : chunks) check_error(chunked_head + chunk, error);
    check_error("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: X-Hop\r\n\r\n0\r\nX-Hop: no\r\n\r\n", ParseError::prohibited_trailer);
}

void test_completion_and_no_body() {
    const std::string chunks = chunked_head + "4 ; foo = \"a;\\\"b\";flag\r\nWiki\r\n5\r\npedia\r\n00;end\r\nX: a\r\nx: b\r\n\r\n";
    check_message(chunks, "Wikipedia", BodyFraming::chunked);
    http::ResponseParser parser;
    parser.consume(chunks);
    CHECK(parser.response().trailers.get_all("x") == (std::vector<std::string_view>{"a", "b"}));
    CHECK(!parser.response().headers.contains("x"));
    check_message("HTTP/1.1 200 OK\r\nContent-Length: 01, 1\r\nContent-Length: 1\r\n\r\nx", "x", BodyFraming::fixed_length);
    check_message("HTTP/1.1 204 \r\n\r\n", "", BodyFraming::none);
    parser.reset();
    parser.consume("HTTP/1.1 204 \r\n\r\n");
    CHECK(parser.response().reason.empty() && parser.response().trailers.empty());
    parser.reset();
    parser.consume("HTTP/1.0 299 Custom\tReason\r\nSet-Cookie: a=1\r\nset-cookie: b=2\r\nContent-Length: 0\r\n\r\n");
    CHECK(parser.complete() && parser.response().status_code == 299 && parser.response().reason == "Custom\tReason");
    CHECK(parser.response().headers.get_all("SET-COOKIE") == (std::vector<std::string_view>{"a=1", "b=2"}));
    const std::string interim = "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 Early Hints\r\nLink: </a>\r\n\r\n";
    check_message(interim + "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", "", BodyFraming::fixed_length);
    parser.reset();
    parser.consume(interim + "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    CHECK(parser.informational_responses().size() == 2);
    CHECK(parser.informational_responses()[0].status_code == 100);
    CHECK(parser.informational_responses()[1].headers.get("Link") == "</a>");
    for (const auto& info : parser.informational_responses()) CHECK(info.body.empty() && info.trailers.empty());
    parser.reset();
    CHECK(parser.informational_responses().empty() && parser.head_bytes() == 0);
    CHECK(parser.consume("HTTP/1.1 204 No Content\r\n\r\n").event == ParseEvent::complete);
    check_message(interim + "HTTP/1.1 200 OK\r\nContent-Length: 99999999\r\n\r\n", "", BodyFraming::none, Method::HEAD);
    for (Method method : {Method::GET, Method::HEAD}) {
        check_message("HTTP/1.1 304 Not Modified\r\nContent-Length: 18446744073709551615\r\n\r\n", "", BodyFraming::none, method);
        check_message("HTTP/1.1 304 Not Modified\r\nTransfer-Encoding: chunked\r\n\r\n", "", BodyFraming::none, method);
    }
    for (const auto* status : {"100 Continue", "204 No Content"}) for (const auto* field : {"Content-Length: 0", "Transfer-Encoding: chunked"}) {
        check_error("HTTP/1.1 "s + status + "\r\n" + field + "\r\n\r\n", ParseError::ambiguous_framing);
    }
    http::ResponseParser tunnel(Method::CONNECT);
    CHECK(tunnel.consume("HTTP/1.1 200 Connection Established\r\n\r\n").event == ParseEvent::error);
    CHECK(tunnel.error() == ParseError::not_implemented);
    // EOF at every earlier position is truncation, even inside trailers/interim.
    for (const auto& wire : {chunks, "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\ndata"s,
                              interim + "HTTP/1.1 204 No Content\r\n\r\n"}) {
        for (std::size_t cut = 0; cut < wire.size(); ++cut) {
            http::ResponseParser truncated;
            CHECK(truncated.consume(std::string_view(wire).substr(0, cut)).event == ParseEvent::need_more);
            CHECK(!truncated.finish_on_eof() && truncated.error() == ParseError::premature_eof);
        }
    }
    const std::string small = chunked_head + "3\r\na\r\n\r\n0\r\n\r\n";
    for (std::size_t a = 0; a <= small.size(); ++a) for (std::size_t b = a; b <= small.size(); ++b) {
        http::ResponseParser split;
        CHECK(split.consume(std::string_view(small).substr(0, a)).consumed == a);
        CHECK(split.consume(std::string_view(small).substr(a, b - a)).consumed == b - a);
        CHECK(split.consume(std::string_view(small).substr(b)).consumed == small.size() - b);
        CHECK(split.complete() && split.response().body == "a\r\n");
    }
}

void test_limit_boundaries() {
    http::RequestLimits limits;
    const std::string head = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
    limits.max_header_bytes = head.size();
    check_message(head, "", BodyFraming::fixed_length, Method::GET, limits);
    --limits.max_header_bytes;
    check_error(head, ParseError::header_too_large, limits);
    limits = {};
    limits.max_request_line_bytes = 15;
    check_message(head, "", BodyFraming::fixed_length, Method::GET, limits);
    --limits.max_request_line_bytes;
    check_error(head, ParseError::status_line_too_long, limits);
    limits = {};
    limits.max_header_count = 1;
    check_error("HTTP/1.1 200 OK\r\nX: a\r\nY: b\r\n\r\n", ParseError::too_many_headers, limits);
    const std::string interim = "HTTP/1.1 100 Continue\r\n\r\n";
    limits.max_header_bytes = interim.size() + head.size();
    check_message(interim + head, "", BodyFraming::fixed_length, Method::GET, limits);
    --limits.max_header_bytes;
    check_error(interim + head, ParseError::header_too_large, limits);
    limits = {};
    limits.max_body_bytes = 3;
    check_message("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc", "abc", BodyFraming::fixed_length, Method::GET, limits);
    check_message(chunked_head + "1\r\na\r\n2\r\nbc\r\n0\r\n\r\n", "abc", BodyFraming::chunked, Method::GET, limits);
    check_message("HTTP/1.0 200 OK\r\n\r\nabc", "abc", BodyFraming::close_delimited, Method::GET, limits);
    check_error("HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\n", ParseError::body_too_large, limits);
    check_error(chunked_head + "2\r\nab\r\n2\r\n", ParseError::body_too_large, limits);
    check_error("HTTP/1.0 200 OK\r\n\r\nabcd", ParseError::body_too_large, limits);
    limits = {};
    limits.max_chunk_extension_bytes = 2;
    check_message(chunked_head + "0;a\r\n\r\n", "", BodyFraming::chunked, Method::GET, limits);
    check_error(chunked_head + "0;ab\r\n\r\n", ParseError::chunk_extension_too_large, limits);
    limits = {};
    limits.max_chunk_line_bytes = 2;
    check_message(chunked_head + "00\r\n\r\n", "", BodyFraming::chunked, Method::GET, limits);
    check_error(chunked_head + "000\r\n", ParseError::chunk_line_too_long, limits);
    limits = {};
    limits.max_trailer_bytes = 8;
    limits.max_trailer_count = 1;
    check_message(chunked_head + "0\r\nX: a\r\n\r\n", "", BodyFraming::chunked, Method::GET, limits);
    --limits.max_trailer_bytes;
    check_error(chunked_head + "0\r\nX: a\r\n\r\n", ParseError::trailer_too_large, limits);
    limits.max_trailer_bytes = 100;
    check_error(chunked_head + "0\r\nX: a\r\nY: b\r\n\r\n", ParseError::too_many_trailers, limits);
    limits = {};
    limits.max_chunk_line_bytes = 0;
    CHECK(!http::validate_request_limits(limits));
}

void test_output() {
    CHECK(test::take(http::encode_chunk(std::span<const std::byte>{})) == "0\r\n\r\n");
    CHECK(http::encode_chunk("hello", {5, 5}));
    CHECK(!http::encode_chunk("hello", {4, 5}) && !http::encode_chunk("hello", {5, 4}));
    CHECK(http::encode_final_chunk(nullptr, {5, 0}) && !http::encode_final_chunk(nullptr, {4, 0}));
    for (const auto& field : std::vector<http::HeaderField>{{"Bad Name", "x"}, {"X", "a\r\nInjected: b"},
        {"Content-Length", "0"}, {"Host", "evil"}, {"Content-Encoding", "gzip"}, {"Connection", "close"}}) {
        http::HeaderMap bad;
        bad.add(field.name, field.value);
        CHECK(!http::encode_final_chunk(&bad));
    }
    http::Response response;
    response.body = "binary\0\xff\r\n"s;
    response.headers.add("Content-Encoding", "gzip");
    for (BodyFraming framing : {BodyFraming::fixed_length, BodyFraming::chunked, BodyFraming::close_delimited}) {
        const auto wire = test::take(http::serialize_response(response, framing));
        check_message(wire, response.body, framing);
        CHECK(!(wire.find("Content-Length:") != std::string::npos && wire.find("Transfer-Encoding:") != std::string::npos));
        if (framing == BodyFraming::close_delimited) CHECK(wire.find("Connection: close\r\n") != std::string::npos);
    }
    response.trailers.add("Checksum", "abc");
    response.trailers.add("checksum", "def");
    const auto final = test::take(http::encode_final_chunk(&response.trailers));
    CHECK(http::encode_final_chunk(&response.trailers, {final.size(), 0}));
    CHECK(!http::encode_final_chunk(&response.trailers, {final.size() - 1, 0}));
    response.headers.add("Transfer-Encoding", "chunked");
    const auto head_size = test::take(http::serialize_response_head(response)).size();
    CHECK(http::serialize_response(response, {head_size, response.body.size()}));
    CHECK(!http::serialize_response(response, {head_size - 1, response.body.size()}));
    CHECK(!http::serialize_response(response, {head_size, response.body.size() - 1}));
    const auto wire = test::take(http::serialize_response(response));
    http::ResponseParser parser;
    CHECK(parser.consume(wire).event == ParseEvent::complete);
    CHECK(parser.response().body == response.body && parser.response().trailers.entries() == response.trailers.entries());
    CHECK(!http::serialize_response(response, BodyFraming::fixed_length));
    CHECK(!http::serialize_response(response, BodyFraming::close_delimited));
    CHECK(!http::serialize_response(response, Method::HEAD));
    response.headers.add("Connection", "Checksum");
    CHECK(!http::serialize_response(response));
    response.headers.remove("Connection");
    response.trailers.clear();
    response.body.clear();
    CHECK(test::take(http::serialize_response(response)).ends_with("\r\n\r\n0\r\n\r\n"));
    for (const auto* te : {"gzip", "gzip, chunked", "chunked, chunked", "chunked,", "", "chunked;x=y"}) {
        response.headers.set("Transfer-Encoding", te);
        CHECK(!http::serialize_response_head(response) && !http::serialize_response(response));
    }
    response.headers.set("Transfer-Encoding", "chunked");
    response.version = http::HttpVersion::http_1_0();
    CHECK(!http::serialize_response_head(response) && !http::serialize_response(response));
    response.version = http::HttpVersion::http_1_1();
    response.headers.remove("Transfer-Encoding");
    response.body = "abc";
    CHECK(!http::serialize_response(response, BodyFraming::none));
    CHECK(!http::serialize_response(response, Method::CONNECT));
    response.headers.add("Content-Length", "999");
    const auto head = test::take(http::serialize_response(response, Method::HEAD));
    CHECK(head.ends_with("\r\n\r\n") && head.find("Content-Length: 999\r\n") != std::string::npos);
    response.headers.remove("Content-Length");
    const auto generated = test::take(http::serialize_response(response, Method::HEAD));
    CHECK(generated.ends_with("\r\n\r\n") && generated.find("Content-Length: 3\r\n") != std::string::npos);
    for (int status : {100, 103, 204, 304}) {
        http::Response no_body(status, "");
        CHECK(test::take(http::serialize_response(no_body)) == "HTTP/1.1 " + std::to_string(status) + " \r\n\r\n");
        no_body.body = "x";
        CHECK(!http::serialize_response(no_body) && !http::serialize_response(no_body, Method::HEAD));
        no_body.body.clear();
        for (const auto& field : std::vector<http::HeaderField>{{"Content-Length", "999"}, {"Transfer-Encoding", "chunked"}}) {
            no_body.headers.clear();
            no_body.headers.add(field.name, field.value);
            CHECK(static_cast<bool>(http::serialize_response_head(no_body)) == (status == 304));
            CHECK(static_cast<bool>(http::serialize_response(no_body)) == (status == 304));
        }
    }
    CHECK(!http::serialize_response(http::Response(101, "Switching Protocols")));
}
} // namespace

int main() {
    test_invalid_messages();
    test_completion_and_no_body();
    test_limit_boundaries();
    test_output();
}
