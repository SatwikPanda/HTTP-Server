#include "cache/policy/cache_policy.hpp"

#include <algorithm>
#include <array>

namespace cache::policy {
namespace {
bool token(char c) {
    const auto value = static_cast<unsigned char>(c);
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
        (value >= '0' && value <= '9') || std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
}
void whitespace(std::string_view& value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
}
void duration(DurationDirective& fact, std::string_view value, bool has_value, bool optional, Directives& result) {
    const bool seen = fact.present;
    fact.present = true;
    if (!has_value && optional) return;
    std::int64_t number{};
    bool valid = has_value && !value.empty();
    for (const char c : value) {
        if (c < '0' || c > '9' || number > (maximum_age - (c - '0')) / 10) { valid = false; break; }
        number = number * 10 + (c - '0');
    }
    if (!valid || (seen && fact.value.count() != number)) {
        fact.valid = false;
        result.malformed = true;
    } else fact.value = std::chrono::seconds(number);
}
}
Directives parse_cache_control(const http::HeaderMap& headers) noexcept {
    Directives result;
    std::size_t total{}, count{};
    if (headers.size() > 100) { result.malformed = true; return result; }
    for (const auto& field : headers) {
        if (!http::header_name_equals(field.name, "Cache-Control")) continue;
        if (field.value.size() > 65536 - total) { result.malformed = true; return result; }
        total += field.value.size();
        std::string_view remaining = field.value;
        whitespace(remaining);
        if (remaining.empty()) result.malformed = true;
        while (!remaining.empty()) {
            if (++count > 256) { result.malformed = true; return result; }
            std::size_t length{};
            while (length < remaining.size() && token(remaining[length])) ++length;
            if (!length) { result.malformed = true; return result; }
            const auto name = remaining.substr(0, length);
            remaining.remove_prefix(length);
            whitespace(remaining);
            // Preserve prohibitions even if the associated value is malformed.
            const auto is = [&](std::string_view wanted) { return http::header_name_equals(name, wanted); };
            if (is("only-if-cached")) result.only_if_cached = true;
            if (is("no-store")) result.no_store = true;
            if (is("no-cache")) result.no_cache = true;
            if (is("private")) result.private_response = true;
            if (is("must-revalidate")) result.must_revalidate = true;
            if (is("proxy-revalidate")) result.proxy_revalidate = true;
            bool has_value = !remaining.empty() && remaining.front() == '=';
            std::array<char, 8192> decoded{};
            std::string_view value;
            if (has_value) {
                remaining.remove_prefix(1); whitespace(remaining);
                if (!remaining.empty() && remaining.front() == '"') {
                    remaining.remove_prefix(1);
                    std::size_t used{};
                    bool closed = false;
                    while (!remaining.empty()) {
                        char c = remaining.front(); remaining.remove_prefix(1);
                        if (c == '"') { closed = true; break; }
                        if (c == '\\') {
                            if (remaining.empty()) break;
                            c = remaining.front(); remaining.remove_prefix(1);
                        }
                        const auto byte = static_cast<unsigned char>(c);
                        if ((byte < 32 && c != '\t') || byte == 127 || used == decoded.size()) {
                            result.malformed = true; return result;
                        }
                        decoded[used++] = c;
                    }
                    if (!closed) { result.malformed = true; return result; }
                    value = {decoded.data(), used};
                } else {
                    length = 0;
                    while (length < remaining.size() && token(remaining[length])) ++length;
                    if (!length) { result.malformed = true; return result; }
                    value = remaining.substr(0, length); remaining.remove_prefix(length);
                }
            }
            if (is("max-age")) duration(result.max_age, value, has_value, false, result);
            else if (is("s-maxage")) duration(result.shared_max_age, value, has_value, false, result);
            else if (is("min-fresh")) duration(result.min_fresh, value, has_value, false, result);
            else if (is("max-stale")) duration(result.max_stale, value, has_value, true, result);
            else if (has_value && (is("only-if-cached") || is("no-store") || is("must-revalidate") || is("proxy-revalidate")))
                result.malformed = true;
            whitespace(remaining);
            if (remaining.empty()) break;
            if (remaining.front() != ',') { result.malformed = true; return result; }
            remaining.remove_prefix(1); whitespace(remaining);
            if (remaining.empty()) result.malformed = true;
        }
    }
    return result;
}
std::string age_header(std::chrono::seconds age) {
    return std::to_string(std::clamp<std::int64_t>(age.count(), 0, maximum_age));
}
} // namespace cache::policy
