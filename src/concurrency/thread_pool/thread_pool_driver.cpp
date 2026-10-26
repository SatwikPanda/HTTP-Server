#include "concurrency/thread_pool/thread_pool_driver.hpp"

#include "net/poller.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>
#include <vector>

namespace concurrency::thread_pool {
namespace {
using SteadyClock = std::chrono::steady_clock;
constexpr std::size_t max_workers = 1024;
constexpr std::size_t max_queue = 65536;
constexpr auto max_drain = std::chrono::hours(24);
constexpr auto acceptor_idle_wait = std::chrono::seconds(1);

// Lets other threads wake a poller only while its owner keeps it alive.
class WakeHandle {
public:
    void set(net::Poller* poller) noexcept { std::lock_guard lock(mutex_); poller_ = poller; }
    void wake() noexcept { std::lock_guard lock(mutex_); if (poller_) (void)poller_->wake(); }
private:
    std::mutex mutex_;
    net::Poller* poller_{};
};

struct QueueItem {
    net::Socket socket;
    core::SteadyTimePoint accepted_at{};
    core::ConnectionId id{};
};

// The one helper job a worker may have outstanding. It lives as long as the
// worker thread, so a late completion drained after its session ended only
// finds an inactive or different job and is dropped.
struct PendingWork {
    bool active{};
    core::OperationId id{};
    std::shared_ptr<Cancellation> cancellation;
    std::optional<WorkEvent> result;
    void retire() noexcept {
        if (active && cancellation) cancellation->cancel();
        active = false;
        cancellation.reset();
        result.reset();
    }
};

WorkEvent work_event(const core::OperationId& id, WorkStatus status, core::Error error) {
    WorkEvent event;
    event.id = id;
    event.status = status;
    event.error = std::move(error);
    return event;
}
} // namespace

core::Result<void> validate_config(const ThreadPoolConfig& config) {
    if (config.workers == 0 || config.workers > max_workers || config.accept_queue == 0 ||
        config.accept_queue > max_queue || config.queue_timeout <= std::chrono::milliseconds::zero() ||
        config.accept_batch == 0 || config.accept_batch > max_queue || config.completion_capacity == 0 ||
        config.completion_capacity > max_queue || config.completion_budget == 0) {
        return core::make_error(core::ErrorCategory::invalid_argument, "thread pool config");
    }
    return {};
}

struct ThreadPoolDriver::State {
    ThreadPoolConfig config;
    ThreadPoolDependencies deps;
    net::Endpoint local;

    mutable std::mutex mutex; // queue, stop state, counters, startup
    std::condition_variable work_cv;
    std::condition_variable startup_cv;
    std::deque<QueueItem> queue;
    bool stopping{};
    std::chrono::milliseconds drain{};
    core::SteadyTimePoint drain_deadline{};
    std::atomic<bool> stop_flag{false};
    std::uint64_t next_connection{};
    std::size_t started_threads{}, ready_threads{};
    core::Error startup_error;
    ThreadPoolStats counters;

    std::mutex lifecycle_mutex; // start/join
    bool started{};
    std::thread acceptor;
    std::vector<std::thread> workers;
    WakeHandle acceptor_wake;
    std::vector<std::unique_ptr<WakeHandle>> worker_wakes;

