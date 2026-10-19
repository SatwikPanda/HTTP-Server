#pragma once
#include "core/result.hpp"
#include <cstdint>
#include <string>
#include <string_view>

namespace http {
struct Authority {
    std::string host; // lower-case, without IPv6 brackets
    std::uint16_t port{80};
    bool ipv6{};
    [[nodiscard]] std::string wire() const;
    bool operator==(const Authority&) const = default;
};
struct Target {
    std::string scheme{"http"};
    Authority authority;
    std::string path_query; // escaped octets and query order preserved
    [[nodiscard]] std::string key() const;
    bool operator==(const Target&) const = default;
};
[[nodiscard]] core::Result<Authority> parse_authority(std::string_view text);
// origin_authority is required for origin-form and ignored for absolute-form.
[[nodiscard]] core::Result<Target> parse_target(std::string_view text, std::string_view origin_authority = {});
} // namespace http
