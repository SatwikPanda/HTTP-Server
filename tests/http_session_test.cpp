#include "http/http_session.hpp"
#include "net/stream.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

class ControlledStreamChannel : public net::StreamChannel {
public:
    std::vector<std::byte> input_data;
    std::size_t input_read_offset{0};
    std::size_t max_read_chunk{0}; // 0 = unlimited
    int read_would_block_cycles{0};

    std::vector<std::byte> output_data;
    std::size_t max_write_chunk{0}; // 0 = unlimited, > 0 forces short writes
    int write_would_block_cycles{0};

    bool is_closed{false};
    std::size_t total_short_writes{0};
    std::size_t total_write_calls{0};
    std::size_t total_read_calls{0};

    [[nodiscard]] net::ReadResult read_some(std::span<std::byte> destination) override {
        total_read_calls++;
        if (is_closed) {
            return net::ReadResult::failure(
                core::make_error(core::ErrorCategory::bad_descriptor, "read_some")
            );
        }
        if (read_would_block_cycles > 0) {
            read_would_block_cycles--;
            return net::ReadResult::would_block_result();
        }
        if (input_read_offset >= input_data.size()) {
            return net::ReadResult::eof();
        }

        std::size_t available = input_data.size() - input_read_offset;
        std::size_t to_copy = std::min(available, destination.size());
        if (max_read_chunk > 0) {
            to_copy = std::min(to_copy, max_read_chunk);
        }

        std::memcpy(destination.data(), input_data.data() + input_read_offset, to_copy);
        input_read_offset += to_copy;
        return net::ReadResult::success(to_copy);
    }

    [[nodiscard]] net::WriteResult write_some(std::span<const std::byte> source) override {
        total_write_calls++;
        if (is_closed) {
            return net::WriteResult::failure(
                core::make_error(core::ErrorCategory::bad_descriptor, "write_some")
            );
        }
        if (write_would_block_cycles > 0) {
            write_would_block_cycles--;
            return net::WriteResult::would_block_result();
        }

        std::size_t to_write = source.size();
        if (max_write_chunk > 0 && to_write > max_write_chunk) {
            to_write = max_write_chunk;
            total_short_writes++;
        }

        const auto* src = reinterpret_cast<const std::byte*>(source.data());
        output_data.insert(output_data.end(), src, src + to_write);

        return net::WriteResult::success(to_write);
    }

    void close() noexcept override {
        is_closed = true;
    }

    [[nodiscard]] bool is_valid() const noexcept override {
        return !is_closed;
    }

    void set_input(std::string_view str) {
        const auto* ptr = reinterpret_cast<const std::byte*>(str.data());
        input_data.assign(ptr, ptr + str.size());
        input_read_offset = 0;
    }

    [[nodiscard]] std::string output_string() const {
        return std::string(
            reinterpret_cast<const char*>(output_data.data()),
            output_data.size()
        );
    }
};

} // namespace

void test_session_normal_request() {
    std::cout << "[RUN] test_session_normal_request..." << std::endl;
    ControlledStreamChannel channel;
    channel.set_input("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");

    http::HttpSessionConfig config;
    config.response_body = "Hello, World!\r\n";

    auto stats = http::run_http_session(channel, config);

    assert(stats.completed_cleanly);
    assert(stats.request_received);
    assert(stats.response_sent);

    std::string out = channel.output_string();
    assert(out.starts_with("HTTP/1.1 200 OK\r\n"));
    assert(out.find("\r\nContent-Length: 15\r\n") != std::string::npos);
    assert(out.find("\r\nConnection: close\r\n") != std::string::npos);
    assert(out.find("\r\nContent-Type: text/plain; charset=utf-8\r\n") != std::string::npos);
    assert(out.find("\r\nDate: ") != std::string::npos);
    assert(out.ends_with("Hello, World!\r\n"));
    std::cout << "[PASS] test_session_normal_request" << std::endl;
}

