#pragma once

// Production-backed milestone 06 adapter. Every helper feeds real bytes to
// http::RequestParser or http::HttpSession; only the stream I/O is scripted.

#include "http/http_session.hpp"
#include "http/request_parser.hpp"
#include "net/stream.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tests {

struct ParseResult { std::size_t consumed; bool error; };

class ParserHarness {
public:
    ParserHarness() = default;
    explicit ParserHarness(http::RequestLimits limits) : parser_(limits) {}

    ParseResult consume(std::string_view bytes) {
        const auto result = parser_.consume(bytes);
        return {result.consumed, result.event == http::ParseEvent::error};
    }

    [[nodiscard]] bool complete() const { return parser_.complete(); }
    [[nodiscard]] std::string target() const { return parser_.request().target; }
    [[nodiscard]] http::ParseError error() const { return parser_.error(); }

    [[nodiscard]] std::vector<std::string> headers(std::string_view name) const {
        std::vector<std::string> values;
        for (auto value : parser_.request().headers.get_all(name)) values.emplace_back(value);
        return values;
    }

    void reset() { parser_.reset(); }

private:
    http::RequestParser parser_;
};

// Hands out scripted input and records output; nothing else is faked.
class ScriptedChannel final : public net::StreamChannel {
public:
    explicit ScriptedChannel(std::string_view input) : input_(input) {}

    [[nodiscard]] net::ReadResult read_some(std::span<std::byte> destination) override {
        if (offset_ == input_.size()) return net::ReadResult::eof();
        const std::size_t count = std::min(destination.size(), input_.size() - offset_);
        std::memcpy(destination.data(), input_.data() + offset_, count);
        offset_ += count;
        return net::ReadResult::success(count);
    }

    [[nodiscard]] net::WriteResult write_some(std::span<const std::byte> source) override {
        output_.append(reinterpret_cast<const char*>(source.data()), source.size());
        return net::WriteResult::success(source.size());
    }

    void close() noexcept override { closed_ = true; }
    [[nodiscard]] bool is_valid() const noexcept override { return !closed_; }

    [[nodiscard]] const std::string& output() const noexcept { return output_; }
    [[nodiscard]] std::size_t bytes_read() const noexcept { return offset_; }

private:
    std::string input_;
    std::string output_;
    std::size_t offset_{0};
    bool closed_{false};
};

// A valid request whose head is exactly `size` bytes, padded with one field.
inline std::string request_of_size(std::size_t size) {
    const std::string start = "GET / HTTP/1.1\r\nHost: a\r\nX-Pad: ";
    const std::string end = "\r\n\r\n";
    CHECK(size >= start.size() + end.size());
    return start + std::string(size - start.size() - end.size(), 'p') + end;
}

inline http::ParseError parse_with(http::RequestLimits limits, std::string_view request) {
    http::RequestParser parser(limits);
    const auto result = parser.consume(request);
    if (result.event == http::ParseEvent::complete) return http::ParseError::none;
    return result.event == http::ParseEvent::error ? parser.error() : http::ParseError::malformed_request_line;
}

// Rejected at the limit, accepted when the limit equals the target length.
inline bool rejects_target_limit(std::size_t limit, std::string_view target) {
    const std::string request = "GET " + std::string(target) + " HTTP/1.1\r\nHost: a\r\n\r\n";
    http::RequestLimits limits;
    limits.max_target_bytes = limit;
    const bool rejected = parse_with(limits, request) == http::ParseError::target_too_long;
    limits.max_target_bytes = target.size();
    return rejected && target.size() > limit && parse_with(limits, request) == http::ParseError::none;
}

inline bool rejects_header_byte_limit(std::size_t limit) {
    http::RequestLimits limits;
    limits.max_header_bytes = limit;
    return parse_with(limits, request_of_size(limit)) == http::ParseError::none &&
           parse_with(limits, request_of_size(limit + 1)) == http::ParseError::header_too_large;
}

inline bool rejects_header_count_limit(std::size_t limit) {
    std::string fields = "Host: a\r\n";
    for (std::size_t i = 1; i < limit; ++i) fields += "X-" + std::to_string(i) + ": v\r\n";
    const std::string at_limit = "GET / HTTP/1.1\r\n" + fields + "\r\n";
    const std::string over_limit = "GET / HTTP/1.1\r\n" + fields + "X-Over: v\r\n\r\n";
    http::RequestLimits limits;
    limits.max_header_count = limit;
    return parse_with(limits, at_limit) == http::ParseError::none &&
           parse_with(limits, over_limit) == http::ParseError::too_many_headers;
}

struct SessionRun {
    http::HttpSessionStats stats;
    std::string output;
    std::size_t bytes_read{0};
};

inline SessionRun run_session(std::string_view input, http::HttpSessionConfig config) {
    ScriptedChannel channel(input);
    auto stats = http::run_http_session(channel, config);
    return {stats, channel.output(), channel.bytes_read()};
}

// The header limit stays generous, so only the connection buffer can refuse.
inline bool rejects_connection_input_limit(std::size_t limit) {
    http::HttpSessionConfig config;
    config.max_header_bytes = 8192;
    config.max_input_bytes = limit;
    const auto fits = run_session(request_of_size(limit), config);
    const auto too_big = run_session(request_of_size(limit + 1), config);
    return fits.stats.response_status == 200 &&
           too_big.stats.input_limit_exceeded && !too_big.stats.header_limit_exceeded &&
           too_big.stats.response_status == 431 && too_big.bytes_read <= limit &&
           too_big.stats.peak_input_bytes <= limit &&
           too_big.output.starts_with("HTTP/1.1 431 ");
}

inline bool session_maps(std::string_view request, http::HttpSessionConfig config, int status) {
    const auto run = run_session(request, config);
    return run.stats.response_status == status && run.stats.response_sent &&
           run.output.starts_with("HTTP/1.1 " + std::to_string(status) + " ");
}

inline bool maps_syntax_to_status(int status) {
    return session_maps("GET / HTTP/1.1 extra\r\nHost: a\r\n\r\n", {}, status) &&
           session_maps("GET / HTTP/1.1\r\nHost: a,b\r\n\r\n", {}, status) &&
           session_maps("GET / HTTP/1.1\r\nHost: a\r\n folded\r\n\r\n", {}, status);
}

inline bool maps_version_to_status(int status) {
    return session_maps("GET / HTTP/2.0\r\nHost: a\r\n\r\n", {}, status);
}

inline bool maps_header_limit_to_status(int status) {
    http::HttpSessionConfig few_fields;
    few_fields.max_header_count = 1;
    http::HttpSessionConfig few_bytes;
    few_bytes.max_header_bytes = 64;
    return session_maps("GET / HTTP/1.1\r\nHost: a\r\nX: 1\r\n\r\n", few_fields, status) &&
           session_maps(request_of_size(65), few_bytes, status);
}

} // namespace tests
