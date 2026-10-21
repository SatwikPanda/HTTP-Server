#include "server/conditional.hpp"

#include "http/date.hpp"
#include "http/validators.hpp"

#include <chrono>

namespace server {
namespace {
std::string hex(std::uint64_t value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    do { text.insert(text.begin(), digits[value & 0xF]); value >>= 4; } while (value != 0);
    return text;
}

http::EntityTag current_tag(const Representation& representation) {
    return {true, '"' + hex(representation.size) + '-' +
        hex(static_cast<std::uint64_t>(representation.modified.time_since_epoch().count())) + '"'};
}
} // namespace

std::string make_etag(const Representation& representation) {
    return "W/" + current_tag(representation).quoted;
}

std::string make_last_modified(const Representation& representation) {
    return http::format_http_date(representation.modified);
}

Conditional evaluate_conditions(const http::HeaderMap& headers, const Representation& representation,
                                core::SystemTimePoint now) {
    if (headers.contains("If-None-Match")) {
        // Repeated field lines are one comma-separated list.
        std::string joined;
        for (const auto value : headers.get_all("If-None-Match")) {
            if (!joined.empty()) joined += ", ";
            joined += value;
        }
        auto parsed = http::parse_validator_list(joined);
        if (!parsed) return Conditional::proceed;
        if (parsed.value().wildcard) return Conditional::not_modified;
        const auto current = current_tag(representation);
        for (const auto& tag : parsed.value().tags) {
            if (http::weak_match(tag, current)) return Conditional::not_modified;
        }
        return Conditional::proceed;
    }
    const auto dates = headers.get_all("If-Modified-Since");
    if (dates.size() != 1) return Conditional::proceed;
    auto since = http::parse_http_date(dates.front(), now);
    if (!since || make_last_modified(representation).empty()) return Conditional::proceed;
    return std::chrono::floor<std::chrono::seconds>(representation.modified) <= since.value()
        ? Conditional::not_modified : Conditional::proceed;
}
} // namespace server
