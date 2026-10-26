#include "concurrency/event_loop/event_loop_driver.hpp"
#include "concurrency/helpers/helper_pool.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace concurrency::event_loop {
namespace {

constexpr std::string_view kOverload503Response =
    "HTTP/1.1 503 Service Unavailable\r\n"
    "Content-Type: text/plain; charset=utf-8\r\n"
    "Content-Length: 25\r\n"
    "Connection: close\r\n"
    "\r\n"
    "503 Service Unavailable\r\n";

core::Error invalid_config(const char* detail = "Invalid event-loop configuration") {
    return core::make_error(core::ErrorCategory::invalid_argument, "event_loop config", 0, detail);
}

core::SteadyTimePoint add_duration(core::SteadyTimePoint base, std::chrono::milliseconds delta) noexcept {
    if (delta <= std::chrono::milliseconds::zero()) return base;
    const auto ticks = std::chrono::duration_cast<core::SteadyTimePoint::duration>(delta);
    if (base > core::SteadyTimePoint::max() - ticks) return core::SteadyTimePoint::max();
    return base + ticks;
}

bool valid_work_limits(const WorkLimits& limits) noexcept {
    return limits.max_buffer_bytes > 0 && limits.max_buffer_bytes <= 16 * 1024 * 1024 &&
           limits.max_candidates > 0 && limits.max_candidates <= 256 &&
           limits.max_path_bytes > 0 && limits.max_path_bytes <= 32768;
}

struct TokenHash {
    std::size_t operator()(const net::RegistrationToken& token) const noexcept {
        std::size_t h1 = std::hash<std::uint64_t>{}(token.slot);
        std::size_t h2 = std::hash<std::uint64_t>{}(token.generation);
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
};

enum class RegistrationRole {
    listener,
    downstream,
    upstream
};

struct RegistrationRecord {
    net::RegistrationToken token{};
    core::ConnectionId connection{};
    RegistrationRole role{RegistrationRole::downstream};
    std::uint64_t transaction_sequence{};
    std::uint64_t operation_sequence{};
    std::uint64_t attempt_sequence{};
};

struct ConnectState {
    bool active{};
    core::OperationId id{};
    core::SteadyTimePoint deadline{core::SteadyTimePoint::max()};
    std::shared_ptr<Cancellation> cancellation;
    std::vector<net::Endpoint> candidates;
    std::size_t next_index{};
    core::Error last_error{};
};

struct UpstreamWaitState {
    bool active{};
    core::OperationId id{};
    net::Interest interest{net::Interest::none};
    core::SteadyTimePoint deadline{core::SteadyTimePoint::max()};
    std::shared_ptr<Cancellation> cancellation;
};

struct HelperWaitState {
    bool active{};
    core::OperationId id{};
    core::SteadyTimePoint deadline{core::SteadyTimePoint::max()};
    std::shared_ptr<Cancellation> cancellation;
};

struct SessionEntry {
    core::ConnectionId id{};
    std::unique_ptr<ConnectionSession> session;

    net::Socket client_socket;
    std::optional<net::RegistrationToken> client_token;
    net::Interest client_interest{net::Interest::none};

    net::Socket upstream_socket;
    std::optional<net::RegistrationToken> upstream_token;
    net::Interest upstream_interest{net::Interest::none};
    core::TransactionId upstream_owner{};
    std::uint64_t upstream_last_op{};
    std::uint64_t upstream_attempt{};
    bool upstream_counted{};

    Action current_action{};
    bool action_dispatched{};

    bool queued{};
    bool closing{};
    bool stop_delivered{};
    bool downstream_ready_read{};
    bool downstream_ready_write{};
    bool upstream_ready_read{};
    bool upstream_ready_write{};

    ConnectState connect{};
    UpstreamWaitState upstream_wait{};
    HelperWaitState helper_wait{};
    std::optional<Event> pending_event{};
};

// Shared completion state that outlives EventLoopDriver if an uncancellable
// helper worker finishes after the loop stops.
struct CompletionPortState {
    std::mutex mutex;
    net::Poller* poller{nullptr};
    bool closed{false};
    std::size_t max_queue{256};
    std::deque<WorkEvent> queue;
    std::uint64_t peak_queued{};
    std::uint64_t dropped_stale{};
};

} // namespace

core::Result<void> validate_event_loop_config(const EventLoopConfig& config) {
    if (!config.clock || !config.metrics) {
        return invalid_config("Clock and metrics pointers must be non-null");
    }
    if (config.listen_backlog <= 0) {
        return invalid_config("Listen backlog must be positive");
    }
    const auto& b = config.budgets;
    if (b.max_accepts_per_turn == 0 || b.max_completions_per_turn == 0 ||
        b.max_sessions_per_turn == 0 || b.max_events_per_session_turn == 0 ||
        b.max_bytes_per_session_turn == 0) {
        return invalid_config("All per-turn budgets must be positive");
    }
    const auto& l = config.limits;
    if (l.max_sessions == 0 || l.max_upstreams == 0 || l.max_connecting == 0 ||
        l.max_pending_jobs == 0 || l.max_completion_queue == 0) {
        return invalid_config("All capacity limits must be positive");
    }
    if (l.max_connecting > l.max_upstreams) {
        return invalid_config("max_connecting cannot exceed max_upstreams");
    }
    if (!valid_work_limits(l.work)) {
        return invalid_config("Invalid work limits");
    }
    return {};
}

class EventLoopDriver::Impl {
public:
    Impl(
        EventLoopConfig config,
        SessionFactory session_factory,
        std::shared_ptr<WorkExecutor> helper_executor,
        std::shared_ptr<helpers::HelperPool> helper_pool
    )
        : config_(std::move(config)),
          session_factory_(std::move(session_factory)),
          helper_executor_(std::move(helper_executor)),
          helper_pool_(std::move(helper_pool)),
          completion_port_(std::make_shared<CompletionPortState>()) {
        completion_port_->max_queue = config_.limits.max_completion_queue;
    }

    ~Impl() {
        request_stop(std::chrono::milliseconds(0));
        join();
        std::lock_guard lock(lifecycle_mutex_);
        cleanup_all_on_owner();
    }

    core::Result<void> bind_listener() {
        std::lock_guard lock(lifecycle_mutex_);
        return bind_listener_locked();
    }

    core::Result<void> start() {
        std::lock_guard lock(lifecycle_mutex_);
        if (driving_ || running_.load() || thread_.joinable()) {
            return core::make_error(core::ErrorCategory::invalid_argument, "EventLoopDriver::start", 0, "Driver already active");
        }
        auto bound = bind_listener_locked();
        if (!bound) return bound.error();

        stop_requested_.store(false, std::memory_order_release);
        drain_deadline_Initialized_ = false;
        driving_ = true;
        running_.store(true, std::memory_order_release);
        try {
            thread_ = std::thread([this] {
                (void)run_loop();
            });
        } catch (...) {
            driving_ = false;
            running_.store(false, std::memory_order_release);
            cleanup_all_on_owner();
            return core::make_error(core::ErrorCategory::system_error, "EventLoopDriver::start", 0, "Failed to start loop thread");
        }
        return {};
    }

    void request_stop(std::chrono::milliseconds drain_timeout) noexcept {
        const auto clamped = std::max(
            std::chrono::milliseconds::zero(),
            std::min(drain_timeout, std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::hours(24)))
        );
        {
            std::lock_guard lock(stop_mutex_);
            if (!stop_requested_.load(std::memory_order_relaxed) || clamped < requested_drain_timeout_) {
                requested_drain_timeout_ = clamped;
                stop_updated_ = true;
            }
            stop_requested_.store(true, std::memory_order_release);
        }
        std::lock_guard port_lock(completion_port_->mutex);
        if (completion_port_->poller) {
            (void)completion_port_->poller->wake();
        }
    }

    void join() noexcept {
        std::thread to_join;
        {
            std::lock_guard lock(lifecycle_mutex_);
            if (thread_.joinable()) {
                to_join = std::move(thread_);
            }
        }
        if (to_join.joinable()) {
            if (to_join.get_id() == std::this_thread::get_id()) {
                return;
            }
            to_join.join();
        }
        std::unique_lock lock(lifecycle_mutex_);
        cv_.wait(lock, [this] { return !driving_; });
        cleanup_all_on_owner();
    }

