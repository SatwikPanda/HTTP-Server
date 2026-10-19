#include "http/date.hpp"
#include <array>
#include <charconv>

namespace http {
core::Result<std::chrono::system_clock::time_point> parse_http_date(
    std::string_view value, std::chrono::system_clock::time_point now) {
    using namespace std::chrono;
    const auto invalid = [] { return core::make_error(core::ErrorCategory::invalid_argument, "HTTP date"); };
    constexpr std::array<std::string_view, 12> months{"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    constexpr std::array<std::string_view, 7> weekdays{"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    constexpr std::array<std::string_view, 7> long_days{"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
    auto number = [](std::string_view s) {
        int n = -1;
        const auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), n);
        return error == std::errc{} && end == s.data() + s.size() ? n : -1;
    };
    int y{}, d{}, wd = -1; unsigned mon = 0;
    std::string_view month_text, time;
    bool relative = false;
    if (value.size() == 29 && value.substr(3,2) == ", " && value[7] == ' ' && value[11] == ' ' && value[16] == ' ' && value.substr(25) == " GMT") {
        for (int i = 0; i < 7; ++i) if (value.substr(0,3) == weekdays[i]) wd = i;
        d = number(value.substr(5,2)); month_text = value.substr(8,3); y = number(value.substr(12,4)); time = value.substr(17,8);
    } else if (value.size() == 24 && value[3] == ' ' && value[7] == ' ' && value[10] == ' ' && value[19] == ' ') {
        for (int i = 0; i < 7; ++i) if (value.substr(0,3) == weekdays[i]) wd = i;
        auto day_text = value.substr(8,2); if (day_text.front() == ' ') day_text.remove_prefix(1);
        d = number(day_text); month_text = value.substr(4,3); y = number(value.substr(20,4)); time = value.substr(11,8);
    } else {
        const auto comma = value.find(", ");
        if (comma == std::string_view::npos) return invalid();
        for (int i = 0; i < 7; ++i) if (value.substr(0,comma) == long_days[i]) wd = i;
        auto rest = value.substr(comma + 2);
        if (rest.size() != 22 || rest[2] != '-' || rest[6] != '-' || rest[9] != ' ' || rest.substr(18) != " GMT") return invalid();
        d = number(rest.substr(0,2)); month_text = rest.substr(3,3); y = number(rest.substr(7,2)); time = rest.substr(10,8);
        if (y < 0) return invalid();
        const auto current_year = int(year_month_day(floor<days>(now)).year());
        y += ((current_year + 50) / 100) * 100;
        relative = true;
    }
    for (unsigned i = 0; i < 12; ++i) if (months[i] == month_text) mon = i + 1;
    if (time.size() != 8 || time[2] != ':' || time[5] != ':') return invalid();
    const int h = number(time.substr(0,2)), m = number(time.substr(3,2)), s = number(time.substr(6,2));
    if (wd < 0 || y < 1601 || y > 9999 || d < 1 || h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 59) return invalid();
    auto date = year(y)/month(mon)/day(static_cast<unsigned>(d));
    if (!date.ok()) return invalid();
    if (relative) {
        auto future = year_month_day(floor<days>(now)) + years(50);
        if (!future.ok()) future = future.year()/future.month()/day(28);
        if (sys_days(date) + hours(h) + minutes(m) + seconds(s) > sys_days(future) + (floor<seconds>(now) - floor<days>(now))) {
            y -= 100; date = year(y)/month(mon)/day(static_cast<unsigned>(d));
        }
    }
    if (!date.ok() || weekday(sys_days(date)).c_encoding() != static_cast<unsigned>(wd)) return invalid();
    const auto result = sys_days(date) + hours(h) + minutes(m) + seconds(s);
    if (result < ceil<seconds>(system_clock::time_point::min()) || result > floor<seconds>(system_clock::time_point::max())) return invalid();
    return time_point_cast<system_clock::duration>(result);
}
} // namespace http
