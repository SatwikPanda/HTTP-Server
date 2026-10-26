#include "observability/bounded_logger.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

namespace observability {
namespace {

bool contains_case_insensitive(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.empty() || haystack.size() < needle.size()) {
        return false;
    }
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            const auto a = static_cast<unsigned char>(haystack[i + j]);
            const auto b = static_cast<unsigned char>(needle[j]);
            if (std::tolower(a) != std::tolower(b)) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

bool is_sensitive_message(std::string_view text) noexcept {
    return contains_case_insensitive(text, "authorization:") ||
           contains_case_insensitive(text, "proxy-authorization:") ||
           contains_case_insensitive(text, "cookie:") ||
           contains_case_insensitive(text, "set-cookie:") ||
           contains_case_insensitive(text, "bearer ") ||
           contains_case_insensitive(text, "basic ");
}

template <std::size_t Capacity>
std::uint8_t copy_sanitized(std::string_view src, std::array<char, Capacity>& dst, bool redact_sensitive) noexcept {
    if (redact_sensitive && is_sensitive_message(src)) {
        src = "[redacted-sensitive]";
    }
    const std::size_t limit = std::min(src.size(), Capacity);
    for (std::size_t i = 0; i < limit; ++i) {
        const char ch = src[i];
        if (ch == '\r' || ch == '\n' || ch == '\0' || static_cast<unsigned char>(ch) < 0x20) {
            dst[i] = ' ';
        } else {
            dst[i] = ch;
        }
    }
    return static_cast<std::uint8_t>(limit);
}

std::string_view level_name(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::debug: return "DEBUG";
        case LogLevel::info: return "INFO";
        case LogLevel::warning: return "WARN";
        case LogLevel::error: return "ERROR";
    }
    return "INFO";
}

std::string_view role_name(core::EndpointRole role) noexcept {
    switch (role) {
        case core::EndpointRole::downstream: return "downstream";
        case core::EndpointRole::upstream: return "upstream";
        case core::EndpointRole::file: return "file";
        case core::EndpointRole::resolver: return "resolver";
    }
    return "downstream";
}

} // namespace

LogRecord make_log_record(
    LogLevel level,
    core::OperationId id,
    std::string_view operation,
    std::string_view message,
    core::Error error,
    int http_status,
    const core::SystemClock& clock
) noexcept {
    LogRecord rec{};
    rec.timestamp = clock.now();
    rec.level = level;
    rec.operation_id = id;
    rec.category = error.category;
    rec.native_code = error.native_code;
    rec.http_status = http_status;

    const std::string_view op_text = !operation.empty() ? operation : std::string_view(error.operation);
    const std::string_view msg_text = !message.empty() ? message : std::string_view(error.message);
    rec.operation_len = copy_sanitized(op_text, rec.operation, false);
    rec.message_len = copy_sanitized(msg_text, rec.message, true);
    return rec;
}

std::string format_log_record(const LogRecord& record) {
    std::ostringstream out;
    out << "[" << level_name(record.level) << "]"
        << " conn=" << record.operation_id.transaction.connection.value
        << "." << record.operation_id.transaction.connection.generation
        << " tx=" << record.operation_id.transaction.sequence
        << " role=" << role_name(record.operation_id.endpoint)
        << " op_seq=" << record.operation_id.sequence;
    if (record.operation_len > 0) {
        out << " op=" << record.operation_view();
    }
    if (record.category != core::ErrorCategory::none) {
        out << " category=" << core::to_string(record.category);
    }
    if (record.native_code != 0) {
        out << " native=" << record.native_code;
    }
    if (record.http_status != 0) {
        out << " status=" << record.http_status;
    }
    if (record.message_len > 0) {
        out << " msg=\"" << record.message_view() << "\"";
    }
    return out.str();
}

