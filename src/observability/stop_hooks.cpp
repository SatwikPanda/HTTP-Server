#include "observability/stop_hooks.hpp"

#include <csignal>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace observability {
namespace {

std::atomic<bool> g_os_stop_flag{false};
static_assert(std::atomic<bool>::is_always_lock_free,
              "OS signal/console stop notification requires a lock-free atomic flag");

#ifdef _WIN32
BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    switch (ctrl_type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            g_os_stop_flag.store(true, std::memory_order_relaxed);
            return TRUE;
        default:
            break;
    }
    return FALSE;
}
#else
extern "C" void posix_signal_handler(int) {
    g_os_stop_flag.store(true, std::memory_order_relaxed);
}
#endif

} // namespace

StopController::StopController(const core::SteadyClock& clock) noexcept
    : clock_(&clock) {}

StopController::~StopController() {
    request_stop();
}

void StopController::set_phase(LifecyclePhase next) noexcept {
    phase_.store(next, std::memory_order_release);
}

void StopController::set_stop_callback(std::function<void()> callback) {
    std::function<void()> invoke_now;
    {
        std::lock_guard lock(mutex_);
        callback_ = std::move(callback);
        if (stop_requested_.load(std::memory_order_acquire) &&
            !callback_invoked_.exchange(true, std::memory_order_acq_rel)) {
            invoke_now = callback_;
        }
    }
    if (invoke_now) {
        try {
            invoke_now();
        } catch (...) {
        }
    }
}

void StopController::request_stop() noexcept {
    stop_requested_.store(true, std::memory_order_release);
    auto current = phase_.load(std::memory_order_acquire);
    while (current != LifecyclePhase::stopping && current != LifecyclePhase::stopped &&
           current != LifecyclePhase::draining) {
        if (phase_.compare_exchange_weak(current, LifecyclePhase::draining, std::memory_order_acq_rel)) {
            break;
        }
    }

    std::function<void()> to_invoke;
    {
        std::lock_guard lock(mutex_);
        if (callback_ && !callback_invoked_.exchange(true, std::memory_order_acq_rel)) {
            to_invoke = callback_;
        }
    }
    if (to_invoke) {
        try {
            to_invoke();
        } catch (...) {
        }
    }
    cv_.notify_all();
}

bool StopController::wait_for_stop(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, timeout, [this] {
        return stop_requested_.load(std::memory_order_acquire);
    });
}

void StopController::reset() noexcept {
    std::lock_guard lock(mutex_);
    stop_requested_.store(false, std::memory_order_release);
    callback_invoked_.store(false, std::memory_order_release);
    phase_.store(LifecyclePhase::constructed, std::memory_order_release);
}

ScopedStopHooks::ScopedStopHooks(StopController& controller, std::chrono::milliseconds poll_interval)
    : controller_(controller),
      poll_interval_(std::max(std::chrono::milliseconds(1), poll_interval)) {
    g_os_stop_flag.store(false, std::memory_order_relaxed);
#ifdef _WIN32
    installed_ = SetConsoleCtrlHandler(console_ctrl_handler, TRUE) != FALSE;
#else
    struct sigaction sa {};
    sa.sa_handler = posix_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    const bool int_ok = sigaction(SIGINT, &sa, nullptr) == 0;
    const bool term_ok = sigaction(SIGTERM, &sa, nullptr) == 0;
    installed_ = int_ok && term_ok;
#endif
    monitor_thread_ = std::thread([this] { monitor_loop(); });
}

ScopedStopHooks::~ScopedStopHooks() {
    {
        std::lock_guard lock(mutex_);
        shutting_down_.store(true, std::memory_order_release);
    }
    cv_.notify_all();
    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }
#ifdef _WIN32
    if (installed_) {
        SetConsoleCtrlHandler(console_ctrl_handler, FALSE);
    }
#else
    if (installed_) {
        std::signal(SIGINT, SIG_DFL);
        std::signal(SIGTERM, SIG_DFL);
    }
#endif
    g_os_stop_flag.store(false, std::memory_order_relaxed);
}

void ScopedStopHooks::simulate_os_stop_signal() noexcept {
    g_os_stop_flag.store(true, std::memory_order_relaxed);
}

void ScopedStopHooks::monitor_loop() noexcept {
    std::unique_lock lock(mutex_);
    while (!shutting_down_.load(std::memory_order_acquire)) {
        if (g_os_stop_flag.exchange(false, std::memory_order_relaxed)) {
            lock.unlock();
            controller_.request_stop();
            lock.lock();
            break;
        }
        cv_.wait_for(lock, poll_interval_);
    }
}

} // namespace observability
