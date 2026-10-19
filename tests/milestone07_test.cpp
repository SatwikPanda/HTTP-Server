#include "http/framing.hpp"
#include "http/http_session.hpp"
#include "http/request_parser.hpp"
#include "net/stream.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

class ControlledChannel final : public net::StreamChannel {
public:
    explicit ControlledChannel(std::string_view input) {
        const auto* ptr = reinterpret_cast<const std::byte*>(input.data());
        input_data_.assign(ptr, ptr + input.size());
    }

    [[nodiscard]] net::ReadResult read_some(std::span<std::byte> destination) override {
        if (closed_) return net::ReadResult::failure(core::make_error(core::ErrorCategory::bad_descriptor, "read_some"));
        if (read_offset_ >= input_data_.size()) return net::ReadResult::eof();
        const std::size_t to_copy = std::min(input_data_.size() - read_offset_, destination.size());
        std::memcpy(destination.data(), input_data_.data() + read_offset_, to_copy);
        read_offset_ += to_copy;
        return net::ReadResult::success(to_copy);
    }

    [[nodiscard]] net::WriteResult write_some(std::span<const std::byte> source) override {
        if (closed_) return net::WriteResult::failure(core::make_error(core::ErrorCategory::bad_descriptor, "write_some"));
        output_.append(reinterpret_cast<const char*>(source.data()), source.size());
        return net::WriteResult::success(source.size());
    }

    void close() noexcept override { closed_ = true; }
    [[nodiscard]] bool is_valid() const noexcept override { return !closed_; }
    [[nodiscard]] const std::string& output() const noexcept { return output_; }

private:
    std::vector<std::byte> input_data_;
    std::size_t read_offset_{0};
    std::string output_;
    bool closed_{false};
};

} // namespace

