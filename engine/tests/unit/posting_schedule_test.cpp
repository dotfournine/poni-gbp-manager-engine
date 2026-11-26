// When an auto reply goes out: the random delay and the posting window in a
// location's own time zone, through both daylight-saving changes.

#include "domain/dates.hpp"
#include "domain/posting_schedule.hpp"

#include <gtest/gtest.h>

#include <chrono>

using namespace poni;
using namespace poni::domain;
using namespace std::chrono;
using namespace std::chrono_literals;

namespace {

constexpr std::string_view kNewYork = "America/New_York";
constexpr std::string_view kLosAngeles = "America/Los_Angeles";

core::TimePoint utc(year_month_day day, minutes time_of_day) { return core::TimePoint(sys_days{day} + time_of_day); }

// 09:00 to 21:00, the example in FR-5.
constexpr PostingWindow kDaytime{9 * 60, 21 * 60};

core::TimePoint moved(core::TimePoint t, const PostingWindow& window = kDaytime, std::string_view zone = kNewYork) {
    const auto result = into_window(t, window, zone);
    EXPECT_TRUE(result) << (result ? "" : result.error());
    return result.value_or(core::TimePoint{});
}

}  // namespace

TEST(PostingSchedule, TheServerKnowsTheZonesTheLocationsAreIn) {
    // Without the operating system's time-zone database nothing here can work.
    EXPECT_TRUE(known_time_zone(kNewYork));
    EXPECT_TRUE(known_time_zone(kLosAngeles));
    EXPECT_TRUE(known_time_zone("America/Vancouver"));
    EXPECT_TRUE(known_time_zone("Asia/Shanghai"));
    EXPECT_FALSE(known_time_zone(""));
    EXPECT_FALSE(known_time_zone("New York"));
    EXPECT_FALSE(known_time_zone("America/Nowhere"));
    EXPECT_FALSE(known_time_zone("/etc/passwd"));
    EXPECT_FALSE(known_time_zone("../../etc/passwd"));
}

TEST(PostingSchedule, ATimeInsideTheWindowIsKept) {
    // 2026-11-10 17:00 UTC is noon in New York (EST, UTC-5).
    const auto noon = utc(2026y / November / 10, 17h);
    EXPECT_EQ(moved(noon), noon);
    EXPECT_TRUE(in_window(noon, kDaytime, kNewYork).value());
    // The first minute is inside, the last one is not.
    EXPECT_EQ(moved(utc(2026y / November / 10, 14h)), utc(2026y / November / 10, 14h));
    EXPECT_FALSE(in_window(utc(2026y / November / 11, 2h), kDaytime, kNewYork).value()) << "21:00 sharp is outside";
}

TEST(PostingSchedule, ATimeOutsideMovesToTheNextOpening) {
    // 07:00 in New York: the window opens the same day at 09:00 (14:00 UTC).
    EXPECT_EQ(moved(utc(2026y / November / 10, 12h)), utc(2026y / November / 10, 14h));
    // 23:30 in New York: tomorrow at 09:00.
    EXPECT_EQ(moved(utc(2026y / November / 11, 4h + 30min)), utc(2026y / November / 11, 14h));
    // The same instant is a different local time on the west coast (20:30 in Los Angeles): still open there.
    EXPECT_EQ(moved(utc(2026y / November / 11, 4h + 30min), kDaytime, kLosAngeles), utc(2026y / November / 11, 4h + 30min));
}

// A zone without daylight saving: the window is the same hours in UTC all year, on the nights New York changes too.
TEST(PostingSchedule, AWindowInAZoneWithoutDaylightSavingKeepsItsHoursAllYear) {
    constexpr std::string_view tokyo = "Asia/Tokyo";  // UTC+9: 09:00 to 21:00 is 00:00 to 12:00 UTC
    for (const auto day : {2027y / January / 15, 2027y / March / 14, 2027y / July / 1, 2027y / November / 7}) {
        EXPECT_TRUE(in_window(utc(day, 0h), kDaytime, tokyo).value()) << "09:00 in Tokyo";
        EXPECT_TRUE(in_window(utc(day, 11h + 59min), kDaytime, tokyo).value()) << "20:59 in Tokyo";
        EXPECT_FALSE(in_window(utc(day, 12h), kDaytime, tokyo).value()) << "21:00 in Tokyo";
        // 22:00 in Tokyo waits for 09:00 the next day.
        EXPECT_EQ(moved(utc(day, 13h), kDaytime, tokyo), utc(year_month_day{sys_days{day} + days{1}}, 0h));
    }
}

