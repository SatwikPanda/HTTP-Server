#pragma once

#include "test_support.hpp"
#include "http/header.hpp"
#include "http/method.hpp"
#include "http/request_parser.hpp"
#include "net/poller.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace test {
class TemporaryDirectory {
public:
    TemporaryDirectory() {
        static std::atomic<unsigned> serial{};
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            path_ = std::filesystem::canonical(std::filesystem::temp_directory_path()) / ("httpserver-p0-" + std::to_string(Clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) return;
        }
        CHECK(false);
    }
    ~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path_, error); }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    std::filesystem::path write(std::string_view name, std::string_view bytes) {
        const auto path = path_ / name;
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        CHECK(!error);
        std::ofstream stream(path, std::ios::binary);
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        stream.close(); CHECK(stream);
        return path;
    }
    std::filesystem::path make_directory(std::string_view relative) {
        const auto dir = path_ / relative;
        std::error_code error;
        std::filesystem::create_directories(dir, error);
        CHECK(!error);
        return dir;
    }
    void set_mtime(const std::filesystem::path& file_path, std::chrono::system_clock::time_point when) {
        std::error_code error;
#ifdef _MSC_VER
        const auto native = std::chrono::clock_cast<std::chrono::file_clock>(when);
#else
        const auto native = std::chrono::file_clock::from_sys(when);
#endif
        std::filesystem::last_write_time(file_path, native, error);
        CHECK(!error);
    }
    [[nodiscard]] bool try_symlink(const std::filesystem::path& target, std::string_view link_relative) {
        std::error_code error;
        std::filesystem::create_symlink(target, path_ / link_relative, error);
        return !error;
    }
private:
    std::filesystem::path path_;
};

// Raw response segments deliberately allow malformed framing and premature EOF.
// Gate gives tests deterministic control of when origin bytes become readable.
class ControlledOrigin {
public:
    explicit ControlledOrigin(std::vector<std::string> segments, std::size_t write_window = 97, bool gated = false)
        : listener_(listener()), endpoint_(take(listener_.local_endpoint())), segments_(std::move(segments)),
          window_(write_window), released_(!gated), thread_([this] { serve(); }) { CHECK(window_ > 0); }
    ~ControlledOrigin() { stop_.store(true); release(); if (thread_.joinable()) thread_.join(); }
    [[nodiscard]] net::HostPort target() const { return {endpoint_.address(), endpoint_.port()}; }
    [[nodiscard]] net::Endpoint endpoint() const { return endpoint_; }
    void release() { { std::lock_guard lock(mutex_); released_ = true; } cv_.notify_all(); }
    void wait_for_request() {
        std::unique_lock lock(mutex_);
        CHECK(cv_.wait_for(lock, 3s, [&] { return request_count_.load() != 0; }));
    }
    void join() { thread_.join(); CHECK(!error_); }
    [[nodiscard]] unsigned request_count() const { return request_count_.load(); }
    [[nodiscard]] const std::string& request() const { return request_; } // after join only
private:
    bool wait(net::Poller& poller, net::RegistrationToken token, net::Interest interest, net::Deadline deadline) {
        CHECK(poller.modify(token, interest));
        while (!stop_ && Clock::now() < deadline) {
            auto events = take(poller.wait(std::min(deadline, Clock::now() + 25ms)));
            for (const auto& event : events) if (event.token == token && poller.is_current(event)) return true;
        }
        return false;
    }
    void serve() {
        auto poller = take(net::Poller::create());
        auto token = take(poller.watch(listener_, net::Interest::read, {1, 1}));
        const auto deadline = Clock::now() + 8s;
        net::Socket socket;
        while (wait(poller, token, net::Interest::read, deadline)) {
            auto accepted = listener_.accept();
            if (accepted) { socket = std::move(accepted.value()); break; }
            if (accepted.error().category != core::ErrorCategory::would_block) { error_ = true; break; }
        }
        CHECK(poller.unwatch(token));
        if (!socket.is_valid()) return;
        CHECK(socket.set_nonblocking(true));
        token = take(poller.watch(socket, net::Interest::read, {2, 1}));
        std::array<std::byte, 512> input{};
        while (request_.find("\r\n\r\n") == std::string::npos && request_.size() < 8192) {
            const auto read = socket.read_some(input);
            if (read.is_ok()) request_.append(reinterpret_cast<const char*>(input.data()), read.bytes_transferred);
            else if (!read.would_block() || !wait(poller, token, net::Interest::read, deadline)) { error_ = true; break; }
        }
        { std::lock_guard lock(mutex_); ++request_count_; } cv_.notify_all();
        { std::unique_lock lock(mutex_); cv_.wait_until(lock, deadline, [&] { return released_ || stop_; }); }
        for (const auto& segment : segments_) {
            std::size_t offset = 0;
            while (offset < segment.size() && !stop_ && Clock::now() < deadline) {
                const auto data = std::as_bytes(std::span(segment)).subspan(offset, std::min(window_, segment.size() - offset));
                const auto write = socket.write_some(data);
                if (write.is_ok()) offset += write.bytes_transferred;
                else if (!write.would_block() || !wait(poller, token, net::Interest::write, deadline)) { error_ = true; break; }
            }
            if (error_) break;
        }
        CHECK(poller.unwatch(token));
    }
    net::Socket listener_;
    net::Endpoint endpoint_;
    std::vector<std::string> segments_;
    std::size_t window_;
    std::atomic<bool> stop_{};
    std::atomic<unsigned> request_count_{};
    std::mutex mutex_;
    std::condition_variable cv_;
    bool released_, error_{};
    std::string request_;
    std::thread thread_;
};

