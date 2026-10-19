#pragma once

#include "core/result.hpp"
#include "http/framing.hpp"
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

// Serializes a complete bounded response. Selects chunked output when that
// Transfer-Encoding is present, otherwise fixed length. Bodyless statuses emit
// neither body nor final chunk; 304 may retain representation length metadata.
[[nodiscard]] core::Result<std::string> serialize_response(
    const Response& response,
    SerializationLimits limits = {}
);

// HEAD sends representation metadata without body bytes (including a populated
// representation body). No-body statuses still reject a populated body.
[[nodiscard]] core::Result<std::string> serialize_response(
    const Response& response, Method originating_method, SerializationLimits limits = {}
);

// Explicit output framing. Chunked generates TE if absent; fixed length generates
// CL if absent. Close-delimited output forbids CL/TE and generates Connection:
// close. The caller must close the transport after sending it. Conflicting
// existing framing is rejected. HEAD/status semantics override body emission.
[[nodiscard]] core::Result<std::string> serialize_response(
    const Response& response, BodyFraming framing, Method originating_method = Method::GET,
    SerializationLimits limits = {}
);

// Serializes the request line and headers of an HTTP request with CRLF termination.
[[nodiscard]] core::Result<std::string> serialize_request_head(
    const Request& request,
    SerializationLimits limits = {}
);

// Encodes a chunk of data with hex size and CRLF: "<hex_size>\r\n<data>\r\n"
// Empty data encodes a final chunk; do not follow it with another final chunk.
// max_body_bytes bounds data; max_head_bytes bounds the size line and CRLFs.
[[nodiscard]] core::Result<std::string> encode_chunk(
    std::span<const std::byte> chunk_data, SerializationLimits limits = {});
[[nodiscard]] core::Result<std::string> encode_chunk(
    std::string_view chunk_data, SerializationLimits limits = {});

// Encodes the terminating 0 chunk and optional trailers: "0\r\n[trailers]\r\n"
// Validates trailer fields and bounds the entire final chunk by max_head_bytes.
[[nodiscard]] core::Result<std::string> encode_final_chunk(
    const HeaderMap* trailers = nullptr, SerializationLimits limits = {});

} // namespace http