    core::Result<void> run() {
        {
            std::lock_guard lock(lifecycle_mutex_);
            if (driving_ || thread_.joinable()) {
                return core::make_error(core::ErrorCategory::invalid_argument, "EventLoopDriver::run", 0, "Driver already active");
            }
            if (!poller_ || !listener_.is_valid()) {
                if (stop_requested_.load(std::memory_order_acquire)) {
                    return {};
                }
                auto bound = bind_listener_locked();
                if (!bound) return bound.error();
            }
            driving_ = true;
            running_.store(true, std::memory_order_release);
        }
        auto res = run_loop();
        return res;
    }

    core::Result<bool> step_once() {
        {
            std::lock_guard lock(lifecycle_mutex_);
            if (driving_ || thread_.joinable()) {
                return core::make_error(core::ErrorCategory::invalid_argument, "EventLoopDriver::step_once", 0, "Driver already active");
            }
            if (!poller_ || !listener_.is_valid()) {
                auto bound = bind_listener_locked();
                if (!bound) return bound.error();
            }
            driving_ = true;
            running_.store(true, std::memory_order_release);
        }
        auto res = execute_iteration(true);
        {
            std::lock_guard lock(lifecycle_mutex_);
            driving_ = false;
            if (!res || !res.value()) {
                running_.store(false, std::memory_order_release);
                cleanup_all_on_owner();
            }
            cv_.notify_all();
        }
        return res;
    }

    bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    net::Endpoint local_endpoint() const noexcept {
        std::lock_guard lock(stats_mutex_);
        return actual_endpoint_;
    }

    EventLoopStats stats() const noexcept {
        std::lock_guard lock(stats_mutex_);
        EventLoopStats snap = stats_;
        {
            std::lock_guard port_lock(completion_port_->mutex);
            snap.queued_completions = completion_port_->queue.size();
            snap.peak_queued_completions = std::max(snap.peak_queued_completions, completion_port_->peak_queued);
            snap.stale_completions_dropped += completion_port_->dropped_stale;
        }
        return snap;
    }

private:
    core::Result<void> bind_listener_locked() {
        auto valid = validate_event_loop_config(config_);
        if (!valid) return valid.error();
        if (!session_factory_) {
            return invalid_config("SessionFactory must be callable");
        }
        if (poller_ && listener_.is_valid()) {
            return {};
        }
        cleanup_all_on_owner();

        auto created_poller = net::Poller::create();
        if (!created_poller) return created_poller.error();
        poller_ = std::make_unique<net::Poller>(std::move(created_poller.value()));

        auto created_socket = net::create_tcp_socket(config_.endpoint.family());
        if (!created_socket) {
            poller_.reset();
            return created_socket.error();
        }
        net::Socket listener = std::move(created_socket.value());
#ifndef _WIN32
        auto reuse = listener.set_reuse_address(true);
        if (!reuse) {
            poller_.reset();
            return reuse.error();
        }
#endif
        auto bound = listener.bind(config_.endpoint);
        if (!bound) {
            poller_.reset();
            return bound.error();
        }
        auto listening = listener.listen(config_.listen_backlog);
        if (!listening) {
            poller_.reset();
            return listening.error();
        }
        auto nonblocking = listener.set_nonblocking(true);
        if (!nonblocking) {
            poller_.reset();
            return nonblocking.error();
        }
        auto local = listener.local_endpoint();
        if (!local) {
            poller_.reset();
            return local.error();
        }

        const core::ConnectionId listener_id{0, ++next_generation_};
        auto watched = poller_->watch(listener, net::Interest::read, listener_id);
        if (!watched) {
            poller_.reset();
            return watched.error();
        }

        listener_token_ = watched.value();
        listener_interest_ = net::Interest::read;
        listener_ready_ = false;
        registrations_[*listener_token_] = RegistrationRecord{
            *listener_token_, listener_id, RegistrationRole::listener, 0, 0, 0
        };
        listener_ = std::move(listener);

        {
            std::lock_guard stats_lock(stats_mutex_);
            actual_endpoint_ = local.value();
        }
        {
            std::lock_guard port_lock(completion_port_->mutex);
            completion_port_->poller = poller_.get();
            completion_port_->closed = false;
            completion_port_->max_queue = config_.limits.max_completion_queue;
        }
        if (helper_pool_) {
            auto port = completion_port_;
            helper_destination_ = helpers::CompletionPort::create(config_.limits.max_completion_queue, [port] {
                std::lock_guard lock(port->mutex);
                if (port->poller) (void)port->poller->wake();
            });
            helper_executor_ = std::make_shared<helpers::HelperExecutor>(helper_pool_, helper_destination_);
        }
        return {};
    }

    core::Result<void> run_loop() {
        core::Result<void> final_result{};
        for (;;) {
            auto step = execute_iteration();
            if (!step) {
                final_result = step.error();
                break;
            }
            if (!step.value()) {
                break;
            }
        }
        {
            std::lock_guard lock(lifecycle_mutex_);
            cleanup_all_on_owner();
            driving_ = false;
            running_.store(false, std::memory_order_release);
            cv_.notify_all();
        }
        return final_result;
    }

    // Returns true if the loop should continue, false when stopped and drained.
    core::Result<bool> execute_iteration(bool single_step = false) {
        const auto now = config_.clock->now();
        {
            std::lock_guard lock(stats_mutex_);
            ++stats_.iterations;
        }

        // 1. Check stop and deadlines
        check_stop_transition(now);
        check_deadlines(now);

        if (stopping_ && sessions_.empty()) {
            return false;
        }

        // 2. Deliver bounded helper completions, dropping stale results
        if (helper_destination_) helper_destination_->drain(config_.budgets.max_completions_per_turn);
        const bool more_completions = process_completions() ||
            (helper_destination_ && helper_destination_->has_ready());

        // 3. Advance bounded accepts and runnable sessions
        const bool more_accepts = process_accepts();
        const bool more_runnable = process_runnable_sessions();

        // Check deadlines again if stop or turn work finished everything
        const auto after_work = config_.clock->now();
        check_stop_transition(after_work);
        if (!more_completions && !more_accepts && !more_runnable && ready_queue_.empty()) {
            check_deadlines(after_work);
        }

        if (stopping_ && sessions_.empty()) {
            return false;
        }

        // 4. Update each endpoint's desired interest
        update_listener_interest();
        for (auto& [conn_id, entry_ptr] : sessions_) {
            (void)conn_id;
            update_session_interests(*entry_ptr);
        }

        // 5. Compute nearest deadline (or immediate wake if runnable work remains)
        bool has_pending_completions = more_completions;
        if (!has_pending_completions) {
            std::lock_guard port_lock(completion_port_->mutex);
            has_pending_completions = !completion_port_->queue.empty();
        }
        const bool immediate_work = has_pending_completions || more_accepts || more_runnable || !ready_queue_.empty();
        auto poll_deadline = immediate_work ? after_work : compute_nearest_deadline(after_work);
        if (single_step) {
            poll_deadline = std::min(poll_deadline, after_work + std::chrono::milliseconds{10});
        }

        // 6. Poll for readiness
        {
            std::lock_guard lock(stats_mutex_);
            ++stats_.readiness_waits;
            if (immediate_work) ++stats_.immediate_wakes;
        }
        auto events_res = poller_->wait(poll_deadline);
        if (!events_res) {
            return events_res.error();
        }

        // 7. Validate and enqueue ready sessions without duplicates
        for (const auto& ev : events_res.value()) {
            if (!poller_->is_current(ev)) {
                record_stale_event();
                continue;
            }
            auto reg_it = registrations_.find(ev.token);
            if (reg_it == registrations_.end()) {
                record_stale_event();
                continue;
            }
            const RegistrationRecord reg = reg_it->second;
            if (reg.connection != ev.owner) {
                record_stale_event();
                continue;
            }
            if (reg.role == RegistrationRole::listener) {
                if (!stopping_ && listener_token_ && *listener_token_ == ev.token) {
                    if (ev.readable || ev.error || ev.hangup) {
                        listener_ready_ = true;
                    }
                }
                continue;
            }

            auto sess_it = sessions_.find(reg.connection.value);
            if (sess_it == sessions_.end() || sess_it->second->id != reg.connection || sess_it->second->closing) {
                record_stale_event();
                continue;
            }
            SessionEntry& entry = *sess_it->second;

            if (reg.role == RegistrationRole::downstream) {
                if (!entry.client_token || *entry.client_token != ev.token) {
                    record_stale_event();
                    continue;
                }
                // Consume readable bytes even when hangup/error is also reported.
                if (ev.readable || ev.hangup || ev.error) entry.downstream_ready_read = true;
                if (ev.writable || ev.hangup || ev.error) entry.downstream_ready_write = true;
                enqueue_ready(entry);
            } else if (reg.role == RegistrationRole::upstream) {
                if (!entry.upstream_token || *entry.upstream_token != ev.token ||
                    entry.upstream_attempt != reg.attempt_sequence ||
                    entry.upstream_owner.sequence != reg.transaction_sequence) {
                    record_stale_event();
                    continue;
                }
                if (ev.readable || ev.hangup || ev.error) entry.upstream_ready_read = true;
                if (ev.writable || ev.hangup || ev.error) entry.upstream_ready_write = true;
                enqueue_ready(entry);
            }
        }

        // Re-check deadlines after poll returns
        const auto post_poll = config_.clock->now();
        check_stop_transition(post_poll);
        check_deadlines(post_poll);

        if (stopping_ && sessions_.empty()) {
            return false;
        }
        return true;
    }

