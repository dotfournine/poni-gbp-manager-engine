// Location discovery and review sync against a real PostgreSQL and a fake
// GBP account: what gets stored, what counts as a change, when a review is
// marked removed, and that running anything twice changes nothing.

#include "db/repo_auth.hpp"
#include "db/repo_reviews.hpp"
#include "engine_fixture.hpp"
#include "fakes.hpp"
#include "jobs/analysis.hpp"
#include "jobs/scheduler.hpp"
#include "jobs/sync.hpp"
#include "jobs/worker.hpp"

#include <boost/json/parse.hpp>
#include <gmock/gmock.h>

using namespace poni;
using namespace std::chrono_literals;
using gbp::ApiError;
using test::make_review;

namespace {

constexpr std::string_view kAccount = "111";
using test::kBuffet;
using test::kSpa;

class SyncTest : public test::EngineTest {
protected:
    void SetUp() override {
        EngineTest::SetUp();
        gbp_.add_location(std::string(kSpa), "Spa");
        gbp_.add_location(std::string(kBuffet), "Buffet");
        sync_.emplace(db(), clock_, gbp_, queue(), jobs::SyncOptions{});
    }

    // Discovers both locations and links the spa to a client. Returns the spa's id.
    std::int64_t linked_spa() {
        await(sync_->discover_locations());
        return link(std::string(kSpa), make_client("LU DREAM SPA"));
    }

    // `n` reviews of the spa, r1 the oldest, one minute apart.
    void seed_reviews(int n) {
        for (int i = 1; i <= n; ++i) {
            const auto time = std::format("2026-10-{:02}T{:02}:{:02}:00Z", 1 + i / 1440, (i / 60) % 24, i % 60);
            gbp_.put(make_review(kAccount, kSpa, std::format("r{}", i), "FIVE", "Lovely", time, time, "Thank you!"));
        }
    }

    std::vector<std::string> events(const std::string& review_id) {
        pqxx::work tx{conn()};
        std::vector<std::string> out;
        for (const auto& row : tx.exec("SELECT e.kind || ':' || e.source FROM review_events e JOIN reviews r ON r.id = e.review_id "
                                       "WHERE r.gbp_review_id = $1 ORDER BY e.id",
                                       pqxx::params{review_id})) {
            out.push_back(row[0].as<std::string>());
        }
        return out;
    }

    test::FakeGbp gbp_;
    std::optional<jobs::Sync> sync_;
};

}  // namespace

// ---- locations -------------------------------------------------------------------

TEST_F(SyncTest, DiscoveryStoresLocationsOnceAndAuditsTheNewOnes) {
    await(sync_->discover_locations());
    await(sync_->discover_locations());

    EXPECT_EQ(count("locations"), 2);
    EXPECT_EQ(count("locations WHERE gbp_account_id = '111' AND access_status = 'active' AND client_id IS NULL"), 2);
    EXPECT_EQ(value<std::string>("SELECT raw->>'title' FROM locations WHERE gbp_location_id = '9001'"), "Spa");
    EXPECT_EQ(count("audit_log WHERE action = 'location.discovered' AND actor = 'system:location-discovery'"), 2);
    EXPECT_EQ(count("integration_status WHERE name = 'location_discovery' AND last_ok_at IS NOT NULL AND last_error IS NULL"), 1);
}

TEST_F(SyncTest, ALocationMissingTwiceIsLostAndComesBack) {
    await(sync_->discover_locations());
    const auto buffet = gbp_.locations.back();
    gbp_.locations.pop_back();

    await(sync_->discover_locations());
    EXPECT_EQ(count("locations WHERE access_status = 'lost'"), 0) << "one miss can be a hiccup on Google's side";

    clock_.advance(15min);
    await(sync_->discover_locations());
    EXPECT_EQ(count("locations WHERE gbp_location_id = '9002' AND access_status = 'lost' AND access_lost_at IS NOT NULL"), 1);
    EXPECT_EQ(count("audit_log WHERE action = 'location.access_lost'"), 1);

    gbp_.locations.push_back(buffet);
    await(sync_->discover_locations());
    EXPECT_EQ(count("locations WHERE access_status = 'active' AND access_lost_at IS NULL AND missed_discoveries = 0"), 2);
    EXPECT_EQ(count("audit_log WHERE action = 'location.access_restored'"), 1);
}

