#include "http/limits.hpp"

namespace http {

core::Result<void> validate_request_limits(const RequestLimits& limits) {
    constexpr std::size_t hard_limit = 16 * 1024 * 1024;
    const auto in_range = [](std::size_t value) { return value >= 1 && value <= hard_limit; };
    if (!in_range(limits.max_request_line_bytes) || !in_range(limits.max_target_bytes) ||
        !in_range(limits.max_header_bytes) || !in_range(limits.max_header_count) ||
        !in_range(limits.max_body_bytes) || !in_range(limits.max_chunk_extension_bytes) ||
        !in_range(limits.max_trailer_bytes) || !in_range(limits.max_trailer_count)) {
        return core::make_error(core::ErrorCategory::invalid_argument, "HTTP request limits");
    }
    return {};
}

} // namespace http
