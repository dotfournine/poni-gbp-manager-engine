// Days and times of day as the domain reads and writes them: "YYYY-MM-DD",
// "HH:MM", and the days in words that reports, their summaries and the help
// with posts show. Calendar arithmetic only: time zones are in
// domain/posting_schedule.hpp and domain/report_schedule.hpp.
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace poni::domain {

// A calendar day without a zone.
using Day = std::chrono::sys_days;

// "YYYY-MM-DD".
std::string format_day(Day day);
std::optional<Day> parse_report_day(std::string_view text);

// "HH:MM" as minutes after midnight; nothing if it isn't a time of day.
std::optional<int> parse_clock_time(std::string_view text);

// For "2026-11-03": "November 3, 2026", and "Nov 3". A day that can't be
// read is "? 0, 0".
std::string day_label(std::string_view day);
std::string short_day(std::string_view day);

// "October 2026" for "2026-10".
std::string month_label(std::string_view month);

// "November 1 to 30, 2026", "November 3, 2026", "October 27 to November 2, 2026".
std::string period_label(std::string_view start, std::string_view end);

// The day with its weekday: "Thursday, 19 November 2026".
std::string day_in_words(Day day);

}  // namespace poni::domain