TEST_F(SyncTest, AFailedDiscoveryNeverMarksAnythingLost) {
    await(sync_->discover_locations());
    gbp_.fail_locations = ApiError{ApiError::Kind::unavailable, 503, "HTTP 503"};
    await(sync_->discover_locations());
    await(sync_->discover_locations());
    gbp_.fail_locations.reset();
    gbp_.fail_accounts = ApiError{ApiError::Kind::unauthenticated, 401, "HTTP 401 token revoked"};
    await(sync_->discover_locations());

    EXPECT_EQ(count("locations WHERE access_status = 'active' AND missed_discoveries = 0"), 2);
    EXPECT_EQ(value<std::string>("SELECT last_error FROM integration_status WHERE name = 'location_discovery'"),
              "HTTP 401 token revoked");
}

TEST_F(SyncTest, AMovedLocationKeepsItsRowAndRebindsTheAccount) {
    await(sync_->discover_locations());
    gbp_.account_id = "222";  // the managing account changed (architecture section 3)
    await(sync_->discover_locations());
    EXPECT_EQ(count("locations"), 2);
    EXPECT_EQ(count("locations WHERE gbp_account_id = '222'"), 2);
    EXPECT_EQ(count("audit_log WHERE action = 'location.account_changed'"), 2);
}

// ---- reviews: full sync ----------------------------------------------------------

TEST_F(SyncTest, LinkingALocationQueuesOneFullSyncAndPollingWaitsForIt) {
    linked_spa();
    seed_reviews(3);

    await(sync_->poll_reviews());
    await(sync_->poll_reviews());
    EXPECT_EQ(count("jobs WHERE kind = 'reviews.full_sync'"), 1) << "one job per location and slot";
    EXPECT_EQ(gbp_.batch_calls, 0) << "a location is polled only after its first full sync";
    EXPECT_EQ(count("reviews"), 0);
}

TEST_F(SyncTest, FullSyncStoresEveryReviewAcrossPages) {
    const auto spa = linked_spa();
    seed_reviews(120);  // three pages of 50

    await(sync_->full_sync(spa));

    EXPECT_EQ(count("reviews"), 120);
    EXPECT_EQ(gbp_.list_calls, 3);
    EXPECT_EQ(count("review_events WHERE kind = 'discovered' AND source = 'full_sync'"), 120);
    EXPECT_EQ(count("reviews WHERE reply_text = 'Thank you!' AND reply_origin = 'external' AND rating = 5"), 120);
    const auto detail = boost::json::parse(value<std::string>("SELECT last_full_sync::text FROM locations WHERE id = " + std::to_string(spa)));
    EXPECT_EQ(detail.at("listed").as_int64(), 120);
    EXPECT_EQ(detail.at("new").as_int64(), 120);
    EXPECT_EQ(detail.at("removed").as_int64(), 0);
    EXPECT_EQ(count("locations WHERE reviews_full_synced_at IS NOT NULL AND next_full_sync_at > reviews_full_synced_at "
                    "AND last_sync_error IS NULL"),
              1);
}

TEST_F(SyncTest, SyncingTwiceChangesNothing) {
    const auto spa = linked_spa();
    seed_reviews(60);
    await(sync_->full_sync(spa));
    const auto changed_before = value<std::string>("SELECT max(last_changed_at)::text FROM reviews");

    clock_.advance(1h);
    await(sync_->full_sync(spa));
    await(sync_->poll_reviews());

    EXPECT_EQ(count("reviews"), 60);
    EXPECT_EQ(count("review_events"), 60);
    EXPECT_EQ(value<std::string>("SELECT max(last_changed_at)::text FROM reviews"), changed_before);
    EXPECT_EQ(count("reviews WHERE last_seen_at > first_seen_at"), 60) << "seen again, but not changed";
}

