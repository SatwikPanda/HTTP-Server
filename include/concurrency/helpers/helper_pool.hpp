#pragma once

#include "concurrency/work.hpp"
#include "core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>

namespace concurrency::helpers {

// Fixed helper capacity. Queued jobs, running jobs (== threads) and each
// destination's outstanding results are bounded separately; nothing grows past
// these numbers under load.
struct HelperConfig {
    std::size_t threads{2};
    std::size_t queue_capacity{64};
    WorkLimits limits{};
    // Diagnostic seam: called before each helper thread is created. An error
    // makes start() fail after already-created threads are stopped and joined.
    std::function<core::Result<void>(std::size_t index)> before_thread_start;
};
[[nodiscard]] core::Result<void> validate_config(const HelperConfig& config);

// The blocking boundary run on helper threads. It is called concurrently from
// several helpers and must not touch driver sockets. `stop` reports pool stop.
class HelperAdapter {
public:
    virtual ~HelperAdapter() = default;
    [[nodiscard]] virtual WorkEvent execute(const WorkAction& action, const std::function<bool()>& stop) = 0;
};
// Production adapter: runs the P0 file/DNS code with a fresh single-use
// reference executor per job, so no per-connection state is shared by helpers.
[[nodiscard]] std::shared_ptr<HelperAdapter> make_reference_adapter(WorkLimits limits = {});

// Only resolver and file requests are helper work.
[[nodiscard]] bool is_helper_work(const WorkRequest& request) noexcept;

// A driver-owned, bounded result destination. Helpers publish into it and call
// `wake`; the owner thread runs completions through drain(). Each admitted job
// reserves one slot up front, so publication never blocks or allocates an
// unbounded fallback. close() invalidates the destination: later results are
// discarded on the helper thread and `wake` is never called after close returns.
class CompletionPort {
public:
    using Wake = std::function<void()>;
    [[nodiscard]] static std::shared_ptr<CompletionPort> create(std::size_t capacity, Wake wake);
    ~CompletionPort();
    CompletionPort(const CompletionPort&) = delete;
    CompletionPort& operator=(const CompletionPort&) = delete;

    // Owner thread. Runs at most `max` ready completions outside the lock and
    // returns how many ran. Wakes coalesce, so callers drain until empty or
    // reschedule the remainder under their own work budget.
    std::size_t drain(std::size_t max = std::numeric_limits<std::size_t>::max());
    // Owner thread, idempotent. Ready results are released without callbacks.
    void close() noexcept;

    [[nodiscard]] bool has_ready() const;
    [[nodiscard]] bool is_closed() const;
    [[nodiscard]] std::size_t outstanding() const; // queued + running + ready
    [[nodiscard]] std::size_t capacity() const noexcept;

private:
    friend class HelperPool;
    struct State;
    explicit CompletionPort(std::unique_ptr<State> state) noexcept;
    [[nodiscard]] bool try_reserve();
    void release_reservation() noexcept;
    void publish(WorkEvent event, WorkExecutor::Completion completion);
    std::unique_ptr<State> state_;
};

enum class Admission { accepted, saturated, stopping, invalid };
[[nodiscard]] core::Error admission_error(Admission admission);

struct HelperStats {
    std::size_t queued{}, running{}, threads{};
    std::uint64_t accepted{}, saturated{}, executed{}, skipped_cancelled{}, skipped_expired{},
        dropped_closed{}, cancelled_by_stop{};
};

// Fixed helper threads over one bounded job queue. Distinct from client
// workers: a client worker only submits and waits for its port, so it never
// needs a slot in its own pool to make progress.
class HelperPool {
public:
    [[nodiscard]] static core::Result<std::shared_ptr<HelperPool>> start(
        HelperConfig config, std::shared_ptr<HelperAdapter> adapter = nullptr);
    ~HelperPool();
    HelperPool(const HelperPool&) = delete;
    HelperPool& operator=(const HelperPool&) = delete;

    // Never blocks. Ownership rule: the action and completion are taken by value.
    // Accepted means exactly one completion reaches `destination` (unless it is
    // closed first). Any other result destroys the pool's copies and calls nothing;
    // the caller keeps whatever it retained itself.
    [[nodiscard]] Admission submit(WorkAction action, const std::shared_ptr<CompletionPort>& destination,
                                   WorkExecutor::Completion completion);

    // Idempotent. Stops admission, completes queued jobs as `cancelled`, lets
    // running jobs finish (an OS resolver call cannot be interrupted, so this
    // can wait for it), then joins. Never call from a helper thread.
    void stop() noexcept;
    [[nodiscard]] HelperStats stats() const;

private:
    struct State;
    explicit HelperPool(std::unique_ptr<State> state) noexcept;
    void run(std::size_t index);
    std::unique_ptr<State> state_;
};

// The P0 WorkExecutor port bound to one destination, for drivers and L.
// Admission failures map to an error; the caller then delivers a matching
// `rejected` WorkEvent to its session on the owner thread.
class HelperExecutor final : public WorkExecutor {
public:
    HelperExecutor(std::shared_ptr<HelperPool> pool, std::shared_ptr<CompletionPort> destination);
    [[nodiscard]] core::Result<void> submit(WorkAction action, Completion completion) override;
    [[nodiscard]] const std::shared_ptr<CompletionPort>& destination() const noexcept { return destination_; }
private:
    std::shared_ptr<HelperPool> pool_;
    std::shared_ptr<CompletionPort> destination_;
};

} // namespace concurrency::helpers
