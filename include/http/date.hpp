#pragma once

#include "core/result.hpp"

#include <chrono>
#include <string>
#include <string_view>

namespace http {

// IMF-fixdate, RFC 850 and asctime. Invalid calendar/weekday/time returns error.
// RFC 850's two-digit year uses now for the >50-years-in-the-future rule.
[[nodiscard]] core::Result<std::chrono::system_clock::time_point> parse_http_date(
    std::string_view value, std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

// Formats a time_point according to RFC 9110 IMF-fixdate:
// Example: "Sun, 06 Nov 1994 08:49:37 GMT"
// Returns an empty string when the year cannot be represented in four digits.
[[nodiscard]] std::string format_http_date(
    std::chrono::system_clock::time_point tp = std::chrono::system_clock::now()
);

} // namespace http
