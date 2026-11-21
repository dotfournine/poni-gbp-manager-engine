// The read of a location's local posts over a real PostgreSQL and a fake
// Google (D-088 points 6 and 8): what shows up, how a send whose outcome was
// unknown is settled, and that nothing is called removed on a list's word.

#include "local_post_fixture.hpp"

#include <gmock/gmock.h>

using namespace poni;
using namespace std::chrono_literals;
using test::kBuffetPosts;
using test::kSpaPosts;
using test::make_local_post;
using test::content_sql;
using test::standard;
using testing::HasSubstr;

namespace {

class LocalPostsSync : public test::LocalPostTest {
protected:
    // A post the engine sent whose outcome it does not know: the row the publish job leaves behind after a timeout.
    // The send started now.
    std::int64_t unsettled(std::int64_t location, const std::string& summary, const std::string& status = "unknown",
                           std::optional<std::string> scheduled = std::nullopt) {
        auto content = standard(summary);
        content.scheduled_time = std::move(scheduled);
        return insert_local_post(location, "origin, status, content, created_by, decided_by, decided_at, approved_content, send_started_at",
                                 std::format("'system', '{}', {}, {}, {}, '{}', '{{}}', '{}'", status, content_sql(content), user_, user_,
                                             db::to_timestamptz(clock_.now()), db::to_timestamptz(clock_.now())));
    }

    // The post as Google would have it if the send arrived at `created`.
    gbp::LocalPost at_google(const char* location, int id, const std::string& summary, std::string_view created) {
        return make_local_post("111", location, std::to_string(id), summary, "LIVE", created);
    }

    std::string at(core::TimePoint t) { return std::format("{:%FT%T}Z", std::chrono::floor<std::chrono::seconds>(t)); }
};

}  // namespace

// ---- what shows up ----------------------------------------------------------------------

TEST_F(LocalPostsSync, APostMadeAtGoogleIsStoredWithWhatGoogleSaysAboutIt) {
    auto made = make_local_post("111", kBuffetPosts, "777", "Now open late on Fridays.", "LIVE", "2026-11-04T03:16:22.379352985Z");
    made.callToAction = gbp::CallToAction{.actionType = "CALL", .url = std::nullopt};
    made.scheduledTime = "2026-11-05T17:30:00Z";
    made.media = std::vector<gbp::PostMedia>{{.mediaFormat = "PHOTO", .googleUrl = "https://lh3.googleusercontent.com/p/a"},
                                             {.mediaFormat = "PHOTO", .googleUrl = "https://lh3.googleusercontent.com/p/b"},
                                             {.mediaFormat = "VIDEO", .googleUrl = "https://lh3.googleusercontent.com/p/c"}};
    made.raw = boost::json::value_from(made);
    google_.add(kBuffetPosts, made);

    const auto outcome = sync(buffet_);
    EXPECT_EQ(outcome.listed, 1);
    EXPECT_EQ(outcome.added, 1);
    const auto id = post_named(name_of(kBuffetPosts, 777));
    EXPECT_EQ(post(id, "origin"), "google");
    EXPECT_EQ(post(id, "status"), "sent");
    EXPECT_EQ(post(id, "google_state"), "live");
    EXPECT_EQ(post(id, "content->>'summary'"), "Now open late on Fridays.");
    EXPECT_EQ(post(id, "content->'call_to_action'->>'action_type'"), "call");
    EXPECT_EQ(post(id, "created_by"), "") << "nobody here wrote it";
    EXPECT_EQ(post(id, "jsonb_array_length(google_media)"), "3");
    EXPECT_EQ(post(id, "google_media->2->>'format'"), "video");
    EXPECT_EQ(post(id, "raw->>'summary'"), "Now open late on Fridays.") << "the payload is kept";
    EXPECT_EQ(post(id, "(google_created_at = '2026-11-04T03:16:22.379352Z'::timestamptz)"), "true");
    EXPECT_NE(post(id, "synced_at"), "");
    EXPECT_NE(value<std::string>(std::format("SELECT coalesce(local_posts_synced_at::text, '') FROM locations WHERE id = {}", buffet_)), "");
    EXPECT_EQ(count(std::format("local_posts WHERE location_id = {}", spa_)), 0) << "one location's posts are one location's";

    // Read again: the same row, nothing new.
    EXPECT_EQ(sync(buffet_).added, 0);
    EXPECT_EQ(count("local_posts"), 1);
}

