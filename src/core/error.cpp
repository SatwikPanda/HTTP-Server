#include "core/error.hpp"

#include <sstream>

namespace core {

std::string_view to_string(ErrorCategory category) noexcept {
    switch (category) {
        case ErrorCategory::none:
            return "none";
        case ErrorCategory::would_block:
            return "would_block";
        case ErrorCategory::interrupted:
            return "interrupted";
        case ErrorCategory::connection_refused:
            return "connection_refused";
        case ErrorCategory::connection_reset:
            return "connection_reset";
        case ErrorCategory::connection_aborted:
            return "connection_aborted";
        case ErrorCategory::not_connected:
            return "not_connected";
        case ErrorCategory::already_connected:
            return "already_connected";
        case ErrorCategory::address_in_use:
            return "address_in_use";
        case ErrorCategory::address_not_available:
            return "address_not_available";
        case ErrorCategory::network_unreachable:
            return "network_unreachable";
        case ErrorCategory::host_unreachable:
            return "host_unreachable";
        case ErrorCategory::timed_out:
            return "timed_out";
        case ErrorCategory::invalid_argument:
            return "invalid_argument";
        case ErrorCategory::bad_descriptor:
            return "bad_descriptor";
        case ErrorCategory::io_error:
            return "io_error";
        case ErrorCategory::system_error:
            return "system_error";
        case ErrorCategory::eof:
            return "eof";
        case ErrorCategory::unknown:
        default:
            return "unknown";
    }
}

std::string Error::to_string() const {
    if (!is_error()) {
        return "success";
    }

    std::ostringstream oss;
    if (!operation.empty()) {
        oss << "[" << operation << "] ";
    }
    oss << core::to_string(category);
    if (!message.empty()) {
        oss << ": " << message;
    }
    if (native_code != 0) {
        oss << " (native=" << native_code << ")";
    }
    return oss.str();
}

Error make_error(
    ErrorCategory category,
    std::string operation,
    int native_code,
    std::string message
) {
    return Error{
        category,
        std::move(operation),
        native_code,
        std::move(message)
    };
}

} // namespace core
