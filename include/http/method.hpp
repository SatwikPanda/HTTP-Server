#pragma once

#include <optional>
#include <string_view>

namespace http {

enum class Method {
    GET,
    HEAD,
    POST,
    PUT,
    DELETE,
    CONNECT,
    OPTIONS,
    TRACE,
    UNKNOWN
};

[[nodiscard]] std::string_view to_string(Method method) noexcept;

[[nodiscard]] std::optional<Method> method_from_string(std::string_view method_str) noexcept;

[[nodiscard]] bool is_safe_method(Method method) noexcept;

[[nodiscard]] bool is_idempotent_method(Method method) noexcept;

} // namespace http
