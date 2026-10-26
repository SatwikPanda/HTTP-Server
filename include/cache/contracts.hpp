#pragma once

#include "core/budget.hpp"
#include "core/clock.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/target.hpp"
#include "http/validators.hpp"
#include "http/framing.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace cache {
using Key = http::Target;
struct KeyHash { std::size_t operator()(const Key& key) const { return std::hash<std::string>{}(key.key()); } };
struct EntryVersion { std::uint64_t value{}; bool operator==(const EntryVersion&) const = default; };
struct Timing {
    core::SteadyTimePoint request_sent{}, final_head_received{};
    core::SystemTimePoint final_head_wall{};
};
class Body {
public:
    [[nodiscard]] static core::Result<std::shared_ptr<const Body>> create(std::vector<std::byte> bytes, core::Reservation reservation) {
        if (reservation.size() < bytes.capacity()) return core::make_error(core::ErrorCategory::invalid_argument, "cache body reservation");
        return std::shared_ptr<const Body>(new Body(std::move(bytes), std::move(reservation)));
    }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::size_t accounted_bytes() const noexcept { return reservation_.size(); }
    void reclassify(core::AllocationKind kind) const noexcept { reservation_.reclassify(kind); }
private:
    Body(std::vector<std::byte> bytes, core::Reservation reservation)
        : reservation_(std::move(reservation)), bytes_(std::move(bytes)) {}
    // Members die in reverse order: free payload before returning its budget.
    mutable core::Reservation reservation_;
    std::vector<std::byte> bytes_;
};
struct Entry {
    Key key;
    EntryVersion version;
    int status{200};
    http::HeaderMap headers;
    std::shared_ptr<const Body> body;
    std::string content_encoding;
    std::optional<http::EntityTag> etag;
    std::optional<core::SystemTimePoint> last_modified;
    Timing timing;
    mutable core::Reservation metadata; // storage alone reclassifies this charge
};
using Snapshot = std::shared_ptr<const Entry>;
enum class StorageOutcome { hit, miss, stored, oversized, exhausted, version_conflict, failed };
struct LookupResult { StorageOutcome outcome{StorageOutcome::miss}; Snapshot entry; EntryVersion observed; core::Error error; };
struct StoreResult { StorageOutcome outcome{StorageOutcome::failed}; EntryVersion version; core::Error error; };
class Storage {
public:
    virtual ~Storage() = default;
    [[nodiscard]] virtual LookupResult lookup(const Key&) = 0;
    // expected is the key's observation token, including on a miss. Versions must
    // never wrap/reuse: exhaustion rejects mutation. Reject stale miss commits.
    [[nodiscard]] virtual StoreResult replace(const Key&, EntryVersion expected, Snapshot candidate) = 0;
    [[nodiscard]] virtual StoreResult erase(const Key&, EntryVersion expected) = 0;
};
enum class DecisionKind { hit, fetch, revalidate, bypass, cache_only_failure };
enum class PolicyReason { eligible, prohibited_request, prohibited_response, stale, missing, malformed, oversized, cache_only };
struct PolicyDecision {
    DecisionKind kind{DecisionKind::bypass};
    PolicyReason reason{PolicyReason::missing};
    bool lookup_allowed{}, store_allowed{}, network_allowed{true};
    std::chrono::seconds current_age{}, freshness_lifetime{};
    http::HeaderMap conditional_headers;
};
enum class ValidationOutcome { refresh_metadata, refetch, reject };
struct RevalidationDecision {
    ValidationOutcome outcome{ValidationOutcome::reject};
    EntryVersion expected;
    http::HeaderMap merged_headers;
    Timing timing;
};
class Policy {
public:
    virtual ~Policy() = default;
    [[nodiscard]] virtual PolicyDecision evaluate(const http::Request&, const Snapshot&, core::SteadyTimePoint, core::SystemTimePoint) const = 0;
    [[nodiscard]] virtual bool can_store(const http::Request&, const http::Response&, http::BodyFraming, bool complete) const = 0;
    [[nodiscard]] virtual core::Result<RevalidationDecision> revalidate(const Snapshot&, const http::Response&, const Timing&) const = 0;
};
} // namespace cache
