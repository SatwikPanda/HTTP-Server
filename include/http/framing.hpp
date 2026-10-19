#pragma once

#include "core/result.hpp"
#include "http/header.hpp"
#include "http/request.hpp"
#include "http/request_parser.hpp"
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

// RFC 9112 section 7.1.2: prohibited trailer fields that must never appear in trailers.
[[nodiscard]] bool is_prohibited_trailer(std::string_view name) noexcept;

// Enforces application boundary restrictions:
// - Bodyless GET/HEAD: reject nonempty bodies with 400 Bad Request
// - Unsupported Expect: reject with 417 Expectation Failed
// - Reject CONNECT and Upgrade
// - Disallowed methods (POST, PUT, DELETE, OPTIONS, TRACE): reject with 405 Method Not Allowed
// - Unknown methods: reject with 501 Not Implemented
[[nodiscard]] StatusCode check_request_restrictions(const Request& request) noexcept;

} // namespace http
