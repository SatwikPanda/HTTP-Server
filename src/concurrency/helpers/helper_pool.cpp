#include "concurrency/helpers/helper_pool.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace concurrency::helpers {
namespace {
constexpr std::size_t max_threads = 256;
constexpr std::size_t max_queue = 65536;
constexpr std::size_t max_port = 65536;

class ReferenceAdapter final : public HelperAdapter {
public:
    explicit ReferenceAdapter(WorkLimits limits) : limits_(limits) {}
    WorkEvent execute(const WorkAction& action, const std::function<bool()>& stop) override {
        // The reference executor tracks one connection's upstream; a fresh one
        // per job keeps helpers stateless and holds no socket for file/DNS work.
        ReferenceWorkExecutor executor(limits_);
        return executor.execute(action, stop);
    }
private:
    WorkLimits limits_;
};

WorkEvent terminal(const core::OperationId& id, WorkStatus status, core::Error error) {
    WorkEvent event;
    event.id = id;
    event.status = status;
    event.error = std::move(error);
    return event;
}
} // namespace

bool is_helper_work(const WorkRequest& request) noexcept {
    const auto role = endpoint_role(request);
    return role == core::EndpointRole::file || role == core::EndpointRole::resolver;
}

core::Result<void> validate_config(const HelperConfig& config) {
    const auto& l = config.limits;
    if (config.threads == 0 || config.threads > max_threads || config.queue_capacity == 0 ||
        config.queue_capacity > max_queue || l.max_buffer_bytes == 0 || l.max_buffer_bytes > 16 * 1024 * 1024 ||
        l.max_candidates == 0 || l.max_candidates > 256 || l.max_path_bytes == 0 || l.max_path_bytes > 32768) {
        return core::make_error(core::ErrorCategory::invalid_argument, "helper config");
    }
    return {};
}

std::shared_ptr<HelperAdapter> make_reference_adapter(WorkLimits limits) {
    return std::make_shared<ReferenceAdapter>(limits);
}

core::Error admission_error(Admission admission) {
    switch (admission) {
    case Admission::accepted: return {};
    case Admission::saturated: return core::make_error(core::ErrorCategory::would_block, "helper admission", 0, "saturated");
    case Admission::stopping: return core::make_error(core::ErrorCategory::would_block, "helper admission", 0, "stopping");
    case Admission::invalid: break;
    }
    return core::make_error(core::ErrorCategory::invalid_argument, "helper admission");
}

// ---- CompletionPort -------------------------------------------------------

struct CompletionPort::State {
    struct Entry { WorkEvent event; WorkExecutor::Completion completion; };
    mutable std::mutex mutex;
    std::deque<Entry> ready;
    std::size_t reserved{};
    std::size_t capacity{};
    bool closed{};
    Wake wake;
};

CompletionPort::CompletionPort(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}
CompletionPort::~CompletionPort() = default;

std::shared_ptr<CompletionPort> CompletionPort::create(std::size_t capacity, Wake wake) {
    if (capacity == 0 || capacity > max_port) return nullptr;
    auto state = std::make_unique<State>();
    state->capacity = capacity;
    state->wake = std::move(wake);
    return std::shared_ptr<CompletionPort>(new CompletionPort(std::move(state)));
}

bool CompletionPort::try_reserve() {
    std::lock_guard lock(state_->mutex);
    if (state_->closed || state_->reserved >= state_->capacity) return false;
    ++state_->reserved;
    return true;
}

void CompletionPort::release_reservation() noexcept {
    std::lock_guard lock(state_->mutex);
    if (state_->reserved > 0) --state_->reserved;
}

void CompletionPort::publish(WorkEvent event, WorkExecutor::Completion completion) {
    std::unique_lock lock(state_->mutex);
    if (state_->closed) {
        if (state_->reserved > 0) --state_->reserved;
        lock.unlock();
        return; // payload, file token and completion are released here, off the owner
    }
    state_->ready.push_back({std::move(event), std::move(completion)});
    // Queue before wake, and wake under the lock: close() cannot return while a
    // wake is in flight, so the owner may destroy its poller right after close().
    if (state_->wake) state_->wake();
}

std::size_t CompletionPort::drain(std::size_t max) {
    std::vector<State::Entry> batch;
    {
        std::lock_guard lock(state_->mutex);
        while (batch.size() < max && !state_->ready.empty()) {
            batch.push_back(std::move(state_->ready.front()));
            state_->ready.pop_front();
            --state_->reserved;
        }
    }
    for (auto& entry : batch) {
        if (entry.completion) entry.completion(std::move(entry.event));
    }
    return batch.size();
}

void CompletionPort::close() noexcept {
    std::deque<State::Entry> discarded;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->closed) return;
        state_->closed = true;
        state_->reserved -= state_->ready.size();
        discarded.swap(state_->ready);
    }
}

bool CompletionPort::has_ready() const { std::lock_guard lock(state_->mutex); return !state_->ready.empty(); }
bool CompletionPort::is_closed() const { std::lock_guard lock(state_->mutex); return state_->closed; }
std::size_t CompletionPort::outstanding() const { std::lock_guard lock(state_->mutex); return state_->reserved; }
std::size_t CompletionPort::capacity() const noexcept { return state_->capacity; }

// ---- HelperPool -----------------------------------------------------------

struct HelperPool::State {
    struct Job {
        WorkAction action;
        std::shared_ptr<CompletionPort> port;
        WorkExecutor::Completion completion;
    };
    HelperConfig config;
    std::shared_ptr<HelperAdapter> adapter;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::deque<Job> queue;
    std::size_t running{};
    bool stopping{};
    std::atomic<bool> stop_flag{false};
    std::vector<std::thread> threads;
    std::mutex join_mutex;
    HelperStats counters;
};