BoundedLogger::BoundedLogger(
    std::size_t capacity,
    LogOverflowPolicy policy,
    LogSink sink,
    bool start_writer
)
    : capacity_(std::max<std::size_t>(1, capacity)),
      policy_(policy),
      sink_(std::move(sink)),
      ring_(capacity_),
      writer_active_(start_writer) {
    if (writer_active_) {
        writer_thread_ = std::thread([this] { writer_loop(); });
    }
}

BoundedLogger::~BoundedLogger() {
    (void)shutdown(std::chrono::milliseconds(200));
}

bool BoundedLogger::log(const LogRecord& record) noexcept {
    if (!enabled()) {
        return false;
    }
    bool notify = false;
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (count_ == capacity_) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            if (policy_ == LogOverflowPolicy::drop_newest) {
                return false;
            }
            // drop_oldest: overwrite oldest slot and advance head.
            ring_[head_] = record;
            head_ = (head_ + 1) % capacity_;
            enqueued_.fetch_add(1, std::memory_order_relaxed);
            notify = writer_active_;
        } else {
            const std::size_t tail = (head_ + count_) % capacity_;
            ring_[tail] = record;
            ++count_;
            enqueued_.fetch_add(1, std::memory_order_relaxed);
            notify = writer_active_;
        }
    }
    if (notify) {
        cv_.notify_one();
    }
    return true;
}

bool BoundedLogger::log_error(
    core::OperationId id,
    const core::Error& error,
    int http_status,
    const core::SystemClock& clock
) noexcept {
    return log(make_log_record(LogLevel::error, id, error.operation, error.message, error, http_status, clock));
}

std::size_t BoundedLogger::drain_some(std::size_t max_records) {
    std::size_t processed = 0;
    while (processed < max_records) {
        LogRecord record{};
        {
            std::lock_guard lock(mutex_);
            if (count_ == 0) {
                drained_cv_.notify_all();
                break;
            }
            record = ring_[head_];
            head_ = (head_ + 1) % capacity_;
            --count_;
            if (count_ == 0) {
                drained_cv_.notify_all();
            }
        }
        if (sink_) {
            sink_(record);
        }
        written_.fetch_add(1, std::memory_order_relaxed);
        ++processed;
    }
    return processed;
}

bool BoundedLogger::shutdown(std::chrono::milliseconds flush_timeout) noexcept {
    bool had_writer = false;
    {
        std::unique_lock lock(mutex_);
        if (stopping_) {
            had_writer = writer_active_;
        } else {
            stopping_ = true;
            had_writer = writer_active_;
            cv_.notify_all();
            if (had_writer && flush_timeout > std::chrono::milliseconds::zero()) {
                drained_cv_.wait_for(lock, flush_timeout, [this] { return count_ == 0; });
            }
        }
    }
    if (had_writer && writer_thread_.joinable()) {
        writer_thread_.join();
    }
    std::lock_guard lock(mutex_);
    writer_active_ = false;
    if (count_ > 0) {
        dropped_.fetch_add(count_, std::memory_order_relaxed);
        count_ = 0;
        head_ = 0;
        return false;
    }
    return true;
}

LoggerStats BoundedLogger::stats() const noexcept {
    std::size_t current_queued = 0;
    {
        std::lock_guard lock(mutex_);
        current_queued = count_;
    }
    return LoggerStats{
        enqueued_.load(std::memory_order_relaxed),
        written_.load(std::memory_order_relaxed),
        dropped_.load(std::memory_order_relaxed),
        current_queued,
        capacity_
    };
}

void BoundedLogger::writer_loop() noexcept {
    for (;;) {
        LogRecord record{};
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || count_ > 0; });
            if (count_ == 0) {
                drained_cv_.notify_all();
                if (stopping_) {
                    return;
                }
                continue;
            }
            record = ring_[head_];
            head_ = (head_ + 1) % capacity_;
            --count_;
            if (count_ == 0) {
                drained_cv_.notify_all();
            }
        }
        try {
            if (sink_) {
                sink_(record);
            }
        } catch (...) {
            // Never allow sink exceptions to terminate the background writer thread.
        }
        written_.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace observability