    void check_stop_transition(core::SteadyTimePoint now) {
        if (!stop_requested_.load(std::memory_order_acquire)) {
            return;
        }
        std::chrono::milliseconds drain_ms{0};
        bool updated = false;
        {
            std::lock_guard lock(stop_mutex_);
            drain_ms = requested_drain_timeout_;
            updated = stop_updated_;
            stop_updated_ = false;
        }

        if (!stopping_) {
            stopping_ = true;
            close_listener_on_owner();
            drain_deadline_ = add_duration(now, drain_ms);
            drain_deadline_Initialized_ = true;
        } else if (updated) {
            const auto candidate = add_duration(now, drain_ms);
            if (!drain_deadline_Initialized_ || candidate < drain_deadline_) {
                drain_deadline_ = candidate;
                drain_deadline_Initialized_ = true;
            }
        }

        // Deliver StopEvent to active sessions
        std::vector<std::uint64_t> ids;
        ids.reserve(sessions_.size());
        for (const auto& [id, entry] : sessions_) {
            (void)entry;
            ids.push_back(id);
        }
        for (const auto id : ids) {
            auto it = sessions_.find(id);
            if (it == sessions_.end()) continue;
            SessionEntry& entry = *it->second;
            if (entry.closing) continue;

            // Cancel any pending job/connect/wait immediately when waiting
            if (entry.connect.active || entry.upstream_wait.active || entry.helper_wait.active) {
                cancel_session_work(entry);
            }

            if (!entry.stop_delivered || updated) {
                entry.stop_delivered = true;
                const auto remaining = drain_deadline_ > now
                    ? std::chrono::duration_cast<std::chrono::milliseconds>(drain_deadline_ - now)
                    : std::chrono::milliseconds::zero();
                deliver_event(entry, StopEvent{remaining});
                if (!entry.closing && !entry.session->is_closed() && !entry.current_action.is_close()) {
                    enqueue_ready(entry);
                } else {
                    close_session(entry);
                }
            }
        }
    }

    void check_deadlines(core::SteadyTimePoint now) {
        if (stopping_ && drain_deadline_Initialized_ && now >= drain_deadline_) {
            // Drain deadline expired: force close all remaining sessions
            std::vector<std::uint64_t> ids;
            ids.reserve(sessions_.size());
            for (const auto& [id, _] : sessions_) ids.push_back(id);
            for (const auto id : ids) {
                auto it = sessions_.find(id);
                if (it != sessions_.end()) {
                    record_timeout();
                    close_session(*it->second);
                }
            }
            return;
        }

        std::vector<std::uint64_t> ids;
        ids.reserve(sessions_.size());
        for (const auto& [id, _] : sessions_) ids.push_back(id);

        for (const auto id : ids) {
            auto it = sessions_.find(id);
            if (it == sessions_.end()) continue;
            SessionEntry& entry = *it->second;
            if (entry.closing) continue;

            // Check connect deadline / cancellation
            if (entry.connect.active) {
                if (entry.connect.cancellation && entry.connect.cancellation->is_cancelled()) {
                    WorkEvent ev;
                    ev.id = entry.connect.id;
                    ev.status = WorkStatus::cancelled;
                    finish_connect_attempt(entry);
                    close_upstream_on_owner(entry);
                    entry.pending_event = std::move(ev);
                    enqueue_ready(entry);
                    continue;
                }
                if (now >= entry.connect.deadline) {
                    record_timeout();
                    WorkEvent ev;
                    ev.id = entry.connect.id;
                    ev.status = WorkStatus::timed_out;
                    ev.error = core::make_error(core::ErrorCategory::timed_out, "connect deadline");
                    finish_connect_attempt(entry);
                    close_upstream_on_owner(entry);
                    entry.pending_event = std::move(ev);
                    enqueue_ready(entry);
                    continue;
                }
            }

            // Check upstream wait deadline / cancellation
            if (entry.upstream_wait.active) {
                if (entry.upstream_wait.cancellation && entry.upstream_wait.cancellation->is_cancelled()) {
                    WorkEvent ev;
                    ev.id = entry.upstream_wait.id;
                    ev.status = WorkStatus::cancelled;
                    entry.upstream_wait = UpstreamWaitState{};
                    close_upstream_on_owner(entry);
                    entry.pending_event = std::move(ev);
                    enqueue_ready(entry);
                    continue;
                }
                if (now >= entry.upstream_wait.deadline) {
                    record_timeout();
                    WorkEvent ev;
                    ev.id = entry.upstream_wait.id;
                    ev.status = WorkStatus::timed_out;
                    ev.error = core::make_error(core::ErrorCategory::timed_out, "upstream wait deadline");
                    entry.upstream_wait = UpstreamWaitState{};
                    close_upstream_on_owner(entry);
                    entry.pending_event = std::move(ev);
                    enqueue_ready(entry);
                    continue;
                }
            }

            // Check helper wait deadline / cancellation
            if (entry.helper_wait.active) {
                if (entry.helper_wait.cancellation && entry.helper_wait.cancellation->is_cancelled()) {
                    WorkEvent ev;
                    ev.id = entry.helper_wait.id;
                    ev.status = WorkStatus::cancelled;
                    release_helper_wait(entry);
                    entry.pending_event = std::move(ev);
                    enqueue_ready(entry);
                    continue;
                }
                if (now >= entry.helper_wait.deadline) {
                    record_timeout();
                    if (entry.helper_wait.cancellation) {
                        entry.helper_wait.cancellation->cancel();
                    }
                    WorkEvent ev;
                    ev.id = entry.helper_wait.id;
                    ev.status = WorkStatus::timed_out;
                    ev.error = core::make_error(core::ErrorCategory::timed_out, "work deadline");
                    release_helper_wait(entry);
                    entry.pending_event = std::move(ev);
                    enqueue_ready(entry);
                    continue;
                }
            }

            // Check downstream WaitAction deadline or session idle/progress timeout
            const auto session_deadline = compute_session_deadline(entry);
            if (now >= session_deadline) {
                record_timeout();
                core::OperationId op_id{};
                if (const auto* wait = entry.current_action.as_wait()) {
                    op_id = wait->id;
                }
                deliver_event(entry, TimeoutEvent{op_id});
                if (entry.session->is_closed() || entry.current_action.is_close()) {
                    close_session(entry);
                } else {
                    enqueue_ready(entry);
                }
            }
        }
    }

    bool process_completions() {
        const std::size_t budget = config_.budgets.max_completions_per_turn;
        std::size_t processed = 0;
        bool more = false;

        while (processed < budget) {
            WorkEvent ev;
            {
                std::lock_guard port_lock(completion_port_->mutex);
                if (completion_port_->queue.empty()) {
                    break;
                }
                ev = std::move(completion_port_->queue.front());
                completion_port_->queue.pop_front();
                more = !completion_port_->queue.empty();
            }
            ++processed;

            if (pending_jobs_count_ > 0) {
                --pending_jobs_count_;
                sync_gauges();
            }

            const auto conn = ev.id.transaction.connection;
            auto it = sessions_.find(conn.value);
            if (it == sessions_.end() || it->second->id != conn || it->second->closing) {
                dispose_stale_work_event(ev);
                record_stale_completion();
                continue;
            }
            SessionEntry& entry = *it->second;
            if (!entry.helper_wait.active || entry.helper_wait.id != ev.id) {
                dispose_stale_work_event(ev);
                record_stale_completion();
                continue;
            }

            entry.helper_wait = HelperWaitState{};
            entry.pending_event = std::move(ev);
            enqueue_ready(entry);
        }
        return more;
    }

