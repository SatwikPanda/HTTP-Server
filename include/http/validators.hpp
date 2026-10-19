#pragma once
#include "core/result.hpp"
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace http {
struct EntityTag {
    bool weak{};
    std::string quoted; // includes quotes; commas and backslashes are opaque
    bool operator==(const EntityTag&) const = default;
};
struct ValidatorList { bool wildcard{}; std::vector<EntityTag> tags; };
[[nodiscard]] core::Result<ValidatorList> parse_validator_list(std::string_view value, std::size_t max_bytes = 8192, std::size_t max_tags = 100);
[[nodiscard]] bool weak_match(const EntityTag& left, const EntityTag& right) noexcept;
} // namespace http
