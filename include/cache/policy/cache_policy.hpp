#pragma once

#include "cache/contracts.hpp"

namespace cache::policy {

inline constexpr std::int64_t maximum_age = 2147483648LL;
struct DurationDirective {
    bool present{};
    bool valid{true};
    std::chrono::seconds value{};
};
struct Directives {
    bool malformed{}, no_store{}, no_cache{}, private_response{}, only_if_cached{};
    bool must_revalidate{}, proxy_revalidate{};
    DurationDirective max_age, shared_max_age, min_fresh, max_stale;
};

// Bounded, allocation-free parser over all occurrences, including quoted commas.
[[nodiscard]] Directives parse_cache_control(const http::HeaderMap&) noexcept;
[[nodiscard]] std::string age_header(std::chrono::seconds age);

struct Limits {
    std::size_t object_bytes{8 * 1024 * 1024};
};

class CachePolicy final : public Policy {
public:
    explicit CachePolicy(Limits limits = {}) : limits_(limits) {}
    [[nodiscard]] PolicyDecision evaluate(const http::Request&, const Snapshot&,
        core::SteadyTimePoint, core::SystemTimePoint) const override;
    [[nodiscard]] bool can_store(const http::Request&, const http::Response&,
        http::BodyFraming, bool complete) const override;
    // Preliminary metadata/framing gate only; completion and byte count must
    // still pass can_store_stream before a candidate may become resident.
    [[nodiscard]] bool can_store_head(const http::Request&, const http::Response&,
        http::BodyFraming) const;
    // Streaming callers must pass the parser's total transfer-decoded byte count;
    // Response::body is intentionally empty in that path.
    [[nodiscard]] bool can_store_stream(const http::Request&, const http::Response&,
        http::BodyFraming, bool complete, std::uint64_t body_bytes) const;
    [[nodiscard]] core::Result<RevalidationDecision> revalidate(const Snapshot&,
        const http::Response&, const Timing&) const override;
private:
    Limits limits_;
};

} // namespace cache::policy
