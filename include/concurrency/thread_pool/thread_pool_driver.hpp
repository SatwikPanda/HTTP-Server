#pragma once

#include "concurrency/helpers/helper_pool.hpp"
#include "concurrency/lifecycle.hpp"
#include "core/result.hpp"
#include "net/endpoint.hpp"
#include "net/socket.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace concurrency::thread_pool {

struct ThreadPoolConfig {
    std::size_t workers{4};
    // Accepted sockets waiting for a worker, and how long one may wait.
    std::size_t accept_queue{64};
    std::chrono::milliseconds queue_timeout{2000};
    // Accept calls per readiness event, so the acceptor also sweeps its queue.
    std::size_t accept_batch{64};
    // Per-worker helper result slots, and completions run per drain.
    std::size_t completion_capacity{4};
    std::size_t completion_budget{16};
    // Diagnostic seam: called before each thread is created (workers are
    // 0..workers-1, the acceptor is `workers`). An error fails start() after
    // the threads already created are stopped and joined.
    std::function<core::Result<void>(std::size_t thread_index)> before_thread_start;
};
[[nodiscard]] core::Result<void> validate_config(const ThreadPoolConfig& config);

struct ThreadPoolDependencies {
    // Bound and listening. The driver takes ownership; the acceptor thread
    // configures and polls it and closes it on exit.
    net::Socket listener;
    SessionFactory sessions;
    // File and resolver work goes here. Without it, a worker runs that work
    // itself, which is only meant for sessions that never request it.
    std::shared_ptr<helpers::HelperPool> helpers;
    // Optional bounded bytes (L builds a 503 through the shared writer) sent
    // once, nonblocking and best effort, before an over-capacity close.
    std::shared_ptr<const std::vector<std::byte>> overload_response;
};

struct ThreadPoolStats {
    std::uint64_t accepted{}, rejected_full{}, rejected_stopping{}, queue_timeouts{}, accept_errors{},
        sessions_started{}, sessions_finished{}, session_failures{}, work_rejected{}, work_timeouts{},
        forced_closes{};
    std::size_t queued{}, active{}, peak_queued{}, peak_active{};
};

// One acceptor thread with a bounded queue of accepted, unregistered sockets,
// and a fixed set of workers that each drive one session at a time.
//
// Ownership: the acceptor owns the listener, its poller and every queued socket.
// A worker owns its poller, its current client socket and that connection's
// upstream socket. Sockets move between threads only while unregistered.
// Only Poller::wake() is called across threads.
class ThreadPoolDriver final : public DriverLifecycle {
public:
    [[nodiscard]] static core::Result<std::unique_ptr<ThreadPoolDriver>> create(
        ThreadPoolConfig config, ThreadPoolDependencies dependencies);
    ~ThreadPoolDriver() override; // request_stop(0) and join
    ThreadPoolDriver(const ThreadPoolDriver&) = delete;
    ThreadPoolDriver& operator=(const ThreadPoolDriver&) = delete;

    // Starts workers first, then the acceptor. A partial failure stops and joins
    // what started and returns the error. Not restartable after stop.
    [[nodiscard]] core::Result<void> start() override;
    // Idempotent, callable before start and from any non-driver thread. Stops
    // accepting, closes queued sockets, and gives active sessions StopEvent with
    // this drain budget; a session still open at the deadline is closed.
    void request_stop(std::chrono::milliseconds drain_timeout) noexcept override;
    void join() noexcept override;

    [[nodiscard]] net::Endpoint local_endpoint() const;
    [[nodiscard]] ThreadPoolStats stats() const;

private:
    struct State;
    explicit ThreadPoolDriver(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;
};

} // namespace concurrency::thread_pool