TEST_F(LocalPostsSync, StatesAndChangesMadeAtGoogleShowUp) {
    google_.add(kSpaPosts, make_local_post("111", kSpaPosts, "1", "Opening soon.", "PROCESSING"));
    sync(spa_);
    const auto id = post_named(name_of(kSpaPosts, 1));
    EXPECT_EQ(post(id, "google_state"), "processing");
    const auto first_update = post(id, "updated_at");

    clock_.advance(1min);
    sync(spa_);
    EXPECT_EQ(post(id, "updated_at"), first_update) << "a read that finds nothing new changes nothing but the time it was read";

    clock_.advance(1min);
    google_.finish_processing();
    sync(spa_);
    EXPECT_EQ(post(id, "google_state"), "live");
    EXPECT_NE(post(id, "updated_at"), first_update);

    // Someone edits the post in Google's own interface.
    auto edited = *google_.find(name_of(kSpaPosts, 1));
    edited.summary = "Opening Monday.";
    edited.raw = boost::json::value_from(edited);
    google_.add(kSpaPosts, edited);
    sync(spa_);
    EXPECT_EQ(post(id, "content->>'summary'"), "Opening Monday.");

    // A state the engine does not know is no state; a kind it does not know is stored as an update, with the payload kept.
    auto odd = edited;
    odd.state = "ARCHIVED";
    odd.topicType = "PRODUCT";
    odd.raw = boost::json::value_from(odd);
    google_.add(kSpaPosts, odd);
    sync(spa_);
    EXPECT_EQ(post(id, "google_state"), "");
    EXPECT_EQ(post(id, "content->>'topic_type'"), "standard");
    EXPECT_EQ(post(id, "raw->>'topicType'"), "PRODUCT");
}

TEST_F(LocalPostsSync, EveryPageIsRead) {
    google_.page_size = 2;
    for (int i = 1; i <= 5; ++i) google_.add(kSpaPosts, make_local_post("111", kSpaPosts, std::to_string(i), std::format("Post {}.", i)));
    EXPECT_EQ(sync(spa_).listed, 5);
    EXPECT_EQ(google_.list_calls, 3);
    EXPECT_EQ(count("local_posts"), 5);
}

// ---- removed only on NOT_FOUND -------------------------------------------------------------

TEST_F(LocalPostsSync, APostTheListLacksIsRemovedOnlyWhenGoogleAnswersNotFoundForIt) {
    google_.add(kSpaPosts, make_local_post("111", kSpaPosts, "1", "Still here."));
    google_.add(kSpaPosts, make_local_post("111", kSpaPosts, "2", "Gone soon."));
    sync(spa_);
    const auto kept = post_named(name_of(kSpaPosts, 1));
    const auto gone = post_named(name_of(kSpaPosts, 2));

    // The list stops showing a post that still exists: it is asked for by name, and stays.
    google_.hidden_from_lists.insert(name_of(kSpaPosts, 1));
    sync(spa_);
    EXPECT_EQ(google_.get_calls, 1);
    EXPECT_EQ(post(kept, "status"), "sent") << "a list that lacks a post says nothing about it";

    // Google cannot be asked: nothing is known, nothing is removed.
    google_.remove(name_of(kSpaPosts, 2));
    google_.fail_get = gbp::ApiError{gbp::ApiError::Kind::unavailable, 503, "HTTP 503"};
    sync(spa_);
    EXPECT_EQ(post(gone, "status"), "sent");
    EXPECT_EQ(post(kept, "status"), "sent");

    // Google answers NOT_FOUND for the one that is gone, and only for that one.
    google_.fail_get.reset();
    clock_.advance(1min);
    sync(spa_);
    EXPECT_EQ(post(gone, "status"), "removed");
    EXPECT_NE(post(gone, "removed_at"), "");
    EXPECT_EQ(post(kept, "status"), "sent");
    EXPECT_EQ(audited("local_post.gone_at_google"), 1);

    // A removed post is not asked for again, and a list that is behind does not bring it back.
    const auto gets = google_.get_calls;
    google_.hidden_from_lists.clear();
    google_.add(kSpaPosts, make_local_post("111", kSpaPosts, "2", "Gone soon."));
    sync(spa_);
    EXPECT_EQ(post(gone, "status"), "removed");
    EXPECT_EQ(google_.get_calls, gets);
    EXPECT_EQ(count("local_posts"), 2);
}

