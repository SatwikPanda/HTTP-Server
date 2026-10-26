#include "observability/metrics_registry.hpp"

#include <algorithm>
#include <cmath>

namespace observability {

std::uint64_t HistogramSnapshot::percentile_upper_bound_us(double quantile) const noexcept {
    if (total_count == 0) {
        return 0;
    }
    const double clamped = std::clamp(quantile, 0.0, 1.0);
    if (clamped <= 0.0) {
        for (std::size_t i = 0; i < kLatencyBuckets; ++i) {
            if (buckets[i] > 0) {
                return i < kLatencyBoundsUs.size() ? kLatencyBoundsUs[i] : max_microseconds;
            }
        }
    }
    const auto target = static_cast<std::uint64_t>(std::ceil(clamped * static_cast<double>(total_count)));
    std::uint64_t cumulative = 0;
    for (std::size_t i = 0; i < kLatencyBuckets; ++i) {
        cumulative += buckets[i];
        if (cumulative >= target) {
            if (i < kLatencyBoundsUs.size()) {
                return kLatencyBoundsUs[i];
            }
            return max_microseconds;
        }
    }
    return max_microseconds;
}

Counter counter_for_status(int status_code) noexcept {
    switch (status_code) {
        case 200: return Counter::status_200;
        case 304: return Counter::status_304;
        case 400: return Counter::status_400;
        case 403: return Counter::status_403;
        case 404: return Counter::status_404;
        case 405: return Counter::status_405;
        case 408: return Counter::status_408;
        case 413: return Counter::status_413;
        case 414: return Counter::status_414;
        case 417: return Counter::status_417;
        case 431: return Counter::status_431;
        case 500: return Counter::status_500;
        case 501: return Counter::status_501;
        case 502: return Counter::status_502;
        case 503: return Counter::status_503;
        case 504: return Counter::status_504;
        case 505: return Counter::status_505;
        default: return Counter::status_other;
    }
}

void MetricsRegistry::record(core::Metric metric, std::uint64_t amount) noexcept {
    if (!enabled()) {
        return;
    }
    switch (metric) {
        case core::Metric::requests_started:
            increment(Counter::requests_started, amount);
            break;
        case core::Metric::responses_completed:
            increment(Counter::responses_completed, amount);
            break;
        case core::Metric::request_errors:
            increment(Counter::request_errors, amount);
            break;
        case core::Metric::wire_bytes:
            increment(Counter::wire_bytes_completed, amount);
            break;
        case core::Metric::body_bytes:
            increment(Counter::body_bytes_completed, amount);
            break;
        case core::Metric::work_rejected:
            increment(Counter::work_rejected, amount);
            break;
        case core::Metric::work_cancelled:
            increment(Counter::work_cancelled, amount);
            break;
        case core::Metric::cache_hit:
            increment(Counter::cache_hits, amount);
            break;
        case core::Metric::cache_miss:
            increment(Counter::cache_misses, amount);
            break;
    }
}

void MetricsRegistry::increment(Counter counter, std::uint64_t amount) noexcept {
    if (!enabled() || counter == Counter::count) {
        return;
    }
    counters_[static_cast<std::size_t>(counter)].fetch_add(amount, std::memory_order_relaxed);
}

void MetricsRegistry::record_status(int status_code) noexcept {
    if (!enabled()) {
        return;
    }
    increment(counter_for_status(status_code), 1);
}

void MetricsRegistry::observe_latency(LatencyKind kind, std::chrono::nanoseconds duration) noexcept {
    if (!enabled() || kind == LatencyKind::count) {
        return;
    }
    const auto clamped_ns = std::max(duration, std::chrono::nanoseconds::zero());
    const auto us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(clamped_ns).count());

    std::size_t bucket = kLatencyBuckets - 1;
    for (std::size_t i = 0; i < kLatencyBoundsUs.size(); ++i) {
        if (us <= kLatencyBoundsUs[i]) {
            bucket = i;
            break;
        }
    }

    auto& hist = histograms_[static_cast<std::size_t>(kind)];
    hist.buckets[bucket].fetch_add(1, std::memory_order_relaxed);
    hist.count.fetch_add(1, std::memory_order_relaxed);
    hist.sum_us.fetch_add(us, std::memory_order_relaxed);

