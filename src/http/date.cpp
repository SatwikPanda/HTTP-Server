#include "http/date.hpp"

#include <array>
#include <cstdio>

namespace http {

std::string format_http_date(std::chrono::system_clock::time_point tp) {
    using namespace std::chrono;
    // Calendar arithmetic avoids locale, shared tm storage, and OS-specific APIs.
    const auto day = floor<days>(tp);
    if (day < sys_days{year{0}/January/1} || day >= sys_days{year{10000}/January/1}) return {};
    const year_month_day date{day};
    const int year_value = static_cast<int>(date.year());
    if (!date.ok() || year_value < 0 || year_value > 9999) return {};
    const hh_mm_ss time{floor<seconds>(tp).time_since_epoch() -
                       duration_cast<seconds>(day.time_since_epoch())};
    constexpr const char* day_names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    constexpr const char* month_names[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    std::array<char, 30> buffer{};
    const int written = std::snprintf(buffer.data(), buffer.size(),
        "%s, %02u %s %04d %02d:%02d:%02d GMT",
        day_names[weekday{day}.c_encoding()], static_cast<unsigned>(date.day()),
        month_names[static_cast<unsigned>(date.month()) - 1], year_value,
        static_cast<int>(time.hours().count()), static_cast<int>(time.minutes().count()),
        static_cast<int>(time.seconds().count()));
    return written == 29 ? std::string(buffer.data(), 29) : std::string{};
}

} // namespace http