    bool process_accepts() {
        if (stopping_ || !listener_ready_ || !listener_.is_valid()) {
            return false;
        }
        const std::size_t budget = config_.budgets.max_accepts_per_turn;
        std::size_t accepted_count = 0;

        while (accepted_count < budget && !stopping_) {
            auto accepted = listener_.accept();
            if (!accepted) {
                const auto cat = accepted.error().category;
                if (cat == core::ErrorCategory::would_block) {
                    listener_ready_ = false;
                    return false;
                }
                if (cat == core::ErrorCategory::interrupted || cat == core::ErrorCategory::connection_aborted) {
                    continue;
                }
                listener_ready_ = false;
                return false;
            }
            ++accepted_count;
            net::Socket client = std::move(accepted.value());
            auto nonblocking = client.set_nonblocking(true);
            if (!nonblocking) {
                client.close();
                record_connection_rejected(false);
                continue;
            }

            if (sessions_.size() >= config_.limits.max_sessions) {
                const bool sent_503 = try_send_overload_503(client);
                client.close();
                record_connection_rejected(sent_503);
                continue;
            }

            const core::ConnectionId conn_id{++next_connection_id_, ++next_generation_};
            auto created_session = session_factory_(conn_id);
            if (!created_session || !created_session.value()) {
                client.close();
                record_connection_rejected(false);
                continue;
            }

            auto watched = poller_->watch(client, net::Interest::none, conn_id);
            if (!watched) {
                client.close();
                record_connection_rejected(false);
                continue;
            }

            auto entry = std::make_unique<SessionEntry>();
            entry->id = conn_id;
            entry->session = std::move(created_session.value());
            entry->client_socket = std::move(client);
            entry->client_token = watched.value();
            entry->client_interest = net::Interest::none;
            entry->current_action = entry->session->initial_action();
            entry->action_dispatched = false;

            registrations_[*entry->client_token] = RegistrationRecord{
                *entry->client_token, conn_id, RegistrationRole::downstream, 0, 0, 0
            };

            SessionEntry* raw = entry.get();
            sessions_.emplace(conn_id.value, std::move(entry));
            record_connection_accepted();

            if (raw->session->is_closed() || raw->current_action.is_close()) {
                close_session(*raw);
            } else {
                enqueue_ready(*raw);
            }
        }

        // If we hit the accept budget, listener might still have pending sockets.
        if (accepted_count >= budget) {
            record_budget_requeue();
            return true;
        }
        return false;
    }

    bool process_runnable_sessions() {
        const std::size_t session_budget = config_.budgets.max_sessions_per_turn;
        std::size_t advanced = 0;

        while (advanced < session_budget && !ready_queue_.empty()) {
            const core::ConnectionId conn_id = ready_queue_.front();
            ready_queue_.pop_front();

            auto it = sessions_.find(conn_id.value);
            if (it == sessions_.end() || it->second->id != conn_id) {
                continue;
            }
            SessionEntry& entry = *it->second;
            entry.queued = false;
            if (entry.closing) {
                close_session(entry);
                continue;
            }

            ++advanced;
            advance_session_turn(entry);
        }

        sync_gauges();
        return !ready_queue_.empty();
    }

    void advance_session_turn(SessionEntry& entry) {
        const std::size_t event_budget = config_.budgets.max_events_per_session_turn;
        const std::size_t byte_budget = config_.budgets.max_bytes_per_session_turn;
        std::size_t events_used = 0;
        std::size_t bytes_used = 0;

        while (!entry.closing && events_used < event_budget && bytes_used < byte_budget) {
            if (entry.session->is_closed() || entry.current_action.is_close()) {
                close_session(entry);
                return;
            }

            // First deliver any queued completion/timeout event
            if (entry.pending_event.has_value()) {
                Event ev = std::move(*entry.pending_event);
                entry.pending_event.reset();
                ++events_used;
                deliver_event(entry, ev);
                continue;
            }

            // If an async connect is active, check if it has readiness
            if (entry.connect.active) {
                if (!entry.upstream_ready_write && !entry.upstream_ready_read) {
                    break;
                }
                entry.upstream_ready_write = false;
                entry.upstream_ready_read = false;
                ++events_used;
                step_connect_readiness(entry);
                continue;
            }

            // If an upstream wait is active, check if it has matching readiness
            if (entry.upstream_wait.active) {
                const bool wants_read = (static_cast<unsigned>(entry.upstream_wait.interest) & static_cast<unsigned>(net::Interest::read)) != 0;
                const bool wants_write = (static_cast<unsigned>(entry.upstream_wait.interest) & static_cast<unsigned>(net::Interest::write)) != 0;
                const bool ready = (wants_read && entry.upstream_ready_read) || (wants_write && entry.upstream_ready_write);
                if (!ready) {
                    break;
                }
                if (wants_read) entry.upstream_ready_read = false;
                if (wants_write) entry.upstream_ready_write = false;
                WorkEvent ev;
                ev.id = entry.upstream_wait.id;
                ev.status = WorkStatus::complete;
                entry.upstream_wait = UpstreamWaitState{};
                ++events_used;
                deliver_event(entry, ev);
                continue;
            }

            // If waiting for a helper completion, nothing to do until it arrives or times out
            if (entry.helper_wait.active) {
                break;
            }

            // Inspect current_action
            if (const auto* wait = entry.current_action.as_wait()) {
                entry.action_dispatched = true;
                const bool wants_read = (static_cast<unsigned>(wait->interest) & static_cast<unsigned>(net::Interest::read)) != 0;
                const bool wants_write = (static_cast<unsigned>(wait->interest) & static_cast<unsigned>(net::Interest::write)) != 0;
                const bool ready = (wants_read && entry.downstream_ready_read) || (wants_write && entry.downstream_ready_write);
                if (!ready) {
                    break;
                }
                const bool r = entry.downstream_ready_read;
                const bool w = entry.downstream_ready_write;
                if (wants_read) entry.downstream_ready_read = false;
                if (wants_write) entry.downstream_ready_write = false;
                ++events_used;
                deliver_event(entry, ReadyEvent{r, w, wait->id});
                continue;
            }

            if (const auto* read = entry.current_action.as_read()) {
                const std::size_t max_bytes = std::min(read->max_bytes, byte_budget - bytes_used);
                if (max_bytes == 0) {
                    break;
                }
                entry.action_dispatched = true;
                entry.downstream_ready_read = false;
                if (read_buffer_.size() < max_bytes) {
                    read_buffer_.resize(max_bytes);
                }
                const auto res = entry.client_socket.read_some(std::span(read_buffer_.data(), max_bytes));
                ++events_used;
                if (res.is_ok()) {
                    const auto count = res.bytes_transferred;
                    if (count == 0 || count > max_bytes) {
                        deliver_event(entry, ErrorEvent{
                            core::make_error(core::ErrorCategory::io_error, "downstream read", 0, "Invalid byte count"),
                            read->id
                        });
                    } else {
                        bytes_used += count;
                        record_bytes_read(count);
                        // If full max_bytes were read, there might still be readable bytes on the socket
                        if (count == max_bytes) {
                            entry.downstream_ready_read = true;
                        }
                        deliver_event(entry, ReadDataEvent{std::span(read_buffer_.data(), count), read->id});
                    }
                } else if (res.is_eof()) {
                    deliver_event(entry, ReadEofEvent{read->id});
                } else if (res.would_block()) {
                    deliver_event(entry, WouldBlockEvent{net::Interest::read, read->id});
                } else {
                    deliver_event(entry, ErrorEvent{res.error, read->id});
                }
                continue;
            }

            if (const auto* write = entry.current_action.as_write()) {
                const auto data = write->data;
                if (data.empty()) {
                    entry.action_dispatched = true;
                    entry.downstream_ready_write = false;
                    ++events_used;
                    deliver_event(entry, WriteCompleteEvent{0, write->id});
                    continue;
                }
                const std::size_t max_bytes = std::min(data.size(), byte_budget - bytes_used);
                if (max_bytes == 0) {
                    break;
                }
                entry.action_dispatched = true;
                entry.downstream_ready_write = false;
                const auto res = entry.client_socket.write_some(data.subspan(0, max_bytes));
                ++events_used;
                if (res.is_ok()) {
                    const auto count = res.bytes_transferred;
                    if (count == 0 || count > max_bytes) {
                        deliver_event(entry, ErrorEvent{
                            core::make_error(core::ErrorCategory::io_error, "downstream write", 0, "Invalid byte count"),
                            write->id
                        });
                    } else {
                        bytes_used += count;
                        record_bytes_written(count);
                        if (count == max_bytes && count < data.size()) {
                            entry.downstream_ready_write = true;
                        }
                        deliver_event(entry, WriteCompleteEvent{count, write->id});
                    }
                } else if (res.would_block()) {
                    deliver_event(entry, WouldBlockEvent{net::Interest::write, write->id});
                } else {
                    deliver_event(entry, ErrorEvent{res.error, write->id});
                }
                continue;
            }

            if (const auto* work = entry.current_action.as_work()) {
                if (entry.action_dispatched) {
                    // Prevent submitting the same outstanding action twice
                    break;
                }
                entry.action_dispatched = true;
                ++events_used;
                execute_work_action(entry, *work, bytes_used, byte_budget);
                continue;
            }

            // Action::is_none()
            break;
        }

        if (entry.closing || entry.session->is_closed() || entry.current_action.is_close()) {
            close_session(entry);
            return;
        }

        // Check if session still has immediate runnable work that was paused by per-turn budgets
        if (can_session_progress_immediately(entry)) {
            record_budget_requeue();
            enqueue_ready(entry);
        }
    }