    auto prev_max = hist.max_us.load(std::memory_order_relaxed);
    while (us > prev_max && !hist.max_us.compare_exchange_weak(prev_max, us, std::memory_order_relaxed)) {
    }
}

void MetricsRegistry::update_peak(std::size_t index, std::size_t current) noexcept {
    auto prev_peak = peak_gauges_[index].load(std::memory_order_relaxed);
    while (current > prev_peak &&
           !peak_gauges_[index].compare_exchange_weak(prev_peak, current, std::memory_order_relaxed)) {
    }
}

void MetricsRegistry::set_gauge(Gauge gauge, std::size_t value) noexcept {
    if (!enabled() || gauge == Gauge::count) {
        return;
    }
    const auto idx = static_cast<std::size_t>(gauge);
    gauges_[idx].store(value, std::memory_order_relaxed);
    update_peak(idx, value);
}

void MetricsRegistry::add_gauge(Gauge gauge, std::size_t delta) noexcept {
    if (!enabled() || gauge == Gauge::count) {
        return;
    }
    const auto idx = static_cast<std::size_t>(gauge);
    const auto current = gauges_[idx].fetch_add(delta, std::memory_order_relaxed) + delta;
    update_peak(idx, current);
}

void MetricsRegistry::sub_gauge(Gauge gauge, std::size_t delta) noexcept {
    if (!enabled() || gauge == Gauge::count) {
        return;
    }
    const auto idx = static_cast<std::size_t>(gauge);
    auto current = gauges_[idx].load(std::memory_order_relaxed);
    for (;;) {
        const auto next = current > delta ? current - delta : 0;
        if (gauges_[idx].compare_exchange_weak(current, next, std::memory_order_relaxed)) {
            break;
        }
    }
}

MetricsSnapshot MetricsRegistry::snapshot(const core::MemoryBudget* budget) const noexcept {
    MetricsSnapshot snap{};
    for (std::size_t i = 0; i < counters_.size(); ++i) {
        snap.counters[i] = counters_[i].load(std::memory_order_relaxed);
    }
    for (std::size_t i = 0; i < gauges_.size(); ++i) {
        snap.gauges[i] = gauges_[i].load(std::memory_order_relaxed);
        snap.peak_gauges[i] = peak_gauges_[i].load(std::memory_order_relaxed);
    }
    for (std::size_t i = 0; i < histograms_.size(); ++i) {
        const auto& src = histograms_[i];
        auto& dst = snap.latencies[i];
        for (std::size_t b = 0; b < kLatencyBuckets; ++b) {
            dst.buckets[b] = src.buckets[b].load(std::memory_order_relaxed);
        }
        dst.total_count = src.count.load(std::memory_order_relaxed);
        dst.total_microseconds = src.sum_us.load(std::memory_order_relaxed);
        dst.max_microseconds = src.max_us.load(std::memory_order_relaxed);
    }
    if (budget != nullptr) {
        snap.memory.candidate_bytes = budget->used(core::AllocationKind::candidate);
        snap.memory.resident_bytes = budget->used(core::AllocationKind::resident);
        snap.memory.pinned_bytes = budget->used(core::AllocationKind::pinned);
        snap.memory.temporary_bytes = budget->used(core::AllocationKind::temporary);
        snap.memory.metadata_bytes = budget->used(core::AllocationKind::metadata);
        snap.memory.enforced_used_bytes = budget->used();
        snap.memory.enforced_capacity_bytes = budget->capacity();
    }
    return snap;
}

void MetricsRegistry::reset() noexcept {
    for (auto& c : counters_) c.store(0, std::memory_order_relaxed);
    for (auto& g : gauges_) g.store(0, std::memory_order_relaxed);
    for (auto& p : peak_gauges_) p.store(0, std::memory_order_relaxed);
    for (auto& h : histograms_) {
        for (auto& b : h.buckets) b.store(0, std::memory_order_relaxed);
        h.count.store(0, std::memory_order_relaxed);
        h.sum_us.store(0, std::memory_order_relaxed);
        h.max_us.store(0, std::memory_order_relaxed);
    }
}

} // namespace observability
