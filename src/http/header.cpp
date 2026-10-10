#include "http/header.hpp"

#include <cctype>

namespace http {

namespace {

inline bool is_tchar(unsigned char c) noexcept {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        return true;
    }
    switch (c) {
        case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
        case '+': case '-': case '.': case '^': case '_': case '`': case '|':
        case '~':
            return true;
        default:
            return false;
    }
}

inline char to_lower_ascii(char c) noexcept {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c + 32);
    }
    return c;
}

} // namespace

bool is_valid_header_name(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    for (char c : name) {
        if (!is_tchar(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

bool is_valid_header_value(std::string_view value) noexcept {
    for (unsigned char c : value) {
        // Must reject CR, LF, NUL, and other non-HTAB control characters or DEL
        if (c == '\r' || c == '\n' || c == '\0' || c == 0x7F) {
            return false;
        }
        if (c < 0x20 && c != 0x09) {
            return false;
        }
    }
    return true;
}

bool header_name_equals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (to_lower_ascii(a[i]) != to_lower_ascii(b[i])) {
            return false;
        }
    }
    return true;
}

void HeaderMap::add(std::string name, std::string value) {
    fields_.push_back(HeaderField{std::move(name), std::move(value)});
}

void HeaderMap::set(std::string name, std::string value) {
    bool replaced = false;
    auto it = fields_.begin();
    while (it != fields_.end()) {
        if (header_name_equals(it->name, name)) {
            if (!replaced) {
                it->value = std::move(value);
                replaced = true;
                ++it;
            } else {
                it = fields_.erase(it);
            }
        } else {
            ++it;
        }
    }
    if (!replaced) {
        fields_.push_back(HeaderField{std::move(name), std::move(value)});
    }
}

bool HeaderMap::contains(std::string_view name) const noexcept {
    for (const auto& field : fields_) {
        if (header_name_equals(field.name, name)) {
            return true;
        }
    }
    return false;
}

std::optional<std::string_view> HeaderMap::get(std::string_view name) const noexcept {
    for (const auto& field : fields_) {
        if (header_name_equals(field.name, name)) {
            return field.value;
        }
    }
    return std::nullopt;
}

std::vector<std::string_view> HeaderMap::get_all(std::string_view name) const {
    std::vector<std::string_view> result;
    for (const auto& field : fields_) {
        if (header_name_equals(field.name, name)) {
            result.push_back(field.value);
        }
    }
    return result;
}

bool HeaderMap::remove(std::string_view name) {
    auto initial_size = fields_.size();
    auto it = fields_.begin();
    while (it != fields_.end()) {
        if (header_name_equals(it->name, name)) {
            it = fields_.erase(it);
        } else {
            ++it;
        }
    }
    return fields_.size() != initial_size;
}

} // namespace http
