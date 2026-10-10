#pragma once

#include <chrono>
#include <string>

namespace http {

// Formats a time_point according to RFC 9110 IMF-fixdate:
// Example: "Sun, 06 Nov 1994 08:49:37 GMT"
[[nodiscard]] std::string format_http_date(
    std::chrono::system_clock::time_point tp = std::chrono::system_clock::now()
);

} // namespace http