HelperPool::HelperPool(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}
HelperPool::~HelperPool() { stop(); }

core::Result<std::shared_ptr<HelperPool>> HelperPool::start(HelperConfig config, std::shared_ptr<HelperAdapter> adapter) {
    if (auto valid = validate_config(config); !valid) return valid.error();
    auto state = std::make_unique<State>();
    state->adapter = adapter ? std::move(adapter) : make_reference_adapter(config.limits);
    state->config = std::move(config);
    std::shared_ptr<HelperPool> pool(new HelperPool(std::move(state)));
    auto& s = *pool->state_;
    s.threads.reserve(s.config.threads);
    for (std::size_t index = 0; index < s.config.threads; ++index) {
        core::Error failure;
        if (s.config.before_thread_start) {
            if (auto allowed = s.config.before_thread_start(index); !allowed) failure = allowed.error();
        }
        if (!failure) {
            try {
                s.threads.emplace_back([raw = pool.get(), index] { raw->run(index); });
            } catch (const std::system_error& error) {
                failure = core::make_error(core::ErrorCategory::system_error, "helper thread start", error.code().value());
            }
        }
        if (failure) {
            pool->stop(); // joins the helpers that did start
            return failure;
        }
    }
    return pool;
}

Admission HelperPool::submit(WorkAction action, const std::shared_ptr<CompletionPort>& destination,
                             WorkExecutor::Completion completion) {
    auto& s = *state_;
    const auto& id = action.id;
    if (!destination || !completion || id.transaction.sequence == 0 || id.sequence == 0 ||
        !is_helper_work(action.request) || id.endpoint != endpoint_role(action.request)) {
        return Admission::invalid;
    }
    std::unique_lock lock(s.mutex);
    if (s.stopping) return Admission::stopping;
    if (s.queue.size() >= s.config.queue_capacity || !destination->try_reserve()) {
        ++s.counters.saturated;
        return Admission::saturated;
    }
    if (!action.cancellation) action.cancellation = std::make_shared<Cancellation>();
    s.queue.push_back({std::move(action), destination, std::move(completion)});
    ++s.counters.accepted;
    lock.unlock();
    s.cv.notify_one();
    return Admission::accepted;
}

void HelperPool::run(std::size_t) {
    auto& s = *state_;
    const std::function<bool()> stop = [&s] { return s.stop_flag.load(std::memory_order_acquire); };
    for (;;) {
        State::Job job;
        {
            std::unique_lock lock(s.mutex);
            s.cv.wait(lock, [&] { return s.stopping || !s.queue.empty(); });
            if (s.queue.empty()) return; // stopping and drained
            job = std::move(s.queue.front());
            s.queue.pop_front();
            ++s.running;
        }
        WorkEvent event;
        enum { none, executed, cancelled, expired, closed } outcome = none;
        if (job.port->is_closed()) {
            outcome = closed;
        } else if (job.action.cancellation->is_cancelled()) {
            outcome = cancelled;
            event = terminal(job.action.id, WorkStatus::cancelled, {});
        } else if (std::chrono::steady_clock::now() >= job.action.deadline) {
            outcome = expired;
            event = terminal(job.action.id, WorkStatus::timed_out,
                             core::make_error(core::ErrorCategory::timed_out, "helper queue deadline"));
        } else {
            outcome = executed;
            event = s.adapter->execute(job.action, stop);
            event.id = job.action.id; // the consumer matches on the submitted identity only
        }
        if (outcome == closed) job.port->release_reservation();
        else job.port->publish(std::move(event), std::move(job.completion));
        job = {}; // release the action's file token and paths before reporting idle
        std::lock_guard lock(s.mutex);
        --s.running;
        switch (outcome) {
        case executed: ++s.counters.executed; break;
        case cancelled: ++s.counters.skipped_cancelled; break;
        case expired: ++s.counters.skipped_expired; break;
        case closed: ++s.counters.dropped_closed; break;
        case none: break;
        }
    }
}

void HelperPool::stop() noexcept {
    auto& s = *state_;
    std::deque<State::Job> abandoned;
    {
        std::lock_guard lock(s.mutex);
        s.stopping = true;
        s.stop_flag.store(true, std::memory_order_release);
        abandoned.swap(s.queue);
        s.counters.cancelled_by_stop += abandoned.size();
    }
    s.cv.notify_all();
    // Reservations make these publications non-blocking; a closed port discards.
    for (auto& job : abandoned) {
        job.action.cancellation->cancel();
        job.port->publish(terminal(job.action.id, WorkStatus::cancelled,
                                   core::make_error(core::ErrorCategory::interrupted, "helper stop")),
                          std::move(job.completion));
    }
    abandoned.clear();
    std::lock_guard join_lock(s.join_mutex);
    for (auto& thread : s.threads) {
        if (thread.joinable() && thread.get_id() != std::this_thread::get_id()) thread.join();
    }
}

HelperStats HelperPool::stats() const {
    std::lock_guard lock(state_->mutex);
    auto copy = state_->counters;
    copy.queued = state_->queue.size();
    copy.running = state_->running;
    copy.threads = state_->threads.size();
    return copy;
}

// ---- HelperExecutor -------------------------------------------------------

HelperExecutor::HelperExecutor(std::shared_ptr<HelperPool> pool, std::shared_ptr<CompletionPort> destination)
    : pool_(std::move(pool)), destination_(std::move(destination)) {}

core::Result<void> HelperExecutor::submit(WorkAction action, Completion completion) {
    if (!pool_) return core::make_error(core::ErrorCategory::invalid_argument, "helper executor");
    const auto admitted = pool_->submit(std::move(action), destination_, std::move(completion));
    if (admitted == Admission::accepted) return {};
    return admission_error(admitted);
}

} // namespace concurrency::helpers
