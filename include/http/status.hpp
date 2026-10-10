#pragma once

#include <cstdint>
#include <string_view>

namespace http {

enum class StatusCode : int {
    Continue = 100,
    SwitchingProtocols = 101,

    OK = 200,
    Created = 201,
    Accepted = 202,
    NoContent = 204,

    MovedPermanently = 301,
    Found = 302,
    SeeOther = 303,
    NotModified = 304,

    BadRequest = 400,
    Forbidden = 403,
    NotFound = 404,
    MethodNotAllowed = 405,
    RequestTimeout = 408,
    Conflict = 409,
    PayloadTooLarge = 413,
    UriTooLong = 414,
    ExpectationFailed = 417,
    HeaderFieldsTooLarge = 431,

    InternalServerError = 500,
    NotImplemented = 501,
    BadGateway = 502,
    ServiceUnavailable = 503,
    GatewayTimeout = 504,
    HttpVersionNotSupported = 505
};

[[nodiscard]] std::string_view default_reason_phrase(StatusCode code) noexcept;

[[nodiscard]] std::string_view default_reason_phrase(int code) noexcept;

[[nodiscard]] bool is_valid_status_code(int code) noexcept;

[[nodiscard]] bool is_valid_reason_phrase(std::string_view phrase) noexcept;

} // namespace http
