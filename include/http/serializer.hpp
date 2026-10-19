#pragma once

#include "core/result.hpp"
#include "http/request.hpp"
#include "http/response.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

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

// Encodes a chunk of data with hex size and CRLF: "<hex_size>\r\n<data>\r\n"
[[nodiscard]] std::string encode_chunk(std::span<const std::byte> chunk_data);
[[nodiscard]] std::string encode_chunk(std::string_view chunk_data);

// Encodes the terminating 0 chunk and optional trailers: "0\r\n[trailers]\r\n"
[[nodiscard]] std::string encode_final_chunk(const HeaderMap* trailers = nullptr);

} // namespace http
