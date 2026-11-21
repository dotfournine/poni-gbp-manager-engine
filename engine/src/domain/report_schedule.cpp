#include "domain/report_schedule.hpp"

#include "domain/posting_schedule.hpp"

#include <cctz/civil_time.h>
#include <cctz/time_zone.h>

#include <format>

namespace poni::domain {

namespace {

using namespace std::chrono;

std::expected<cctz::time_zone, std::string> load(std::string_view zone) {
    cctz::time_zone tz;
    if (!known_time_zone(zone) || !cctz::load_time_zone(std::string(zone), &tz)) {
        return std::unexpected(std::format("unknown time zone '{}'", zone));
    }
    return tz;
}

cctz::civil_day civil(Day day) {
    const year_month_day ymd{day};
    return cctz::civil_day(static_cast<int>(ymd.year()), static_cast<int>(static_cast<unsigned>(ymd.month())),
                           static_cast<int>(static_cast<unsigned>(ymd.day())));
}

Day to_day(const cctz::civil_day& day) {
    return sys_days{year{static_cast<int>(day.year())} / month{static_cast<unsigned>(day.month())} /
                    std::chrono::day{static_cast<unsigned>(day.day())}};
}

// The moment a local time of day happens on a local day.
core::TimePoint moment(const cctz::time_zone& tz, Day day, int minute_of_day) {
    const cctz::civil_second local = cctz::civil_second(civil(day)) + static_cast<cctz::diff_t>(minute_of_day) * 60;
    const auto lookup = tz.lookup(local);
    // skipped: `trans` is the first moment after the gap. repeated: `pre` is the first of the two.
    const auto at = lookup.kind == cctz::time_zone::civil_lookup::SKIPPED ? lookup.trans : lookup.pre;
    return time_point_cast<microseconds>(at);
}

// The latest day at or before `today` the schedule runs on.
Day run_day_at_or_before(const ReportSchedule& schedule, Day today) {
    if (schedule.frequency == kWeekly) {
        const auto iso = static_cast<int>(weekday{today}.iso_encoding());
        return today - days{(iso - schedule.weekday + 7) % 7};
    }
    if (schedule.frequency == kMonthly) {
        const year_month_day ymd{today};
        auto month_of_run = ymd.year() / ymd.month();
        if (static_cast<int>(static_cast<unsigned>(ymd.day())) < schedule.month_day) month_of_run -= months{1};
        return sys_days{month_of_run / std::chrono::day{static_cast<unsigned>(schedule.month_day)}};
    }
    return today;
}

}  // namespace

std::optional<std::string> schedule_problem(const ReportSchedule& schedule) {
    if (schedule.frequency != kDaily && schedule.frequency != kWeekly && schedule.frequency != kMonthly) {
        return "'frequency' must be daily, weekly or monthly";
    }
    if (schedule.frequency == kWeekly && (schedule.weekday < 1 || schedule.weekday > 7)) {
        return "a weekly schedule needs 'weekday' from 1 (Monday) to 7 (Sunday)";
    }
    if (schedule.frequency == kMonthly && (schedule.month_day < 1 || schedule.month_day > 28)) {
        return "a monthly schedule needs 'month_day' from 1 to 28";
    }
    if (schedule.minute_of_day < 0 || schedule.minute_of_day > 1439) return "'time' must be a time of day";
    if (!known_time_zone(schedule.time_zone)) return std::format("unknown time zone '{}'", schedule.time_zone);
    return std::nullopt;
}

std::expected<Occurrence, std::string> last_occurrence(const ReportSchedule& schedule, core::TimePoint now) {
    if (auto problem = schedule_problem(schedule)) return std::unexpected(std::move(*problem));
    const auto tz = load(schedule.time_zone);
    if (!tz) return std::unexpected(tz.error());
    const auto today = to_day(cctz::civil_day(cctz::convert(time_point_cast<seconds>(now), *tz)));
    auto run = run_day_at_or_before(schedule, today);
    auto at = moment(*tz, run, schedule.minute_of_day);
    if (at > now) {
        // Today is a run day, but its time has not come: the run before it.
        run = run_day_at_or_before(schedule, run - days{1});
        at = moment(*tz, run, schedule.minute_of_day);
    }
    return Occurrence{.local_day = run, .at = at};
}

Period period_for(std::string_view frequency, Day run_day) {
    if (frequency == kWeekly) return {run_day - days{7}, run_day - days{1}};
    if (frequency == kMonthly) {
        const year_month_day ymd{run_day};
        const auto before = ymd.year() / ymd.month() - months{1};
        return {sys_days{before / std::chrono::day{1}}, sys_days{before / last}};
    }
    return {run_day - days{1}, run_day - days{1}};
}

Period previous_period(const Period& period) {
    const year_month_day start{period.start};
    const auto month_of_start = start.year() / start.month();
    if (start.day() == std::chrono::day{1} && period.end == sys_days{month_of_start / last}) {
        const auto before = month_of_start - months{1};
        return {sys_days{before / std::chrono::day{1}}, sys_days{before / last}};
    }
    const auto length = period.end - period.start + days{1};
    return {period.start - length, period.start - days{1}};
}

std::expected<Bounds, std::string> period_bounds(const Period& period, std::string_view zone) {
    const auto tz = load(zone);
    if (!tz) return std::unexpected(tz.error());
    return Bounds{.from = moment(*tz, period.start, 0), .to = moment(*tz, period.end + days{1}, 0)};
}

std::expected<Day, std::string> local_day(core::TimePoint t, std::string_view zone) {
    const auto tz = load(zone);
    if (!tz) return std::unexpected(tz.error());
    return to_day(cctz::civil_day(cctz::convert(time_point_cast<seconds>(t), *tz)));
}

}  // namespace poni::domain
