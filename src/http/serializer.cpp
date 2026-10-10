#include "http/serializer.hpp"

namespace http {

core::Result<std::string> serialize_response_head(const Response& response) {
    if (!response.version.is_valid()) {
        return core::make_error(
            core::ErrorCategory::invalid_argument,
            "serialize_response_head",
            0,
            "Invalid HTTP version: major must be >= 1"
        );
    }

    if (!is_valid_status_code(response.status_code)) {
        return core::make_error(
            core::ErrorCategory::invalid_argument,
            "serialize_response_head",
            0,
            "Invalid HTTP status code: " + std::to_string(response.status_code)
        );
    }

    if (!is_valid_reason_phrase(response.reason)) {
        return core::make_error(
            core::ErrorCategory::invalid_argument,
            "serialize_response_head",
            0,
            "Invalid characters in reason phrase"
        );
    }

    // Validate headers
    for (const auto& field : response.headers) {
        if (!is_valid_header_name(field.name)) {
            return core::make_error(
                core::ErrorCategory::invalid_argument,
                "serialize_response_head",
                0,
                "Invalid HTTP header name: " + field.name
            );
        }
        if (!is_valid_header_value(field.value)) {
            return core::make_error(
                core::ErrorCategory::invalid_argument,
                "serialize_response_head",
                0,
                "Invalid HTTP header value for field: " + field.name
            );
        }
    }

    std::string out;
    out.reserve(256 + response.headers.size() * 64);

    // Status line: HTTP/1.1 200 OK\r\n
    out += response.version.to_string();
    out += ' ';
    out += std::to_string(response.status_code);
    out += ' ';
    out += response.reason;
    out += "\r\n";

    // Headers
    for (const auto& field : response.headers) {
        out += field.name;
        out += ": ";
        out += field.value;
        out += "\r\n";
    }

    // CRLF terminating the header block
    out += "\r\n";

    return out;
}

core::Result<std::string> serialize_response(const Response& response) {
    auto head_res = serialize_response_head(response);
    if (!head_res) {
        return head_res.error();
    }

    std::string out = std::move(head_res.value());
    if (!response.body.empty()) {
        out += response.body;
    }
    return out;
}

core::Result<std::string> serialize_request_head(const Request& request) {
    if (!request.version.is_valid()) {
        return core::make_error(
            core::ErrorCategory::invalid_argument,
            "serialize_request_head",
            0,
            "Invalid HTTP version"
        );
    }

    if (request.target.empty()) {
        return core::make_error(
            core::ErrorCategory::invalid_argument,
            "serialize_request_head",
            0,
            "Request target cannot be empty"
        );
    }

    for (const auto& field : request.headers) {
        if (!is_valid_header_name(field.name)) {
            return core::make_error(
                core::ErrorCategory::invalid_argument,
                "serialize_request_head",
                0,
                "Invalid HTTP header name: " + field.name
            );
        }
        if (!is_valid_header_value(field.value)) {
            return core::make_error(
                core::ErrorCategory::invalid_argument,
                "serialize_request_head",
                0,
                "Invalid HTTP header value for field: " + field.name
            );
        }
    }

    std::string out;
    out.reserve(256 + request.headers.size() * 64);

    // Request line: GET / HTTP/1.1\r\n
    out += request.raw_method.empty() ? std::string(to_string(request.method)) : request.raw_method;
    out += ' ';
    out += request.target;
    out += ' ';
    out += request.version.to_string();
    out += "\r\n";

    // Headers
    for (const auto& field : request.headers) {
        out += field.name;
        out += ": ";
        out += field.value;
        out += "\r\n";
    }

    // CRLF terminating header block
    out += "\r\n";

    return out;
}

} // namespace http