TEST(PostingSchedule, AWindowMayCrossMidnight) {
    const PostingWindow evening{18 * 60, 2 * 60};  // 18:00 to 02:00
    // 01:00 in New York is inside; 03:00 is not, and waits for 18:00.
    EXPECT_EQ(moved(utc(2026y / November / 10, 6h), evening), utc(2026y / November / 10, 6h));
    EXPECT_EQ(moved(utc(2026y / November / 10, 8h), evening), utc(2026y / November / 10, 23h));
    EXPECT_EQ(moved(utc(2026y / November / 10, 23h + 30min), evening), utc(2026y / November / 10, 23h + 30min));
}

TEST(PostingSchedule, TheNightTheClocksGoForwardTheWindowOpensAnHourEarlierInUtc) {
    // New York goes from EST to EDT on 2027-03-14 at 02:00. A review at
    // 22:00 on the 13th (EST, 03:00 UTC on the 14th) waits for 09:00 EDT,
    // which is 13:00 UTC: one hour of UTC earlier than the day before.
    EXPECT_EQ(moved(utc(2027y / March / 13, 3h)), utc(2027y / March / 13, 14h)) << "the day before: 09:00 EST";
    EXPECT_EQ(moved(utc(2027y / March / 14, 3h)), utc(2027y / March / 14, 13h)) << "the day of the change: 09:00 EDT";
    // And in the evening the window closes at 21:00 EDT, 01:00 UTC.
    EXPECT_TRUE(in_window(utc(2027y / March / 15, 0h + 59min), kDaytime, kNewYork).value());
    EXPECT_FALSE(in_window(utc(2027y / March / 15, 1h), kDaytime, kNewYork).value());
}

TEST(PostingSchedule, AnOpeningTimeTheClocksSkipBecomesTheMomentTheyJump) {
    // 02:30 does not exist in New York on 2027-03-14: the clocks go from
    // 01:59:59 EST to 03:00 EDT, at 07:00 UTC.
    const PostingWindow night{2 * 60 + 30, 5 * 60};
    EXPECT_EQ(moved(utc(2027y / March / 14, 5h), night), utc(2027y / March / 14, 7h));
    // The day before, 02:30 EST is 07:30 UTC.
    EXPECT_EQ(moved(utc(2027y / March / 13, 5h), night), utc(2027y / March / 13, 7h + 30min));
}

TEST(PostingSchedule, TheNightTheClocksGoBackTheWindowOpensAnHourLaterInUtc) {
    // New York goes from EDT to EST on 2027-11-07 at 02:00. 22:00 EDT on the
    // 6th is 02:00 UTC on the 7th; 09:00 EST on the 7th is 14:00 UTC.
    EXPECT_EQ(moved(utc(2027y / November / 6, 2h)), utc(2027y / November / 6, 13h)) << "the day before: 09:00 EDT";
    EXPECT_EQ(moved(utc(2027y / November / 7, 2h)), utc(2027y / November / 7, 14h)) << "the day of the change: 09:00 EST";
}

TEST(PostingSchedule, AnOpeningTimeThatHappensTwiceIsTheFirstOne) {
    // 01:30 happens twice in New York on 2027-11-07: at 05:30 UTC (EDT) and 06:30 UTC (EST).
    const PostingWindow night{1 * 60 + 30, 5 * 60};
    EXPECT_EQ(moved(utc(2027y / November / 7, 4h), night), utc(2027y / November / 7, 5h + 30min));
    // An hour later the local time is 01:45 again (now EST), and inside the window.
    EXPECT_TRUE(in_window(utc(2027y / November / 7, 5h + 45min), night, kNewYork).value());
    EXPECT_TRUE(in_window(utc(2027y / November / 7, 6h + 45min), night, kNewYork).value());
}