    bool can_session_progress_immediately(const SessionEntry& entry) const noexcept {
        if (entry.closing || entry.session->is_closed() || entry.current_action.is_close()) {
            return true;
        }
        if (entry.pending_event.has_value()) {
            return true;
        }
        if (entry.connect.active) {
            return entry.upstream_ready_write || entry.upstream_ready_read;
        }
        if (entry.upstream_wait.active) {
            const bool wants_read = (static_cast<unsigned>(entry.upstream_wait.interest) & static_cast<unsigned>(net::Interest::read)) != 0;
            const bool wants_write = (static_cast<unsigned>(entry.upstream_wait.interest) & static_cast<unsigned>(net::Interest::write)) != 0;
            return (wants_read && entry.upstream_ready_read) || (wants_write && entry.upstream_ready_write);
        }
        if (entry.helper_wait.active) {
            return false;
        }
        if (const auto* wait = entry.current_action.as_wait()) {
            const bool wants_read = (static_cast<unsigned>(wait->interest) & static_cast<unsigned>(net::Interest::read)) != 0;
            const bool wants_write = (static_cast<unsigned>(wait->interest) & static_cast<unsigned>(net::Interest::write)) != 0;
            return (wants_read && entry.downstream_ready_read) || (wants_write && entry.downstream_ready_write);
        }
        if (entry.current_action.is_read()) {
            return !entry.action_dispatched || entry.downstream_ready_read;
        }
        if (entry.current_action.is_write()) {
            // A freshly prepared write segment (!action_dispatched) or a partial write
            // that capped at the per-turn byte budget (downstream_ready_write) can progress.
            return !entry.action_dispatched || entry.downstream_ready_write;
        }
        if (entry.current_action.as_work()) {
            return !entry.action_dispatched;
        }
        return false;
    }

    void execute_work_action(SessionEntry& entry, const WorkAction& work, std::size_t& bytes_used, std::size_t byte_budget) {
        WorkEvent result;
        result.id = work.id;
        const auto invalid = [] { return core::make_error(core::ErrorCategory::invalid_argument, "work request"); };

        const auto& limits = config_.limits.work;
        if (!valid_work_limits(limits)) {
            result.status = WorkStatus::rejected;
            result.error = invalid();
            deliver_event(entry, result);
            return;
        }
        if (work.id.transaction.sequence == 0 || work.id.sequence == 0 ||
            work.id.transaction.connection != entry.id ||
            work.id.endpoint != endpoint_role(work.request)) {
            result.status = WorkStatus::failed;
            result.error = invalid();
            deliver_event(entry, result);
            return;
        }

        const auto now = config_.clock->now();
        if (stopping_ || (work.cancellation && work.cancellation->is_cancelled())) {
            result.status = WorkStatus::cancelled;
            if (work.id.endpoint == core::EndpointRole::upstream) {
                close_upstream_on_owner(entry);
            }
            deliver_event(entry, result);
            return;
        }
        if (now >= work.deadline) {
            record_timeout();
            result.status = WorkStatus::timed_out;
            result.error = core::make_error(core::ErrorCategory::timed_out, "work deadline");
            if (work.id.endpoint == core::EndpointRole::upstream) {
                close_upstream_on_owner(entry);
            }
            deliver_event(entry, result);
            return;
        }

        // Dispatch file and resolver jobs to helper service
        if (work.id.endpoint == core::EndpointRole::file || work.id.endpoint == core::EndpointRole::resolver) {
            submit_helper_work(entry, work);
            return;
        }

        // Upstream socket work on the loop thread
        if (entry.upstream_owner.sequence != 0 &&
            (entry.upstream_owner.connection != work.id.transaction.connection ||
             work.id.transaction.sequence < entry.upstream_owner.sequence ||
             (work.id.transaction == entry.upstream_owner && work.id.sequence <= entry.upstream_last_op))) {
            result.status = WorkStatus::cancelled;
            deliver_event(entry, result);
            return;
        }
        if (entry.upstream_owner != work.id.transaction) {
            close_upstream_on_owner(entry);
            entry.upstream_owner = work.id.transaction;
        }
        entry.upstream_last_op = work.id.sequence;

        std::visit([&](const auto& req) {
            using T = std::decay_t<decltype(req)>;
            if constexpr (std::is_same_v<T, ConnectWork>) {
                start_connect_work(entry, work, req);
            } else if constexpr (std::is_same_v<T, UpstreamReadWork>) {
                entry.upstream_ready_read = false;
                if (req.max_bytes == 0 || req.max_bytes > limits.max_buffer_bytes) {
                    result.status = WorkStatus::rejected;
                    result.error = invalid();
                    deliver_event(entry, result);
                    return;
                }
                if (!entry.upstream_socket.is_valid()) {
                    result.status = WorkStatus::failed;
                    result.error = core::make_error(core::ErrorCategory::not_connected, "upstream read");
                    deliver_event(entry, result);
                    return;
                }
                const std::size_t cap = std::min(req.max_bytes, std::max<std::size_t>(1, byte_budget - bytes_used));
                auto buf = std::make_shared<Bytes>(cap);
                const auto read_res = entry.upstream_socket.read_some(*buf);
                if (read_res.is_ok() && read_res.bytes_transferred > 0 && read_res.bytes_transferred <= buf->size()) {
                    buf->resize(read_res.bytes_transferred);
                    bytes_used += read_res.bytes_transferred;
                    record_bytes_read(read_res.bytes_transferred);
                    if (read_res.bytes_transferred == cap) {
                        entry.upstream_ready_read = true;
                    }
                    result.bytes = std::move(buf);
                    result.transferred = read_res.bytes_transferred;
                    result.status = WorkStatus::progress;
                } else if (read_res.is_eof()) {
                    result.status = WorkStatus::eof;
                } else if (read_res.would_block()) {
                    result.status = WorkStatus::would_block;
                } else {
                    result.status = WorkStatus::failed;
                    result.error = read_res.is_ok() ? invalid() : read_res.error;
                }
                deliver_event(entry, result);
            } else if constexpr (std::is_same_v<T, UpstreamWriteWork>) {
                entry.upstream_ready_write = false;
                if (!req.bytes || req.count == 0 || req.count > limits.max_buffer_bytes ||
                    req.offset > req.bytes->size() || req.count > req.bytes->size() - req.offset) {
                    result.status = WorkStatus::rejected;
                    result.error = invalid();
                    deliver_event(entry, result);
                    return;
                }
                if (!entry.upstream_socket.is_valid()) {
                    result.status = WorkStatus::failed;
                    result.error = core::make_error(core::ErrorCategory::not_connected, "upstream write");
                    deliver_event(entry, result);
                    return;
                }
                const std::size_t cap = std::min(req.count, std::max<std::size_t>(1, byte_budget - bytes_used));
                const auto write_res = entry.upstream_socket.write_some(
                    std::span(*req.bytes).subspan(req.offset, cap)
                );
                if (write_res.is_ok() && write_res.bytes_transferred > 0 && write_res.bytes_transferred <= cap) {
                    bytes_used += write_res.bytes_transferred;
                    record_bytes_written(write_res.bytes_transferred);
                    if (write_res.bytes_transferred == cap && cap < req.count) {
                        entry.upstream_ready_write = true;
                    }
                    result.transferred = write_res.bytes_transferred;
                    result.status = WorkStatus::progress;
                } else if (write_res.would_block()) {
                    result.status = WorkStatus::would_block;
                } else {
                    result.status = WorkStatus::failed;
                    result.error = write_res.is_ok() ? invalid() : write_res.error;
                }
                deliver_event(entry, result);
            } else if constexpr (std::is_same_v<T, UpstreamWaitWork>) {
                if (req.interest != net::Interest::read && req.interest != net::Interest::write) {
                    result.status = WorkStatus::failed;
                    result.error = invalid();
                    deliver_event(entry, result);
                    return;
                }
                if (!entry.upstream_socket.is_valid() || !entry.upstream_token) {
                    result.status = WorkStatus::failed;
                    result.error = core::make_error(core::ErrorCategory::not_connected, "upstream wait");
                    deliver_event(entry, result);
                    return;
                }
                const bool wants_read = req.interest == net::Interest::read;
                const bool wants_write = req.interest == net::Interest::write;
                if ((wants_read && entry.upstream_ready_read) || (wants_write && entry.upstream_ready_write)) {
                    if (wants_read) entry.upstream_ready_read = false;
                    if (wants_write) entry.upstream_ready_write = false;
                    result.status = WorkStatus::complete;
                    deliver_event(entry, result);
                    return;
                }
                entry.upstream_wait.active = true;
                entry.upstream_wait.id = work.id;
                entry.upstream_wait.interest = req.interest;
                entry.upstream_wait.deadline = work.deadline;
                entry.upstream_wait.cancellation = work.cancellation;
            } else if constexpr (std::is_same_v<T, UpstreamCloseWork>) {
                close_upstream_on_owner(entry);
                result.status = WorkStatus::complete;
                deliver_event(entry, result);
            }
        }, work.request);
    }