TEST_F(SyncTest, StoresTheWholeReviewIncludingGooglesTranslationSplit) {
    const auto spa = linked_spa();
    gbp_.put(make_review(kAccount, kSpa, "zh", "FIVE",
                         "(Translated by Google) A very comfortable experience\n\n(Original)\n很舒服的一次体验",
                         "2026-10-01T10:00:00.123456789Z", "2026-10-02T11:00:00Z"));
    gbp_.put(make_review(kAccount, kSpa, "stars-only", "FOUR", std::nullopt, "2026-10-03T10:00:00Z", "2026-10-03T10:00:00Z"));
    await(sync_->full_sync(spa));

    pqxx::work tx{conn()};
    const auto zh = tx.exec("SELECT original_text, translated_text, language, comment, reviewer_name, gbp_name, "
                            "raw->>'reviewId', created_time::text, reply_text IS NULL, reply_origin IS NULL "
                            "FROM reviews WHERE gbp_review_id = 'zh'")
                        .one_row();
    EXPECT_EQ(zh[0].as<std::string>(), "很舒服的一次体验");
    EXPECT_EQ(zh[1].as<std::string>(), "A very comfortable experience");
    EXPECT_EQ(zh[2].as<std::string>(), "zh");
    EXPECT_TRUE(zh[3].as<std::string>().starts_with("(Translated by Google)")) << "the comment is kept as Google sent it";
    EXPECT_EQ(zh[4].as<std::string>(), "Reviewer zh");
    EXPECT_EQ(zh[5].as<std::string>(), "accounts/111/locations/9001/reviews/zh");
    EXPECT_EQ(zh[6].as<std::string>(), "zh");
    EXPECT_THAT(zh[7].as<std::string>(), testing::HasSubstr("10:00:00.123456"));
    EXPECT_TRUE(zh[8].as<bool>());
    EXPECT_TRUE(zh[9].as<bool>());

    const auto stars = tx.exec("SELECT rating, comment IS NULL, original_text IS NULL, language IS NULL FROM reviews "
                               "WHERE gbp_review_id = 'stars-only'")
                           .one_row();
    EXPECT_EQ(stars[0].as<int>(), 4);
    EXPECT_TRUE(stars[1].as<bool>() && stars[2].as<bool>() && stars[3].as<bool>());
}

TEST_F(SyncTest, AReviewWithUnreadableTimestampsIsSkippedNotFatal) {
    const auto spa = linked_spa();
    seed_reviews(2);
    gbp_.put(make_review(kAccount, kSpa, "odd", "FIVE", "x", "yesterday", "2026-10-03T10:00:00Z"));
    await(sync_->full_sync(spa));
    EXPECT_EQ(count("reviews"), 2);
    EXPECT_EQ(value<std::string>("SELECT last_full_sync->>'skipped' FROM locations WHERE id = " + std::to_string(spa)), "1");
}

TEST_F(SyncTest, AReviewIsRemovedOnlyWhenGoogleSaysItIsGone) {
    const auto spa = linked_spa();
    seed_reviews(10);
    await(sync_->full_sync(spa));

    gbp_.remove(std::string(kSpa), "r5");        // really gone
    gbp_.hidden_from_lists = {"r6"};             // still there, but Google's list leaves it out
    clock_.advance(1h);
    await(sync_->full_sync(spa));

    EXPECT_EQ(count("reviews WHERE gbp_review_id = 'r5' AND removed_at IS NOT NULL"), 1);
    EXPECT_EQ(count("reviews WHERE removed_at IS NOT NULL"), 1) << "missing from the list is not enough";
    EXPECT_THAT(events("r5"), testing::ElementsAre("discovered:full_sync", "removed:full_sync"));
    EXPECT_THAT(events("r6"), testing::ElementsAre("discovered:full_sync"));
    const auto detail = boost::json::parse(value<std::string>("SELECT last_full_sync::text FROM locations WHERE id = " + std::to_string(spa)));
    EXPECT_EQ(detail.at("listed").as_int64(), 8);
    EXPECT_EQ(detail.at("missing_from_list").as_int64(), 2);
    EXPECT_EQ(detail.at("removed").as_int64(), 1);
    EXPECT_EQ(detail.at("still_there").as_int64(), 1);

    // It comes back (Google restored it): the review is active again.
    gbp_.put(make_review(kAccount, kSpa, "r5", "FIVE", "Lovely", "2026-10-01T00:05:00Z", "2026-10-01T00:05:00Z", "Thank you!"));
    clock_.advance(1h);
    await(sync_->full_sync(spa));
    EXPECT_EQ(count("reviews WHERE removed_at IS NOT NULL"), 0);
    EXPECT_THAT(events("r5"), testing::ElementsAre("discovered:full_sync", "removed:full_sync", "restored:full_sync"));
}

