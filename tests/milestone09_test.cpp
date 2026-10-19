#include "core/clock.hpp"
#include "core/result.hpp"
#include "http/http_server.hpp"
#include "http/http_session.hpp"
#include "http/response_parser.hpp"
#include "net/poller.hpp"
#include "net/resolver.hpp"
#include "net/runtime.hpp"
#include "net/stream.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;

class ControlledChannel final : public net::StreamChannel {
public:
    explicit ControlledChannel(std::string_view input = {}) {
        append_input(input);
    }

    void append_input(std::string_view input) {
        const auto* ptr = reinterpret_cast<const std::byte*>(input.data());
        input_data_.insert(input_data_.end(), ptr, ptr + input.size());
    }

    void set_eof_on_empty(bool enabled = true) noexcept {
        eof_on_empty_ = enabled;
    }

    void set_max_read_chunk(std::size_t chunk) noexcept {
        max_read_chunk_ = chunk;
    }

    void set_max_write_chunk(std::size_t chunk) noexcept {
        max_write_chunk_ = chunk;
    }

    [[nodiscard]] net::ReadResult read_some(std::span<std::byte> destination) override {
        if (closed_) {
            return net::ReadResult::failure(core::make_error(core::ErrorCategory::bad_descriptor, "read_some"));
        }
        if (read_offset_ >= input_data_.size()) {
            if (eof_on_empty_) return net::ReadResult::eof();
            return net::ReadResult::would_block_result();
        }
        std::size_t available = input_data_.size() - read_offset_;
        std::size_t to_copy = std::min(available, destination.size());
        if (max_read_chunk_ > 0) {
            to_copy = std::min(to_copy, max_read_chunk_);
        }
        std::memcpy(destination.data(), input_data_.data() + read_offset_, to_copy);
        read_offset_ += to_copy;
        return net::ReadResult::success(to_copy);
    }

    [[nodiscard]] net::WriteResult write_some(std::span<const std::byte> source) override {
        if (closed_) {
            return net::WriteResult::failure(core::make_error(core::ErrorCategory::bad_descriptor, "write_some"));
        }
        std::size_t to_write = source.size();
        if (max_write_chunk_ > 0 && to_write > max_write_chunk_) {
            to_write = max_write_chunk_;
        }
        const auto* src = reinterpret_cast<const char*>(source.data());
        output_.append(src, to_write);
        return net::WriteResult::success(to_write);
    }

    void close() noexcept override { closed_ = true; }
    [[nodiscard]] bool is_valid() const noexcept override { return !closed_; }
    [[nodiscard]] const std::string& output() const noexcept { return output_; }

private:
    std::vector<std::byte> input_data_;
    std::size_t read_offset_{0};
    std::size_t max_read_chunk_{0};
    std::size_t max_write_chunk_{0};
    std::string output_;
    bool eof_on_empty_{true};
    bool closed_{false};
};

std::vector<http::Response> parse_responses(std::string_view raw) {
    std::vector<http::Response> responses;
    std::string_view remaining = raw;
    while (!remaining.empty()) {
        http::ResponseParser parser;
        auto res = parser.consume(remaining);
        if (parser.failed() || !parser.complete()) break;
        responses.push_back(parser.response());
        remaining.remove_prefix(res.consumed);
    }
    return responses;
}

void test_sequential_and_pipelined_keep_alive() {
    std::cout << "[RUN] test_sequential_and_pipelined_keep_alive..." << std::endl;
    // 3 pipelined requests in one stream
    const std::string pipelined =
        "GET /first HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /second HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /third HTTP/1.1\r\nHost: localhost\r\n\r\n";

    ControlledChannel channel(pipelined);
    http::HttpSessionConfig config;
    config.keep_alive = true;
    config.max_requests_per_connection = 10;
    config.response_body = "Demo Body\r\n";

    auto stats = http::run_http_session(channel, config);
    CHECK(stats.completed_cleanly);
    CHECK(stats.requests_served == 3);

    auto responses = parse_responses(channel.output());
    CHECK(responses.size() == 3);
    for (std::size_t i = 0; i < responses.size(); ++i) {
        CHECK(responses[i].status_code == 200);
        CHECK(responses[i].body == "Demo Body\r\n");
        auto conn = responses[i].headers.get("Connection");
        CHECK(conn.has_value());
        CHECK(conn.value() == "keep-alive");
    }
    std::cout << "[PASS] test_sequential_and_pipelined_keep_alive" << std::endl;
}