TEST_F(LocalPostsSync, ALocationThatMovedToAnotherAccountKeepsItsPostsAsTheSameRows) {
    google_.add(kSpaPosts, make_local_post("111", kSpaPosts, "1", "Before the move."));
    google_.add(kSpaPosts, make_local_post("111", kSpaPosts, "2", "Also before."));
    sync(spa_);
    const auto first = post_named(name_of(kSpaPosts, 1));

    // The location now belongs to account 222, and Google names its posts under it.
    google_.account_id = "222";
    exec(std::format("UPDATE locations SET gbp_account_id = '222' WHERE id = {}", spa_));
    google_.posts[kSpaPosts].clear();
    google_.add(kSpaPosts, make_local_post("222", kSpaPosts, "1", "Before the move."));
    google_.add(kSpaPosts, make_local_post("222", kSpaPosts, "2", "Also before."));
    // One of them no list shows: it is asked for under the account the location has now, and found.
    google_.hidden_from_lists.insert("accounts/222/locations/9001/localPosts/2");
    const auto outcome = sync(spa_);
    EXPECT_EQ(outcome.added, 0) << "the same posts, not new ones made at Google";
    EXPECT_EQ(count("local_posts"), 2);
    EXPECT_EQ(post(first, "google_name"), "accounts/222/locations/9001/localPosts/1");
    EXPECT_EQ(count("local_posts WHERE status = 'removed'"), 0) << "a post asked for under an account the location has left is not gone";
    EXPECT_EQ(google_.get_calls, 1);
}

TEST_F(LocalPostsSync, AListThatCannotBeReadChangesNothingAndIsShown) {
    google_.add(kSpaPosts, make_local_post("111", kSpaPosts, "1", "Here."));
    sync(spa_);
    const auto id = post_named(name_of(kSpaPosts, 1));
    const auto waiting = unsettled(spa_, "Sent a while ago.");
    clock_.advance(20min);

    google_.fail_list = gbp::ApiError{gbp::ApiError::Kind::unavailable, 503, "HTTP 503 UNAVAILABLE"};
    EXPECT_THROW(sync(spa_), std::runtime_error) << "the job runs again";
    EXPECT_EQ(post(id, "status"), "sent");
    EXPECT_EQ(post(waiting, "status"), "unknown") << "a send is not given up because Google could not be asked";
    EXPECT_EQ(problems(), 0);
    const auto error = [&] {
        return value<std::string>(std::format("SELECT coalesce(local_posts_sync_error, '') FROM locations WHERE id = {}", spa_));
    };
    EXPECT_THAT(error(), HasSubstr("503"));

    google_.fail_list.reset();
    sync(spa_);
    EXPECT_EQ(error(), "") << "a read that worked clears the error";

    // A location that is no longer linked is not read at all.
    exec(std::format("UPDATE locations SET client_id = NULL WHERE id = {}", buffet_));
    const auto lists = google_.list_calls;
    EXPECT_EQ(sync(buffet_).listed, 0);
    EXPECT_EQ(google_.list_calls, lists);
}

// ---- a send whose outcome was unknown (D-088 point 6) ---------------------------------------

TEST_F(LocalPostsSync, AnUnknownSendIsAdoptedWhenGoogleHasThePost) {
    const auto id = unsettled(spa_, "Fall specials start Monday.");
    google_.add(kSpaPosts, at_google(kSpaPosts, 41, "Fall specials start Monday.", at(clock_.now() + 2s)));
    clock_.advance(1min);

    const auto outcome = sync(spa_);
    EXPECT_EQ(outcome.adopted, 1);
    EXPECT_EQ(outcome.added, 0) << "it is the engine's own post, not one made at Google";
    EXPECT_EQ(count("local_posts"), 1);
    EXPECT_EQ(post(id, "status"), "sent");
    EXPECT_EQ(post(id, "origin"), "system");
    EXPECT_EQ(post(id, "google_name"), name_of(kSpaPosts, 41));
    EXPECT_EQ(post(id, "google_state"), "live");
    EXPECT_NE(post(id, "sent_at"), "");
    EXPECT_EQ(post(id, "status_reason"), "");
    EXPECT_EQ(audited("local_post.adopted"), 1);
    EXPECT_EQ(problems(), 0);
}

