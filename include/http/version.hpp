#pragma once

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

namespace http {

struct HttpVersion {
    std::uint16_t major{1};
    std::uint16_t minor{1};

    static constexpr HttpVersion http_1_0() noexcept { return {1, 0}; }
    static constexpr HttpVersion http_1_1() noexcept { return {1, 1}; }

    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] bool is_valid() const noexcept {
        return major >= 1;
    }

    bool operator==(const HttpVersion& other) const = default;
    auto operator<=>(const HttpVersion& other) const = default;
};

} // namespace http
