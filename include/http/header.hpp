#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace http {

struct HeaderField {
    std::string name;
    std::string value;

    bool operator==(const HeaderField& other) const = default;
};

[[nodiscard]] bool is_valid_header_name(std::string_view name) noexcept;

[[nodiscard]] bool is_valid_header_value(std::string_view value) noexcept;

[[nodiscard]] bool header_name_equals(std::string_view a, std::string_view b) noexcept;

class HeaderMap {
public:
    HeaderMap() = default;

    // Append a header field. Preserves repeated fields in insertion order.
    void add(std::string name, std::string value);

    // Replace all existing occurrences of name (case-insensitively), or append if not present.
    void set(std::string name, std::string value);

    // Check if a header exists (case-insensitive lookup).
    [[nodiscard]] bool contains(std::string_view name) const noexcept;

    // Get the first matching header value (case-insensitive lookup).
    [[nodiscard]] std::optional<std::string_view> get(std::string_view name) const noexcept;

    // Get all matching header values in order without joining them.
    [[nodiscard]] std::vector<std::string_view> get_all(std::string_view name) const;

    // Remove all header fields matching name (case-insensitively). Returns true if any were removed.
    bool remove(std::string_view name);

    [[nodiscard]] const std::vector<HeaderField>& entries() const noexcept {
        return fields_;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return fields_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return fields_.empty();
    }

    void clear() noexcept {
        fields_.clear();
    }

    auto begin() noexcept { return fields_.begin(); }
    auto end() noexcept { return fields_.end(); }
    auto begin() const noexcept { return fields_.begin(); }
    auto end() const noexcept { return fields_.end(); }
    auto cbegin() const noexcept { return fields_.cbegin(); }
    auto cend() const noexcept { return fields_.cend(); }

private:
    std::vector<HeaderField> fields_;
};

} // namespace http
