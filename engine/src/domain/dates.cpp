#include "domain/dates.hpp"

#include <array>
#include <charconv>
#include <format>

namespace poni::domain {

namespace {

using namespace std::chrono;

constexpr std::array<std::string_view, 12> kMonths = {"January", "February", "March",     "April",   "May",      "June",
                                                      "July",    "August",   "September", "October", "November", "December"};

struct Ymd {
    int year = 0;
    int month = 0;
    int day = 0;
};

Ymd split(std::string_view day) {
    const auto parsed = parse_report_day(day);
    if (!parsed) return {};
    const year_month_day ymd{*parsed};
    return {static_cast<int>(ymd.year()), static_cast<int>(static_cast<unsigned>(ymd.month())),
            static_cast<int>(static_cast<unsigned>(ymd.day()))};
}

std::string_view month_name(int month) { return month >= 1 && month <= 12 ? kMonths[static_cast<std::size_t>(month - 1)] : "?"; }

}  // namespace

std::string format_day(Day day) { return std::format("{:%F}", day); }

std::optional<Day> parse_report_day(std::string_view text) {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') return std::nullopt;
    int y = 0;
    unsigned m = 0, d = 0;
    const auto number = [&](std::size_t from, std::size_t size, auto& out) {
        const auto [end, ec] = std::from_chars(text.data() + from, text.data() + from + size, out);
        return ec == std::errc{} && end == text.data() + from + size;
    };
    if (!number(0, 4, y) || !number(5, 2, m) || !number(8, 2, d)) return std::nullopt;
    const year_month_day ymd{year{y}, month{m}, day{d}};
    if (!ymd.ok()) return std::nullopt;
    return sys_days{ymd};
}

std::optional<int> parse_clock_time(std::string_view text) {
    const auto digit = [&](std::size_t i) { return text[i] >= '0' && text[i] <= '9'; };
    if (text.size() != 5 || text[2] != ':' || !digit(0) || !digit(1) || !digit(3) || !digit(4)) return std::nullopt;
    const int hour = (text[0] - '0') * 10 + (text[1] - '0');
    const int minute = (text[3] - '0') * 10 + (text[4] - '0');
    if (hour > 23 || minute > 59) return std::nullopt;
    return hour * 60 + minute;
}

std::string day_label(std::string_view day) {
    const auto d = split(day);
    return std::format("{} {}, {}", month_name(d.month), d.day, d.year);
}

std::string short_day(std::string_view day) {
    const auto d = split(day);
    return std::format("{} {}", month_name(d.month).substr(0, 3), d.day);
}

std::string month_label(std::string_view month) {
    const auto m = split(std::string(month) + "-01");
    return std::format("{} {}", month_name(m.month), m.year);
}

std::string period_label(std::string_view start, std::string_view end) {
    const auto a = split(start), b = split(end);
    if (start == end) return day_label(start);
    if (a.year == b.year && a.month == b.month) return std::format("{} {} to {}, {}", month_name(a.month), a.day, b.day, a.year);
    if (a.year == b.year) return std::format("{} {} to {} {}, {}", month_name(a.month), a.day, month_name(b.month), b.day, a.year);
    return std::format("{} to {}", day_label(start), day_label(end));
}

std::string day_in_words(Day day) {
    constexpr std::array<std::string_view, 7> weekdays{"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    const year_month_day date{day};
    return std::format("{}, {} {} {}", weekdays[weekday{day}.c_encoding()], static_cast<unsigned>(date.day()),
                       kMonths[static_cast<unsigned>(date.month()) - 1], static_cast<int>(date.year()));
}

}  // namespace poni::domain