TEST_F(LocalPostsSync, OnlyAPostWithTheSameContentCreatedSinceTheSendIsAdopted) {
    const auto scheduled = unsettled(spa_, "Holiday hours.", "unknown", "2026-12-19T17:00:00.000000Z");
    // The same text, but posted last week: someone's earlier post, not this send.
    google_.add(kSpaPosts, at_google(kSpaPosts, 1, "Holiday hours.", "2026-11-03T10:00:00Z"));
    // Created since the send, with another text.
    google_.add(kSpaPosts, at_google(kSpaPosts, 2, "Holiday hours!", at(clock_.now() + 1s)));
    // The same text created since the send, but not scheduled for the time that was sent.
    google_.add(kSpaPosts, at_google(kSpaPosts, 3, "Holiday hours.", at(clock_.now() + 1s)));
    clock_.advance(1min);
    auto outcome = sync(spa_);
    EXPECT_EQ(outcome.adopted, 0);
    EXPECT_EQ(outcome.added, 3) << "they are posts made at Google";
    EXPECT_EQ(post(scheduled, "status"), "unknown");

    // The send's own post: same text, same scheduled time, and created a little before the engine's clock says the send
    // started, because the two clocks differ.
    auto own = at_google(kSpaPosts, 4, "  Holiday hours.\n", at(clock_.now() - 1min - 90s));
    own.scheduledTime = "2026-12-19T17:00:00Z";
    own.state = "SCHEDULED";
    own.raw = boost::json::value_from(own);
    google_.add(kSpaPosts, own);
    outcome = sync(spa_);
    EXPECT_EQ(outcome.adopted, 1);
    EXPECT_EQ(post(scheduled, "google_name"), name_of(kSpaPosts, 4));
    EXPECT_EQ(post(scheduled, "google_state"), "scheduled");
    EXPECT_EQ(count("local_posts"), 4);
}

TEST_F(LocalPostsSync, TwoUnknownSendsOfTheSameContentTakeTwoDifferentPosts) {
    const auto first = unsettled(spa_, "Same words.");
    const auto second = unsettled(spa_, "Same words.");
    google_.add(kSpaPosts, at_google(kSpaPosts, 1, "Same words.", at(clock_.now() + 1s)));
    clock_.advance(1min);
    EXPECT_EQ(sync(spa_).adopted, 1) << "one post at Google settles one send";
    EXPECT_EQ(post(first, "status"), "sent");
    EXPECT_EQ(post(second, "status"), "unknown");

    google_.add(kSpaPosts, at_google(kSpaPosts, 2, "Same words.", at(clock_.now())));
    EXPECT_EQ(sync(spa_).adopted, 1);
    EXPECT_EQ(post(second, "google_name"), name_of(kSpaPosts, 2));
    EXPECT_EQ(count("local_posts"), 2);
}

TEST_F(LocalPostsSync, APostGoesToTheSendThatStartedLastBeforeItWasCreated) {
    // The first send was lost and failed. Staff copied the post and sent the copy, whose answer was lost too, but
    // which Google created. That post is the copy's: given to the first send, the copy would be reported as failed
    // while it is live, and sent a third time.
    const auto first = unsettled(spa_, "Fall specials start Monday.");
    clock_.advance(16min);
    sync(spa_);
    ASSERT_EQ(post(first, "status"), "failed");
    const auto copy = unsettled(spa_, "Fall specials start Monday.");
    google_.add(kSpaPosts, at_google(kSpaPosts, 1, "Fall specials start Monday.", at(clock_.now() + 2s)));
    clock_.advance(1min);
    EXPECT_EQ(sync(spa_).adopted, 1);
    EXPECT_EQ(post(copy, "status"), "sent");
    EXPECT_EQ(post(copy, "google_name"), name_of(kSpaPosts, 1));
    EXPECT_EQ(post(first, "status"), "failed") << "the send that was given up stays given up";
    EXPECT_EQ(count("local_posts"), 2);
}

TEST_F(LocalPostsSync, TwoSendsOfTheSameTextMinutesApartEachGetTheirOwnPostWhateverOrderGoogleListsThemIn) {
    const auto earlier = unsettled(spa_, "Same words.");
    const auto earlier_created = at(clock_.now() + 1s);
    clock_.advance(5min);
    const auto later = unsettled(spa_, "Same words.");
    // Newest first, as Google lists.
    google_.add(kSpaPosts, at_google(kSpaPosts, 1, "Same words.", earlier_created));
    google_.add(kSpaPosts, at_google(kSpaPosts, 2, "Same words.", at(clock_.now() + 1s)));
    clock_.advance(1min);
    EXPECT_EQ(sync(spa_).adopted, 2);
    EXPECT_EQ(post(earlier, "google_name"), name_of(kSpaPosts, 1));
    EXPECT_EQ(post(later, "google_name"), name_of(kSpaPosts, 2));
    EXPECT_EQ(count("local_posts"), 2);
}

