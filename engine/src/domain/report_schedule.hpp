// When a report is due and which days it covers (D-085). A schedule runs
// daily, weekly or monthly at a local time; a period is a span of whole
// local days. Like domain/posting_schedule.hpp this reads the operating
// system's time-zone database through cctz and nothing else: no clock, the
// caller passes the time.
#pragma once

#include "core/clock.hpp"
#include "domain/dates.hpp"

#include <chrono>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace poni::domain {

inline constexpr std::string_view kDaily = "daily";
inline constexpr std::string_view kWeekly = "weekly";
inline constexpr std::string_view kMonthly = "monthly";

struct ReportSchedule {
    std::string frequency;  // kDaily, kWeekly or kMonthly
    int weekday = 0;        // 1 (Monday) to 7 (Sunday); weekly only
    int month_day = 0;      // 1 to 28; monthly only
    int minute_of_day = 0;  // local time
    std::string time_zone;
};

// What is wrong with the schedule, if anything.
std::optional<std::string> schedule_problem(const ReportSchedule& schedule);

struct Occurrence {
    Day local_day;        // the day of the run, in the schedule's zone
    core::TimePoint at;   // the moment of the run
};

// The schedule's latest run at or before `now`. A local time that does not
// exist on the day clocks go forward is the first moment after the gap; one
// that exists twice on the day they go back is the first of the two.
std::expected<Occurrence, std::string> last_occurrence(const ReportSchedule& schedule, core::TimePoint now);

// Both days are part of the period.
struct Period {
    Day start;
    Day end;
    friend bool operator==(const Period&, const Period&) = default;
};

// What a run on `run_day` covers: the day before, the seven days before, or
// the calendar month before.
Period period_for(std::string_view frequency, Day run_day);

// The period a report compares with: the calendar month before a calendar
// month, else the same number of days just before.
Period previous_period(const Period& period);

struct Bounds {
    core::TimePoint from;  // the first moment of the first day
    core::TimePoint to;    // the first moment after the last day
};
std::expected<Bounds, std::string> period_bounds(const Period& period, std::string_view zone);

// The local day `t` falls on.
std::expected<Day, std::string> local_day(core::TimePoint t, std::string_view zone);

}  // namespace poni::domain
