#pragma once

#include "core/clock.hpp"
#include "core/error.hpp"
#include "core/id.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace observability {

enum class LogLevel {
    debug,
    info,
    warning,
    error
};

enum class LogOverflowPolicy {
    drop_newest,
    drop_oldest
};

inline constexpr std::size_t kMaxLogOperationBytes = 48;
inline constexpr std::size_t kMaxLogMessageBytes = 160;

// Fixed-capacity record so logging admission never allocates unbounded strings.
struct LogRecord {
    core::SystemTimePoint timestamp{};
    LogLevel level{LogLevel::info};
    core::OperationId operation_id{};
    core::ErrorCategory category{core::ErrorCategory::none};
    int native_code{0};
    int http_status{0};
    std::uint8_t operation_len{0};
    std::uint8_t message_len{0};
    std::array<char, kMaxLogOperationBytes> operation{};
    std::array<char, kMaxLogMessageBytes> message{};

    [[nodiscard]] std::string_view operation_view() const noexcept {
        return {operation.data(), operation_len};
    }
    [[nodiscard]] std::string_view message_view() const noexcept {
        return {message.data(), message_len};
    }
};

struct LoggerStats {
    std::uint64_t enqueued{};
    std::uint64_t written{};
    std::uint64_t dropped{};
    std::size_t queued{};
    std::size_t capacity{};
};

using LogSink = std::function<void(const LogRecord&)>;

[[nodiscard]] LogRecord make_log_record(
    LogLevel level,
    core::OperationId id,
    std::string_view operation,
    std::string_view message,
    core::Error error = {},
    int http_status = 0,
    const core::SystemClock& clock = core::real_system_clock()
) noexcept;

[[nodiscard]] std::string format_log_record(const LogRecord& record);

// Bounded nonblocking structured logger with background writer thread.
class BoundedLogger {
public:
    explicit BoundedLogger(
        std::size_t capacity = 256,
        LogOverflowPolicy policy = LogOverflowPolicy::drop_newest,
        LogSink sink = {},
        bool start_writer = true
    );
    ~BoundedLogger();

    BoundedLogger(const BoundedLogger&) = delete;
    BoundedLogger& operator=(const BoundedLogger&) = delete;

    void set_enabled(bool enabled) noexcept {
        enabled_.store(enabled, std::memory_order_relaxed);
    }
    [[nodiscard]] bool enabled() const noexcept {
        return enabled_.load(std::memory_order_relaxed);
    }

    // Nonblocking admission; never blocks on sink I/O or allocates heap strings.
    bool log(const LogRecord& record) noexcept;

    bool log_error(
        core::OperationId id,
        const core::Error& error,
        int http_status = 0,
        const core::SystemClock& clock = core::real_system_clock()
    ) noexcept;

    // Synchronously drain up to max_records on the caller thread (useful for tests).
    std::size_t drain_some(std::size_t max_records = 64);

    // Bounded flush and shutdown of the background writer thread.
    // Returns true if the queue drained completely before the deadline.
    bool shutdown(std::chrono::milliseconds flush_timeout = std::chrono::milliseconds(500)) noexcept;

    [[nodiscard]] LoggerStats stats() const noexcept;

private:
    void writer_loop() noexcept;

    std::size_t capacity_;
    LogOverflowPolicy policy_;
    LogSink sink_;
    std::atomic<bool> enabled_{true};

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable drained_cv_;
    std::vector<LogRecord> ring_;
    std::size_t head_{0};
    std::size_t count_{0};
    bool stopping_{false};
    bool writer_active_{false};

    std::atomic<std::uint64_t> enqueued_{0};
    std::atomic<std::uint64_t> written_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::thread writer_thread_;
};

} // namespace observability
