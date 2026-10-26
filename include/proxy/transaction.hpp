#pragma once

#include "cache/contracts.hpp"
#include "http/application.hpp"
#include "http/limits.hpp"
#include "proxy/forwarding.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>

namespace proxy {

// Bounds and phase deadlines for one forwarding transaction.
struct ProxyLimits {
    // Resolution plus all connection candidates share this deadline. With the
    // synchronous reference executor DNS is checked only after the OS call
    // returns; asynchronous enforcement requires E's helper dispatch.
    std::chrono::milliseconds connect_timeout{5000};
    // From connection success until the final response head is parsed,
    // covering request writes and interim heads.
    std::chrono::milliseconds response_timeout{10000};
    // Maximum gap between useful upstream body bytes. It is not running while
    // the transaction is paused on downstream output; the session's output
    // progress deadline bounds a slow client instead.
    std::chrono::milliseconds body_timeout{10000};
    // Upper bound on one upstream read and therefore on buffered parser input.
    std::size_t max_read_bytes{16 * 1024};
    // Resolved candidates beyond this count are ignored.
    std::size_t max_candidates{8};
    // Admission limit across every transaction created by one factory.
    std::size_t max_active_transactions{256};
    // Origin response head/trailer/total body bounds.
    http::RequestLimits response_limits{};
};

struct ProxyConfig {
    ForwardingConfig forwarding;
    ProxyLimits limits;
};

[[nodiscard]] core::Result<void> validate_proxy_config(const ProxyConfig& config);

enum class Outcome { completed, client_error, bad_gateway, gateway_timeout, unavailable, internal_error, cancelled };

// Thread-safe outcome counters shared by every transaction of a factory. The
// active count is also the admission gauge; it returns to zero once every
// transaction has been destroyed.
class ProxyStats {
public:
    struct Snapshot {
        std::uint64_t started{}, active{}, peak_active{}, origin_connections{};
        std::uint64_t completed{}, client_error{}, bad_gateway{}, gateway_timeout{};
        std::uint64_t unavailable{}, internal_error{}, cancelled{};
    };
    [[nodiscard]] Snapshot snapshot() const noexcept;
    [[nodiscard]] bool try_admit(std::size_t limit) noexcept;
    void release() noexcept;
    void record(Outcome outcome) noexcept;
    void record_origin_connection() noexcept { origin_connections_.fetch_add(1, std::memory_order_relaxed); }
private:
    std::atomic<std::uint64_t> started_{}, active_{}, peak_active_{}, origin_connections_{};
    std::atomic<std::uint64_t> completed_{}, client_error_{}, bad_gateway_{}, gateway_timeout_{};
    std::atomic<std::uint64_t> unavailable_{}, internal_error_{}, cancelled_{};
};

using ExchangeTiming = cache::Timing;

// Per-transaction seam for K. Callbacks run on the session owner thread and
// must not block. Returning an error fails the transaction through the normal
// commitment rules (502 before downstream commitment, close afterwards).
class ExchangeObserver {
public:
    virtual ~ExchangeObserver() = default;
    // Before the request is serialized: may add validators or edit fields.
    [[nodiscard]] virtual core::Result<void> prepare(OutboundRequest&) { return {}; }
    // Origin final head and the filtered downstream head that will be sent.
    [[nodiscard]] virtual core::Result<void> on_final_head(const http::Response& /*origin*/,
        http::Response& /*downstream*/, const ExchangeTiming&) { return {}; }
    // Transfer-decoded representation bytes; the span is valid only for this call.
    [[nodiscard]] virtual core::Result<void> on_body(std::span<const std::byte>) { return {}; }
    // Successful parser completion only. For a conditional validation request
    // that received 304 Not Modified, `on_complete` runs after the upstream
    // close action completes; K may intercept the ResponseHead step to substitute its
    // refreshed 200 representation.
    virtual void on_complete(const http::HeaderMap& /*trailers*/) {}
    // Failure or cancellation; called at most once and never after on_complete.
    virtual void on_failure(const core::Error&) {}
};

// Builds one uncached forwarding transaction. Rejections (bad target, loop,
// admission) are reported as a TransactionFailure on StartTransaction.
[[nodiscard]] std::unique_ptr<http::Transaction> make_forward_transaction(
    const http::Request& client, const http::TransactionContext& context, const ProxyConfig& config,
    std::shared_ptr<ProxyStats> stats, std::unique_ptr<ExchangeObserver> observer = {});

// Immutable shared factory. `stats` may be supplied to observe outcomes; one
// is created otherwise, so admission is always enforced per factory.
[[nodiscard]] core::Result<std::shared_ptr<const http::TransactionFactory>> make_forward_factory(
    ProxyConfig config = {}, std::shared_ptr<ProxyStats> stats = {});

} // namespace proxy