TEST_F(LocalPostsSync, APostCreatedLongAfterASendWasGivenUpIsNotThatSends) {
    const auto id = unsettled(spa_, "Opening hours.");
    clock_.advance(16min);
    sync(spa_);
    ASSERT_EQ(post(id, "status"), "failed");
    // Hours later someone posts the same words in Google's own interface.
    clock_.advance(3h);
    google_.add(kSpaPosts, at_google(kSpaPosts, 1, "Opening hours.", at(clock_.now())));
    const auto outcome = sync(spa_);
    EXPECT_EQ(outcome.adopted, 0);
    EXPECT_EQ(outcome.added, 1) << "a post made at Google";
    EXPECT_EQ(post(id, "status"), "failed");
}

TEST_F(LocalPostsSync, AnUnknownSendThatNeverShowsFailsAfterFifteenMinutesAndStaffHearOnce) {
    const auto id = unsettled(spa_, "Never arrived.");
    clock_.advance(14min);
    EXPECT_EQ(sync(spa_).failed, 0);
    EXPECT_EQ(post(id, "status"), "unknown") << "not yet: Google may still be taking it in";
    EXPECT_EQ(problems(), 0);

    clock_.advance(1min);
    EXPECT_EQ(sync(spa_).failed, 1);
    EXPECT_EQ(post(id, "status"), "failed");
    EXPECT_THAT(post(id, "status_reason"), HasSubstr("not at Google 15 minutes later"));
    EXPECT_EQ(problems(), 1);
    EXPECT_EQ(audited("local_post.send_failed"), 1);
    EXPECT_EQ(value<std::string>("SELECT link_path FROM notification_deliveries"), std::format("/#/posts/{}", id));
    EXPECT_THAT(value<std::string>("SELECT body FROM notification_deliveries"), HasSubstr("Never arrived."));
    EXPECT_THAT(value<std::string>("SELECT subject FROM notification_deliveries"), testing::Not(HasSubstr("Never arrived")))
        << "a post's text goes into the body only";
    EXPECT_TRUE(google_.creates.empty()) << "nothing here sends anything";

    clock_.advance(1h);
    EXPECT_EQ(sync(spa_).failed, 0);
    EXPECT_EQ(problems(), 1);
}

TEST_F(LocalPostsSync, AFailedSendThatShowsAtGoogleAfterAllIsStillAdopted) {
    const auto id = unsettled(spa_, "Late arrival.");
    const auto sent_at = clock_.now();
    clock_.advance(16min);
    sync(spa_);
    ASSERT_EQ(post(id, "status"), "failed");

    // Google lists it an hour later, created when the send went out.
    clock_.advance(1h);
    google_.add(kSpaPosts, at_google(kSpaPosts, 9, "Late arrival.", at(sent_at + 3s)));
    const auto outcome = sync(spa_);
    EXPECT_EQ(outcome.adopted, 1);
    EXPECT_EQ(outcome.added, 0) << "not a second post made at Google";
    EXPECT_EQ(post(id, "status"), "sent");
    EXPECT_THAT(post(id, "status_reason"), HasSubstr("after its send had been reported as failed"));
    EXPECT_EQ(count("local_posts"), 1);
}

TEST_F(LocalPostsSync, APostLeftSendingByAnEngineThatStoppedIsTreatedAsUnknown) {
    const auto id = unsettled(spa_, "Mid-flight.", "sending");
    clock_.advance(1min);
    sync(spa_);
    EXPECT_EQ(post(id, "status"), "sending") << "the job may still be at work";
    EXPECT_EQ(problems(), 0);

    clock_.advance(5min);
    sync(spa_);
    EXPECT_EQ(post(id, "status"), "unknown");
    EXPECT_THAT(post(id, "status_reason"), HasSubstr("not sent again"));
    EXPECT_EQ(problems(), 1);
    EXPECT_EQ(audited("local_post.send_outcome_unknown"), 1);

    clock_.advance(10min);
    sync(spa_);
    EXPECT_EQ(post(id, "status"), "failed");
    EXPECT_EQ(problems(), 2);

    // Had Google taken it, the stale claim would have been settled as sent in one step.
    const auto arrived = unsettled(spa_, "Arrived mid-flight.", "sending");
    google_.add(kSpaPosts, at_google(kSpaPosts, 5, "Arrived mid-flight.", at(clock_.now() + 1s)));
    clock_.advance(6min);
    sync(spa_);
    EXPECT_EQ(post(arrived, "status"), "sent");
    EXPECT_EQ(problems(), 2);
}

