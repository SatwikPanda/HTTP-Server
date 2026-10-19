#include "core/config.hpp"

namespace core {
Result<void> validate_config(const RuntimeConfig& c) {
    const auto positive = [](auto n) { return n > 0 && n <= 1000000; };
    const auto duration = [](auto d) { return d > std::chrono::milliseconds::zero() && d <= std::chrono::hours(24); };
    if (c.bind_address.empty() || c.bind_address.size() > 255 || c.bind_address.find('\0') != std::string::npos || c.port == 0 ||
        !positive(c.workers) || !positive(c.helpers) || !positive(c.sessions) || !positive(c.upstreams) ||
        !positive(c.accept_queue) || !positive(c.job_queue) || !positive(c.completion_queue) || !positive(c.max_requests) ||
        c.buffers.input < 4 || c.buffers.input > 16 * 1024 * 1024 || c.buffers.output < 128 || c.buffers.output > 16 * 1024 * 1024 ||
        c.buffers.low_water >= c.buffers.high_water || c.buffers.high_water > c.buffers.output ||
        !positive(c.cache.max_entries) || c.cache.object_bytes == 0 || c.cache.object_bytes > c.cache.resident_bytes ||
        c.cache.resident_bytes > c.cache.total_bytes || (c.cache.enabled && c.application != ApplicationMode::proxy) ||
        !duration(c.idle_timeout) || !duration(c.progress_timeout) || !duration(c.connect_timeout) || !duration(c.work_timeout) ||
        c.drain_timeout < std::chrono::milliseconds::zero() || c.drain_timeout > std::chrono::hours(24)) {
        return make_error(ErrorCategory::invalid_argument, "runtime configuration");
    }
    return {};
}
} // namespace core
