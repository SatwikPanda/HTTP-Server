#pragma once

#include "core/budget.hpp"
#include "core/metrics.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace observability {

enum class Counter {
    requests_accepted,
    requests_started,
    responses_completed,
    request_errors,
    client_cancellations,
    wire_bytes_attempted,
    wire_bytes_completed,
    body_bytes_completed,
    work_rejected,
    work_cancelled,
    accept_rejected,
    upstream_rejected,
    timeouts_idle,
    timeouts_progress,
    timeouts_connect,
    timeouts_work,
    cache_hits,
    cache_misses,
    cache_bypasses,
    cache_insertions,
    cache_evictions,
    cache_duplicate_misses,
    cache_revalidations_sent,
    cache_revalidated_304,
    cache_revalidated_200,
    cache_revalidation_errors,
    status_200,
    status_304,
    status_400,
    status_403,
    status_404,
    status_405,
    status_408,
    status_413,
    status_414,
    status_417,
    status_431,
    status_500,
    status_501,
    status_502,
    status_503,
    status_504,
    status_505,
    status_other,
    count
};

enum class Gauge {
    active_sessions,
    queued_accepts,
    active_upstreams,
    connecting_upstreams,
    queued_jobs,
    queued_completions,
    paused_producers,
    cache_entries,
    count
};

enum class LatencyKind {
    request,
    queue,
    origin,
    count
};

// Fixed microsecond upper bounds for latency histograms; the final bucket is overflow.
inline constexpr std::size_t kLatencyBuckets = 12;
inline constexpr std::array<std::uint64_t, kLatencyBuckets - 1> kLatencyBoundsUs{
    100, 250, 500, 1000, 2500, 5000, 10000, 25000, 100000, 500000, 2000000
};

struct HistogramSnapshot {
    std::array<std::uint64_t, kLatencyBuckets> buckets{};
    std::uint64_t total_count{};
    std::uint64_t total_microseconds{};
    std::uint64_t max_microseconds{};

    [[nodiscard]] std::uint64_t percentile_upper_bound_us(double quantile) const noexcept;
};

struct MemorySnapshot {
    std::size_t candidate_bytes{};
    std::size_t resident_bytes{};
    std::size_t pinned_bytes{};
    std::size_t temporary_bytes{};
    std::size_t metadata_bytes{};
    std::size_t enforced_used_bytes{};
    std::size_t enforced_capacity_bytes{};
};

struct MetricsSnapshot {
    std::array<std::uint64_t, static_cast<std::size_t>(Counter::count)> counters{};
    std::array<std::size_t, static_cast<std::size_t>(Gauge::count)> gauges{};
    std::array<std::size_t, static_cast<std::size_t>(Gauge::count)> peak_gauges{};
    std::array<HistogramSnapshot, static_cast<std::size_t>(LatencyKind::count)> latencies{};
    MemorySnapshot memory{};

    [[nodiscard]] std::uint64_t counter(Counter c) const noexcept {
        return counters[static_cast<std::size_t>(c)];
    }
    [[nodiscard]] std::size_t gauge(Gauge g) const noexcept {
        return gauges[static_cast<std::size_t>(g)];
    }
    [[nodiscard]] std::size_t peak_gauge(Gauge g) const noexcept {
        return peak_gauges[static_cast<std::size_t>(g)];
    }
    [[nodiscard]] const HistogramSnapshot& latency(LatencyKind k) const noexcept {
        return latencies[static_cast<std::size_t>(k)];
    }
};

// Lock-free bounded metrics collector implementing core::MetricsSink.
class MetricsRegistry final : public core::MetricsSink {
public:
    explicit MetricsRegistry(bool enabled = true) noexcept : enabled_(enabled) {}

    void set_enabled(bool enabled) noexcept {
        enabled_.store(enabled, std::memory_order_relaxed);
    }
    [[nodiscard]] bool enabled() const noexcept {
        return enabled_.load(std::memory_order_relaxed);
    }

    void record(core::Metric metric, std::uint64_t amount = 1) noexcept override;
    void increment(Counter counter, std::uint64_t amount = 1) noexcept;
    void record_status(int status_code) noexcept;
    void observe_latency(LatencyKind kind, std::chrono::nanoseconds duration) noexcept;

    void set_gauge(Gauge gauge, std::size_t value) noexcept;
    void add_gauge(Gauge gauge, std::size_t delta = 1) noexcept;
    void sub_gauge(Gauge gauge, std::size_t delta = 1) noexcept;

    [[nodiscard]] MetricsSnapshot snapshot(const core::MemoryBudget* budget = nullptr) const noexcept;
    void reset() noexcept;

private:
    struct AtomicHistogram {
        std::array<std::atomic<std::uint64_t>, kLatencyBuckets> buckets{};
        std::atomic<std::uint64_t> count{0};
        std::atomic<std::uint64_t> sum_us{0};
        std::atomic<std::uint64_t> max_us{0};
    };

    void update_peak(std::size_t index, std::size_t current) noexcept;

    std::atomic<bool> enabled_{true};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(Counter::count)> counters_{};
    std::array<std::atomic<std::size_t>, static_cast<std::size_t>(Gauge::count)> gauges_{};
    std::array<std::atomic<std::size_t>, static_cast<std::size_t>(Gauge::count)> peak_gauges_{};
    std::array<AtomicHistogram, static_cast<std::size_t>(LatencyKind::count)> histograms_{};
};

[[nodiscard]] Counter counter_for_status(int status_code) noexcept;

} // namespace observability