// ---- Google rejects a post -----------------------------------------------------------------

TEST_F(LocalPostsSync, GoogleRejectingAPostIsToldToStaffOnce) {
    google_.add(kSpaPosts, make_local_post("111", kSpaPosts, "1", "Fine at first."));
    // Rejected before the engine ever saw it: shown as rejected, with no notice of a change the engine did not see.
    google_.add(kSpaPosts, make_local_post("111", kSpaPosts, "2", "Rejected long ago.", "REJECTED"));
    sync(spa_);
    EXPECT_EQ(problems(), 0);
    EXPECT_EQ(post(post_named(name_of(kSpaPosts, 2)), "google_state"), "rejected");

    google_.reject(name_of(kSpaPosts, 1));
    const auto outcome = sync(spa_);
    EXPECT_EQ(outcome.rejected, 1);
    const auto id = post_named(name_of(kSpaPosts, 1));
    EXPECT_EQ(post(id, "google_state"), "rejected");
    EXPECT_EQ(post(id, "status"), "sent") << "Google has it, and shows it to nobody";
    EXPECT_EQ(problems(), 1);
    EXPECT_EQ(value<std::string>("SELECT subject FROM notification_deliveries"), "Google rejected a post");
    EXPECT_EQ(audited("local_post.rejected_by_google"), 1);

    sync(spa_);
    EXPECT_EQ(problems(), 1) << "told once";
}

// ---- when a location is read ------------------------------------------------------------------

TEST_F(LocalPostsSync, EveryLinkedLocationIsReadOnceAnHourAndOneWithAnUnsettledSendOnEveryTick) {
    google_.add(kSpaPosts, make_local_post("111", kSpaPosts, "1", "Spa."));
    google_.add(kBuffetPosts, make_local_post("111", kBuffetPosts, "2", "Buffet."));
    EXPECT_EQ(await(local_posts_->enqueue_due()), 2u) << "never read yet";
    EXPECT_EQ(await(local_posts_->enqueue_due()), 0u) << "one job per location and tick";
    run_jobs();
    EXPECT_EQ(count("local_posts"), 2);

    clock_.advance(30min);
    EXPECT_EQ(await(local_posts_->enqueue_due()), 0u);
    clock_.advance(30min);
    EXPECT_EQ(await(local_posts_->enqueue_due()), 2u) << "an hour has passed";
    run_jobs();

    // A send that is not settled is looked at on every tick, not once an hour.
    unsettled(spa_, "Waiting.");
    clock_.advance(5min);
    EXPECT_EQ(await(local_posts_->enqueue_due()), 1u);
    run_jobs();
    clock_.advance(5min);
    EXPECT_EQ(await(local_posts_->enqueue_due()), 1u);
    run_jobs();
    clock_.advance(5min);
    EXPECT_EQ(await(local_posts_->enqueue_due()), 1u);
    run_jobs();
    EXPECT_EQ(count("local_posts WHERE status = 'failed'"), 1) << "settled 15 minutes after the send";
    clock_.advance(5min);
    EXPECT_EQ(await(local_posts_->enqueue_due()), 0u);

    // An archived client's locations are not read.
    clock_.advance(2h);
    exec(std::format("UPDATE clients SET archived = true WHERE id = {}", buffet_client_));
    EXPECT_EQ(await(local_posts_->enqueue_due()), 1u);
}

TEST_F(LocalPostsSync, AgainstTheGoogleMockNothingIsScheduled) {
    for (const bool mock : {false, true}) {
        jobs::Registry registry;
        jobs::install(registry, *local_posts_, mock);
        EXPECT_EQ(registry.recurring().size(), mock ? 0u : 1u) << (mock ? "the mock has no posts (D-088 point 13)" : "");
    }
}
