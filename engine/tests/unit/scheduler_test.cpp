#include "jobs/scheduler.hpp"

#include "support.hpp"

#include <gtest/gtest.h>

using namespace poni;
using namespace std::chrono_literals;

TEST(SchedulerSlots, AlignToTheInterval) {
    // t0 is 12:00:00 UTC.
    EXPECT_EQ(jobs::slot_start(test::t0(), 300s), test::t0());
    EXPECT_EQ(jobs::slot_start(test::t0() + 299s + 999ms, 300s), test::t0());
    EXPECT_EQ(jobs::slot_start(test::t0() + 300s, 300s), test::t0() + 300s);
    EXPECT_EQ(jobs::slot_start(test::t0() + 59min, 1h), test::t0());
    EXPECT_EQ(jobs::slot_start(test::t0() - 1us, 1h), test::t0() - 1h);
}

TEST(SchedulerSlots, KeyNamesTheScheduleAndTheSlot) {
    const jobs::Recurring heartbeat{.kind = "system.heartbeat", .interval = 300s};
    const auto a = jobs::slot_key(heartbeat, jobs::slot_start(test::t0() + 10s, 300s));
    const auto b = jobs::slot_key(heartbeat, jobs::slot_start(test::t0() + 290s, 300s));
    const auto c = jobs::slot_key(heartbeat, jobs::slot_start(test::t0() + 300s, 300s));
    EXPECT_EQ(a, "system.heartbeat@1794312000");
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}