void test_pipelined_fragmented_reads() {
    std::cout << "[RUN] test_pipelined_fragmented_reads..." << std::endl;
    const std::string pipelined =
        "GET /first HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /second HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /third HTTP/1.1\r\nHost: localhost\r\n\r\n";

    // Establish baseline output
    std::string baseline_output;
    {
        ControlledChannel ch(pipelined);
        http::HttpSessionConfig config;
        config.keep_alive = true;
        config.max_requests_per_connection = 5;
        auto stats = http::run_http_session(ch, config);
        CHECK(stats.completed_cleanly && stats.requests_served == 3);
        baseline_output = ch.output();
    }

    // Run across multiple chunk sizes (1, 2, 5, 7, 13, 23, 40 bytes)
    for (std::size_t chunk : {1u, 2u, 5u, 7u, 13u, 23u, 40u}) {
        ControlledChannel ch(pipelined);
        ch.set_max_read_chunk(chunk);
        ch.set_max_write_chunk(11);
        http::HttpSessionConfig config;
        config.keep_alive = true;
        config.max_requests_per_connection = 5;

        auto stats = http::run_http_session(ch, config);
        CHECK(stats.completed_cleanly);
        CHECK(stats.requests_served == 3);
        CHECK(ch.output() == baseline_output);
    }
    std::cout << "[PASS] test_pipelined_fragmented_reads" << std::endl;
}

