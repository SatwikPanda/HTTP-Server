#pragma once

#include "test_support.hpp"
#include "net/poller.hpp"

#include <atomic>
#include <array>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
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
    std::filesystem::path write(std::string_view name, std::string_view bytes) {
        const auto path = path_ / name;
        CHECK(path.parent_path() == path_);
        std::ofstream stream(path, std::ios::binary);
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        stream.close(); CHECK(stream);
        return path;
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
} // namespace test
