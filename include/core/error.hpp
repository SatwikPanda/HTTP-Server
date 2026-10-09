#pragma once

#include <string>
#include <string_view>

namespace core {

enum class ErrorCategory {
    none = 0,
    would_block,
    interrupted,
    connection_refused,
    connection_reset,
    connection_aborted,
    not_connected,
    already_connected,
    address_in_use,
    address_not_available,
    network_unreachable,
    host_unreachable,
    timed_out,
    invalid_argument,
    bad_descriptor,
    io_error,
    system_error,
    eof,
    unknown
};

[[nodiscard]] std::string_view to_string(ErrorCategory category) noexcept;

struct Error {
    ErrorCategory category{ErrorCategory::none};
    std::string operation{};
    int native_code{0};
    std::string message{};

    [[nodiscard]] bool is_error() const noexcept {
        return category != ErrorCategory::none;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return is_error();
    }

    [[nodiscard]] std::string to_string() const;
};

[[nodiscard]] Error make_error(
    ErrorCategory category,
    std::string operation,
    int native_code = 0,
    std::string message = {}
);

} // namespace core