TEST_F(SyncTest, AnEmptyListFromGoogleDoesNotWipeTheStore) {
    const auto spa = linked_spa();
    seed_reviews(10);
    await(sync_->full_sync(spa));

    // Google answers with an empty list although every review still exists.
    for (int i = 1; i <= 10; ++i) gbp_.hidden_from_lists.push_back(std::format("r{}", i));
    clock_.advance(1h);
    await(sync_->full_sync(spa));
    EXPECT_EQ(count("reviews WHERE removed_at IS NULL"), 10);
    EXPECT_EQ(gbp_.get_calls, 10) << "each missing review was asked for by name";

    // And when the single-review call fails too, nothing is concluded.
    gbp_.fail_get = ApiError{ApiError::Kind::unavailable, 503, "HTTP 503"};
    clock_.advance(1h);
    await(sync_->full_sync(spa));
    EXPECT_EQ(count("reviews WHERE removed_at IS NULL"), 10);
    EXPECT_EQ(value<std::string>("SELECT last_full_sync->>'unchecked' FROM locations WHERE id = " + std::to_string(spa)), "10");
}

TEST_F(SyncTest, AFailedFullSyncIsRetriedAndShownOnTheLocation) {
    const auto spa = linked_spa();
    seed_reviews(5);
    await(sync_->full_sync(spa));
    gbp_.fail_list = ApiError{ApiError::Kind::permission_denied, 403, "HTTP 403 PERMISSION_DENIED"};

    EXPECT_THROW(await(sync_->full_sync(spa)), std::runtime_error) << "throwing makes the job queue retry it";
    EXPECT_EQ(value<std::string>("SELECT last_sync_error FROM locations WHERE id = " + std::to_string(spa)),
              "HTTP 403 PERMISSION_DENIED");
    EXPECT_EQ(count("reviews WHERE removed_at IS NULL"), 5);
}

// ---- reviews: polling ------------------------------------------------------------

TEST_F(SyncTest, PollFindsANewReviewAndStopsAtTheFirstPageWithNothingNew) {
    const auto spa = linked_spa();
    seed_reviews(120);
    await(sync_->full_sync(spa));
    await(sync_->poll_reviews());
    EXPECT_EQ(gbp_.batch_calls, 1) << "nothing new on the first page: one call";
    EXPECT_EQ(gbp_.last_batch_locations, std::vector<std::string>{std::string(kSpa)});

    gbp_.put(make_review(kAccount, kSpa, "new1", "ONE", "Cold food", "2026-11-10T11:58:00Z", "2026-11-10T11:58:00Z"));
    clock_.advance(2min);
    await(sync_->poll_reviews());

    EXPECT_EQ(gbp_.batch_calls, 3) << "page 1 had the new review, page 2 had nothing new, page 3 was not fetched";
    EXPECT_EQ(count("reviews"), 121);
    EXPECT_THAT(events("new1"), testing::ElementsAre("discovered:poll"));
    EXPECT_EQ(count("reviews WHERE gbp_review_id = 'new1' AND rating = 1 AND reply_text IS NULL"), 1);
    EXPECT_EQ(count("locations WHERE id = " + std::to_string(spa) + " AND reviews_polled_at = '" +
                    db::to_timestamptz(clock_.now()) + "'"),
              1);
}