    void start_connect_work(SessionEntry& entry, const WorkAction& work, const ConnectWork& req) {
        close_upstream_on_owner(entry);

        WorkEvent result;
        result.id = work.id;
        if (req.candidates.empty() || req.candidates.size() > config_.limits.work.max_candidates) {
            result.status = WorkStatus::rejected;
            result.error = core::make_error(core::ErrorCategory::invalid_argument, "connect candidates");
            deliver_event(entry, result);
            return;
        }
        if (active_upstreams_count_ >= config_.limits.max_upstreams ||
            active_connecting_count_ >= config_.limits.max_connecting) {
            result.status = WorkStatus::rejected;
            result.error = core::make_error(core::ErrorCategory::would_block, "upstream admission limit");
            deliver_event(entry, result);
            return;
        }

        entry.connect.active = true;
        entry.connect.id = work.id;
        entry.connect.deadline = work.deadline;
        entry.connect.cancellation = work.cancellation;
        entry.connect.candidates = req.candidates;
        entry.connect.next_index = 0;
        entry.connect.last_error = {};

        ++active_connecting_count_;
        sync_gauges();

        try_next_connect_candidate(entry);
    }

    void try_next_connect_candidate(SessionEntry& entry) {
        const auto now = config_.clock->now();
        while (entry.connect.active && entry.connect.next_index < entry.connect.candidates.size()) {
            if (stopping_ || (entry.connect.cancellation && entry.connect.cancellation->is_cancelled())) {
                WorkEvent ev;
                ev.id = entry.connect.id;
                ev.status = WorkStatus::cancelled;
                finish_connect_attempt(entry);
                close_upstream_on_owner(entry);
                deliver_event(entry, ev);
                return;
            }
            if (now >= entry.connect.deadline) {
                record_timeout();
                WorkEvent ev;
                ev.id = entry.connect.id;
                ev.status = WorkStatus::timed_out;
                ev.error = core::make_error(core::ErrorCategory::timed_out, "connect deadline");
                finish_connect_attempt(entry);
                close_upstream_on_owner(entry);
                deliver_event(entry, ev);
                return;
            }

            close_upstream_on_owner(entry);
            if (active_upstreams_count_ >= config_.limits.max_upstreams) {
                WorkEvent ev;
                ev.id = entry.connect.id;
                ev.status = WorkStatus::rejected;
                ev.error = core::make_error(core::ErrorCategory::would_block, "upstream limit");
                finish_connect_attempt(entry);
                deliver_event(entry, ev);
                return;
            }

            const net::Endpoint& candidate = entry.connect.candidates[entry.connect.next_index++];
            auto created = net::create_tcp_socket(candidate.family());
            if (!created) {
                entry.connect.last_error = created.error();
                continue;
            }
            net::Socket sock = std::move(created.value());
            auto begun = sock.begin_connect(candidate);
            if (!begun) {
                entry.connect.last_error = begun.error();
                sock.close();
                continue;
            }

            ++entry.upstream_attempt;
            auto watched = poller_->watch(sock, net::Interest::none, entry.id);
            if (!watched) {
                entry.connect.last_error = watched.error();
                sock.close();
                continue;
            }

            entry.upstream_socket = std::move(sock);
            entry.upstream_token = watched.value();
            entry.upstream_interest = net::Interest::none;
            entry.upstream_ready_read = false;
            entry.upstream_ready_write = false;
            entry.upstream_counted = true;
            ++active_upstreams_count_;
            sync_gauges();

            registrations_[*entry.upstream_token] = RegistrationRecord{
                *entry.upstream_token,
                entry.id,
                RegistrationRole::upstream,
                entry.upstream_owner.sequence,
                entry.connect.id.sequence,
                entry.upstream_attempt
            };

            if (begun.value() == net::ConnectState::connected) {
                // Verify pending socket error before declaring success
                auto finished = entry.upstream_socket.finish_connect();
                if (finished) {
                    WorkEvent ev;
                    ev.id = entry.connect.id;
                    ev.status = WorkStatus::complete;
                    finish_connect_attempt(entry);
                    deliver_event(entry, ev);
                    return;
                }
                if (finished.error().category != core::ErrorCategory::would_block) {
                    entry.connect.last_error = finished.error();
                    close_upstream_on_owner(entry);
                    continue;
                }
            }

            // Connection attempt is in progress; wait for writable readiness
            return;
        }

        // All candidates exhausted
        if (entry.connect.active) {
            WorkEvent ev;
            ev.id = entry.connect.id;
            ev.status = WorkStatus::failed;
            ev.error = entry.connect.last_error
                ? entry.connect.last_error
                : core::make_error(core::ErrorCategory::connection_refused, "connect candidates failed");
            finish_connect_attempt(entry);
            close_upstream_on_owner(entry);
            deliver_event(entry, ev);
        }
    }

    void step_connect_readiness(SessionEntry& entry) {
        if (!entry.connect.active || !entry.upstream_socket.is_valid()) {
            return;
        }
        auto finished = entry.upstream_socket.finish_connect();
        if (finished) {
            WorkEvent ev;
            ev.id = entry.connect.id;
            ev.status = WorkStatus::complete;
            finish_connect_attempt(entry);
            deliver_event(entry, ev);
            return;
        }
        if (finished.error().category == core::ErrorCategory::would_block) {
            return;
        }
        entry.connect.last_error = finished.error();
        close_upstream_on_owner(entry);
        try_next_connect_candidate(entry);
    }

    void finish_connect_attempt(SessionEntry& entry) noexcept {
        if (entry.connect.active) {
            entry.connect = ConnectState{};
            if (active_connecting_count_ > 0) {
                --active_connecting_count_;
            }
            sync_gauges();
        }
    }

