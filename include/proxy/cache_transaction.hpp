#pragma once

#include "cache/policy/cache_policy.hpp"
#include "cache/storage/lru_storage.hpp"
#include "proxy/transaction.hpp"

#include <array>
#include <atomic>

namespace proxy {

enum class CacheEvent : std::size_t {
    hit, miss, bypass, cache_only_failure, validation_request, validation_304,
    validation_200, validation_failed, stored, discarded, version_conflict,
    duplicate_miss, cancelled, rejected, count
};

// Fixed-size, thread-safe counters; values wrap only after 2^64 events.
class CacheStats {
public:
    [[nodiscard]] std::uint64_t count(CacheEvent event) const noexcept {
        return counters_[static_cast<std::size_t>(event)].load(std::memory_order_relaxed);
    }
    void record(CacheEvent event) noexcept {
        counters_[static_cast<std::size_t>(event)].fetch_add(1, std::memory_order_relaxed);
    }
private:
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(CacheEvent::count)> counters_{};
};

struct CacheProxyConfig {
    ProxyConfig forwarding;
    cache::policy::Limits policy;
};

// Selectable through HttpSessionConfig::application, just like B's uncached
// factory. Owns shared services; clocks and immutable requests remain borrowed
// under TransactionContext's lifetime contract. All network work goes through B.
// Admission covers hits as well as network transactions. One unconditional retry
// is allowed after an unusable/conflicting 304; repeated 304 becomes 502.
[[nodiscard]] core::Result<std::shared_ptr<const http::TransactionFactory>> make_cache_factory(
    std::shared_ptr<cache::storage::LruStorage> storage, CacheProxyConfig config = {},
    std::shared_ptr<CacheStats> stats = {}, std::shared_ptr<ProxyStats> forwarding_stats = {});

} // namespace proxy