TEST(PostingSchedule, AnUnknownZoneIsAnErrorNotUtc) {
    EXPECT_FALSE(into_window(utc(2026y / November / 10, 12h), kDaytime, "America/Nowhere"));
    EXPECT_FALSE(in_window(utc(2026y / November / 10, 12h), kDaytime, ""));
}

TEST(PostingSchedule, TheDelayIsAPointInItsRange) {
    const auto now = utc(2026y / November / 10, 17h);
    const PostingTiming timing{.delay_min_minutes = 20, .delay_max_minutes = 120, .window = std::nullopt};
    EXPECT_EQ(delayed(now, timing, 0.0), now + 20min);
    EXPECT_EQ(delayed(now, timing, 0.5), now + 70min);
    EXPECT_LT(delayed(now, timing, 0.999999), now + 120min);
    EXPECT_EQ(delayed(now, timing, 7.0), now + 120min) << "a roll out of range is clamped";
    EXPECT_EQ(delayed(now, PostingTiming{}, 0.9), now) << "no delay: as fast as possible";
}

TEST(PostingSchedule, TheDelayComesFirstAndThenTheWindow) {
    // 20:30 in New York plus 45 minutes is 21:15, outside: tomorrow's opening
    // at 09:00, and the delay again from there, so not everything that
    // waited overnight goes out at 09:00 sharp.
    const auto now = utc(2026y / November / 11, 1h + 30min);
    const PostingTiming timing{.delay_min_minutes = 45, .delay_max_minutes = 45, .window = kDaytime};
    EXPECT_EQ(post_time(now, timing, kNewYork, 0.3).value(), utc(2026y / November / 11, 14h + 45min));
    // Inside the window nothing is added.
    EXPECT_EQ(post_time(utc(2026y / November / 10, 17h), timing, kNewYork, 0.3).value(), utc(2026y / November / 10, 17h + 45min));
    // No delay: the opening itself.
    EXPECT_EQ(post_time(utc(2026y / November / 11, 4h + 30min), {.delay_min_minutes = 0, .delay_max_minutes = 0, .window = kDaytime},
                        kNewYork, 0.3)
                  .value(),
              utc(2026y / November / 11, 14h));
    // Without a window no zone is needed.
    EXPECT_EQ(post_time(now, {.delay_min_minutes = 45, .delay_max_minutes = 45, .window = std::nullopt}, "", 0.3).value(), now + 45min);
    EXPECT_FALSE(post_time(now, timing, "", 0.3)) << "a window needs a zone";
}

TEST(PostingSchedule, TheReplySettingsTimingIsReadAndRefusedWhenItIsNonsense) {
    ReplySettings reply;
    EXPECT_FALSE(posting_timing(reply).value().window) << "the defaults: no delay, no window";

    reply.auto_reply_delay_min_minutes = 20;
    reply.auto_reply_delay_max_minutes = 120;
    reply.auto_reply_window_start = "09:00";
    reply.auto_reply_window_end = "21:00";
    const auto timing = posting_timing(reply);
    ASSERT_TRUE(timing);
    EXPECT_EQ(timing->delay_max_minutes, 120);
    EXPECT_EQ(timing->window->start, 9 * 60);
    EXPECT_EQ(timing->window->end, 21 * 60);

    // A pair that does not fit: a stored row is not a save's word, so the use asks again.
    reply.auto_reply_delay_min_minutes = 200;
    EXPECT_FALSE(posting_timing(reply));
    reply.auto_reply_delay_min_minutes = 20;
    reply.auto_reply_window_end = "";
    EXPECT_FALSE(posting_timing(reply));
    reply.auto_reply_window_end = "09:00";
    EXPECT_FALSE(posting_timing(reply));

    EXPECT_EQ(parse_clock_time("00:00"), 0);
    EXPECT_EQ(parse_clock_time("23:59"), 23 * 60 + 59);
    EXPECT_FALSE(parse_clock_time("24:00"));
    EXPECT_FALSE(parse_clock_time("9:00"));
    EXPECT_FALSE(parse_clock_time("09:60"));
    EXPECT_FALSE(parse_clock_time("0900"));
}
