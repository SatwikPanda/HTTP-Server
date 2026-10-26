#pragma once

#include "test_support.hpp"
#include "fixtures/parallel_io.hpp"
#include "concurrency/event_loop/event_loop_driver.hpp"
#include "concurrency/session.hpp"
#include "concurrency/work.hpp"
#include "http/http_session.hpp"
#include "http/reference_application.hpp"
#include "http/response_parser.hpp"
#include "http/serializer.hpp"
#include "server/static_service.hpp"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace event_loop_test {

using namespace std::chrono_literals;

inline std::span<const std::byte> as_bytes(std::string_view text) noexcept {
    return std::as_bytes(std::span(text.data(), text.size()));
}

// Asynchronous multi-worker test executor for file and DNS work.
// Never blocks the caller in submit(); executes file/DNS work on background
// helper threads using ReferenceWorkExecutor::execute(), with optional gate
// control and bounded queue admission.
class AsyncTestWorkExecutor final : public concurrency::WorkExecutor {
public:
    explicit AsyncTestWorkExecutor(
        std::size_t worker_count = 2,
        std::size_t max_queued = 256,
        concurrency::WorkLimits limits = {}
    )
        : max_queued_(max_queued), limits_(limits) {
        CHECK(worker_count > 0 && max_queued > 0);
        workers_.reserve(worker_count);
        for (std::size_t i = 0; i < worker_count; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    ~AsyncTestWorkExecutor() override {
        stop();
    }

    void stop() noexcept {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            paused_ = false;
        }
        cv_.notify_all();
        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
        workers_.clear();
    }

    void pause_execution(bool paused) {
        {
            std::lock_guard lock(mutex_);
            paused_ = paused;
        }
        cv_.notify_all();
    }

    void set_reject_submissions(bool reject) {
        std::lock_guard lock(mutex_);
        reject_all_ = reject;
    }

    void set_custom_hook(std::function<void(const concurrency::WorkAction&, concurrency::WorkEvent&)> hook) {
        std::lock_guard lock(mutex_);
        hook_ = std::move(hook);
    }

    [[nodiscard]] std::size_t submitted_count() const noexcept {
        return submitted_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t completed_count() const noexcept {
        return completed_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::thread::id last_worker_thread() const noexcept {
        std::lock_guard lock(mutex_);
        return last_worker_id_;
    }

    core::Result<void> submit(concurrency::WorkAction action, Completion completion) override {
        if (!completion) {
            return core::make_error(core::ErrorCategory::invalid_argument, "completion callback");
        }
        {
            std::lock_guard lock(mutex_);
            if (stopping_ || reject_all_ || queue_.size() >= max_queued_) {
                return core::make_error(core::ErrorCategory::would_block, "async helper admission");
            }
            queue_.push_back(Item{std::move(action), std::move(completion)});
            ++submitted_;
        }
        cv_.notify_one();
        return {};
    }

private:
    struct Item {
        concurrency::WorkAction action;
        Completion completion;
    };

    void worker_loop() {
        for (;;) {
            Item item;
            std::function<void(const concurrency::WorkAction&, concurrency::WorkEvent&)> hook_copy;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this] { return stopping_ || (!paused_ && !queue_.empty()); });
                if (stopping_ && queue_.empty()) {
                    return;
                }
                if (paused_ || queue_.empty()) {
                    continue;
                }
                item = std::move(queue_.front());
                queue_.pop_front();
                last_worker_id_ = std::this_thread::get_id();
                hook_copy = hook_;
            }
            // Instantiate a fresh ReferenceWorkExecutor per job because
            // ReferenceWorkExecutor tracks single-connection owner state.
            concurrency::ReferenceWorkExecutor job_executor(limits_);
            auto event = job_executor.execute(item.action, [this] {
                std::lock_guard lock(mutex_);
                return stopping_;
            });
            if (hook_copy) {
                hook_copy(item.action, event);
            }
            ++completed_;
            item.completion(std::move(event));
        }
    }

    std::size_t max_queued_;
    concurrency::WorkLimits limits_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Item> queue_;
    std::vector<std::thread> workers_;
    bool stopping_{false};
    bool paused_{false};
    bool reject_all_{false};
    std::thread::id last_worker_id_{};
    std::function<void(const concurrency::WorkAction&, concurrency::WorkEvent&)> hook_;
    std::atomic<std::size_t> submitted_{0};
    std::atomic<std::size_t> completed_{0};
};

