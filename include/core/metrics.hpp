#pragma once
#include <cstdint>

namespace core {
enum class Metric { requests_started, responses_completed, request_errors, wire_bytes, body_bytes, work_rejected, work_cancelled, cache_hit, cache_miss };
class MetricsSink {
public:
    virtual ~MetricsSink() = default;
    // Fixed dimensions only. Implementations must be bounded and nonblocking.
    virtual void record(Metric, std::uint64_t amount = 1) noexcept = 0;
};
class NullMetrics final : public MetricsSink {
public:
    void record(Metric, std::uint64_t = 1) noexcept override {}
};
inline MetricsSink& null_metrics() noexcept { static NullMetrics sink; return sink; }
} // namespace core