TEST_F(SyncTest, EachLocationIsPolledAtItsOwnInterval) {
    const auto spa = linked_spa();
    seed_reviews(3);
    await(sync_->full_sync(spa));
    await(sync_->poll_reviews());
    ASSERT_EQ(gbp_.batch_calls, 1);

    clock_.advance(60s);
    await(sync_->poll_reviews());
    EXPECT_EQ(gbp_.batch_calls, 1) << "not due yet: the default interval is two minutes";

    clock_.advance(61s);
    await(sync_->poll_reviews());
    EXPECT_EQ(gbp_.batch_calls, 2);
}

TEST_F(SyncTest, OneBatchCallCoversEveryDueLocationOfAnAccount) {
    await(sync_->discover_locations());
    const auto client = make_client("Both");
    const auto spa = link(std::string(kSpa), client);
    const auto buffet = link(std::string(kBuffet), client);
    seed_reviews(2);
    gbp_.put(make_review(kAccount, kBuffet, "b1", "THREE", "Fine", "2026-10-05T00:00:00Z", "2026-10-05T00:00:00Z"));
    await(sync_->full_sync(spa));
    await(sync_->full_sync(buffet));

    gbp_.put(make_review(kAccount, kBuffet, "b2", "FIVE", "Great", "2026-11-10T11:00:00Z", "2026-11-10T11:00:00Z"));
    await(sync_->poll_reviews());
    EXPECT_EQ(gbp_.batch_calls, 1);
    EXPECT_THAT(gbp_.last_batch_locations, testing::UnorderedElementsAre(std::string(kSpa), std::string(kBuffet)));
    EXPECT_EQ(count("reviews r JOIN locations l ON l.id = r.location_id WHERE l.gbp_location_id = '9002'"), 2);
}

TEST_F(SyncTest, PollNoticesAnEditAndAReplyPostedOutsideTheSystem) {
    const auto spa = linked_spa();
    gbp_.put(make_review(kAccount, kSpa, "a", "FIVE", "Nice", "2026-11-01T10:00:00Z", "2026-11-01T10:00:00Z"));
    gbp_.put(make_review(kAccount, kSpa, "b", "FIVE", "Good", "2026-11-02T10:00:00Z", "2026-11-02T10:00:00Z"));
    await(sync_->full_sync(spa));

    // Someone replies to "a" in the GBP app: the review's updateTime doesn't move.
    gbp_.put(make_review(kAccount, kSpa, "a", "FIVE", "Nice", "2026-11-01T10:00:00Z", "2026-11-01T10:00:00Z",
                         "Thanks for coming!", "2026-11-10T09:00:00Z"));
    // The reviewer of "b" changes their mind.
    gbp_.put(make_review(kAccount, kSpa, "b", "TWO", "Actually, not good", "2026-11-02T10:00:00Z", "2026-11-10T10:00:00Z"));
    await(sync_->poll_reviews());

    EXPECT_THAT(events("a"), testing::ElementsAre("discovered:full_sync", "reply_added:poll"));
    EXPECT_THAT(events("b"), testing::ElementsAre("discovered:full_sync", "review_edited:poll"));
    EXPECT_EQ(count("reviews WHERE gbp_review_id = 'a' AND reply_text = 'Thanks for coming!' AND reply_origin = 'external'"), 1);
    EXPECT_EQ(count("reviews WHERE gbp_review_id = 'b' AND rating = 2 AND comment = 'Actually, not good'"), 1);
    EXPECT_EQ(value<std::string>("SELECT detail->'before'->>'comment' FROM review_events WHERE kind = 'review_edited'"), "Good");
}

TEST_F(SyncTest, AFailedPollIsRecordedAndTheNextOneClearsIt) {
    const auto spa = linked_spa();
    seed_reviews(2);
    await(sync_->full_sync(spa));
    gbp_.fail_batch = ApiError{ApiError::Kind::rate_limited, 429, "HTTP 429 RESOURCE_EXHAUSTED"};
    await(sync_->poll_reviews());  // does not throw: the next tick tries again
    EXPECT_EQ(value<std::string>("SELECT last_sync_error FROM locations WHERE id = " + std::to_string(spa)),
              "HTTP 429 RESOURCE_EXHAUSTED");

    gbp_.fail_batch.reset();
    clock_.advance(2min);
    await(sync_->poll_reviews());
    EXPECT_EQ(count("locations WHERE id = " + std::to_string(spa) + " AND last_sync_error IS NULL"), 1);
}

