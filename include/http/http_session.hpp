#pragma once

#include "http/date.hpp"
#include "http/response.hpp"
#include "http/serializer.hpp"
#include "net/socket.hpp"
#include "net/stream.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>

namespace http {

struct HttpSessionConfig {
    std::size_t max_header_bytes{8192};
    std::chrono::milliseconds wait_retry_delay{1};
    std::chrono::milliseconds retry_timeout{5000};
    std::string response_body{"Hello, World!\r\n"};
    std::string content_type{"text/plain; charset=utf-8"};
    StatusCode response_status{StatusCode::OK};
    std::optional<std::chrono::system_clock::time_point> fixed_wall_time{std::nullopt};
};

struct HttpSessionStats {
    std::size_t total_bytes_read{0};
    std::size_t total_bytes_written{0};
    std::size_t short_write_count{0};
    std::size_t would_block_read_count{0};
    std::size_t would_block_write_count{0};
    bool request_received{false};
    bool response_sent{false};
    bool header_limit_exceeded{false};
    bool completed_cleanly{false};
};

// Process a single HTTP request-response session on an abstract StreamChannel
HttpSessionStats run_http_session(
    net::StreamChannel& channel,
    const HttpSessionConfig& config = {}
);

// Process a single HTTP request-response session on a concrete Socket
HttpSessionStats run_http_session(
    net::Socket& socket,
    const HttpSessionConfig& config = {}
);

} // namespace http
