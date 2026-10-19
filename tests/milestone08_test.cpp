#include "http/framing.hpp"
#include "http/response_parser.hpp"
#include "http/serializer.hpp"
#include "test_support.hpp"

#include <string>
#include <string_view>
#include <vector>

int main() {
    // 1. Status-line parsing
    {
        http::ResponseParser parser;
        auto res = parser.consume("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
        CHECK(parser.complete() && !parser.failed());
        CHECK(parser.response().status_code == 200);
        CHECK(parser.response().reason == "OK");
        CHECK(parser.response().version == http::HttpVersion::http_1_1());

        // HTTP/1.0
        parser.reset();
        parser.consume("HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\n\r\n");
        CHECK(parser.complete() && !parser.failed());
        CHECK(parser.response().status_code == 404);
        CHECK(parser.response().version == http::HttpVersion::http_1_0());

        // Empty reason phrase
        parser.reset();
        parser.consume("HTTP/1.1 204 \r\n\r\n");
        CHECK(parser.complete() && !parser.failed());
        CHECK(parser.response().status_code == 204);

        // Malformed status lines
        for (const auto malformed : {
            "HTTP/2.0 200 OK\r\n\r\n",
            "HTTP/1.1 OK\r\n\r\n",
            "HTTP/1.1 999 OK\r\n\r\n",
            "HTTP/1.1 20 OK\r\n\r\n",
            "HTTP/1.1 200OK\r\n\r\n"}) {
            http::ResponseParser p;
            p.consume(malformed);
            CHECK(p.failed());
        }
    }

    // 2. Fixed-length upstream response
    {
        const std::string fixed = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
        for (std::size_t split = 0; split <= fixed.size(); ++split) {
            http::ResponseParser parser;
            auto first = parser.consume(std::string_view(fixed).substr(0, split));
            CHECK(!parser.failed());
            auto second = parser.consume(std::string_view(fixed).substr(split));
            CHECK(!parser.failed() && parser.complete());
            CHECK(first.consumed + second.consumed == fixed.size());
            CHECK(parser.response().body == "hello");
            CHECK(parser.framing() == http::BodyFraming::fixed_length);
        }
    }

    // 3. Chunked upstream response
    {
        const std::string chunked =
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
            "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
        for (std::size_t split = 0; split <= chunked.size(); ++split) {
            http::ResponseParser parser;
            auto first = parser.consume(std::string_view(chunked).substr(0, split));
            CHECK(!parser.failed());
            auto second = parser.consume(std::string_view(chunked).substr(split));
            CHECK(!parser.failed() && parser.complete());
            CHECK(first.consumed + second.consumed == chunked.size());
            CHECK(parser.response().body == "Wikipedia");
            CHECK(parser.framing() == http::BodyFraming::chunked);
        }
    }

    // 4. Close-delimited upstream response & finish_on_eof()
    {
        const std::string stream_data = "HTTP/1.0 200 OK\r\nServer: origin\r\n\r\nstreamed body content until EOF";
        http::ResponseParser parser;
        auto res = parser.consume(stream_data);
        CHECK(!parser.failed() && !parser.complete());
        CHECK(parser.framing() == http::BodyFraming::close_delimited);
        CHECK(parser.response().body == "streamed body content until EOF");

        // finish_on_eof() marks valid completion
        CHECK(parser.finish_on_eof());
        CHECK(parser.complete());

        // Premature EOF on fixed-length response
        http::ResponseParser p_fixed;
        p_fixed.consume("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort");
        CHECK(!p_fixed.finish_on_eof());
        CHECK(p_fixed.failed() && p_fixed.error() == http::ParseError::premature_eof);

        // Premature EOF on chunked response
        http::ResponseParser p_chunk;
        p_chunk.consume("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nWiki");
        CHECK(!p_chunk.finish_on_eof());
        CHECK(p_chunk.failed() && p_chunk.error() == http::ParseError::premature_eof);
    }

    // 5. Informational responses (1xx) and protocol switching rejection
    {
        const std::string interim =
            "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\ndone";
        http::ResponseParser parser;
        auto res = parser.consume(interim);
        CHECK(!parser.failed() && parser.complete());
        CHECK(parser.informational_responses().size() == 1);
        CHECK(parser.informational_responses()[0].status_code == 100);
        CHECK(parser.response().status_code == 200);
        CHECK(parser.response().body == "done");

        // Protocol switching (101) rejected
        http::ResponseParser p_switch;
        p_switch.consume("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n\r\n");
        CHECK(p_switch.failed());
    }

    // 6. No body on HEAD, 204, 304
    {
        // Response to HEAD request has NO body even with Content-Length present
        const std::string head_resp = "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n";
        http::ResponseParser head_parser(http::Method::HEAD);
        auto res = head_parser.consume(head_resp);
        CHECK(head_parser.complete() && !head_parser.failed());
        CHECK(head_parser.response().body.empty());
        CHECK(head_parser.framing() == http::BodyFraming::none);

        // 204 No Content has NO body
        http::ResponseParser p_204;
        p_204.consume("HTTP/1.1 204 No Content\r\n\r\n");
        CHECK(p_204.complete() && p_204.response().body.empty());
        CHECK(p_204.framing() == http::BodyFraming::none);

        // 304 Not Modified has NO body
        http::ResponseParser p_304;
        p_304.consume("HTTP/1.1 304 Not Modified\r\nETag: \"test\"\r\n\r\n");
        CHECK(p_304.complete() && p_304.response().body.empty());
        CHECK(p_304.framing() == http::BodyFraming::none);
    }

    // 7. Chunked serialization
    {
        CHECK(test::take(http::encode_chunk("hello")) == "5\r\nhello\r\n");
        CHECK(test::take(http::encode_chunk("")) == "0\r\n\r\n");
        CHECK(test::take(http::encode_final_chunk()) == "0\r\n\r\n");

        http::HeaderMap trailers;
        trailers.add("Checksum", "12345");
        CHECK(test::take(http::encode_final_chunk(&trailers)) == "0\r\nChecksum: 12345\r\n\r\n");
    }

    // 8. Serializer mutual exclusion (never generate both CL and TE)
    {
        http::Response resp(http::StatusCode::OK);
        resp.headers.set("Transfer-Encoding", "chunked");
        resp.headers.set("Content-Length", "10");
        CHECK(!http::serialize_response_head(resp));
        CHECK(!http::serialize_response(resp));

        // Valid chunked response serialization (head only or TE without CL)
        http::Response chunked_resp(http::StatusCode::OK);
        chunked_resp.headers.set("Transfer-Encoding", "chunked");
        auto wire = http::serialize_response_head(chunked_resp);
        CHECK(wire);
        CHECK(wire.value().find("Transfer-Encoding: chunked\r\n") != std::string::npos);
        CHECK(wire.value().find("Content-Length:") == std::string::npos);
    }

    // 9. Opaque compressed representation
    {
        // Binary gzip bytes are decoded through chunking without modification
        using namespace std::string_literals;
        std::string binary_gz = "\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03\x4b\xcb\xcf\x07\x00\x21\x65\x73\x8b\x03\x00\x00\x00"s;
        CHECK(binary_gz.size() == 23);
        std::string chunked_gz =
            "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nTransfer-Encoding: chunked\r\n\r\n" +
            test::take(http::encode_chunk(binary_gz)) +
            test::take(http::encode_final_chunk());

        http::ResponseParser parser;
        parser.consume(chunked_gz);
        CHECK(parser.complete() && !parser.failed());
        CHECK(parser.response().body == binary_gz);
        CHECK(parser.response().headers.get("content-encoding") == "gzip");
    }

    return 0;
}
