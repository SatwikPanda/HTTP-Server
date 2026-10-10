#include "http/http_session.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <string_view>
#include <thread>
#include <vector>

namespace http {

namespace {

bool contains_header_end(std::string_view sv) noexcept {
    return sv.find("\r\n\r\n") != std::string_view::npos ||
           sv.find("\n\n") != std::string_view::npos;
}

} // namespace

HttpSessionStats run_http_session(
    net::StreamChannel& channel,
    const HttpSessionConfig& config
) {
    HttpSessionStats stats{};

    std::vector<std::byte> accumulated_headers;
    accumulated_headers.reserve(1024);

    std::array<std::byte, 512> read_chunk{};
    auto last_progress_time = std::chrono::steady_clock::now();

    // 1. Accumulate incoming bytes until header block end is found or limit exceeded
    while (channel.is_valid()) {
        net::ReadResult read_res = channel.read_some(read_chunk);

        if (read_res.is_ok()) {
            std::size_t n = read_res.bytes_transferred;
            stats.total_bytes_read += n;
            accumulated_headers.insert(
                accumulated_headers.end(),
                read_chunk.begin(),
                read_chunk.begin() + n
            );
            last_progress_time = std::chrono::steady_clock::now();

            std::string_view accumulated_sv(
                reinterpret_cast<const char*>(accumulated_headers.data()),
                accumulated_headers.size()
            );

            if (contains_header_end(accumulated_sv)) {
                stats.request_received = true;
                break;
            }

            if (accumulated_headers.size() > config.max_header_bytes) {
                stats.header_limit_exceeded = true;
                break;
            }
            continue;
        }

        if (read_res.is_eof()) {
            // Client closed stream before header block completed
            break;
        }

        if (read_res.would_block()) {
            stats.would_block_read_count++;
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - last_progress_time
            );
            if (elapsed >= config.retry_timeout) {
                break;
            }
            std::this_thread::sleep_for(config.wait_retry_delay);
            continue;
        }

        if (read_res.is_error()) {
            break;
        }
    }

    // 2. If valid headers were received or limit exceeded, formulate and send response
    if (stats.request_received || stats.header_limit_exceeded) {
        Response resp;
        if (stats.header_limit_exceeded) {
            resp.set_status(StatusCode::HeaderFieldsTooLarge);
            resp.set_body("431 Request Header Fields Too Large\r\n", "text/plain; charset=utf-8");
        } else {
            resp.set_status(config.response_status);
            resp.set_body(config.response_body, config.content_type);
        }

        std::string date_val = config.fixed_wall_time.has_value()
            ? format_http_date(config.fixed_wall_time.value())
            : format_http_date();
        resp.headers.set("Date", std::move(date_val));
        resp.headers.set("Connection", "close");

        auto serialized_res = serialize_response(resp);
        if (!serialized_res) {
            return stats;
        }

        const std::string& serialized_str = serialized_res.value();
        std::size_t unsent_offset = 0;
        const std::size_t total_to_send = serialized_str.size();
        last_progress_time = std::chrono::steady_clock::now();

        // 3. Write out response completely, handling short writes and would-block
        while (channel.is_valid() && unsent_offset < total_to_send) {
            std::span<const std::byte> to_send(
                reinterpret_cast<const std::byte*>(serialized_str.data() + unsent_offset),
                total_to_send - unsent_offset
            );

            net::WriteResult write_res = channel.write_some(to_send);

            if (write_res.is_ok()) {
                std::size_t sent = write_res.bytes_transferred;
                if (sent < to_send.size()) {
                    stats.short_write_count++;
                }
                unsent_offset += sent;
                stats.total_bytes_written += sent;
                last_progress_time = std::chrono::steady_clock::now();
                continue;
            }

            if (write_res.would_block()) {
                stats.would_block_write_count++;
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - last_progress_time
                );
                if (elapsed >= config.retry_timeout) {
                    break;
                }
                std::this_thread::sleep_for(config.wait_retry_delay);
                continue;
            }

            if (write_res.is_error()) {
                break;
            }
        }

        if (unsent_offset == total_to_send) {
            stats.response_sent = true;
            stats.completed_cleanly = true;
        }
    }

    return stats;
}

HttpSessionStats run_http_session(
    net::Socket& socket,
    const HttpSessionConfig& config
) {
    net::SocketStreamChannel channel(socket);
    return run_http_session(channel, config);
}

} // namespace http