    void submit_helper_work(SessionEntry& entry, const WorkAction& work) {
        WorkEvent rejected;
        rejected.id = work.id;
        rejected.status = WorkStatus::rejected;

        // Pre-validate request bounds before helper submission
        const auto& limits = config_.limits.work;
        bool valid_req = true;
        std::visit([&](const auto& req) {
            using T = std::decay_t<decltype(req)>;
            if constexpr (std::is_same_v<T, ResolveWork>) {
                if (req.target.host.empty() || req.target.host.size() > limits.max_path_bytes) valid_req = false;
            } else if constexpr (std::is_same_v<T, FileInfoWork> || std::is_same_v<T, FileOpenWork>) {
                const auto text = req.path.native();
                if (text.empty() || text.size() > limits.max_path_bytes ||
                    text.find(typename std::filesystem::path::value_type{}) != text.npos) {
                    valid_req = false;
                }
            } else if constexpr (std::is_same_v<T, FileReadWork>) {
                if (!req.file || req.max_bytes == 0 || req.max_bytes > limits.max_buffer_bytes ||
                    req.offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
                    valid_req = false;
                }
            }
        }, work.request);

        if (!valid_req) {
            rejected.error = core::make_error(core::ErrorCategory::invalid_argument, "work request bounds");
            deliver_event(entry, rejected);
            return;
        }

        if (!helper_executor_) {
            rejected.error = core::make_error(core::ErrorCategory::invalid_argument, "helper executor unavailable");
            deliver_event(entry, rejected);
            return;
        }

        {
            std::lock_guard port_lock(completion_port_->mutex);
            if (pending_jobs_count_ >= config_.limits.max_pending_jobs ||
                completion_port_->queue.size() + pending_jobs_count_ >= completion_port_->max_queue) {
                rejected.error = core::make_error(core::ErrorCategory::would_block, "helper queue full");
                deliver_event(entry, rejected);
                return;
            }
        }

        // Register helper_wait before calling submit() so even an inline test double
        // callback finds the active wait state and queues cleanly.
        entry.helper_wait.active = true;
        entry.helper_wait.id = work.id;
        entry.helper_wait.deadline = work.deadline;
        entry.helper_wait.cancellation = work.cancellation;
        ++pending_jobs_count_;
        sync_gauges();

        auto port = completion_port_;
        const std::size_t max_candidates = limits.max_candidates;
        auto submitted = helper_executor_->submit(work, [port, max_candidates](WorkEvent completion) {
            if (completion.status == WorkStatus::complete && completion.candidates.size() > max_candidates) {
                completion.candidates.clear();
                completion.status = WorkStatus::rejected;
                completion.error = core::make_error(core::ErrorCategory::invalid_argument, "too many resolved candidates");
            }
            {
                std::lock_guard lock(port->mutex);
                if (port->closed || port->queue.size() >= port->max_queue) {
                    completion.bytes.reset();
                    completion.file = {};
                    completion.candidates.clear();
                    ++port->dropped_stale;
                    return;
                }
                port->queue.push_back(std::move(completion));
                port->peak_queued = std::max<std::uint64_t>(port->peak_queued, port->queue.size());
                // Keep the lifetime lock through wake: cleanup can destroy the
                // poller immediately after clearing this pointer.
                if (port->poller) (void)port->poller->wake();
            }
        });

        if (!submitted) {
            release_helper_wait(entry);
            rejected.error = submitted.error();
            deliver_event(entry, rejected);
        }
    }

    void release_helper_wait(SessionEntry& entry) noexcept {
        if (entry.helper_wait.active) {
            entry.helper_wait = HelperWaitState{};
            if (pending_jobs_count_ > 0) {
                --pending_jobs_count_;
            }
            sync_gauges();
        }
    }

    static void dispose_stale_work_event(WorkEvent& ev) noexcept {
        ev.bytes.reset();
        ev.file = {};
        ev.candidates.clear();
        ev.transferred = 0;
    }

    void deliver_event(SessionEntry& entry, const Event& ev) {
        if (entry.closing || entry.session->is_closed()) {
            entry.closing = true;
            return;
        }
        entry.current_action = entry.session->on_event(ev);
        entry.action_dispatched = false;
        update_session_peaks(*entry.session);
        if (entry.session->is_closed() || entry.current_action.is_close()) {
            entry.closing = true;
        }
    }

    void enqueue_ready(SessionEntry& entry) {
        if (entry.queued || entry.closing) {
            return;
        }
        entry.queued = true;
        ready_queue_.push_back(entry.id);
        sync_gauges();
    }

    void update_listener_interest() {
        if (!poller_ || !listener_token_ || !listener_.is_valid()) {
            return;
        }
        net::Interest desired = net::Interest::read;
        // Continue bounded accepts at capacity so new clients receive the
        // configured overload close/503 instead of waiting for an idle session.
        // process_accepts' per-turn budget preserves timer/session fairness.
        if (stopping_) {
            desired = net::Interest::none;
        }
        if (desired != listener_interest_) {
            if (poller_->modify(*listener_token_, desired)) {
                listener_interest_ = desired;
            }
        }
    }

    void update_session_interests(SessionEntry& entry) {
        if (entry.closing || !poller_) {
            return;
        }

        // Downstream interest
        net::Interest down_desired = net::Interest::none;
        if (!entry.connect.active && !entry.upstream_wait.active && !entry.helper_wait.active) {
            if (const auto* wait = entry.current_action.as_wait()) {
                down_desired = wait->interest;
            } else if (entry.current_action.is_read()) {
                down_desired = net::Interest::read;
            } else if (entry.current_action.is_write()) {
                down_desired = net::Interest::write;
            }
        }
        if (entry.client_token && down_desired != entry.client_interest) {
            if (poller_->modify(*entry.client_token, down_desired)) {
                entry.client_interest = down_desired;
            }
        }

        // Upstream interest: watch write ONLY while connect is in progress or upstream wait requests write
        net::Interest up_desired = net::Interest::none;
        if (entry.connect.active) {
            up_desired = net::Interest::write;
        } else if (entry.upstream_wait.active) {
            up_desired = entry.upstream_wait.interest;
        }
        if (entry.upstream_token && up_desired != entry.upstream_interest) {
            if (poller_->modify(*entry.upstream_token, up_desired)) {
                entry.upstream_interest = up_desired;
            }
        }
    }

    core::SteadyTimePoint compute_session_deadline(const SessionEntry& entry) const noexcept {
        auto deadline = add_duration(entry.session->last_progress_time(), entry.session->idle_timeout());
        if (const auto* wait = entry.current_action.as_wait()) {
            if (wait->deadline != core::SteadyTimePoint{}) {
                deadline = std::min(deadline, wait->deadline);
            }
        }
        if (stopping_ && drain_deadline_Initialized_) {
            deadline = std::min(deadline, drain_deadline_);
        }
        return deadline;
    }

    core::SteadyTimePoint compute_nearest_deadline(core::SteadyTimePoint now) const noexcept {
        core::SteadyTimePoint nearest = add_duration(now, std::chrono::milliseconds(60000));
        if (stopping_ && drain_deadline_Initialized_) {
            nearest = std::min(nearest, drain_deadline_);
        }
        for (const auto& [id, entry_ptr] : sessions_) {
            (void)id;
            const SessionEntry& entry = *entry_ptr;
            if (entry.closing) continue;
            nearest = std::min(nearest, compute_session_deadline(entry));
            if (entry.connect.active) {
                nearest = std::min(nearest, entry.connect.deadline);
            }
            if (entry.upstream_wait.active) {
                nearest = std::min(nearest, entry.upstream_wait.deadline);
            }
            if (entry.helper_wait.active) {
                nearest = std::min(nearest, entry.helper_wait.deadline);
            }
        }
        return nearest;
    }

    void cancel_session_work(SessionEntry& entry) noexcept {
        if (entry.connect.cancellation) entry.connect.cancellation->cancel();
        if (entry.upstream_wait.cancellation) entry.upstream_wait.cancellation->cancel();
        if (entry.helper_wait.cancellation) entry.helper_wait.cancellation->cancel();
        if (const auto* work = entry.current_action.as_work()) {
            if (work->cancellation) work->cancellation->cancel();
        }
        finish_connect_attempt(entry);
        entry.upstream_wait = UpstreamWaitState{};
        release_helper_wait(entry);
    }

    void close_upstream_on_owner(SessionEntry& entry) noexcept {
        if (entry.upstream_token) {
            registrations_.erase(*entry.upstream_token);
            if (poller_) {
                (void)poller_->unwatch(*entry.upstream_token);
            }
            entry.upstream_token.reset();
        }
        entry.upstream_interest = net::Interest::none;
        entry.upstream_ready_read = false;
        entry.upstream_ready_write = false;
        entry.upstream_socket.close();
        if (entry.upstream_counted) {
            entry.upstream_counted = false;
            if (active_upstreams_count_ > 0) {
                --active_upstreams_count_;
            }
            sync_gauges();
        }
    }

    void close_session(SessionEntry& entry) noexcept {
        const std::uint64_t key = entry.id.value;
        entry.closing = true;
        update_session_peaks(*entry.session);
        cancel_session_work(entry);
        close_upstream_on_owner(entry);

        if (entry.client_token) {
            registrations_.erase(*entry.client_token);
            if (poller_) {
                (void)poller_->unwatch(*entry.client_token);
            }
            entry.client_token.reset();
        }
        entry.client_interest = net::Interest::none;
        entry.client_socket.close();
        entry.session.reset();

        sessions_.erase(key);
        sync_gauges();
    }

