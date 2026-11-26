#include "domain/posting_schedule.hpp"

#include "domain/dates.hpp"

#include <cctz/civil_time.h>
#include <cctz/time_zone.h>

#include <algorithm>
#include <chrono>
#include <format>

namespace poni::domain {

namespace {

using Seconds = std::chrono::time_point<std::chrono::system_clock, std::chrono::seconds>;

std::expected<cctz::time_zone, std::string> load(std::string_view zone) {
    cctz::time_zone tz;
    // cctz answers "UTC" for a name it can't load, and says so by returning false.
    if (zone.empty() || !cctz::load_time_zone(std::string(zone), &tz)) {
        return std::unexpected(std::format("unknown time zone '{}'", zone));
    }
    return tz;
}

bool inside(const PostingWindow& window, int minute_of_day) {
    if (window.start < window.end) return minute_of_day >= window.start && minute_of_day < window.end;
    return minute_of_day >= window.start || minute_of_day < window.end;
}

}  // namespace

std::expected<PostingTiming, std::string> posting_timing(const ReplySettings& settings) {
    PostingTiming timing{.delay_min_minutes = settings.auto_reply_delay_min_minutes,
                         .delay_max_minutes = settings.auto_reply_delay_max_minutes,
                         .window = std::nullopt};
    if (timing.delay_min_minutes < 0 || timing.delay_min_minutes > timing.delay_max_minutes) {
        return std::unexpected(std::string("the delay's minimum is greater than its maximum"));
    }
    const bool has_start = !settings.auto_reply_window_start.empty();
    const bool has_end = !settings.auto_reply_window_end.empty();
    if (has_start != has_end) return std::unexpected(std::string("the posting window has only one end"));
    if (has_start) {
        const auto start = parse_clock_time(settings.auto_reply_window_start);
        const auto end = parse_clock_time(settings.auto_reply_window_end);
        if (!start || !end || *start == *end) return std::unexpected(std::string("the posting window is not a span of time"));
        timing.window = PostingWindow{*start, *end};
    }
    return timing;
}

bool known_time_zone(std::string_view name) {
    // Names only, as the IANA database spells them: no "UTC+8" or file paths.
    if (name.empty() || name.size() > 64 || name.front() == '/' || name.find("..") != std::string_view::npos) return false;
    return load(name).has_value();
}

core::TimePoint delayed(core::TimePoint now, const PostingTiming& timing, double roll) {
    const auto low = std::chrono::minutes(timing.delay_min_minutes);
    const auto high = std::chrono::minutes(std::max(timing.delay_min_minutes, timing.delay_max_minutes));
    const auto span = std::chrono::duration_cast<std::chrono::seconds>(high - low);
    const auto extra = std::chrono::seconds(static_cast<std::int64_t>(static_cast<double>(span.count()) * std::clamp(roll, 0.0, 1.0)));
    return now + low + extra;
}

std::expected<bool, std::string> in_window(core::TimePoint t, const PostingWindow& window, std::string_view zone) {
    const auto tz = load(zone);
    if (!tz) return std::unexpected(tz.error());
    const auto local = cctz::convert(std::chrono::time_point_cast<std::chrono::seconds>(t), *tz);
    return inside(window, local.hour() * 60 + local.minute());
}

std::expected<core::TimePoint, std::string> into_window(core::TimePoint t, const PostingWindow& window, std::string_view zone) {
    const auto tz = load(zone);
    if (!tz) return std::unexpected(tz.error());
    const auto local = cctz::convert(std::chrono::time_point_cast<std::chrono::seconds>(t), *tz);
    const int minute_of_day = local.hour() * 60 + local.minute();
    if (inside(window, minute_of_day)) return t;

    // Outside: the window opens later today, or tomorrow.
    cctz::civil_day day(local);
    if (minute_of_day >= window.start) ++day;
    const cctz::civil_second opening(day.year(), day.month(), day.day(), window.start / 60, window.start % 60, 0);
    const auto found = tz->lookup(opening);
    // A local time that the clocks skip has no instant: take the moment the
    // clocks jump, which is the first one after it. A local time that
    // happens twice: the first.
    const Seconds at = found.kind == cctz::time_zone::civil_lookup::SKIPPED ? found.trans : found.pre;
    return std::chrono::time_point_cast<std::chrono::microseconds>(at);
}

std::expected<core::TimePoint, std::string> post_time(core::TimePoint now, const PostingTiming& timing, std::string_view zone,
                                                      double roll) {
    const auto due = delayed(now, timing, roll);
    if (!timing.window) return due;
    const auto moved = into_window(due, *timing.window, zone);
    if (!moved || *moved == due) return moved;
    // Moved to an opening. Without the delay again, every reply that waited
    // overnight would go out in the same second the window opens.
    return into_window(*moved + (due - now), *timing.window, zone);
}

}  // namespace poni::domain
