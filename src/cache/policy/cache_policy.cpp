#include "cache/policy/cache_policy.hpp"

#include "http/date.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <new>

namespace cache::policy {
namespace {
using Seconds = std::chrono::seconds;
constexpr std::size_t metadata_limit = 65536;
bool bounded(const http::HeaderMap& headers) {
    if (headers.size() > 100) return false;
    std::size_t size{};
    for (const auto& field : headers) {
        if (field.name.size() > metadata_limit - size) return false;
        size += field.name.size();
        if (field.value.size() > metadata_limit - size) return false;
        size += field.value.size();
        if (!http::is_valid_header_name(field.name) || !http::is_valid_header_value(field.value)) return false;
    }
    return true;
}
bool request_allowed(const http::Request& request, const Directives& directives) {
    if (request.method != http::Method::GET || !request.body.empty() || !bounded(request.headers) ||
        directives.no_store || directives.malformed) return false;
    for (const auto field : {"Authorization", "Cookie", "Range", "If-Match", "If-None-Match",
            "If-Modified-Since", "If-Unmodified-Since", "If-Range"})
        if (request.headers.contains(field)) return false;
    return true;
}
bool response_allowed(int status, const http::HeaderMap& headers, const Directives& directives) {
    return status == 200 && bounded(headers) && !directives.malformed && !directives.no_store &&
        !directives.private_response && !headers.contains("Set-Cookie") && !headers.contains("Vary") &&
        !headers.contains("Content-Range");
}
std::optional<std::string_view> single(const http::HeaderMap& headers, std::string_view name) {
    std::optional<std::string_view> result;
    for (const auto& field : headers) if (http::header_name_equals(field.name, name)) {
        if (result) return {};
        result = field.value;
    }
    return result;
}
std::optional<core::SystemTimePoint> date(const http::HeaderMap& headers, std::string_view name, core::SystemTimePoint now) {
    auto value = single(headers, name);
    if (!value) return {};
    auto parsed = http::parse_http_date(*value, now);
    return parsed ? std::optional(parsed.value()) : std::nullopt;
}
std::optional<http::EntityTag> etag(const http::HeaderMap& headers) {
    auto value = single(headers, "ETag");
    if (!value) return {};
    auto parsed = http::parse_validator_list(*value);
    if (!parsed || parsed->wildcard || parsed->tags.size() != 1) return {};
    return parsed->tags.front();
}
std::optional<std::uint64_t> number(std::string_view value) {
    if (value.empty()) return {};
    std::uint64_t parsed{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size()) return {};
    return parsed;
}
// Convert before subtracting, avoiding signed native clock-duration overflow.
template<class Time> long double elapsed(Time later, Time earlier) {
    const auto seconds = std::chrono::duration<long double>(later.time_since_epoch()).count() -
        std::chrono::duration<long double>(earlier.time_since_epoch()).count();
    return std::clamp(seconds, 0.0L, static_cast<long double>(maximum_age));
}
Seconds saturated(long double value) {
    return Seconds(static_cast<Seconds::rep>(std::clamp(std::ceil(value), 0.0L, static_cast<long double>(maximum_age))));
}
struct Freshness { Seconds age{}, lifetime{}; };
Freshness freshness(const Entry& entry, const Directives& control, core::SteadyTimePoint now) {
    Freshness result;
    auto origin_date = date(entry.headers, "Date", entry.timing.final_head_wall);
    if (!origin_date) return {Seconds(maximum_age), Seconds(0)};
    std::uint64_t received_age{};
    if (entry.headers.contains("Age")) {
        auto text = single(entry.headers, "Age");
        auto parsed = text ? number(*text) : std::nullopt;
        if (!parsed) return {Seconds(maximum_age), Seconds(0)};
        received_age = std::min<std::uint64_t>(*parsed, maximum_age);
    }
    const auto initial = std::max(elapsed(entry.timing.final_head_wall, *origin_date),
        static_cast<long double>(received_age) + elapsed(entry.timing.final_head_received, entry.timing.request_sent));
    result.age = saturated(initial + elapsed(now, entry.timing.final_head_received));
    if (control.shared_max_age.present) result.lifetime = control.shared_max_age.value;
    else if (control.max_age.present) result.lifetime = control.max_age.value;
    else if (auto expires = date(entry.headers, "Expires", entry.timing.final_head_wall))
        result.lifetime = Seconds(static_cast<Seconds::rep>(elapsed(*expires, *origin_date)));
    return result;
}
http::HeaderMap condition(const Entry& entry) {
    http::HeaderMap result;
    // Derive from immutable headers, avoiding mismatched cached convenience fields.
    if (auto tag = etag(entry.headers)) result.add("If-None-Match", (tag->weak ? "W/" : "") + tag->quoted);
    else if (auto modified = date(entry.headers, "Last-Modified", entry.timing.final_head_wall))
        result.add("If-Modified-Since", http::format_http_date(*modified));
    return result;
}
bool matches_key(const http::Request& request, const Key& key) {
    auto parsed = http::parse_target(request.target, request.headers.get("Host").value_or(""));
    return parsed && parsed.value() == key;
}
bool hop_or_framing(std::string_view name) {
    for (const auto field : {"Connection", "Keep-Alive", "Proxy-Authenticate", "Proxy-Authorization",
            "TE", "Trailer", "Transfer-Encoding", "Upgrade", "Content-Length", "Proxy-Connection"})
        if (http::header_name_equals(name, field)) return true;
    return false;
}
bool nominated(const http::HeaderMap& headers, std::string_view name) {
    for (const auto& field : headers) if (http::header_name_equals(field.name, "Connection")) {
        std::string_view list = field.value;
        while (!list.empty()) {
            const auto comma = list.find(',');
            auto token = list.substr(0, comma);
            while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) token.remove_prefix(1);
            while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) token.remove_suffix(1);
            if (http::header_name_equals(token, name)) return true;
            if (comma == std::string_view::npos) break;
            list.remove_prefix(comma + 1);
        }
    }
    return false;
}
bool nominated_cache_metadata(const http::HeaderMap& headers) {
    // Sanitization must not turn no-cache into freshness, erase received Age,
    // or relabel an encoded representation. Such responses may be forwarded
    // but cannot safely participate in cache capture/metadata refresh.
    for (const auto name : {"Cache-Control", "Date", "Age", "Expires", "ETag", "Last-Modified",
            "Vary", "Set-Cookie", "Content-Encoding", "Content-Type", "Content-Location",
            "Content-Range", "Content-Length"})
        if (nominated(headers, name)) return true;
    return false;
}
}

