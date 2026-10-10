#pragma once

#include "core/result.hpp"
#include "http/request.hpp"
#include "http/response.hpp"

#include <string>

namespace http {

// Serializes the status line and headers of an HTTP response with CRLF termination.
// Rejects invalid status codes, reason phrases, or header names/values.
[[nodiscard]] core::Result<std::string> serialize_response_head(
    const Response& response
);

// Serializes the complete HTTP response including body.
[[nodiscard]] core::Result<std::string> serialize_response(
    const Response& response
);

// Serializes the request line and headers of an HTTP request with CRLF termination.
[[nodiscard]] core::Result<std::string> serialize_request_head(
    const Request& request
);

} // namespace http
