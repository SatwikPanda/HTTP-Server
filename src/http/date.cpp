#include "http/date.hpp"

#include <array>
#include <cstdio>
#include <ctime>

namespace http {

namespace {

constexpr const char* kDayNames[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

constexpr const char* kMonthNames[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

} // namespace

std::string format_http_date(std::chrono::system_clock::time_point tp) {
    std::time_t time_val = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_val{};

#ifdef _WIN32
    gmtime_s(&tm_val, &time_val);
#else
    gmtime_r(&time_val, &tm_val);
#endif

    const char* day_name = (tm_val.tm_wday >= 0 && tm_val.tm_wday < 7)
        ? kDayNames[tm_val.tm_wday]
        : "Sun";

    const char* month_name = (tm_val.tm_mon >= 0 && tm_val.tm_mon < 12)
        ? kMonthNames[tm_val.tm_mon]
        : "Jan";

    // Format: "Sun, 06 Nov 1994 08:49:37 GMT" (exactly 29 chars)
    std::array<char, 32> buffer{};
    std::snprintf(
        buffer.data(),
        buffer.size(),
        "%s, %02d %s %04d %02d:%02d:%02d GMT",
        day_name,
        tm_val.tm_mday,
        month_name,
        tm_val.tm_year + 1900,
        tm_val.tm_hour,
        tm_val.tm_min,
        tm_val.tm_sec
    );

    return std::string(buffer.data());
}

} // namespace http
