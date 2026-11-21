#include "jobs/backoff.hpp"

#include <gtest/gtest.h>

using namespace std::chrono_literals;
using poni::jobs::retry_delay;

TEST(Backoff, DoublesFromThirtySeconds) {
    EXPECT_EQ(retry_delay(1), 30s);
    EXPECT_EQ(retry_delay(2), 60s);
    EXPECT_EQ(retry_delay(3), 120s);
    EXPECT_EQ(retry_delay(4), 240s);
}

TEST(Backoff, IsCappedAtOneHour) {
    EXPECT_EQ(retry_delay(8), 3600s);
    EXPECT_EQ(retry_delay(50), 3600s);
    EXPECT_EQ(retry_delay(1'000'000), 3600s);
}

TEST(Backoff, TreatsNonPositiveAttemptsAsTheFirst) {
    EXPECT_EQ(retry_delay(0), 30s);
    EXPECT_EQ(retry_delay(-3), 30s);
}