void test_session_fragmented_reads() {
    std::cout << "[RUN] test_session_fragmented_reads..." << std::endl;
    ControlledStreamChannel channel;
    channel.set_input("GET /index.html HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nUser-Agent: curl/7.88\r\n\r\n");
    channel.max_read_chunk = 1; // 1 byte at a time

    http::HttpSessionConfig config;
    config.response_body = "Fixed Body";

    auto stats = http::run_http_session(channel, config);

    assert(stats.completed_cleanly);
    assert(stats.request_received);
    assert(stats.response_sent);

    std::string out = channel.output_string();
    assert(out.starts_with("HTTP/1.1 200 OK\r\n"));
    assert(out.find("\r\nContent-Length: 10\r\n") != std::string::npos);
    assert(out.ends_with("Fixed Body"));
    std::cout << "[PASS] test_session_fragmented_reads" << std::endl;
}

void test_session_forced_short_writes() {
    std::cout << "[RUN] test_session_forced_short_writes..." << std::endl;
    ControlledStreamChannel channel;
    channel.set_input("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
    channel.max_write_chunk = 3; // Force short writes of 3 bytes

    http::HttpSessionConfig config;
    config.response_body = "Testing forced short writes payload!";

    auto stats = http::run_http_session(channel, config);

    assert(stats.completed_cleanly);
    assert(stats.response_sent);
    assert(stats.short_write_count > 0);

    std::string out = channel.output_string();
    assert(out.starts_with("HTTP/1.1 200 OK\r\n"));
    assert(out.ends_with("Testing forced short writes payload!"));
    std::cout << "[PASS] test_session_forced_short_writes" << std::endl;
}

void test_session_would_block_cycles() {
    std::cout << "[RUN] test_session_would_block_cycles..." << std::endl;
    ControlledStreamChannel channel;
    channel.set_input("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
    channel.read_would_block_cycles = 3;
    channel.write_would_block_cycles = 2;

    http::HttpSessionConfig config;
    config.wait_retry_delay = std::chrono::milliseconds(1);
    config.retry_timeout = std::chrono::milliseconds(500);

    auto stats = http::run_http_session(channel, config);

    assert(stats.completed_cleanly);
    assert(stats.would_block_read_count == 3);
    assert(stats.would_block_write_count == 2);
    assert(stats.response_sent);
    std::cout << "[PASS] test_session_would_block_cycles" << std::endl;
}

void test_session_empty_disconnect() {
    std::cout << "[RUN] test_session_empty_disconnect..." << std::endl;
    ControlledStreamChannel channel;
    // input is empty: client disconnects immediately on accept

    http::HttpSessionConfig config;
    auto stats = http::run_http_session(channel, config);

    assert(!stats.request_received);
    assert(!stats.response_sent);
    assert(channel.output_data.empty());
    std::cout << "[PASS] test_session_empty_disconnect" << std::endl;
}

void test_session_header_limit_exceeded() {
    std::cout << "[RUN] test_session_header_limit_exceeded..." << std::endl;
    ControlledStreamChannel channel;
    // Send 10,000 bytes of headers without ending \r\n\r\n
    std::string huge_headers(10000, 'X');
    channel.set_input(huge_headers);

    http::HttpSessionConfig config;
    config.max_header_bytes = 4096;

    auto stats = http::run_http_session(channel, config);

    assert(stats.header_limit_exceeded);
    assert(stats.response_sent);

    std::string out = channel.output_string();
    assert(out.starts_with("HTTP/1.1 431 Request Header Fields Too Large\r\n"));
    assert(out.find("Connection: close\r\n") != std::string::npos);
    std::cout << "[PASS] test_session_header_limit_exceeded" << std::endl;
}

int main() {
    std::cout << "Starting HTTP Session Tests..." << std::endl;
    test_session_normal_request();
    test_session_fragmented_reads();
    test_session_forced_short_writes();
    test_session_would_block_cycles();
    test_session_empty_disconnect();
    test_session_header_limit_exceeded();
    std::cout << "All HTTP Session Tests PASSED!" << std::endl;
    return 0;
}