TEST_F(SyncTest, OnlyLinkedAccessibleLocationsOfActiveClientsAreSynced) {
    const auto spa = linked_spa();
    seed_reviews(2);
    await(sync_->full_sync(spa));
    gbp_.put(make_review(kAccount, kBuffet, "b1", "FIVE", "x", "2026-10-05T00:00:00Z", "2026-10-05T00:00:00Z"));
    const auto buffet = value<std::int64_t>("SELECT id FROM locations WHERE gbp_location_id = '9002'");

    await(sync_->full_sync(buffet));  // not linked to a client
    EXPECT_EQ(count("reviews WHERE location_id = " + std::to_string(buffet)), 0);

    exec("UPDATE clients SET archived = true");
    clock_.advance(1h);
    await(sync_->poll_reviews());
    EXPECT_EQ(gbp_.batch_calls, 0) << "an archived client's locations rest";
    EXPECT_EQ(count("jobs WHERE kind = 'reviews.full_sync'"), 0);

    exec("UPDATE clients SET archived = false");
    exec("UPDATE locations SET access_status = 'lost'");
    await(sync_->poll_reviews());
    EXPECT_EQ(gbp_.batch_calls, 0) << "so does a location we lost access to";
}

// ---- reviews: notifications ------------------------------------------------------

TEST_F(SyncTest, ANotifiedReviewIsFetchedAndStored) {
    const auto spa = linked_spa();
    seed_reviews(1);
    await(sync_->full_sync(spa));
    gbp_.put(make_review(kAccount, kSpa, "fresh", "FOUR", "Just now", "2026-11-10T11:59:00Z", "2026-11-10T11:59:00Z"));

    await(sync_->ingest_notification("accounts/111/locations/9001/reviews/fresh"));
    await(sync_->ingest_notification("accounts/111/locations/9001/reviews/fresh"));
    EXPECT_THAT(events("fresh"), testing::ElementsAre("discovered:notification"));
}

TEST_F(SyncTest, ANotifiedReviewThatIsGoneIsMarkedRemoved) {
    const auto spa = linked_spa();
    seed_reviews(2);
    await(sync_->full_sync(spa));
    gbp_.remove(std::string(kSpa), "r1");

    await(sync_->ingest_notification("accounts/111/locations/9001/reviews/r1"));
    EXPECT_THAT(events("r1"), testing::ElementsAre("discovered:full_sync", "removed:notification"));

    // Notified and gone before we ever saw it (spike B7): nothing to store, and not an error.
    await(sync_->ingest_notification("accounts/111/locations/9001/reviews/never-seen"));
    EXPECT_EQ(count("reviews"), 2);
}

TEST_F(SyncTest, NotificationsForOtherLocationsOrWithBadNamesAreIgnored) {
    linked_spa();
    gbp_.put(make_review(kAccount, kBuffet, "b1", "FIVE", "x", "2026-10-05T00:00:00Z", "2026-10-05T00:00:00Z"));
    await(sync_->ingest_notification("accounts/111/locations/9002/reviews/b1"));  // the buffet isn't linked
    await(sync_->ingest_notification("not a review name"));
    EXPECT_EQ(gbp_.get_calls, 0);
    EXPECT_EQ(count("reviews"), 0);
}

TEST_F(SyncTest, ANotificationThatCannotBeFetchedIsRetried) {
    linked_spa();
    gbp_.fail_get = ApiError{ApiError::Kind::unavailable, 503, "HTTP 503"};
    EXPECT_THROW(await(sync_->ingest_notification("accounts/111/locations/9001/reviews/x")), std::runtime_error);
}

