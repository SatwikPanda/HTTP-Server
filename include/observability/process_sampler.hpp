#pragma once

#include "core/clock.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

namespace observability {

struct ProcessSample {
    core::SteadyTimePoint timestamp{};
    std::optional<std::chrono::microseconds> user_cpu_time;
    std::optional<std::chrono::microseconds> system_cpu_time;
    std::optional<std::chrono::microseconds> total_cpu_time;
    std::optional<double> cpu_utilization_ratio; // total CPU delta / wall delta (1.0 == 1 core fully busy)
    std::optional<std::size_t> resident_memory_bytes;
    std::optional<std::size_t> peak_resident_memory_bytes;
};

// Rate-bounded sampler isolating OS-specific process CPU and memory queries.
// Unsupported measurements are reported as std::nullopt, never fabricated as zero.
class ProcessSampler {
public:
    explicit ProcessSampler(
        std::chrono::milliseconds min_interval = std::chrono::milliseconds(100),
        const core::SteadyClock& clock = core::real_steady_clock()
    ) noexcept;

    [[nodiscard]] ProcessSample sample() noexcept;
    [[nodiscard]] ProcessSample force_sample() noexcept;

private:
    std::chrono::milliseconds min_interval_;
    const core::SteadyClock* clock_;
    std::mutex mutex_;
    bool has_last_{false};
    ProcessSample last_sample_{};
};

} // namespace observability