// Multi-request scripted loopback origin with named barriers, bounded request capture,
// and configurable terminal behavior (for proxy/cache/revalidation integration).
enum class ScriptTerminalBehavior {
    complete_and_close,
    drop_before_head,
    truncate_after_first_segment,
    hold_until_stopped
};

struct OriginResponseScript {
    std::vector<std::string> segments{"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"};
    std::size_t write_window{97};
    std::string barrier_before_response{};
    ScriptTerminalBehavior terminal{ScriptTerminalBehavior::complete_and_close};

    static OriginResponseScript fixed_length(
        int status,
        std::string_view reason,
        std::string_view body,
        std::initializer_list<std::pair<std::string_view, std::string_view>> headers = {},
        std::size_t window = 97,
        std::string barrier = {}
    ) {
        std::string head = "HTTP/1.1 " + std::to_string(status) + " " + std::string(reason) + "\r\n";
        for (const auto& [k, v] : headers) {
            head += std::string(k) + ": " + std::string(v) + "\r\n";
        }
        head += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
        return OriginResponseScript{{std::move(head), std::string(body)}, window, std::move(barrier), ScriptTerminalBehavior::complete_and_close};
    }

    static OriginResponseScript raw(
        std::vector<std::string> raw_segments,
        std::size_t window = 97,
        std::string barrier = {},
        ScriptTerminalBehavior term = ScriptTerminalBehavior::complete_and_close
    ) {
        return OriginResponseScript{std::move(raw_segments), window, std::move(barrier), term};
    }
};

struct CapturedOriginRequest {
    std::string raw_bytes{};
    http::Method method{http::Method::GET};
    std::string target{};
    http::HeaderMap headers{};
    bool parse_ok{false};
};

class ScriptedOrigin {
public:
    explicit ScriptedOrigin(std::vector<OriginResponseScript> scripts, std::size_t max_captured = 64)
        : listener_(listener()),
          endpoint_(take(listener_.local_endpoint())),
          scripts_(std::move(scripts)),
          max_captured_(std::max<std::size_t>(1, max_captured)),
          thread_([this] { serve_loop(); }) {}

    ~ScriptedOrigin() {
        stop();
        join();
    }

    ScriptedOrigin(const ScriptedOrigin&) = delete;
    ScriptedOrigin& operator=(const ScriptedOrigin&) = delete;

    [[nodiscard]] net::HostPort target() const { return {endpoint_.address(), endpoint_.port()}; }
    [[nodiscard]] net::Endpoint endpoint() const { return endpoint_; }

    void wait_for_requests(std::size_t count, std::chrono::milliseconds timeout = 3000ms) {
        std::unique_lock lock(mutex_);
        CHECK(cv_.wait_for(lock, timeout, [&] { return request_count_ >= count || stop_; }));
        CHECK(request_count_ >= count);
    }

    void wait_for_barrier(std::string_view name, std::chrono::milliseconds timeout = 3000ms) {
        std::unique_lock lock(mutex_);
        const std::string key(name);
        CHECK(cv_.wait_for(lock, timeout, [&] { return reached_barriers_[key] > 0 || stop_; }));
        CHECK(reached_barriers_[key] > 0);
    }

    void release_barrier(std::string_view name) {
        {
            std::lock_guard lock(mutex_);
            released_barriers_[std::string(name)] = true;
        }
        cv_.notify_all();
    }

    void stop() noexcept {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
    }

    void join() noexcept {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] std::size_t accepted_connections() const noexcept {
        std::lock_guard lock(mutex_);
        return accepted_connections_;
    }

    [[nodiscard]] std::size_t request_count() const noexcept {
        std::lock_guard lock(mutex_);
        return request_count_;
    }

    [[nodiscard]] std::vector<CapturedOriginRequest> captured_requests() const {
        std::lock_guard lock(mutex_);
        return captured_;
    }

private:
    bool wait_io(net::Poller& poller, net::RegistrationToken token, net::Interest interest, net::Deadline deadline) {
        CHECK(poller.modify(token, interest));
        while (Clock::now() < deadline) {
            {
                std::lock_guard lock(mutex_);
                if (stop_) return false;
            }
            auto events = take(poller.wait(std::min(deadline, Clock::now() + 20ms)));
            for (const auto& ev : events) {
                if (ev.token == token && poller.is_current(ev)) return true;
            }
        }
        return false;
    }

