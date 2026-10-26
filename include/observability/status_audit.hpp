#pragma once

#include "http/method.hpp"
#include "http/response.hpp"
#include "http/status.hpp"

#include <array>
#include <string_view>

namespace observability {

inline constexpr std::array<http::StatusCode, 17> kAuditedStatusCodes{
    http::StatusCode::OK,                  // 200
    http::StatusCode::NotModified,         // 304
    http::StatusCode::BadRequest,          // 400
    http::StatusCode::Forbidden,           // 403
    http::StatusCode::NotFound,            // 404
    http::StatusCode::MethodNotAllowed,    // 405
    http::StatusCode::RequestTimeout,      // 408
    http::StatusCode::PayloadTooLarge,     // 413
    http::StatusCode::UriTooLong,          // 414
    http::StatusCode::ExpectationFailed,   // 417
    http::StatusCode::HeaderFieldsTooLarge, // 431
    http::StatusCode::InternalServerError, // 500
    http::StatusCode::NotImplemented,      // 501
    http::StatusCode::BadGateway,          // 502
    http::StatusCode::ServiceUnavailable,  // 503
    http::StatusCode::GatewayTimeout,      // 504
    http::StatusCode::HttpVersionNotSupported // 505
};

struct StatusPolicy {
    http::StatusCode status{http::StatusCode::InternalServerError};
    bool body_allowed{true};
    bool requires_allow_header{false};
    bool forces_connection_close{false};
    std::string_view safe_public_body{};
};

[[nodiscard]] bool is_audited_status(http::StatusCode status) noexcept;
[[nodiscard]] StatusPolicy status_policy_for(http::StatusCode status, http::Method request_method = http::Method::GET) noexcept;

// Returns true only when no downstream wire bytes have been committed yet.
[[nodiscard]] constexpr bool can_emit_error_response(bool downstream_committed) noexcept {
    return !downstream_committed;
}

// Builds a sanitized error response that never leaks local filesystem paths or native error text.
[[nodiscard]] http::Response build_sanitized_error_response(
    http::StatusCode status,
    http::Method request_method = http::Method::GET,
    bool force_close = true
);

} // namespace observability
