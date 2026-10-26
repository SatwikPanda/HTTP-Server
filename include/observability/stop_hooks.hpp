#pragma once

#include "core/clock.hpp"
#include "core/result.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace observability {

enum class LifecyclePhase {
    constructed,
    starting,
    running,
    draining,
    stopping,
    stopped
};

// Thread-safe normal-thread stop controller and OS signal/console bridge.
// Signal/console callbacks touch only a lock-free atomic flag; a dedicated normal
// control thread observes that flag and invokes the registered stop callback.
class StopController {
public:
    explicit StopController(const core::SteadyClock& clock = core::real_steady_clock()) noexcept;
    ~StopController();

    StopController(const StopController&) = delete;
    StopController& operator=(const StopController&) = delete;

    [[nodiscard]] LifecyclePhase phase() const noexcept {
        return phase_.load(std::memory_order_acquire);
    }
    void set_phase(LifecyclePhase next) noexcept;

    // Register the normal-thread callback invoked at most once when stop is requested.
    void set_stop_callback(std::function<void()> callback);

    // Idempotent normal-thread stop request; safe in any phase (partial startup,
    // running, draining, or already stopped).
    void request_stop() noexcept;

    [[nodiscard]] bool stop_requested() const noexcept {
        return stop_requested_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool wait_for_stop(std::chrono::milliseconds timeout);
    void reset() noexcept;

private:
    const core::SteadyClock* clock_;
    std::atomic<LifecyclePhase> phase_{LifecyclePhase::constructed};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> callback_invoked_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::function<void()> callback_;
};

// RAII OS signal (SIGINT/SIGTERM) and Windows console control hook registration.
// Destructor stops the monitor thread and unregisters OS hooks before returning,
// ensuring OS callbacks can never touch a destroyed StopController.
class ScopedStopHooks {
public:
    explicit ScopedStopHooks(
        StopController& controller,
        std::chrono::milliseconds poll_interval = std::chrono::milliseconds(10)
    );
    ~ScopedStopHooks();

    ScopedStopHooks(const ScopedStopHooks&) = delete;
    ScopedStopHooks& operator=(const ScopedStopHooks&) = delete;

    [[nodiscard]] bool installed() const noexcept { return installed_; }

    // Simulates an OS signal/console event setting the lock-free flag directly.
    static void simulate_os_stop_signal() noexcept;

private:
    void monitor_loop() noexcept;

    StopController& controller_;
    std::chrono::milliseconds poll_interval_;
    bool installed_{false};
    std::atomic<bool> shutting_down_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread monitor_thread_;
};

} // namespace observability
