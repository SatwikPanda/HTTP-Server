#pragma once

#include "core/result.hpp"
#include "http/header.hpp"
#include "http/request.hpp"
#include "http/request_parser.hpp"
#include "http/response.hpp"
#include "http/status.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace http {

enum class BodyFraming {
    none,
    fixed_length,
    chunked,
    close_delimited
};

[[nodiscard]] std::string_view to_string(BodyFraming framing) noexcept;

struct RequestFraming {
    BodyFraming framing{BodyFraming::none};
    std::uint64_t content_length{0};
    ParseError error{ParseError::none};
};

// Inspects request headers and determines message body framing per RFC 9112 section 6.
// Detects ambiguous framing (TE + CL), conflicting or invalid Content-Length,
// numeric overflow, and validates Transfer-Encoding.
[[nodiscard]] RequestFraming select_request_framing(const HeaderMap& headers) noexcept;

// Determines response body framing per RFC 9112 section 6.3:
// - HEAD request -> none
// - 1xx, 204, 304 -> none
// - The only supported transfer coding, chunked -> chunked
// - Content-Length -> fixed_length
// - Otherwise -> close_delimited
// Rejects ambiguous/invalid lengths, unsupported codings, HTTP/1.0 TE,
// framing fields on 1xx/204, 101, and successful CONNECT tunnel responses.
[[nodiscard]] RequestFraming select_response_framing(const Response& response, Method originating_method = Method::GET) noexcept;

// RFC 9112 section 7.1.2: prohibited trailer fields that must never appear in trailers.
[[nodiscard]] bool is_prohibited_trailer(std::string_view name, const HeaderMap* head = nullptr) noexcept;

// Validates the suffix after chunk-size, including semicolons and optional BWS.
// Unknown extension names are allowed; malformed tokens/quoted strings are not.
[[nodiscard]] bool is_valid_chunk_extensions(std::string_view text) noexcept;

// Enforces application boundary restrictions:
// - Bodyless GET/HEAD: reject nonempty bodies with 400 Bad Request
// - Unsupported Expect: reject with 417 Expectation Failed
// - Reject CONNECT and Upgrade
// - Disallowed methods (POST, PUT, DELETE, OPTIONS, TRACE): reject with 405 Method Not Allowed
// - Unknown methods: reject with 501 Not Implemented
[[nodiscard]] StatusCode check_request_restrictions(const Request& request) noexcept;

} // namespace http