    void report_startup(core::Error error) {
        { std::lock_guard lock(mutex); if (error && !startup_error) startup_error = std::move(error); ++ready_threads; }
        startup_cv.notify_all();
    }
    bool wait_startup(std::size_t expected) {
        std::unique_lock lock(mutex);
        startup_cv.wait(lock, [&] { return ready_threads >= expected; });
        return !startup_error;
    }
    void reject(net::Socket& socket) {
        if (deps.overload_response && !deps.overload_response->empty()) {
            (void)socket.write_some(*deps.overload_response); // one nonblocking attempt
        }
        socket.close();
    }
    void run_acceptor();
    void admit(net::Socket socket);
    void sweep_expired();
    void run_worker(std::size_t index);
    void drive(QueueItem& item, net::Poller& poller, const std::shared_ptr<helpers::CompletionPort>& port,
               PendingWork& pending);
};

ThreadPoolDriver::ThreadPoolDriver(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

ThreadPoolDriver::~ThreadPoolDriver() {
    request_stop(std::chrono::milliseconds::zero());
    join();
}

core::Result<std::unique_ptr<ThreadPoolDriver>> ThreadPoolDriver::create(ThreadPoolConfig config,
                                                                        ThreadPoolDependencies deps) {
    if (auto valid = validate_config(config); !valid) return valid.error();
    if (!deps.listener.is_valid() || !deps.sessions) {
        return core::make_error(core::ErrorCategory::invalid_argument, "thread pool dependencies");
    }
    auto local = deps.listener.local_endpoint();
    if (!local) return local.error();
    if (auto nonblocking = deps.listener.set_nonblocking(true); !nonblocking) return nonblocking.error();
    auto state = std::make_unique<State>();
    state->local = local.value();
    for (std::size_t i = 0; i < config.workers; ++i) state->worker_wakes.push_back(std::make_unique<WakeHandle>());
    state->config = std::move(config);
    state->deps = std::move(deps);
    return std::unique_ptr<ThreadPoolDriver>(new ThreadPoolDriver(std::move(state)));
}

core::Result<void> ThreadPoolDriver::start() {
    auto& s = *state_;
    {
        std::lock_guard lifecycle(s.lifecycle_mutex);
        if (s.started) return core::make_error(core::ErrorCategory::invalid_argument, "thread pool start", 0, "already started");
        s.started = true;
        {
            std::lock_guard lock(s.mutex);
            if (s.stopping) return core::make_error(core::ErrorCategory::invalid_argument, "thread pool start", 0, "stopped");
        }
        // Workers first: each creates its poller on its own thread and reports.
        core::Error failure;
        auto allowed = [&](std::size_t index) {
            if (!s.config.before_thread_start) return true;
            auto result = s.config.before_thread_start(index);
            if (!result) failure = result.error();
            return bool(result);
        };
        try {
            for (std::size_t i = 0; i < s.config.workers && allowed(i); ++i) {
                s.workers.emplace_back([&s, i] { s.run_worker(i); });
                std::lock_guard lock(s.mutex);
                ++s.started_threads;
            }
        } catch (const std::system_error& error) {
            failure = core::make_error(core::ErrorCategory::system_error, "worker thread start", error.code().value());
        }
        std::size_t expected = 0;
        { std::lock_guard lock(s.mutex); expected = s.started_threads; }
        if (!s.wait_startup(expected) && !failure) {
            std::lock_guard lock(s.mutex);
            failure = s.startup_error;
        }
        if (!failure && allowed(s.config.workers)) {
            try {
                s.acceptor = std::thread([&s] { s.run_acceptor(); });
                if (!s.wait_startup(expected + 1)) { std::lock_guard lock(s.mutex); failure = s.startup_error; }
            } catch (const std::system_error& error) {
                failure = core::make_error(core::ErrorCategory::system_error, "acceptor thread start", error.code().value());
            }
        }
        if (!failure) return {};
        { std::lock_guard lock(s.mutex); s.startup_error = failure; }
        request_stop(std::chrono::milliseconds::zero());
    }
    join(); // outside the lifecycle lock: cleans up the partial start
    std::lock_guard lock(s.mutex);
    return s.startup_error;
}

void ThreadPoolDriver::request_stop(std::chrono::milliseconds drain_timeout) noexcept {
    auto& s = *state_;
    const auto bounded = std::clamp(drain_timeout, std::chrono::milliseconds::zero(),
                                    std::chrono::duration_cast<std::chrono::milliseconds>(max_drain));
    {
        std::lock_guard lock(s.mutex);
        const auto deadline = SteadyClock::now() + bounded;
        if (!s.stopping) { s.stopping = true; s.drain = bounded; s.drain_deadline = deadline; }
        else if (deadline < s.drain_deadline) { s.drain = bounded; s.drain_deadline = deadline; }
        s.stop_flag.store(true, std::memory_order_release);
    }
    s.work_cv.notify_all();
    s.acceptor_wake.wake();
    for (auto& wake : s.worker_wakes) wake->wake();
}

void ThreadPoolDriver::join() noexcept {
    auto& s = *state_;
    std::lock_guard lifecycle(s.lifecycle_mutex);
    if (s.acceptor.joinable()) s.acceptor.join();
    for (auto& worker : s.workers) if (worker.joinable()) worker.join();
}

net::Endpoint ThreadPoolDriver::local_endpoint() const { return state_->local; }

ThreadPoolStats ThreadPoolDriver::stats() const {
    std::lock_guard lock(state_->mutex);
    auto copy = state_->counters;
    copy.queued = state_->queue.size();
    return copy;
}

// ---- acceptor ---------------------------------------------------------------

void ThreadPoolDriver::State::run_acceptor() {
    auto created = net::Poller::create();
    if (!created) { report_startup(created.error()); return; }
    auto poller = std::move(created.value());
    auto& listener = deps.listener;
    auto watched = poller.watch(listener, net::Interest::read, core::ConnectionId{});
    if (!watched) { report_startup(watched.error()); return; }
    const auto token = watched.value();
    acceptor_wake.set(&poller);
    report_startup({});

    while (!stop_flag.load(std::memory_order_acquire)) {
        auto deadline = SteadyClock::now() + acceptor_idle_wait;
        {
            std::lock_guard lock(mutex);
            if (!queue.empty()) deadline = std::min(deadline, queue.front().accepted_at + config.queue_timeout);
        }
        auto events = poller.wait(deadline);
        if (!events) {
            std::lock_guard lock(mutex);
            ++counters.accept_errors;
            break; // a broken poller cannot recover; the server stops admitting
        }
        sweep_expired();
        if (stop_flag.load(std::memory_order_acquire)) break;
        const bool readable = std::any_of(events.value().begin(), events.value().end(), [&](const auto& event) {
            return event.token == token && poller.is_current(event);
        });
        if (!readable) continue;
        for (std::size_t n = 0; n < config.accept_batch; ++n) {
            auto accepted = listener.accept();
            if (!accepted) {
                if (accepted.error().category != core::ErrorCategory::would_block) {
                    std::lock_guard lock(mutex);
                    ++counters.accept_errors;
                }
                break;
            }
            auto socket = std::move(accepted.value());
            // Never rely on the listener's mode being inherited.
            if (!socket.set_nonblocking(true)) {
                socket.close();
                std::lock_guard lock(mutex);
                ++counters.accept_errors;
                continue;
            }
            admit(std::move(socket));
        }
    }

    acceptor_wake.set(nullptr);
    (void)poller.unwatch(token);
    listener.close();
    std::deque<QueueItem> abandoned;
    { std::lock_guard lock(mutex); abandoned.swap(queue); }
    for (auto& item : abandoned) item.socket.close(); // queued, never registered: owner closes
}

void ThreadPoolDriver::State::admit(net::Socket socket) {
    {
        std::lock_guard lock(mutex);
        if (stopping) {
            ++counters.rejected_stopping;
        } else if (queue.size() >= config.accept_queue) {
            ++counters.rejected_full;
        } else {
            ++counters.accepted;
            queue.push_back({std::move(socket), SteadyClock::now(), {++next_connection, 1}});
            counters.peak_queued = std::max(counters.peak_queued, queue.size());
            work_cv.notify_one();
            return;
        }
    }
    reject(socket);
}

void ThreadPoolDriver::State::sweep_expired() {
    std::vector<QueueItem> expired;
    {
        std::lock_guard lock(mutex);
        const auto now = SteadyClock::now();
        while (!queue.empty() && now - queue.front().accepted_at >= config.queue_timeout) {
            expired.push_back(std::move(queue.front()));
            queue.pop_front();
            ++counters.queue_timeouts;
        }
    }
    for (auto& item : expired) reject(item.socket);
}

// ---- workers ----------------------------------------------------------------

void ThreadPoolDriver::State::run_worker(std::size_t index) {
    auto created = net::Poller::create();
    if (!created) { report_startup(created.error()); return; }
    auto poller = std::move(created.value());
    auto& wake = *worker_wakes[index];
    PendingWork pending;
    std::shared_ptr<helpers::CompletionPort> port;
    if (deps.helpers) {
        port = helpers::CompletionPort::create(config.completion_capacity, [&wake] { wake.wake(); });
        if (!port) { report_startup(core::make_error(core::ErrorCategory::invalid_argument, "worker completion port")); return; }
    }
    wake.set(&poller);
    report_startup({});

    for (;;) {
        QueueItem item;
        {
            std::unique_lock lock(mutex);
            work_cv.wait(lock, [&] { return stopping || !queue.empty(); });
            if (stopping) break; // queued sockets that never started are closed by the acceptor
            item = std::move(queue.front());
            queue.pop_front();
            ++counters.active;
            counters.peak_active = std::max(counters.peak_active, counters.active);
        }
        if (SteadyClock::now() - item.accepted_at >= config.queue_timeout) {
            reject(item.socket);
            std::lock_guard lock(mutex);
            ++counters.queue_timeouts;
            --counters.active;
            continue;
        }
        drive(item, poller, port, pending);
        pending.retire();
        if (port) (void)port->drain(config.completion_budget); // discard stale results promptly
        std::lock_guard lock(mutex);
        --counters.active;
    }

    pending.retire();
    if (port) port->close(); // after this, helpers never call wake for this worker
    wake.set(nullptr);
}

void ThreadPoolDriver::State::drive(QueueItem& item, net::Poller& poller,
                                     const std::shared_ptr<helpers::CompletionPort>& port, PendingWork& pending) {
    auto& socket = item.socket;
    auto created = deps.sessions(item.id);
    if (!created || !created.value()) {
        socket.close();
        std::lock_guard lock(mutex);
        ++counters.session_failures;
        return;
    }
    auto session = std::move(created.value());
    { std::lock_guard lock(mutex); ++counters.sessions_started; }
    auto watched = poller.watch(socket, net::Interest::none, item.id);
    if (!watched) {
        (void)session->on_event(ErrorEvent{watched.error()});
        socket.close();
        std::lock_guard lock(mutex);
        ++counters.session_failures;
        return;
    }
    const auto token = watched.value();

    // Upstream sockets for this connection belong to this worker; the reference
    // executor performs one nonblocking step or a bounded wait per action.
    ReferenceWorkExecutor upstream;
    std::optional<helpers::HelperExecutor> helper_executor;
    if (port) helper_executor.emplace(deps.helpers, port);
    const std::function<bool()> stop_requested = [this] { return stop_flag.load(std::memory_order_acquire); };
    std::vector<std::byte> buffer;
    bool stop_delivered = false;
    auto hard_deadline = core::SteadyTimePoint::max();
    bool forced = false;

    Action action = session->initial_action();
    while (!session->is_closed() && !action.is_close() && !action.is_none()) {
        const auto now = SteadyClock::now();
        if (!stop_delivered && stop_requested()) {
            stop_delivered = true;
            std::chrono::milliseconds drain_budget{};
            { std::lock_guard lock(mutex); drain_budget = drain; hard_deadline = drain_deadline; }
            pending.retire(); // a waiting job is cancelled immediately
            action = session->on_event(StopEvent{drain_budget});
            continue;
        }
        if (stop_delivered && now >= hard_deadline) { forced = true; break; }
        if (session->idle_timeout() > std::chrono::milliseconds::zero() &&
            now - session->last_progress_time() >= session->idle_timeout()) {
            action = session->on_event(TimeoutEvent{});
            break;
        }

        if (const auto* read = action.as_read()) {
            const auto id = read->id;
            const auto max_bytes = read->max_bytes;
            if (buffer.size() < max_bytes) buffer.resize(max_bytes);
            auto result = socket.read_some(std::span(buffer.data(), max_bytes));
            if (result.is_ok()) {
                if (result.bytes_transferred == 0 || result.bytes_transferred > max_bytes) {
                    action = session->on_event(ErrorEvent{core::make_error(core::ErrorCategory::io_error, "worker read", 0, "Invalid progress count"), id});
                } else {
                    action = session->on_event(ReadDataEvent{std::span(buffer.data(), result.bytes_transferred), id});
                }
            } else if (result.is_eof()) {
                action = session->on_event(ReadEofEvent{id});
            } else if (result.would_block()) {
                action = session->on_event(WouldBlockEvent{net::Interest::read, id});
            } else {
                action = session->on_event(ErrorEvent{result.error, id});
            }
        } else if (const auto* write = action.as_write()) {
            const auto id = write->id;
            const auto data = write->data;
            if (data.empty()) { action = session->on_event(WriteCompleteEvent{0, id}); continue; }
            auto result = socket.write_some(data);
            if (result.is_ok()) {
                if (result.bytes_transferred == 0 || result.bytes_transferred > data.size()) {
                    action = session->on_event(ErrorEvent{core::make_error(core::ErrorCategory::io_error, "worker write", 0, "Invalid progress count"), id});
                } else {
                    action = session->on_event(WriteCompleteEvent{result.bytes_transferred, id});
                }
            } else if (result.would_block()) {
                action = session->on_event(WouldBlockEvent{net::Interest::write, id});
            } else {
                action = session->on_event(ErrorEvent{result.error, id});
            }
        } else if (const auto* wait = action.as_wait()) {
            const auto id = wait->id;
            const auto wait_deadline = wait->deadline;
            if (auto modified = poller.modify(token, wait->interest); !modified) {
                action = session->on_event(ErrorEvent{modified.error(), id});
                continue;
            }
            std::optional<Action> next;
            while (!next) {
                if (!stop_delivered && stop_requested()) break;
                const auto now_wait = SteadyClock::now();
                if (now_wait >= wait_deadline) { next = session->on_event(TimeoutEvent{id}); break; }
                if (now_wait >= hard_deadline) break;
                auto events = poller.wait(std::min(wait_deadline, hard_deadline));
                if (!events) { next = session->on_event(ErrorEvent{events.error(), id}); break; }
                if (port) (void)port->drain(config.completion_budget); // only stale results can be here
                for (const auto& event : events.value()) {
                    if (event.token == token && poller.is_current(event)) {
                        // Attempt I/O on error/hangup too; the socket call reports the cause.
                        next = session->on_event(ReadyEvent{event.readable || event.hangup || event.error,
                                                            event.writable || event.hangup || event.error, id});
                        break;
                    }
                }
            }
            (void)poller.modify(token, net::Interest::none); // no readiness while not waiting
            if (next) action = std::move(*next);
        } else if (const auto* work_ptr = action.as_work()) {
            const WorkAction work = *work_ptr;
            if (!helper_executor || !helpers::is_helper_work(work.request)) {
                action = session->on_event(upstream.execute(work, stop_requested));
                continue;
            }
            pending.retire();
            pending.active = true;
            pending.id = work.id;
            pending.cancellation = work.cancellation;
            auto submitted = helper_executor->submit(work, [&pending](WorkEvent event) {
                if (pending.active && event.id == pending.id && !pending.result) pending.result = std::move(event);
            });
            if (!submitted) {
                pending.active = false;
                { std::lock_guard lock(mutex); ++counters.work_rejected; }
                action = session->on_event(work_event(work.id, WorkStatus::rejected, submitted.error()));
                continue;
            }
            std::optional<Action> next;
            while (!next) {
                (void)port->drain(config.completion_budget);
                if (pending.result) {
                    auto result = std::move(*pending.result);
                    pending.active = false;
                    pending.cancellation.reset();
                    pending.result.reset();
                    next = session->on_event(result);
                    break;
                }
                if (!stop_delivered && stop_requested()) break; // top of loop cancels and stops
                const auto now_work = SteadyClock::now();
                if (now_work >= work.deadline) {
                    // Do not wait for the helper; its eventual result is stale.
                    pending.retire();
                    { std::lock_guard lock(mutex); ++counters.work_timeouts; }
                    next = session->on_event(work_event(work.id, WorkStatus::timed_out,
                        core::make_error(core::ErrorCategory::timed_out, "helper work deadline")));
                    break;
                }
                if (now_work >= hard_deadline) break;
                auto events = poller.wait(std::min(work.deadline, hard_deadline));
                if (!events) {
                    pending.retire();
                    next = session->on_event(ErrorEvent{events.error()});
                    break;
                }
            }
            if (next) action = std::move(*next);
        } else {
            break;
        }
    }

    pending.retire();
    if (forced) { std::lock_guard lock(mutex); ++counters.forced_closes; }
    if (!session->is_closed() && !stop_delivered && stop_requested()) {
        (void)session->on_event(StopEvent{std::chrono::milliseconds::zero()});
    }
    (void)poller.unwatch(token); // unwatch before close, on the owner
    socket.close();
    session.reset();
    std::lock_guard lock(mutex);
    ++counters.sessions_finished;
}

} // namespace concurrency::thread_pool
