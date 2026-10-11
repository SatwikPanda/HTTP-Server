#include "http/http_session.hpp"
#include "net/stream.hpp"
#include "http/serializer.hpp"

#include <algorithm>
#include "test_support.hpp"
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
    bool zero_read{false};
    bool zero_write{false};
    bool oversized_read_count{false};
    bool oversized_write_count{false};
    core::Error read_error{};
    core::Error write_error{};

    [[nodiscard]] net::ReadResult read_some(std::span<std::byte> destination) override {
        total_read_calls++;
        if (zero_read) return net::ReadResult::success(0);
        if (oversized_read_count) return net::ReadResult::success(destination.size() + 1);
        if (read_error) return net::ReadResult::failure(read_error);
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
        if (zero_write) return net::WriteResult::success(0);
        if (oversized_write_count) return net::WriteResult::success(source.size() + 1);
        if (write_error) return net::WriteResult::failure(write_error);
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

    CHECK(stats.completed_cleanly);
    CHECK(stats.request_received);
    CHECK(stats.response_sent);

    std::string out = channel.output_string();
    CHECK(out.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(out.find("\r\nContent-Length: 15\r\n") != std::string::npos);
    CHECK(out.find("\r\nConnection: close\r\n") != std::string::npos);
    CHECK(out.find("\r\nContent-Type: text/plain; charset=utf-8\r\n") != std::string::npos);
    CHECK(out.find("\r\nDate: ") != std::string::npos);
    CHECK(out.ends_with("Hello, World!\r\n"));
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

    CHECK(stats.completed_cleanly);
    CHECK(stats.request_received);
    CHECK(stats.response_sent);

    std::string out = channel.output_string();
    CHECK(out.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(out.find("\r\nContent-Length: 10\r\n") != std::string::npos);
    CHECK(out.ends_with("Fixed Body"));
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

    CHECK(stats.completed_cleanly);
    CHECK(stats.response_sent);
    CHECK(stats.short_write_count > 0);

    std::string out = channel.output_string();
    CHECK(out.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(out.ends_with("Testing forced short writes payload!"));
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

    CHECK(stats.completed_cleanly);
    CHECK(stats.would_block_read_count == 3);
    CHECK(stats.would_block_write_count == 2);
    CHECK(stats.response_sent);
    std::cout << "[PASS] test_session_would_block_cycles" << std::endl;
}

void test_session_empty_disconnect() {
    std::cout << "[RUN] test_session_empty_disconnect..." << std::endl;
    ControlledStreamChannel channel;
    // input is empty: client disconnects immediately on accept

    http::HttpSessionConfig config;
    auto stats = http::run_http_session(channel, config);

    CHECK(!stats.request_received);
    CHECK(!stats.response_sent);
    CHECK(channel.output_data.empty());
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

    CHECK(stats.header_limit_exceeded);
    CHECK(stats.response_sent);

    std::string out = channel.output_string();
    CHECK(out.starts_with("HTTP/1.1 431 Request Header Fields Too Large\r\n"));
    CHECK(out.find("Connection: close\r\n") != std::string::npos);
    std::cout << "[PASS] test_session_header_limit_exceeded" << std::endl;
}

void test_exact_limits_and_fragments() {
    const std::string request = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    http::HttpSessionConfig config;
    config.fixed_wall_time = std::chrono::system_clock::from_time_t(784111777);
    config.max_header_bytes = request.size();
    const auto expected = test::take(http::serialize_response(test::take(http::make_fixed_response(config))));
    for (std::size_t chunk = 1; chunk <= request.size(); ++chunk) {
        ControlledStreamChannel channel;
        channel.set_input(request);
        channel.max_read_chunk = chunk;
        channel.max_write_chunk = 7;
        const auto stats = http::run_http_session(channel, config);
        CHECK(stats.completed_cleanly && stats.response_sent && !stats.error);
        CHECK(stats.peak_input_bytes <= config.max_header_bytes);
        CHECK(stats.peak_output_bytes <= config.max_output_bytes);
        CHECK(channel.output_string() == expected);
    }
    ControlledStreamChannel oversized;
    oversized.set_input(request); // Terminator would previously bypass the bound.
    config.max_header_bytes = request.size() - 1;
    auto stats = http::run_http_session(oversized, config);
    CHECK(stats.header_limit_exceeded && stats.response_sent);
    CHECK(stats.total_bytes_read == config.max_header_bytes);
    CHECK(stats.peak_input_bytes == config.max_header_bytes);
    CHECK(oversized.output_string().starts_with("HTTP/1.1 431 "));

    ControlledStreamChannel truncated;
    truncated.set_input("GET / HTTP/1.1\r\nHost: localhost\r\n");
    config.max_header_bytes = 8192;
    stats = http::run_http_session(truncated, config);
    CHECK(stats.error.category == core::ErrorCategory::eof);
    CHECK(!stats.response_sent && truncated.output_data.empty());
    ControlledStreamChannel bare_lf;
    bare_lf.set_input("GET / HTTP/1.1\nHost: localhost\n\n");
    CHECK(!http::run_http_session(bare_lf, config).response_sent);
}

void test_invalid_progress_and_io_errors() {
    for (int scenario = 0; scenario < 6; ++scenario) {
        ControlledStreamChannel channel;
        channel.set_input("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
        channel.zero_read = scenario == 0;
        channel.zero_write = scenario == 1;
        channel.oversized_read_count = scenario == 2;
        channel.oversized_write_count = scenario == 3;
        const auto error = core::make_error(core::ErrorCategory::connection_reset, "controlled I/O", 123);
        if (scenario == 4) channel.read_error = error;
        if (scenario == 5) channel.write_error = error;
        const auto stats = http::run_http_session(channel);
        CHECK(!stats.completed_cleanly && !stats.response_sent);
        CHECK(stats.error.category == (scenario < 4 ? core::ErrorCategory::io_error : core::ErrorCategory::connection_reset));
        if (scenario >= 4) CHECK(stats.error.native_code == 123 && stats.error.operation == "controlled I/O");
        CHECK(channel.total_read_calls == 1);
        CHECK(channel.total_write_calls <= 1);
    }
    for (bool writing : {false, true}) {
        ControlledStreamChannel channel;
        channel.set_input("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
        channel.read_would_block_cycles = writing ? 0 : 100;
        channel.write_would_block_cycles = writing ? 100 : 0;
        http::HttpSessionConfig config;
        config.retry_timeout = std::chrono::milliseconds(1);
        config.wait_retry_delay = std::chrono::milliseconds(100);
        const auto stats = http::run_http_session(channel, config);
        CHECK(stats.error.category == core::ErrorCategory::timed_out);
        CHECK(!stats.response_sent);
        CHECK(channel.total_read_calls == 1);
        CHECK(channel.total_write_calls == (writing ? 1u : 0u));
    }
}

void test_clock_deadlines_and_configuration() {
    using namespace std::chrono_literals;
    using namespace concurrency;
    core::ManualSteadyClock steady;
    core::ManualSystemClock wall(std::chrono::system_clock::from_time_t(784111777));
    http::HttpSessionConfig config;
    config.retry_timeout = 500ms;
    http::HttpSession idle(config, {1, 1}, steady, wall);
    CHECK(idle.initial_action().is_read());
    auto wait = idle.on_event(WouldBlockEvent{net::Interest::read});
    CHECK(wait.is_wait() && wait.as_wait()->deadline == steady.now() + 500ms);
    wall.advance(24h); // Wall-clock jumps do not affect I/O deadlines.
    steady.advance(500ms);
    CHECK(idle.on_event(ReadyEvent{true, false}).is_close());
    CHECK(idle.error().category == core::ErrorCategory::timed_out);

    http::HttpSession late(config, {1, 3}, steady, wall);
    CHECK(late.initial_action().is_read());
    steady.advance(500ms);
    const std::string late_request = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    CHECK(late.on_event(ReadDataEvent{std::as_bytes(std::span(late_request))}).is_close());
    CHECK(late.error().category == core::ErrorCategory::timed_out);

    wall.set(std::chrono::system_clock::from_time_t(784111777));
    http::HttpSession draining(config, {1, 2}, steady, wall);
    CHECK(draining.initial_action().is_read());
    const std::string request = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto write = draining.on_event(ReadDataEvent{std::as_bytes(std::span(request))});
    CHECK(write.is_write());
    const std::string wire(reinterpret_cast<const char*>(write.as_write()->data.data()), write.as_write()->data.size());
    CHECK(wire.find("Date: Sun, 06 Nov 1994 08:49:37 GMT\r\n") != std::string::npos);
    CHECK(draining.on_event(StopEvent{5ms}).is_write());
    steady.advance(3ms);
    CHECK(draining.on_event(WriteCompleteEvent{1}).is_write());
    steady.advance(2ms);
    CHECK(draining.on_event(WouldBlockEvent{net::Interest::write}).is_close());
    CHECK(draining.error().category == core::ErrorCategory::timed_out);

    for (int scenario = 0; scenario < 7; ++scenario) {
        auto invalid = config;
        if (scenario == 0) invalid.max_header_bytes = 0;
        if (scenario == 1) invalid.max_output_bytes = 0;
        if (scenario == 2) invalid.retry_timeout = 0ms;
        if (scenario == 3) invalid.wait_retry_delay = 0ms;
        if (scenario == 4) invalid.content_type = "text/plain\r\nX: injected";
        if (scenario == 5) invalid.response_status = http::StatusCode::NoContent;
        if (scenario == 6) invalid.response_body.assign(invalid.max_output_bytes + 1, 'x');
        ControlledStreamChannel channel;
        const auto stats = http::run_http_session(channel, invalid);
        CHECK(stats.error.category == core::ErrorCategory::invalid_argument);
        CHECK(channel.total_read_calls == 0 && channel.total_write_calls == 0);
    }
    ControlledStreamChannel small_output;
    small_output.set_input(request);
    config.max_output_bytes = 64;
    CHECK(http::run_http_session(small_output, config).error);
    CHECK(small_output.output_data.empty());
    ControlledStreamChannel closed;
    closed.close();
    CHECK(http::run_http_session(closed).error.category == core::ErrorCategory::bad_descriptor);
}

int main() {
    std::cout << "Starting HTTP Session Tests..." << std::endl;
    test_session_normal_request();
    test_session_fragmented_reads();
    test_session_forced_short_writes();
    test_session_would_block_cycles();
    test_session_empty_disconnect();
    test_session_header_limit_exceeded();
    test_exact_limits_and_fragments();
    test_invalid_progress_and_io_errors();
    test_clock_deadlines_and_configuration();
    std::cout << "All HTTP Session Tests PASSED!" << std::endl;
    return 0;
}