    void serve_loop() {
        auto poller = take(net::Poller::create());
        auto listen_token = take(poller.watch(listener_, net::Interest::read, {10, 1}));
        const auto overall_deadline = Clock::now() + 10s;

        std::size_t script_idx = 0;
        while (script_idx < scripts_.size() && Clock::now() < overall_deadline) {
            {
                std::lock_guard lock(mutex_);
                if (stop_) break;
            }
            if (!wait_io(poller, listen_token, net::Interest::read, overall_deadline)) {
                break;
            }
            auto accepted = listener_.accept();
            if (!accepted) {
                if (accepted.error().category == core::ErrorCategory::would_block) continue;
                break;
            }
            net::Socket conn = std::move(accepted.value());
            CHECK(conn.set_nonblocking(true));
            {
                std::lock_guard lock(mutex_);
                ++accepted_connections_;
            }
            const auto& script = scripts_[script_idx++];
            handle_one_connection(poller, conn, script, overall_deadline, script_idx);
        }
        CHECK(poller.unwatch(listen_token));
    }

    void handle_one_connection(
        net::Poller& poller,
        net::Socket& socket,
        const OriginResponseScript& script,
        net::Deadline deadline,
        std::size_t seq
    ) {
        auto token = take(poller.watch(socket, net::Interest::read, {20 + seq, 1}));
        std::string raw;
        std::array<std::byte, 512> buf{};
        while (raw.find("\r\n\r\n") == std::string::npos && raw.size() < 16384) {
            const auto read = socket.read_some(buf);
            if (read.is_ok()) {
                raw.append(reinterpret_cast<const char*>(buf.data()), read.bytes_transferred);
            } else if (read.is_eof()) {
                break;
            } else if (!read.would_block() || !wait_io(poller, token, net::Interest::read, deadline)) {
                break;
            }
        }

        CapturedOriginRequest cap{};
        cap.raw_bytes = raw;
        http::RequestParser parser;
        const auto parsed = parser.consume(raw);
        if (parsed.event == http::ParseEvent::complete) {
            cap.parse_ok = true;
            cap.method = parser.request().method;
            cap.target = parser.request().target;
            cap.headers = parser.request().headers;
        }

        {
            std::lock_guard lock(mutex_);
            ++request_count_;
            if (captured_.size() < max_captured_) {
                captured_.push_back(std::move(cap));
            }
            if (!script.barrier_before_response.empty()) {
                ++reached_barriers_[script.barrier_before_response];
            }
        }
        cv_.notify_all();

        if (!script.barrier_before_response.empty()) {
            std::unique_lock lock(mutex_);
            cv_.wait_until(lock, deadline, [&] {
                return stop_ || released_barriers_[script.barrier_before_response];
            });
        }

        if (script.terminal == ScriptTerminalBehavior::drop_before_head) {
            CHECK(poller.unwatch(token));
            socket.close();
            return;
        }

        const std::size_t max_segments =
            (script.terminal == ScriptTerminalBehavior::truncate_after_first_segment && !script.segments.empty())
                ? 1
                : script.segments.size();

        for (std::size_t s = 0; s < max_segments; ++s) {
            const auto& segment = script.segments[s];
            std::size_t offset = 0;
            while (offset < segment.size() && Clock::now() < deadline) {
                {
                    std::lock_guard lock(mutex_);
                    if (stop_) break;
                }
                const auto chunk_len = std::min(std::max<std::size_t>(1, script.write_window), segment.size() - offset);
                const auto slice = std::as_bytes(std::span(segment)).subspan(offset, chunk_len);
                const auto wrote = socket.write_some(slice);
                if (wrote.is_ok()) {
                    offset += wrote.bytes_transferred;
                } else if (!wrote.would_block() || !wait_io(poller, token, net::Interest::write, deadline)) {
                    break;
                }
            }
        }

        if (script.terminal == ScriptTerminalBehavior::hold_until_stopped) {
            std::unique_lock lock(mutex_);
            cv_.wait_until(lock, deadline, [&] { return stop_; });
        }

        CHECK(poller.unwatch(token));
        socket.close();
    }

    net::Socket listener_;
    net::Endpoint endpoint_;
    std::vector<OriginResponseScript> scripts_;
    std::size_t max_captured_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_{false};
    std::size_t accepted_connections_{0};
    std::size_t request_count_{0};
    std::vector<CapturedOriginRequest> captured_;
    std::map<std::string, std::size_t> reached_barriers_;
    std::map<std::string, bool> released_barriers_;
    std::thread thread_;
};

} // namespace test