int main() {
    // 1. Framing selection and header parsing
    {
        // No body
        http::RequestParser p1;
        auto res = p1.consume("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
        CHECK(!p1.failed() && p1.complete());
        CHECK(p1.request().body.empty());
        CHECK(p1.framing() == http::ParserFraming::none);
    }

    // 2. Fixed-length body parsing across all split points
    {
        const std::string fixed_req = "POST /api HTTP/1.1\r\nHost: a\r\nContent-Length: 11\r\n\r\nhello world";
        for (std::size_t split = 0; split <= fixed_req.size(); ++split) {
            http::RequestParser parser;
            auto first = parser.consume(std::string_view(fixed_req).substr(0, split));
            CHECK(!parser.failed());
            auto second = parser.consume(std::string_view(fixed_req).substr(split));
            CHECK(!parser.failed() && parser.complete());
            CHECK(first.consumed + second.consumed == fixed_req.size());
            CHECK(parser.request().body == "hello world");
            CHECK(parser.framing() == http::ParserFraming::fixed_length);
        }

        // Byte by byte
        http::RequestParser bytewise;
        for (char c : fixed_req) {
            auto r = bytewise.consume(std::string_view(&c, 1));
            CHECK(!bytewise.failed());
        }
        CHECK(bytewise.complete() && bytewise.request().body == "hello world");
    }

    // 3. Pipelining: bytes following fixed-length body are preserved
    {
        const std::string first_req = "POST /1 HTTP/1.1\r\nHost: a\r\nContent-Length: 4\r\n\r\ntest";
        const std::string second_req = "GET /2 HTTP/1.1\r\nHost: a\r\n\r\n";
        const std::string combined = first_req + second_req;

        http::RequestParser parser;
        auto res1 = parser.consume(combined);
        CHECK(parser.complete() && !parser.failed());
        CHECK(res1.consumed == first_req.size());
        CHECK(parser.request().body == "test");

        parser.reset();
        auto res2 = parser.consume(std::string_view(combined).substr(res1.consumed));
        CHECK(parser.complete() && !parser.failed());
        CHECK(res2.consumed == second_req.size());
        CHECK(parser.request().target == "/2");
    }

    // 4. Chunked body decoding across split points
    {
        const std::string chunked_req =
            "POST /chunk HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n"
            "4\r\nWiki\r\n5\r\npedia\r\nf\r\n in \r\n\r\nchunks.\r\n0\r\n\r\n";
        const std::string expected_body = "Wikipedia in \r\n\r\nchunks.";

        for (std::size_t split = 0; split <= chunked_req.size(); ++split) {
            http::RequestParser parser;
            auto first = parser.consume(std::string_view(chunked_req).substr(0, split));
            CHECK(!parser.failed());
            auto second = parser.consume(std::string_view(chunked_req).substr(split));
            CHECK(!parser.failed() && parser.complete());
            CHECK(first.consumed + second.consumed == chunked_req.size());
            CHECK(parser.request().body == expected_body);
            CHECK(parser.framing() == http::ParserFraming::chunked);
        }

        // Byte by byte
        http::RequestParser bytewise;
        for (char c : chunked_req) {
            bytewise.consume(std::string_view(&c, 1));
            CHECK(!bytewise.failed());
        }
        CHECK(bytewise.complete() && bytewise.request().body == expected_body);
    }

    // 5. Chunk extensions and trailers
    {
        const std::string ext_req =
            "POST /ext HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n"
            "5;foo=bar;baz\r\nHello\r\n0;last\r\nChecksum: abc123\r\nX-Trailer: custom\r\n\r\n";
        http::RequestParser parser;
        auto res = parser.consume(ext_req);
        CHECK(parser.complete() && !parser.failed());
        CHECK(parser.request().body == "Hello");
        CHECK(parser.request().trailers.get("checksum") == "abc123");
        CHECK(parser.request().trailers.get("x-trailer") == "custom");
    }

    // 6. Prohibited trailers rejected
    for (const std::string prohibited : {
        "Host: evil.com",
        "Content-Length: 10",
        "Transfer-Encoding: chunked",
        "Authorization: secret",
        "Cookie: sess=1",
        "Expect: 100-continue"}) {
        const std::string req =
            "POST /p HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n"
            "4\r\ntest\r\n0\r\n" + prohibited + "\r\n\r\n";
        http::RequestParser parser;
        parser.consume(req);
        CHECK(parser.failed());
        CHECK(parser.error() == http::ParseError::prohibited_trailer);
    }

    // 7. Ambiguous framing and invalid lengths rejected
    {
        // TE + CL
        http::RequestParser p_te_cl;
        p_te_cl.consume("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n");
        CHECK(p_te_cl.failed() && p_te_cl.error() == http::ParseError::ambiguous_framing);

        // Conflicting CL
        http::RequestParser p_conf;
        p_conf.consume("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n");
        CHECK(p_conf.failed() && p_conf.error() == http::ParseError::conflicting_content_length);

        // Identical duplicate CL accepted
        http::RequestParser p_dup;
        p_dup.consume("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n12345");
        CHECK(p_dup.complete() && !p_dup.failed());
        CHECK(p_dup.request().body == "12345");

        // Invalid CL (non-digit, negative, empty)
        for (const auto bad_cl : {"-1", "abc", "", "12a"}) {
            http::RequestParser p_bad;
            p_bad.consume("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: " + std::string(bad_cl) + "\r\n\r\n");
            CHECK(p_bad.failed() && p_bad.error() == http::ParseError::invalid_content_length);
        }

        // Numeric overflow
        http::RequestParser p_over;
        p_over.consume("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 999999999999999999999999999999\r\n\r\n");
        CHECK(p_over.failed() && p_over.error() == http::ParseError::content_length_overflow);
    }

    // 8. Transfer-Encoding coding restrictions
    {
        // Not ending in chunked
        http::RequestParser p1;
        p1.consume("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked, gzip\r\n\r\n");
        CHECK(p1.failed() && p1.error() == http::ParseError::invalid_transfer_encoding);

        // Unsupported coding alone
        http::RequestParser p2;
        p2.consume("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: gzip\r\n\r\n");
        CHECK(p2.failed() && p2.error() == http::ParseError::unsupported_transfer_coding);
        CHECK(http::status_for(p2.error()) == http::StatusCode::NotImplemented);
    }

    // 9. Limits enforcement
    {
        // Body limit
        http::RequestLimits limits;
        limits.max_body_bytes = 10;
        http::RequestParser p_body_limit(limits);
        p_body_limit.consume("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 15\r\n\r\n123456789012345");
        CHECK(p_body_limit.failed() && p_body_limit.error() == http::ParseError::body_too_large);
        CHECK(http::status_for(p_body_limit.error()) == http::StatusCode::PayloadTooLarge);

        // Chunk size limit
        http::RequestParser p_chunk_limit(limits);
        p_chunk_limit.consume("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\nb\r\n12345678901\r\n");
        CHECK(p_chunk_limit.failed() && p_chunk_limit.error() == http::ParseError::body_too_large);

        // Chunk extension limit
        limits.max_chunk_extension_bytes = 5;
        http::RequestParser p_ext_limit(limits);
        p_ext_limit.consume("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n4;toolong=12345\r\n");
        CHECK(p_ext_limit.failed() && p_ext_limit.error() == http::ParseError::chunk_extension_too_large);
    }

    // 10. Premature EOF detection
    {
        http::RequestParser p_eof;
        p_eof.consume("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 10\r\n\r\n12345");
        CHECK(!p_eof.complete());
        CHECK(!p_eof.finish_on_eof());
        CHECK(p_eof.failed() && p_eof.error() == http::ParseError::premature_eof);
    }

    // 11. Application restrictions (in HttpSession)
    {
        http::HttpSessionConfig config;

        // Nonempty body on GET must be rejected with 400 and closed
        {
            ControlledChannel ch("GET / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nhello");
            auto stats = http::run_http_session(ch, config);
            CHECK(stats.response_sent);
            CHECK(ch.output().starts_with("HTTP/1.1 400 "));
        }

        // Nonempty body on HEAD must be rejected with 400 and closed
        {
            ControlledChannel ch("HEAD / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nhello");
            auto stats = http::run_http_session(ch, config);
            CHECK(stats.response_sent);
            CHECK(ch.output().starts_with("HTTP/1.1 400 "));
        }

        // Empty body on GET (Content-Length: 0) is valid
        {
            ControlledChannel ch("GET / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n");
            auto stats = http::run_http_session(ch, config);
            CHECK(stats.completed_cleanly && stats.response_sent);
            CHECK(ch.output().starts_with("HTTP/1.1 200 OK"));
        }

        // Unsupported Expect: 100-continue rejected with 417
        {
            ControlledChannel ch("GET / HTTP/1.1\r\nHost: localhost\r\nExpect: 100-continue\r\n\r\n");
            auto stats = http::run_http_session(ch, config);
            CHECK(stats.response_sent);
            CHECK(ch.output().starts_with("HTTP/1.1 417 Expectation Failed"));
        }

        // Recognized method not allowed for file server (e.g. POST) receives 405 and Allow: GET, HEAD
        {
            ControlledChannel ch("POST /submit HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\n\r\ntest");
            auto stats = http::run_http_session(ch, config);
            CHECK(stats.response_sent);
            CHECK(ch.output().starts_with("HTTP/1.1 405 Method Not Allowed"));
            CHECK(ch.output().find("Allow: GET, HEAD\r\n") != std::string::npos);
        }

        // CONNECT rejected
        {
            ControlledChannel ch("CONNECT example.com:443 HTTP/1.1\r\nHost: example.com\r\n\r\n");
            auto stats = http::run_http_session(ch, config);
            CHECK(stats.response_sent);
            CHECK(ch.output().starts_with("HTTP/1.1 405 "));
        }

        // Upgrade rejected with 400
        {
            ControlledChannel ch("GET /chat HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n\r\n");
            auto stats = http::run_http_session(ch, config);
            CHECK(stats.response_sent);
            CHECK(ch.output().starts_with("HTTP/1.1 400 "));
        }
    }

    return 0;
}