    void close_listener_on_owner() noexcept {
        if (listener_token_) {
            registrations_.erase(*listener_token_);
            if (poller_) {
                (void)poller_->unwatch(*listener_token_);
            }
            listener_token_.reset();
        }
        listener_interest_ = net::Interest::none;
        listener_ready_ = false;
        listener_.close();
    }

    void cleanup_all_on_owner() noexcept {
        if (helper_destination_) helper_destination_->close();
        {
            std::lock_guard port_lock(completion_port_->mutex);
            completion_port_->closed = true;
            completion_port_->poller = nullptr;
        }

        std::vector<std::uint64_t> ids;
        ids.reserve(sessions_.size());
        for (const auto& [id, _] : sessions_) ids.push_back(id);
        for (const auto id : ids) {
            auto it = sessions_.find(id);
            if (it != sessions_.end()) {
                close_session(*it->second);
            }
        }
        sessions_.clear();
        ready_queue_.clear();
        close_listener_on_owner();
        registrations_.clear();

        {
            std::lock_guard port_lock(completion_port_->mutex);
            for (auto& ev : completion_port_->queue) {
                dispose_stale_work_event(ev);
            }
            completion_port_->queue.clear();
        }

        pending_jobs_count_ = 0;
        active_upstreams_count_ = 0;
        active_connecting_count_ = 0;
        sync_gauges();
        poller_.reset();
    }

    bool try_send_overload_503(net::Socket& client) noexcept {
        if (!config_.send_overload_503) {
            return false;
        }
        const auto bytes = std::as_bytes(std::span(kOverload503Response.data(), kOverload503Response.size()));
        const auto wrote = client.write_some(bytes);
        if (wrote.is_ok() && wrote.bytes_transferred == bytes.size()) {
            config_.metrics->record(core::Metric::wire_bytes, wrote.bytes_transferred);
            return true;
        }
        return false;
    }

    void update_session_peaks(const ConnectionSession& session) noexcept {
        std::lock_guard lock(stats_mutex_);
        stats_.peak_session_input_bytes = std::max<std::uint64_t>(
            stats_.peak_session_input_bytes, session.peak_input_bytes()
        );
        stats_.peak_session_output_bytes = std::max<std::uint64_t>(
            stats_.peak_session_output_bytes, session.peak_output_bytes()
        );
    }

    void sync_gauges() noexcept {
        std::lock_guard lock(stats_mutex_);
        stats_.active_sessions = sessions_.size();
        stats_.peak_active_sessions = std::max<std::uint64_t>(stats_.peak_active_sessions, sessions_.size());
        stats_.active_upstreams = active_upstreams_count_;
        stats_.peak_active_upstreams = std::max<std::uint64_t>(stats_.peak_active_upstreams, active_upstreams_count_);
        stats_.active_connecting = active_connecting_count_;
        stats_.peak_active_connecting = std::max<std::uint64_t>(stats_.peak_active_connecting, active_connecting_count_);
        stats_.pending_jobs = pending_jobs_count_;
        stats_.peak_pending_jobs = std::max<std::uint64_t>(stats_.peak_pending_jobs, pending_jobs_count_);
        stats_.runnable_sessions = ready_queue_.size();
        stats_.peak_runnable_sessions = std::max<std::uint64_t>(stats_.peak_runnable_sessions, ready_queue_.size());
    }

    void record_connection_accepted() noexcept {
        {
            std::lock_guard lock(stats_mutex_);
            ++stats_.connections_accepted;
        }
        sync_gauges();
    }

    void record_connection_rejected(bool sent_503) noexcept {
        std::lock_guard lock(stats_mutex_);
        ++stats_.connections_rejected;
        if (sent_503) {
            ++stats_.overload_responses_sent;
        }
    }

    void record_stale_event() noexcept {
        std::lock_guard lock(stats_mutex_);
        ++stats_.stale_events_dropped;
    }

    void record_stale_completion() noexcept {
        std::lock_guard lock(stats_mutex_);
        ++stats_.stale_completions_dropped;
    }

    void record_timeout() noexcept {
        std::lock_guard lock(stats_mutex_);
        ++stats_.timeouts_fired;
    }

    void record_budget_requeue() noexcept {
        std::lock_guard lock(stats_mutex_);
        ++stats_.budget_requeues;
    }

    void record_bytes_read(std::size_t count) noexcept {
        std::lock_guard lock(stats_mutex_);
        stats_.total_bytes_read += count;
    }

    void record_bytes_written(std::size_t count) noexcept {
        std::lock_guard lock(stats_mutex_);
        stats_.total_bytes_written += count;
    }

    EventLoopConfig config_;
    SessionFactory session_factory_;
    std::shared_ptr<WorkExecutor> helper_executor_;
    std::shared_ptr<helpers::HelperPool> helper_pool_;
    std::shared_ptr<helpers::CompletionPort> helper_destination_;
    std::shared_ptr<CompletionPortState> completion_port_;

    net::Socket listener_;
    std::unique_ptr<net::Poller> poller_;
    std::optional<net::RegistrationToken> listener_token_;
    net::Interest listener_interest_{net::Interest::none};
    bool listener_ready_{false};

    std::unordered_map<net::RegistrationToken, RegistrationRecord, TokenHash> registrations_;
    std::unordered_map<std::uint64_t, std::unique_ptr<SessionEntry>> sessions_;
    std::deque<core::ConnectionId> ready_queue_;
    std::vector<std::byte> read_buffer_;

    std::uint64_t next_connection_id_{0};
    std::uint64_t next_generation_{0};
    std::size_t active_upstreams_count_{0};
    std::size_t active_connecting_count_{0};
    std::size_t pending_jobs_count_{0};

    std::mutex lifecycle_mutex_;
    std::condition_variable cv_;
    std::thread thread_;
    bool driving_{false};
    std::atomic<bool> running_{false};

    std::mutex stop_mutex_;
    std::atomic<bool> stop_requested_{false};
    std::chrono::milliseconds requested_drain_timeout_{0};
    bool stop_updated_{false};
    bool stopping_{false};
    bool drain_deadline_Initialized_{false};
    core::SteadyTimePoint drain_deadline_{};

    mutable std::mutex stats_mutex_;
    net::Endpoint actual_endpoint_{};
    EventLoopStats stats_{};
};

EventLoopDriver::EventLoopDriver(
    EventLoopConfig config,
    SessionFactory session_factory,
    std::shared_ptr<WorkExecutor> helper_executor,
    std::shared_ptr<helpers::HelperPool> helper_pool
)
    : impl_(std::make_unique<Impl>(
          std::move(config),
          std::move(session_factory),
          std::move(helper_executor), std::move(helper_pool)
      )) {}

EventLoopDriver::~EventLoopDriver() = default;

core::Result<void> EventLoopDriver::start() {
    return impl_->start();
}

void EventLoopDriver::request_stop(std::chrono::milliseconds drain_timeout) noexcept {
    impl_->request_stop(drain_timeout);
}

void EventLoopDriver::join() noexcept {
    impl_->join();
}

core::Result<void> EventLoopDriver::bind_listener() {
    return impl_->bind_listener();
}

core::Result<void> EventLoopDriver::run() {
    return impl_->run();
}

core::Result<bool> EventLoopDriver::step_once() {
    return impl_->step_once();
}

bool EventLoopDriver::is_running() const noexcept {
    return impl_->is_running();
}

net::Endpoint EventLoopDriver::local_endpoint() const noexcept {
    return impl_->local_endpoint();
}

EventLoopStats EventLoopDriver::stats() const noexcept {
    return impl_->stats();
}

core::Result<std::unique_ptr<EventLoopDriver>> make_event_loop_driver(
    EventLoopConfig config,
    SessionFactory session_factory,
    std::shared_ptr<WorkExecutor> helper_executor,
    std::shared_ptr<helpers::HelperPool> helper_pool
) {
    auto valid = validate_event_loop_config(config);
    if (!valid) return valid.error();
    if (!session_factory) {
        return invalid_config("SessionFactory must be callable");
    }
    if (helper_executor && helper_pool) return invalid_config("Choose one helper backend");
    return std::make_unique<EventLoopDriver>(
        std::move(config),
        std::move(session_factory),
        std::move(helper_executor), std::move(helper_pool)
    );
}

} // namespace concurrency::event_loop