TEST_F(SyncTest, PullingNotificationsEnqueuesEachReviewOnceAndAcknowledges) {
    linked_spa();
    test::FakeSubscriber subscriber;
    subscriber.publish("m1", R"({"type": "NEW_REVIEW", "review": "accounts/111/locations/9001/reviews/n1", "location": "locations/9001"})");
    subscriber.publish("m2", R"({"type": "GOOGLE_UPDATE", "location": "locations/9001"})");
    subscriber.publish("m3", "not json at all");
    subscriber.publish("m4", R"({"type": "UPDATED_REVIEW", "review": "accounts/111/locations/9001/reviews/n1"})");

    await(sync_->pull_notifications(subscriber));
    EXPECT_EQ(count("jobs WHERE kind = 'reviews.ingest_notification'"), 2);
    EXPECT_EQ(value<std::string>("SELECT payload->>'review' FROM jobs WHERE idempotency_key = 'm1'"),
              "accounts/111/locations/9001/reviews/n1");
    EXPECT_EQ(subscriber.acknowledged.size(), 4u) << "messages we don't act on are acknowledged too";
    EXPECT_TRUE(subscriber.waiting.empty());
    EXPECT_EQ(count("integration_status WHERE name = 'pubsub' AND last_error IS NULL AND detail ? 'last_message_at'"), 1);

    // Pub/Sub delivers at least once: the same message again adds no job.
    subscriber.publish("m1", R"({"type": "NEW_REVIEW", "review": "accounts/111/locations/9001/reviews/n1"})");
    await(sync_->pull_notifications(subscriber));
    EXPECT_EQ(count("jobs WHERE kind = 'reviews.ingest_notification'"), 2);
}

TEST_F(SyncTest, AFailedPullShowsOnTheStatusAndDoesNotThrow) {
    test::FakeSubscriber subscriber;
    subscriber.fail_pull = ApiError{ApiError::Kind::unauthenticated, 400, "HTTP 400 invalid_grant: reauth related error"};
    await(sync_->pull_notifications(subscriber));
    EXPECT_EQ(value<std::string>("SELECT last_error FROM integration_status WHERE name = 'pubsub'"),
              "HTTP 400 invalid_grant: reauth related error");

    subscriber.fail_pull.reset();
    await(sync_->pull_notifications(subscriber));
    EXPECT_EQ(count("integration_status WHERE name = 'pubsub' AND last_error IS NULL AND last_ok_at IS NOT NULL"), 1);
}

// ---- the whole path --------------------------------------------------------------

TEST_F(SyncTest, SchedulerQueueAndWorkersCarryANewReviewIntoTheStore) {
    jobs::Registry registry;
    test::FakeSubscriber subscriber;
    jobs::install(registry, *sync_, &subscriber);
    // A sync that stored something wakes the pipeline's sweep, which the engine always registers; it is not this test's subject.
    registry.add(std::string(jobs::kAnalyzePendingKind), [](const jobs::Job&) -> boost::asio::awaitable<void> { co_return; });
    jobs::Scheduler scheduler(io_.get_executor(), queue(), registry.recurring(), {});
    jobs::Worker worker(io_.get_executor(), queue(), registry, {.id = "test-worker"});
    const auto run_all = [&] {
        await(scheduler.tick());
        while (await(worker.run_once())) {
        }
    };

    run_all();  // discovery finds the locations
    link(std::string(kSpa), make_client("LU DREAM SPA"));
    seed_reviews(3);
    clock_.advance(1min);
    run_all();  // the poll job queues the full sync, which the worker runs next
    EXPECT_EQ(count("reviews"), 3);

    // A review is written on Google and announced through Pub/Sub at once...
    gbp_.put(make_review(kAccount, kSpa, "late", "FIVE", "Wonderful", "2026-11-10T12:01:30Z", "2026-11-10T12:01:30Z"));
    subscriber.publish("m9", R"({"type": "NEW_REVIEW", "review": "accounts/111/locations/9001/reviews/late"})");
    clock_.advance(30s);
    run_all();
    EXPECT_THAT(events("late"), testing::ElementsAre("discovered:notification"));

    // ...and one that is never announced is found by polling within the interval.
    gbp_.put(make_review(kAccount, kSpa, "quiet", "FOUR", "Good", "2026-11-10T12:02:00Z", "2026-11-10T12:02:00Z"));
    clock_.advance(3min);
    run_all();
    EXPECT_THAT(events("quiet"), testing::ElementsAre("discovered:poll"));
    EXPECT_EQ(count("jobs WHERE status = 'failed'"), 0);
}
