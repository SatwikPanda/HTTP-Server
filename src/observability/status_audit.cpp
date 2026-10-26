#include "observability/status_audit.hpp"

#include <algorithm>
#include <string>

namespace observability {

bool is_audited_status(http::StatusCode status) noexcept {
    return std::find(kAuditedStatusCodes.begin(), kAuditedStatusCodes.end(), status) != kAuditedStatusCodes.end();
}

StatusPolicy status_policy_for(http::StatusCode status, http::Method request_method) noexcept {
    StatusPolicy policy{};
    policy.status = status;
    policy.requires_allow_header = (status == http::StatusCode::MethodNotAllowed);

    const int code = static_cast<int>(status);
    if (request_method == http::Method::HEAD || (code >= 100 && code < 200) ||
        status == http::StatusCode::NoContent || status == http::StatusCode::NotModified) {
        policy.body_allowed = false;
    } else {
        policy.body_allowed = true;
    }

    switch (status) {
        case http::StatusCode::OK:
        case http::StatusCode::NotModified:
        case http::StatusCode::Forbidden:
        case http::StatusCode::NotFound:
        case http::StatusCode::MethodNotAllowed:
        case http::StatusCode::BadGateway:
        case http::StatusCode::GatewayTimeout:
            policy.forces_connection_close = false;
            break;
        case http::StatusCode::BadRequest:
        case http::StatusCode::RequestTimeout:
        case http::StatusCode::PayloadTooLarge:
        case http::StatusCode::UriTooLong:
        case http::StatusCode::ExpectationFailed:
        case http::StatusCode::HeaderFieldsTooLarge:
        case http::StatusCode::InternalServerError:
        case http::StatusCode::NotImplemented:
        case http::StatusCode::ServiceUnavailable:
        case http::StatusCode::HttpVersionNotSupported:
        default:
            policy.forces_connection_close = true;
            break;
    }

    switch (status) {
        case http::StatusCode::OK: policy.safe_public_body = "200 OK\r\n"; break;
        case http::StatusCode::NotModified: policy.safe_public_body = ""; break;
        case http::StatusCode::BadRequest: policy.safe_public_body = "400 Bad Request\r\n"; break;
        case http::StatusCode::Forbidden: policy.safe_public_body = "403 Forbidden\r\n"; break;
        case http::StatusCode::NotFound: policy.safe_public_body = "404 Not Found\r\n"; break;
        case http::StatusCode::MethodNotAllowed: policy.safe_public_body = "405 Method Not Allowed\r\n"; break;
        case http::StatusCode::RequestTimeout: policy.safe_public_body = "408 Request Timeout\r\n"; break;
        case http::StatusCode::PayloadTooLarge: policy.safe_public_body = "413 Payload Too Large\r\n"; break;
        case http::StatusCode::UriTooLong: policy.safe_public_body = "414 URI Too Long\r\n"; break;
        case http::StatusCode::ExpectationFailed: policy.safe_public_body = "417 Expectation Failed\r\n"; break;
        case http::StatusCode::HeaderFieldsTooLarge: policy.safe_public_body = "431 Request Header Fields Too Large\r\n"; break;
        case http::StatusCode::InternalServerError: policy.safe_public_body = "500 Internal Server Error\r\n"; break;
        case http::StatusCode::NotImplemented: policy.safe_public_body = "501 Not Implemented\r\n"; break;
        case http::StatusCode::BadGateway: policy.safe_public_body = "502 Bad Gateway\r\n"; break;
        case http::StatusCode::ServiceUnavailable: policy.safe_public_body = "503 Service Unavailable\r\n"; break;
        case http::StatusCode::GatewayTimeout: policy.safe_public_body = "504 Gateway Timeout\r\n"; break;
        case http::StatusCode::HttpVersionNotSupported: policy.safe_public_body = "505 HTTP Version Not Supported\r\n"; break;
        default: policy.safe_public_body = "500 Internal Server Error\r\n"; break;
    }
    return policy;
}

http::Response build_sanitized_error_response(
    http::StatusCode status,
    http::Method request_method,
    bool force_close
) {
    const int code = static_cast<int>(status);
    const auto normalized = (code >= 400 && code <= 599) ? status : http::StatusCode::InternalServerError;
    const auto policy = status_policy_for(normalized, request_method);

    http::Response response(normalized);
    if (policy.requires_allow_header) {
        response.headers.set("Allow", "GET, HEAD");
    }
    const bool close_conn = force_close || policy.forces_connection_close;
    response.headers.set("Connection", close_conn ? "close" : "keep-alive");
    response.headers.set("Content-Type", "text/plain; charset=utf-8");
    response.headers.set("Content-Length", std::to_string(policy.safe_public_body.size()));
    if (policy.body_allowed) {
        response.body = std::string(policy.safe_public_body);
    }
    return response;
}

} // namespace observability