void test_client_connection_close_header() {
    std::cout << "[RUN] test_client_connection_close_header..." << std::endl;
    // Single request with Connection: close
    {
        ControlledChannel ch("GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        http::HttpSessionConfig config;
        config.keep_alive = true;
        config.max_requests_per_connection = 10;

        auto stats = http::run_http_session(ch, config);
        CHECK(stats.completed_cleanly);
        CHECK(stats.requests_served == 1);

        auto responses = parse_responses(ch.output());
        CHECK(responses.size() == 1);
        CHECK(responses[0].headers.get("Connection").value_or("") == "close");
    }

    // Pipelined: request 2 specifies Connection: close, request 3 is discarded
    {
        const std::string pipelined =
            "GET /1 HTTP/1.1\r\nHost: localhost\r\n\r\n"
            "GET /2 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
            "GET /3 HTTP/1.1\r\nHost: localhost\r\n\r\n";

        ControlledChannel ch(pipelined);
        http::HttpSessionConfig config;
        config.keep_alive = true;
        config.max_requests_per_connection = 10;

        auto stats = http::run_http_session(ch, config);
        CHECK(stats.completed_cleanly);
        CHECK(stats.requests_served == 2);

        auto responses = parse_responses(ch.output());
        CHECK(responses.size() == 2);
        CHECK(responses[0].headers.get("Connection").value_or("") == "keep-alive");
        CHECK(responses[1].headers.get("Connection").value_or("") == "close");
    }
    std::cout << "[PASS] test_client_connection_close_header" << std::endl;
}

void test_http_1_0_persistence_semantics() {
    std::cout << "[RUN] test_http_1_0_persistence_semantics..." << std::endl;
    // HTTP/1.0 without Connection: keep-alive closes by default
    {
        const std::string req =
            "GET /1 HTTP/1.0\r\n\r\n"
            "GET /2 HTTP/1.0\r\n\r\n";
        ControlledChannel ch(req);
        http::HttpSessionConfig config;
        config.keep_alive = true;
        config.max_requests_per_connection = 5;

        auto stats = http::run_http_session(ch, config);
        CHECK(stats.completed_cleanly);
        CHECK(stats.requests_served == 1);
        auto responses = parse_responses(ch.output());
        CHECK(responses.size() == 1);
        CHECK(responses[0].headers.get("Connection").value_or("") == "close");
    }

    // HTTP/1.0 with Connection: keep-alive persists
    {
        const std::string req =
            "GET /1 HTTP/1.0\r\nConnection: keep-alive\r\n\r\n"
            "GET /2 HTTP/1.0\r\n\r\n";
        ControlledChannel ch(req);
        http::HttpSessionConfig config;
        config.keep_alive = true;
        config.max_requests_per_connection = 5;

        auto stats = http::run_http_session(ch, config);
        CHECK(stats.completed_cleanly);
        CHECK(stats.requests_served == 2);
        auto responses = parse_responses(ch.output());
        CHECK(responses.size() == 2);
        CHECK(responses[0].headers.get("Connection").value_or("") == "keep-alive");
        CHECK(responses[1].headers.get("Connection").value_or("") == "close");
    }
    std::cout << "[PASS] test_http_1_0_persistence_semantics" << std::endl;
}

void test_max_requests_per_connection() {
    std::cout << "[RUN] test_max_requests_per_connection..." << std::endl;
    const std::string pipelined =
        "GET /1 HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /2 HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /3 HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /4 HTTP/1.1\r\nHost: localhost\r\n\r\n";

    ControlledChannel ch(pipelined);
    http::HttpSessionConfig config;
    config.keep_alive = true;
    config.max_requests_per_connection = 2; // Limit to 2 requests

    auto stats = http::run_http_session(ch, config);
    CHECK(stats.completed_cleanly);
    CHECK(stats.requests_served == 2);

    auto responses = parse_responses(ch.output());
    CHECK(responses.size() == 2);
    CHECK(responses[0].headers.get("Connection").value_or("") == "keep-alive");
    CHECK(responses[1].headers.get("Connection").value_or("") == "close");
    std::cout << "[PASS] test_max_requests_per_connection" << std::endl;
}

void test_deadlines_and_timeouts() {
    std::cout << "[RUN] test_deadlines_and_timeouts..." << std::endl;
    core::ManualSteadyClock steady;
    core::ManualSystemClock wall(std::chrono::system_clock::from_time_t(784111777));

    // 1. Idle timeout on keep-alive connection closes cleanly
    {
        http::HttpSessionConfig config;
        config.keep_alive = true;
        config.idle_timeout = 300ms;
        config.request_progress_timeout = 2000ms;
        config.output_progress_timeout = 2000ms;
        http::HttpSession session(config, {1, 1}, steady, wall);

        CHECK(session.initial_action().is_read());
        const std::string req = "GET /1 HTTP/1.1\r\nHost: localhost\r\n\r\n";
        auto write_action = session.on_event(concurrency::ReadDataEvent{std::as_bytes(std::span(req))});
        CHECK(write_action.is_write());
        auto write_data = write_action.as_write()->data;

        // Finish writing response 1
        auto next = session.on_event(concurrency::WriteCompleteEvent{write_data.size()});
        CHECK(next.is_read());
        CHECK(session.stats().requests_served == 1);
        CHECK(!session.is_closed());

        // Connection is now idle waiting for next request
        steady.advance(300ms);
        auto timeout_action = session.on_event(concurrency::TimeoutEvent{});
        CHECK(session.is_closed());
        CHECK(!session.error()); // Clean close on idle keep-alive timeout!
    }

    // 2. Request progress timeout on incomplete request
    {
        http::HttpSessionConfig config;
        config.keep_alive = true;
        config.idle_timeout = 5000ms;
        config.request_progress_timeout = 400ms;
        http::HttpSession session(config, {1, 2}, steady, wall);

        CHECK(session.initial_action().is_read());
        const std::string partial = "GET /inc";
        auto wait_or_read = session.on_event(concurrency::ReadDataEvent{std::as_bytes(std::span(partial))});
        CHECK(wait_or_read.is_read());

        steady.advance(400ms);
        auto timeout_action = session.on_event(concurrency::TimeoutEvent{});
        CHECK(session.is_closed());
        CHECK(session.error().category == core::ErrorCategory::timed_out);
    }

    // 3. Output progress timeout on blocked write
    {
        http::HttpSessionConfig config;
        config.keep_alive = true;
        config.output_progress_timeout = 350ms;
        http::HttpSession session(config, {1, 3}, steady, wall);

        CHECK(session.initial_action().is_read());
        const std::string req = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
        auto write_action = session.on_event(concurrency::ReadDataEvent{std::as_bytes(std::span(req))});
        CHECK(write_action.is_write());

        auto wait_action = session.on_event(concurrency::WouldBlockEvent{net::Interest::write});
        CHECK(wait_action.is_wait());

        steady.advance(350ms);
        auto timeout_action = session.on_event(concurrency::TimeoutEvent{});
        CHECK(session.is_closed());
        CHECK(session.error().category == core::ErrorCategory::timed_out);
    }
    std::cout << "[PASS] test_deadlines_and_timeouts" << std::endl;
}

void test_error_and_rejected_body_isolation() {
    std::cout << "[RUN] test_error_and_rejected_body_isolation..." << std::endl;
    // Syntax error in request 1 aborts pipeline and closes
    {
        const std::string bad_pipeline =
            "BAD / HTTP/9.9\r\n\r\n"
            "GET /valid HTTP/1.1\r\nHost: localhost\r\n\r\n";
        ControlledChannel ch(bad_pipeline);
        http::HttpSessionConfig config;
        config.keep_alive = true;
        config.max_requests_per_connection = 5;

        auto stats = http::run_http_session(ch, config);
        CHECK(stats.requests_served == 1);

        auto responses = parse_responses(ch.output());
        CHECK(responses.size() == 1);
        CHECK(responses[0].status_code == 505 || responses[0].status_code == 400);
        CHECK(responses[0].headers.get("Connection").value_or("") == "close");
    }

    // Rejected nonempty body on GET aborts pipeline and closes with 400
    {
        const std::string rejected_body_pipeline =
            "GET / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nhello"
            "GET /valid HTTP/1.1\r\nHost: localhost\r\n\r\n";
        ControlledChannel ch(rejected_body_pipeline);
        http::HttpSessionConfig config;
        config.keep_alive = true;
        config.max_requests_per_connection = 5;

        auto stats = http::run_http_session(ch, config);
        CHECK(stats.requests_served == 1);

        auto responses = parse_responses(ch.output());
        CHECK(responses.size() == 1);
        CHECK(responses[0].status_code == 400);
        CHECK(responses[0].headers.get("Connection").value_or("") == "close");
    }
    std::cout << "[PASS] test_error_and_rejected_body_isolation" << std::endl;
}

// Real loopback socket integration test
class ServerTask {
public:
    template<class Function>
    explicit ServerTask(Function function) : thread_([this, function = std::move(function)] {
        auto result = function();
        {
            std::lock_guard lock(mutex_);
            result_ = std::move(result);
            completed_ = true;
        }
        completed_cv_.notify_all();
    }) {}

    bool wait_for(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return completed_cv_.wait_for(lock, timeout, [&] { return completed_; });
    }
    core::Result<void> get() {
        std::lock_guard lock(mutex_);
        CHECK(completed_);
        return result_;
    }
private:
    std::mutex mutex_;
    std::condition_variable completed_cv_;
    core::Result<void> result_;
    bool completed_{};
    std::jthread thread_;
};

net::Socket connect_to(const http::HttpServer& server) {
    const std::array candidates{server.local_endpoint()};
    return test::take(net::connect_candidates(candidates, test::Clock::now() + 2s));
}

class ClientIo {
public:
    explicit ClientIo(net::Socket& socket)
        : socket_(socket), poller_(test::take(net::Poller::create())),
          token_(test::take(poller_.watch(socket_, net::Interest::read, {2, 1}))) {}
    ~ClientIo() { CHECK(poller_.unwatch(token_)); }

    void write(std::string_view data) {
        const auto deadline = test::Clock::now() + 2s;
        std::size_t sent{};
        const auto bytes = std::as_bytes(std::span(data));
        while (sent < bytes.size()) {
            CHECK(test::Clock::now() < deadline);
            const auto result = socket_.write_some(bytes.subspan(sent));
            if (result.is_ok()) {
                CHECK(result.bytes_transferred > 0 && result.bytes_transferred <= bytes.size() - sent);
                sent += result.bytes_transferred;
            } else {
                CHECK(result.would_block());
                wait(net::Interest::write, deadline);
            }
        }
    }

    std::string read_bytes(std::size_t count) {
        std::string output;
        std::array<std::byte, 512> buffer{};
        const auto deadline = test::Clock::now() + 2s;
        while (output.size() < count) {
            CHECK(test::Clock::now() < deadline);
            const auto result = socket_.read_some(buffer);
            if (result.is_eof()) return output;
            if (result.is_ok()) {
                output.append(reinterpret_cast<const char*>(buffer.data()), result.bytes_transferred);
            } else {
                CHECK(result.would_block());
                wait(net::Interest::read, deadline);
            }
        }
        return output;
    }

    std::string read_to_eof() {
        std::string output;
        std::array<std::byte, 512> buffer{};
        const auto deadline = test::Clock::now() + 2s;
        for (;;) {
            CHECK(test::Clock::now() < deadline);
            const auto result = socket_.read_some(buffer);
            if (result.is_eof()) return output;
            if (result.is_ok()) {
                output.append(reinterpret_cast<const char*>(buffer.data()), result.bytes_transferred);
            } else {
                CHECK(result.would_block());
                wait(net::Interest::read, deadline);
            }
        }
    }

private:
    void wait(net::Interest interest, net::Deadline deadline) {
        CHECK(poller_.modify(token_, interest));
        for (;;) {
            CHECK(test::Clock::now() < deadline);
            const auto events = test::take(poller_.wait(deadline));
            for (const auto& event : events) {
                if (event.token == token_ && poller_.is_current(event)) return;
            }
        }
    }
    net::Socket& socket_;
    net::Poller poller_;
    net::RegistrationToken token_;
};

void test_real_socket_loopback_integration() {
    std::cout << "[RUN] test_real_socket_loopback_integration..." << std::endl;
    auto runtime = test::take(net::initialize_network());

    http::HttpServerConfig config;
    config.endpoint = net::Endpoint::ipv4_loopback(0);
    config.session_config.keep_alive = true;
    config.session_config.max_requests_per_connection = 4;
    config.session_config.response_body = "Loopback KeepAlive\r\n";

    http::HttpServer server(config);
    CHECK(server.start());

    ServerTask serving([&] { return server.accept_and_handle_one(); });
    auto client = connect_to(server);

    {
        ClientIo io(client);
        // 1. Sequential request 1
        io.write("GET /first HTTP/1.1\r\nHost: localhost\r\n\r\n");
        // Read response 1
        std::string resp1_raw;
        while (resp1_raw.find("\r\n\r\n") == std::string::npos ||
               resp1_raw.find("Loopback KeepAlive\r\n") == std::string::npos) {
            resp1_raw += io.read_bytes(1);
        }
        CHECK(resp1_raw.starts_with("HTTP/1.1 200 OK\r\n"));
        CHECK(resp1_raw.find("Connection: keep-alive\r\n") != std::string::npos);

        // 2. Pipelined requests 2 and 3
        const std::string pipeline =
            "GET /second HTTP/1.1\r\nHost: localhost\r\n\r\n"
            "GET /third HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        io.write(pipeline);

        // Read remaining responses until EOF (since request 3 requested Connection: close)
        std::string remaining_responses = io.read_to_eof();
        auto responses = parse_responses(remaining_responses);
        CHECK(responses.size() == 2);
        CHECK(responses[0].status_code == 200);
        CHECK(responses[0].headers.get("Connection").value_or("") == "keep-alive");
        CHECK(responses[1].status_code == 200);
        CHECK(responses[1].headers.get("Connection").value_or("") == "close");
    }

    client.close();
    CHECK(serving.wait_for(2s));
    CHECK(serving.get());
    server.stop();
    std::cout << "[PASS] test_real_socket_loopback_integration" << std::endl;
}

} // namespace

int main() {
    std::cout << "Starting Milestone 09 Acceptance Tests..." << std::endl;
    test_sequential_and_pipelined_keep_alive();
    test_pipelined_fragmented_reads();
    test_client_connection_close_header();
    test_http_1_0_persistence_semantics();
    test_max_requests_per_connection();
    test_deadlines_and_timeouts();
    test_error_and_rejected_body_isolation();
    test_real_socket_loopback_integration();
    std::cout << "All Milestone 09 Tests Passed!" << std::endl;
    return 0;
}
