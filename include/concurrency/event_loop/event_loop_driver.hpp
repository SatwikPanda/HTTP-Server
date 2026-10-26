#pragma once

#include "concurrency/lifecycle.hpp"
#include "concurrency/session.hpp"
#include "concurrency/work.hpp"
#include "core/clock.hpp"
#include "core/metrics.hpp"
#include "core/result.hpp"
#include "net/endpoint.hpp"
#include "net/poller.hpp"
#include "net/socket.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace concurrency::event_loop {

struct EventLoopBudgets {
    // Maximum client sockets accepted in a single loop turn.
    std::size_t max_accepts_per_turn{16};
    // Maximum helper completions dequeued and delivered in a single loop turn.
    std::size_t max_completions_per_turn{64};
    // Maximum runnable session turns advanced in a single loop turn.
    std::size_t max_sessions_per_turn{64};
    // Maximum state transitions / events processed for one session in a single turn.
    std::size_t max_events_per_session_turn{8};
    // Maximum downstream+upstream bytes transferred for one session in a single turn.
    std::size_t max_bytes_per_session_turn{64 * 1024};
};

struct EventLoopLimits {
    // Maximum concurrent active client sessions owned by the loop.
    std::size_t max_sessions{1024};
    // Maximum concurrent active upstream sockets (connecting + connected).
    std::size_t max_upstreams{256};
    // Maximum concurrent in-progress nonblocking upstream connect attempts.
    std::size_t max_connecting{128};
    // Maximum concurrent outstanding helper jobs across all sessions.
    std::size_t max_pending_jobs{256};
    // Maximum unread helper completions queued for the loop thread.
    std::size_t max_completion_queue{256};
    // Work payload / candidate / path validation limits.
    WorkLimits work{};
};

struct EventLoopConfig {
    net::Endpoint endpoint{net::Endpoint::ipv4_loopback(8080)};
    int listen_backlog{128};
    EventLoopBudgets budgets{};
    EventLoopLimits limits{};
    // Optional synchronous 503 overload response when a session is rejected at accept.
    // Attempted only if the accepted socket is immediately writable without blocking.
    bool send_overload_503{true};
    const core::SteadyClock* clock{&core::real_steady_clock()};
    core::MetricsSink* metrics{&core::null_metrics()};
};

struct EventLoopStats {
    std::uint64_t iterations{};
    std::uint64_t readiness_waits{};
    std::uint64_t immediate_wakes{};
    std::uint64_t connections_accepted{};
    std::uint64_t connections_rejected{};
    std::uint64_t overload_responses_sent{};
    std::uint64_t active_sessions{};
    std::uint64_t peak_active_sessions{};
    std::uint64_t active_upstreams{};
    std::uint64_t peak_active_upstreams{};
    std::uint64_t active_connecting{};
    std::uint64_t peak_active_connecting{};
    std::uint64_t pending_jobs{};
    std::uint64_t peak_pending_jobs{};
    std::uint64_t queued_completions{};
    std::uint64_t peak_queued_completions{};
    std::uint64_t runnable_sessions{};
    std::uint64_t peak_runnable_sessions{};
    std::uint64_t stale_events_dropped{};
    std::uint64_t stale_completions_dropped{};
    std::uint64_t timeouts_fired{};
    std::uint64_t budget_requeues{};
    std::uint64_t peak_session_input_bytes{};
    std::uint64_t peak_session_output_bytes{};
    std::uint64_t total_bytes_read{};
    std::uint64_t total_bytes_written{};
};

[[nodiscard]] core::Result<void> validate_event_loop_config(const EventLoopConfig& config);

// Single-threaded event-loop driver. Owns listener, downstream client sockets,
// upstream origin sockets, readiness registrations, and live ConnectionSessions
// on its loop thread. File and DNS work are dispatched to the injected
// asynchronous WorkExecutor and delivered back via a bounded, wake-integrated
// completion queue.
class EventLoopDriver final : public DriverLifecycle {
public:
    EventLoopDriver(
        EventLoopConfig config,
        SessionFactory session_factory,
        std::shared_ptr<WorkExecutor> helper_executor = nullptr
    );
    ~EventLoopDriver() override;

    EventLoopDriver(const EventLoopDriver&) = delete;
    EventLoopDriver& operator=(const EventLoopDriver&) = delete;

    [[nodiscard]] core::Result<void> start() override;
    void request_stop(std::chrono::milliseconds drain_timeout) noexcept override;
    void join() noexcept override;

    // Bind listener and initialize poller without launching a background thread,
    // or inspect the bound endpoint after start().
    [[nodiscard]] core::Result<void> bind_listener();
    // Run the event loop synchronously on the calling thread until stopped.
    [[nodiscard]] core::Result<void> run();
    // Execute at most one bounded iteration on the calling thread (for deterministic tests).
    [[nodiscard]] core::Result<bool> step_once();

    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] net::Endpoint local_endpoint() const noexcept;
    [[nodiscard]] EventLoopStats stats() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] core::Result<std::unique_ptr<EventLoopDriver>> make_event_loop_driver(
    EventLoopConfig config,
    SessionFactory session_factory,
    std::shared_ptr<WorkExecutor> helper_executor = nullptr
);

} // namespace concurrency::event_loop
