#pragma once

#include "core/result.hpp"
#include "http/request.hpp"
#include "http/response.hpp"

#include <cstddef>
#include <string>

namespace http {

struct SerializationLimits {
    std::size_t max_head_bytes{8192};
    std::size_t max_body_bytes{1024 * 1024};
};

// Serializes the status line and headers of an HTTP response with CRLF termination.
// Rejects invalid status codes, reason phrases, or header names/values.
[[nodiscard]] core::Result<std::string> serialize_response_head(
    const Response& response,
    SerializationLimits limits = {}
);

// Serializes a bounded fixed-length response. Content-Length must match the
// body; generates it if absent. Transfer-Encoding is unsupported until the
// framing milestone. Use the head-only API for HEAD representation metadata.
[[nodiscard]] core::Result<std::string> serialize_response(
    const Response& response,
    SerializationLimits limits = {}
);

// Serializes the request line and headers of an HTTP request with CRLF termination.
[[nodiscard]] core::Result<std::string> serialize_request_head(
    const Request& request,
    SerializationLimits limits = {}
);

} // namespace http
