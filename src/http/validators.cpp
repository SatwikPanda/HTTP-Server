#include "http/validators.hpp"

namespace http {
core::Result<ValidatorList> parse_validator_list(std::string_view value, std::size_t max_bytes, std::size_t max_tags) {
    const auto invalid = [] { return core::make_error(core::ErrorCategory::invalid_argument, "HTTP validators"); };
    if (value.size() > max_bytes) return invalid();
    auto trim = [&] {
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    };
    trim();
    if (value.starts_with('*')) {
        value.remove_prefix(1); trim();
        if (!value.empty()) return invalid();
        return ValidatorList{true, {}};
    }
    ValidatorList result;
    while (!value.empty()) {
        if (result.tags.size() >= max_tags) return invalid();
        const bool weak = value.starts_with("W/");
        if (weak) value.remove_prefix(2);
        if (value.empty() || value.front() != '"') return invalid();
        const auto end = value.find('"', 1);
        if (end == std::string_view::npos) return invalid();
        for (const unsigned char c : value.substr(1, end - 1)) if (c < 0x21 || c == 0x7f) return invalid();
        result.tags.push_back({weak, std::string(value.substr(0, end + 1))});
        value.remove_prefix(end + 1); trim();
        if (value.empty()) return result;
        if (value.front() != ',') return invalid();
        value.remove_prefix(1); trim();
        if (value.empty()) return invalid();
    }
    return invalid();
}
bool weak_match(const EntityTag& left, const EntityTag& right) noexcept { return left.quoted == right.quoted; }
} // namespace http