// Nonblocking loopback client helper with readiness waits.
class LoopbackClient {
public:
    explicit LoopbackClient(const net::Endpoint& server_endpoint, std::chrono::milliseconds timeout = 5000ms)
        : poller_(test::take(net::Poller::create())),
          socket_(test::take(net::create_tcp_socket(server_endpoint.family()))),
          deadline_(test::Clock::now() + timeout) {
        CHECK(socket_.set_nonblocking(true));
        auto begun = test::take(socket_.begin_connect(server_endpoint));
        token_ = test::take(poller_.watch(socket_, net::Interest::write, {999, 1}));
        if (begun == net::ConnectState::in_progress) {
            while (test::Clock::now() < deadline_) {
                wait(net::Interest::write);
                auto fin = socket_.finish_connect();
                if (fin) break;
                CHECK(fin.error().category == core::ErrorCategory::would_block);
            }
        } else {
            CHECK(socket_.finish_connect());
        }
    }

    ~LoopbackClient() {
        close();
    }

    LoopbackClient(const LoopbackClient&) = delete;
    LoopbackClient& operator=(const LoopbackClient&) = delete;

    void close() noexcept {
        if (token_) {
            (void)poller_.unwatch(*token_);
            token_.reset();
        }
        socket_.close();
    }

    void shutdown_write() {
        CHECK(socket_.shutdown_write());
    }

    void write_all(std::string_view text) {
        std::size_t offset = 0;
        while (offset < text.size()) {
            const auto res = socket_.write_some(as_bytes(text.substr(offset)));
            if (res.is_ok()) {
                CHECK(res.bytes_transferred > 0);
                offset += res.bytes_transferred;
            } else {
                CHECK(res.would_block());
                wait(net::Interest::write);
            }
        }
    }

    std::string read_until_eof(std::size_t chunk_size = 4096) {
        std::string out;
        std::vector<std::byte> buf(chunk_size);
        for (;;) {
            const auto res = socket_.read_some(buf);
            if (res.is_eof()) break;
            if (res.is_ok()) {
                out.append(reinterpret_cast<const char*>(buf.data()), res.bytes_transferred);
            } else if (res.would_block()) {
                wait(net::Interest::read);
            } else {
                // Connection reset on close after response is possible on Windows if unread bytes exist
                break;
            }
        }
        return out;
    }

    std::string read_exact(std::size_t expected_bytes) {
        std::string out;
        out.reserve(expected_bytes);
        std::vector<std::byte> buf(1024);
        while (out.size() < expected_bytes) {
            const auto want = std::min(buf.size(), expected_bytes - out.size());
            const auto res = socket_.read_some(std::span(buf.data(), want));
            if (res.is_ok()) {
                out.append(reinterpret_cast<const char*>(buf.data()), res.bytes_transferred);
            } else if (res.would_block()) {
                wait(net::Interest::read);
            } else {
                break;
            }
        }
        return out;
    }

    http::Response read_http_response(http::Method method = http::Method::GET) {
        http::RequestLimits limits;
        limits.max_body_bytes = 32 * 1024 * 1024;
        http::ResponseParser parser(method, limits);
        std::array<std::byte, 4096> buf{};
        for (;;) {
            if (!unread_.empty()) {
                const auto parsed = parser.consume(unread_);
                unread_.erase(0, parsed.consumed);
                if (parsed.event == http::ParseEvent::complete) {
                    return parser.response();
                }
                CHECK(parsed.event != http::ParseEvent::error);
            }
            const auto res = socket_.read_some(buf);
            if (res.is_ok()) {
                unread_.append(reinterpret_cast<const char*>(buf.data()), res.bytes_transferred);
            } else if (res.is_eof()) {
                CHECK(parser.finish_on_eof());
                const auto parsed = parser.consume(unread_);
                unread_.erase(0, parsed.consumed);
                CHECK(parsed.event == http::ParseEvent::complete);
                return parser.response();
            } else if (res.would_block()) {
                wait(net::Interest::read);
            } else {
                CHECK(false);
            }
        }
    }

    net::Socket& raw_socket() noexcept { return socket_; }

private:
    void wait(net::Interest interest) {
        CHECK(token_.has_value());
        CHECK(poller_.modify(*token_, interest));
        while (test::Clock::now() < deadline_) {
            auto events = test::take(poller_.wait(std::min(deadline_, test::Clock::now() + 50ms)));
            for (const auto& ev : events) {
                if (ev.token == *token_ && poller_.is_current(ev)) {
                    return;
                }
            }
        }
        CHECK(false);
    }

    net::Poller poller_;
    net::Socket socket_;
    std::optional<net::RegistrationToken> token_;
    net::Deadline deadline_;
    std::string unread_;
};

} // namespace event_loop_test