PolicyDecision CachePolicy::evaluate(const http::Request& request, const Snapshot& snapshot,
    core::SteadyTimePoint now, core::SystemTimePoint wall_now) const {
    (void)wall_now; // Receipt wall time is frozen; later clock jumps cannot change age.
    PolicyDecision decision;
    const auto request_control = parse_cache_control(request.headers);
    // Malformed request directives fail closed, including an unparseable suffix
    // that could hide only-if-cached. No bypass path may start network activity.
    const bool cache_only = request_control.only_if_cached || request_control.malformed;
    decision.network_allowed = !cache_only;
    auto fail_cache_only = [&] {
        if (cache_only) {
            decision.kind = DecisionKind::cache_only_failure;
            decision.reason = request_control.malformed ? PolicyReason::malformed : PolicyReason::cache_only;
            decision.store_allowed = false;
            decision.conditional_headers.clear();
        }
    };
    if (!request_allowed(request, request_control)) {
        decision.reason = PolicyReason::prohibited_request;
        fail_cache_only();
        return decision;
    }
    decision.lookup_allowed = true;
    decision.store_allowed = !cache_only;
    decision.kind = DecisionKind::fetch;
    decision.reason = PolicyReason::missing;
    if (!snapshot || !snapshot->body || !matches_key(request, snapshot->key)) {
        fail_cache_only(); return decision;
    }
    const auto response_control = parse_cache_control(snapshot->headers);
    if (!response_allowed(snapshot->status, snapshot->headers, response_control) || snapshot->body->bytes().size() > limits_.object_bytes) {
        decision.reason = PolicyReason::prohibited_response;
        fail_cache_only(); return decision;
    }
    const auto times = freshness(*snapshot, response_control, now);
    decision.current_age = times.age;
    decision.freshness_lifetime = times.lifetime;
    const bool fresh = times.age < times.lifetime && !request_control.no_cache && !response_control.no_cache &&
        (!request_control.max_age.present || times.age <= request_control.max_age.value) &&
        (!request_control.min_fresh.present || request_control.min_fresh.value < times.lifetime - times.age);
    if (fresh) {
        decision.kind = DecisionKind::hit;
        decision.reason = PolicyReason::eligible;
        decision.network_allowed = false;
        decision.store_allowed = false;
        return decision;
    }
    decision.reason = PolicyReason::stale;
    decision.conditional_headers = condition(*snapshot);
    if (!decision.conditional_headers.empty()) decision.kind = DecisionKind::revalidate;
    fail_cache_only();
    return decision;
}
bool CachePolicy::can_store(const http::Request& request, const http::Response& response,
    http::BodyFraming framing, bool complete) const {
    return can_store_stream(request, response, framing, complete, response.body.size());
}
bool CachePolicy::can_store_stream(const http::Request& request, const http::Response& response,
    http::BodyFraming framing, bool complete, std::uint64_t body_bytes) const {
    if (!complete || body_bytes > limits_.object_bytes || !can_store_head(request, response, framing)) return false;
    if (response.headers.contains("Content-Length")) {
        const auto selected = http::select_response_framing(response);
        if (selected.content_length != body_bytes) return false;
    }
    if (!bounded(response.trailers)) return false;
    for (const auto& field : response.trailers) if (http::is_prohibited_trailer(field.name, &response.headers)) return false;
    return true;
}
bool CachePolicy::can_store_head(const http::Request& request, const http::Response& response,
    http::BodyFraming framing) const {
    const auto request_control = parse_cache_control(request.headers);
    const auto response_control = parse_cache_control(response.headers);
    if (request_control.only_if_cached || nominated_cache_metadata(response.headers) ||
        !request_allowed(request, request_control) || !response_allowed(response.status_code, response.headers, response_control) ||
        (framing != http::BodyFraming::fixed_length && framing != http::BodyFraming::chunked)) return false;
    if (response.headers.contains("Content-Length") || response.headers.contains("Transfer-Encoding")) {
        const auto selected = http::select_response_framing(response);
        if (selected.error != http::ParseError::none || selected.framing != framing ||
            (framing == http::BodyFraming::fixed_length && selected.content_length > limits_.object_bytes)) return false;
    }
    // No heuristic lifetime. Zero/absent freshness is worth retaining only with
    // a syntactically useful validator. Actual date interpretation uses Timing.
    const auto origin_date = date(response.headers, "Date", {});
    const auto expires = date(response.headers, "Expires", {});
    const bool lifetime = origin_date && (response_control.shared_max_age.present ? response_control.shared_max_age.value > Seconds(0) :
        response_control.max_age.present ? response_control.max_age.value > Seconds(0) : expires && *expires > *origin_date);
    return lifetime || etag(response.headers).has_value() || date(response.headers, "Last-Modified", {}).has_value();
}
core::Result<RevalidationDecision> CachePolicy::revalidate(const Snapshot& selected,
    const http::Response& response, const Timing& timing) const {
    if (!selected || !selected->body || !selected->version.value)
        return core::make_error(core::ErrorCategory::invalid_argument, "cache validation snapshot");
    RevalidationDecision decision{ValidationOutcome::refetch, selected->version, {}, timing};
    if (!bounded(selected->headers) || !bounded(response.headers) || response.status_code != 304 ||
        !response.body.empty() || !response.trailers.empty()) return decision;
    try {
        const auto control = parse_cache_control(response.headers);
        if (!response_allowed(selected->status, selected->headers, parse_cache_control(selected->headers)) ||
            selected->body->bytes().size() > limits_.object_bytes || !response_allowed(200, response.headers, control)) {
            decision.outcome = ValidationOutcome::reject; return decision;
        }
        const auto old_tag = etag(selected->headers);
        const auto new_tag = etag(response.headers);
        if (old_tag) {
            if (!new_tag || !http::weak_match(*old_tag, *new_tag) || (!new_tag->weak && old_tag->weak)) return decision;
        } else {
            const auto old_date = date(selected->headers, "Last-Modified", selected->timing.final_head_wall);
            const auto new_date = date(response.headers, "Last-Modified", timing.final_head_wall);
            if (!old_date || !new_date || old_date != new_date || response.headers.contains("ETag")) return decision;
        }
        if (nominated_cache_metadata(response.headers)) return decision;
        // Representation metadata cannot relabel the retained bytes.
        for (const auto field : {"Content-Encoding", "Content-Type", "Content-Location"}) {
            if (response.headers.contains(field) && response.headers.get_all(field) != selected->headers.get_all(field)) {
                decision.outcome = ValidationOutcome::reject; return decision;
            }
        }
        if (response.headers.contains("Content-Length")) {
            auto length = single(response.headers, "Content-Length");
            auto parsed = length ? number(*length) : std::nullopt;
            if (!parsed || *parsed != selected->body->bytes().size()) return decision;
        }
        if (response.headers.contains("Transfer-Encoding")) return decision;
        if (!date(response.headers, "Date", timing.final_head_wall)) return decision;
        for (const auto& field : selected->headers) {
            if (!hop_or_framing(field.name) && !nominated(selected->headers, field.name) && !nominated(response.headers, field.name) &&
                !http::header_name_equals(field.name, "Age")) decision.merged_headers.add(field.name, field.value);
        }
        // Replace occurrences as a group so repeated end-to-end fields survive.
        for (const auto& field : response.headers) {
            if (!hop_or_framing(field.name) && !nominated(response.headers, field.name)) decision.merged_headers.remove(field.name);
        }
        for (const auto& field : response.headers) {
            if (!hop_or_framing(field.name) && !nominated(response.headers, field.name)) decision.merged_headers.add(field.name, field.value);
        }
        if (!decision.merged_headers.contains("Age")) decision.merged_headers.add("Age", "0");
        if (!response_allowed(200, decision.merged_headers, parse_cache_control(decision.merged_headers))) {
            decision.outcome = ValidationOutcome::reject;
            decision.merged_headers.clear();
            return decision;
        }
        decision.outcome = ValidationOutcome::refresh_metadata;
        return decision;
    } catch (const std::bad_alloc&) {
        return core::make_error(core::ErrorCategory::system_error, "cache validation allocation");
    }
}
} // namespace cache::policy
