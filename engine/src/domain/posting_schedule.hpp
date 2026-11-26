// When an auto reply goes out (FR-5): a random delay, then moved into the
// location's posting window. The window is in the location's local time, so
// this is the one place that knows about time zones (D-029, D-071): cctz
// reads the operating system's time-zone database, which follows the IANA
// rules through daylight-saving changes.
//
// No I/O apart from that database, and no clock: the caller passes the time.
#pragma once

#include "core/clock.hpp"
#include "domain/reply_settings.hpp"

#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace poni::domain {

// A daily span of local time, in minutes after midnight. `start` is inside,
// `end` is not. A start later than the end crosses midnight (18:00 to 02:00).
struct PostingWindow {
    int start = 0;
    int end = 0;
};

struct PostingTiming {
    std::int64_t delay_min_minutes = 0;
    std::int64_t delay_max_minutes = 0;
    std::optional<PostingWindow> window;
};

// The timing the policy asks for, or what is wrong with its delay or its
// window as a pair. A save is refused for it (domain::check_settings); a
// stored row is not a save's word, so a policy like that never auto replies.
std::expected<PostingTiming, std::string> posting_timing(const PolicyRules& policy);

// True if the operating system's database knows this IANA name.
bool known_time_zone(std::string_view name);

// `now` plus a delay between the two bounds; `roll` is in [0, 1).
core::TimePoint delayed(core::TimePoint now, const PostingTiming& timing, double roll);

// The earliest moment at or after `t` that is inside the window: `t` itself
// if it is, else the next time the window opens. On the day clocks go
// forward over the opening time, that is the first moment that exists
// after it; on the day they go back, the first of the two.
// Fails if the zone is unknown.
std::expected<core::TimePoint, std::string> into_window(core::TimePoint t, const PostingWindow& window, std::string_view zone);

// Whether `t` is inside the window. Fails if the zone is unknown.
std::expected<bool, std::string> in_window(core::TimePoint t, const PostingWindow& window, std::string_view zone);

// The delay and the window together: when a draft written at `now` is posted.
// A reply that has to wait for the window to open gets its delay again from
// the opening, so replies that waited overnight don't all go out at once.
// A window needs a zone; without a window the zone is not looked at.
std::expected<core::TimePoint, std::string> post_time(core::TimePoint now, const PostingTiming& timing, std::string_view zone,
                                                      double roll);

}  // namespace poni::domain
