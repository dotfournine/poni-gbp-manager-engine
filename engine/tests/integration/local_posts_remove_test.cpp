// Removing a local post at Google, over a real PostgreSQL and a fake Google
// (D-088 point 7): what a person may delete, that nothing is deleted without
// a person's request, and what happens when the delete goes wrong.

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
using Kind = gbp::ApiError::Kind;

namespace {

class LocalPostsRemove : public test::LocalPostTest {
protected:
    // A post that is at Google and that the engine has read: made in Google's interface.
    std::int64_t at_google(const char* location, int id, const std::string& summary, std::string_view state = "LIVE") {
        google_.add(location, make_local_post("111", location, std::to_string(id), summary, state));
        sync(location == std::string(kSpaPosts) ? spa_ : buffet_);
        return post_named(name_of(location, id));
    }

    repo::Result<model::LocalPost> request(std::int64_t id) {
        return as_staff([&](pqxx::work& tx, audit::Writer& audit) {
            return repo::request_local_post_removal(tx, audit, id, test::kMediaBase);
        });
    }

    void remove(std::int64_t id, bool last_attempt = false) { await(local_posts_->remove(id, last_attempt)); }

    std::int64_t row(const std::string& columns, const std::string& values) {
        return insert_local_post(spa_, columns, values);
    }
};

}  // namespace

TEST_F(LocalPostsRemove, APersonRemovesAPostAndTheAuditLogKeepsWhatItSaid) {
    auto made = make_local_post("111", kBuffetPosts, "777", "Now open late on Fridays.", "SCHEDULED");
    made.scheduledTime = "2026-12-19T17:00:00Z";
    made.media = std::vector<gbp::PostMedia>{{.mediaFormat = "PHOTO", .googleUrl = "https://lh3.googleusercontent.com/p/a"},
                                             {.mediaFormat = "PHOTO", .googleUrl = "https://lh3.googleusercontent.com/p/b"}};
    made.raw = boost::json::value_from(made);
    google_.add(kBuffetPosts, made);
    sync(buffet_);
    const auto id = post_named(name_of(kBuffetPosts, 777));
    ASSERT_EQ(post(id, "origin"), "google") << "a post made in Google's own interface can be removed too";

    const auto requested = request(id);
    ASSERT_TRUE(requested) << requested.error().message;
    EXPECT_EQ(requested->status, "sent") << "it is still at Google until Google has deleted it";
    EXPECT_EQ(requested->removal_requested_by, "staff@xvisionmediaus.com");
    EXPECT_TRUE(google_.deletes.empty()) << "the request itself deletes nothing";
    EXPECT_EQ(count("jobs WHERE kind = 'local_posts.remove'"), 1);

    run_jobs();
    ASSERT_EQ(google_.deletes.size(), 1u);
    EXPECT_EQ(google_.deletes[0], name_of(kBuffetPosts, 777));
    EXPECT_EQ(post(id, "status"), "removed");
    EXPECT_NE(post(id, "removed_at"), "");
    EXPECT_TRUE(google_.posts[kBuffetPosts].empty());

    // Google keeps nothing of a deleted post; the audit entry does.
    const auto entry = [&](const std::string& expression) {
        return value<std::string>("SELECT " + expression + " FROM audit_log WHERE action = 'local_post.remove_requested'");
    };
    EXPECT_EQ(entry("actor"), "staff@xvisionmediaus.com");
    EXPECT_EQ(entry("after->'content'->>'summary'"), "Now open late on Fridays.");
    EXPECT_EQ(entry("after->'content'->>'scheduled_time'"), "2026-12-19T17:00:00.000000Z");
    EXPECT_EQ(entry("jsonb_array_length(after->'media')::text"), "2");
    EXPECT_EQ(entry("after->>'google_name'"), name_of(kBuffetPosts, 777));
    EXPECT_EQ(entry("after->>'origin'"), "google");
    EXPECT_EQ(value<std::string>("SELECT actor FROM audit_log WHERE action = 'local_post.removed'"), "system:local-posts");
    EXPECT_EQ(problems(), 0);

    // The next read does not bring it back, and nothing is deleted again.
    clock_.advance(1h);
    sync(buffet_);
    run_jobs();
    EXPECT_EQ(post(id, "status"), "removed");
    EXPECT_EQ(google_.deletes.size(), 1u);
}

TEST_F(LocalPostsRemove, RemovalWorksWhilePostingIsSwitchedOff) {
    // Off after installation, and nobody switched it on here: deleting is how a scheduled post is stopped.
    ASSERT_EQ(value<bool>("SELECT posting_enabled FROM system_settings"), false);
    exec(std::format("UPDATE locations SET posting_enabled = false WHERE id = {}", spa_));
    const auto id = at_google(kSpaPosts, 1, "Scheduled by mistake.", "SCHEDULED");
    ASSERT_TRUE(request(id));
    run_jobs();
    EXPECT_EQ(google_.deletes.size(), 1u);
    EXPECT_EQ(post(id, "status"), "removed");
}

TEST_F(LocalPostsRemove, NothingIsDeletedWithoutAPersonsRequest) {
    const auto id = at_google(kSpaPosts, 1, "Staying.");
    remove(id);
    remove(id, true);
    EXPECT_TRUE(google_.deletes.empty()) << "the job alone deletes nothing";
    EXPECT_EQ(post(id, "status"), "sent");
    remove(999999);
    EXPECT_TRUE(google_.deletes.empty());

    // A job that someone put into the queue by another way deletes nothing either.
    await(queue().enqueue({.kind = std::string(repo::kRemoveLocalPostKind), .payload = boost::json::object{{"local_post_id", id}}}));
    run_jobs();
    EXPECT_TRUE(google_.deletes.empty());
}

TEST_F(LocalPostsRemove, OnlyAPostThatGoogleHasCanBeRemoved) {
    const auto draft = row("origin, status, content", "'system', 'draft', " + content_sql(standard("A draft.")));
    const auto refused = request(draft);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "local_post_not_at_google") << "a draft is rejected, not removed";

    const auto unknown = row("origin, status, content, decided_by, approved_content, send_started_at",
                             std::format("'system', 'unknown', {}, {}, '{{}}', '{}'", content_sql(standard("Maybe there.")), user_,
                                         db::to_timestamptz(clock_.now())));
    const auto not_yet = request(unknown);
    ASSERT_FALSE(not_yet);
    EXPECT_EQ(not_yet.error().code, "local_post_not_at_google");
    EXPECT_THAT(not_yet.error().message, HasSubstr("not known yet"));

    // Nor one that failed, was rejected, or is on its way: none of them has a name at Google.
    for (const auto* status : {"failed", "rejected", "approved", "sending"}) {
        const auto other = row("origin, status, content, decided_by, approved_content, send_started_at",
                               std::format("'system', '{}', {}, {}, '{{}}', '{}'", status, content_sql(standard("Not there.")), user_,
                                           db::to_timestamptz(clock_.now())));
        EXPECT_EQ(request(other).error().code, "local_post_not_at_google") << status;
    }

    EXPECT_EQ(request(999999).error().kind, repo::Failure::Kind::not_found);
    EXPECT_EQ(count("jobs WHERE kind = 'local_posts.remove'"), 0);
    EXPECT_TRUE(google_.deletes.empty());
}

TEST_F(LocalPostsRemove, AskingAgainChangesNothing) {
    const auto id = at_google(kSpaPosts, 1, "Once is enough.");
    ASSERT_TRUE(request(id));
    ASSERT_TRUE(request(id));
    EXPECT_EQ(count("jobs WHERE kind = 'local_posts.remove'"), 1);
    EXPECT_EQ(audited("local_post.remove_requested"), 1);
    run_jobs();
    EXPECT_EQ(google_.deletes.size(), 1u);

    const auto after = request(id);
    ASSERT_TRUE(after) << "removed already: nothing to do, and no error";
    EXPECT_EQ(after->status, "removed");
    run_jobs();
    EXPECT_EQ(google_.deletes.size(), 1u);
    EXPECT_EQ(audited("local_post.remove_requested"), 1);
}

TEST_F(LocalPostsRemove, APostThatIsAlreadyGoneAtGoogleIsRemoved) {
    const auto id = at_google(kSpaPosts, 1, "Deleted in Google's interface a minute ago.");
    google_.remove(name_of(kSpaPosts, 1));
    ASSERT_TRUE(request(id));
    run_jobs();
    EXPECT_EQ(post(id, "status"), "removed") << "Google does not find it, and the location's own list confirms it is gone";
    EXPECT_TRUE(google_.deletes.empty()) << "nothing was left to delete";
    EXPECT_EQ(problems(), 0);
}

TEST_F(LocalPostsRemove, ANotFoundThatIsNotAboutThePostRemovesNothing) {
    // The location's account as the engine has it is not the account that has the location (it moved, or access
    // was lost, and discovery has not noticed yet). Google answers NOT_FOUND for every address under it. Taken for
    // "the post is gone", staff would believe a scheduled post was stopped while Google goes on to publish it.
    const auto id = at_google(kSpaPosts, 1, "Scheduled, and still there.", "SCHEDULED");
    exec(std::format("UPDATE locations SET gbp_account_id = '999' WHERE id = {}", spa_));
    ASSERT_TRUE(request(id));
    EXPECT_THROW(remove(id), std::runtime_error) << "it can't be confirmed either way: the job runs again";
    EXPECT_EQ(post(id, "status"), "sent");
    remove(id, true);
    EXPECT_EQ(post(id, "status"), "sent") << "never marked removed on a NOT_FOUND alone";
    EXPECT_EQ(post(id, "removal_requested_at"), "");
    EXPECT_THAT(post(id, "status_reason"), HasSubstr("could not be read to confirm"));
    EXPECT_THAT(post(id, "status_reason"), HasSubstr("may still be there"));
    EXPECT_TRUE(google_.deletes.empty());
    EXPECT_NE(google_.find(name_of(kSpaPosts, 1)), nullptr);
    EXPECT_EQ(problems(), 1);

    // Google does not find the post under its address and still lists it for the location: not gone.
    exec(std::format("UPDATE locations SET gbp_account_id = '111' WHERE id = {}", spa_));
    clock_.advance(1min);
    ASSERT_TRUE(request(id));
    google_.fail_get = test::FakeLocalPosts::not_found();
    remove(id);
    EXPECT_EQ(post(id, "status"), "sent");
    EXPECT_THAT(post(id, "status_reason"), HasSubstr("still lists it"));
    EXPECT_TRUE(google_.deletes.empty());
}

TEST_F(LocalPostsRemove, ThePostIsReadJustBeforeItIsDeletedAndThatCopyIsKept) {
    const auto id = at_google(kSpaPosts, 1, "As it was an hour ago.");
    // Someone changed it in Google's interface since the last read.
    auto changed = *google_.find(name_of(kSpaPosts, 1));
    changed.summary = "As it is now.";
    changed.raw = boost::json::value_from(changed);
    google_.add(kSpaPosts, changed);
    ASSERT_TRUE(request(id));
    run_jobs();
    EXPECT_EQ(post(id, "status"), "removed");
    EXPECT_EQ(post(id, "content->>'summary'"), "As it is now.");
    EXPECT_EQ(post(id, "raw->>'summary'"), "As it is now.") << "the row holds the last copy there will be";
    EXPECT_EQ(value<std::string>("SELECT after->'content'->>'summary' FROM audit_log WHERE action = 'local_post.removed'"), "As it is now.");
    EXPECT_EQ(value<std::string>("SELECT after->'content'->>'summary' FROM audit_log WHERE action = 'local_post.remove_requested'"),
              "As it was an hour ago.")
        << "what the person saw when they asked";
}

TEST_F(LocalPostsRemove, ARequestThatTheOutboundClientWouldNotSendIsGivenBackAtOnce) {
    const auto id = at_google(kSpaPosts, 1, "Odd address.");
    ASSERT_TRUE(request(id));
    google_.client_refuses = true;
    EXPECT_NO_THROW(remove(id)) << "trying again would be refused again";
    EXPECT_EQ(post(id, "status"), "sent");
    EXPECT_EQ(post(id, "removal_requested_at"), "") << "not left saying that a removal is under way";
    EXPECT_EQ(problems(), 1);
}

TEST_F(LocalPostsRemove, ARequestIsCarriedOutPromptlyOrGivenBack) {
    // Its job is gone: the request is given back, with a notice, instead of standing forever.
    const auto orphaned = at_google(kSpaPosts, 1, "The job vanished.");
    ASSERT_TRUE(request(orphaned));
    exec("DELETE FROM jobs WHERE kind = 'local_posts.remove'");
    clock_.advance(10min);
    await(local_posts_->enqueue_due());
    EXPECT_NE(post(orphaned, "removal_requested_at"), "") << "its job may only be late";
    clock_.advance(6min);
    await(local_posts_->enqueue_due());
    EXPECT_EQ(post(orphaned, "removal_requested_at"), "");
    EXPECT_THAT(post(orphaned, "status_reason"), HasSubstr("did not finish"));
    EXPECT_EQ(problems(), 1);

    // Its job ran very late (the engine was down): what a person asked an hour ago is not deleted without them.
    const auto late = at_google(kSpaPosts, 2, "Asked long ago.");
    ASSERT_TRUE(request(late));
    exec("UPDATE jobs SET run_at = run_at + interval '61 minutes' WHERE kind = 'local_posts.remove' AND status = 'queued'");
    clock_.advance(61min);
    run_jobs();
    EXPECT_TRUE(google_.deletes.empty());
    EXPECT_EQ(post(late, "status"), "sent");
    EXPECT_THAT(post(late, "status_reason"), HasSubstr("within 60 minutes"));
}

TEST_F(LocalPostsRemove, TwoRunsOfOneJobThatDisagreeLeaveThePostRemoved) {
    const auto id = at_google(kSpaPosts, 1, "Deleted by the slower run.");
    ASSERT_TRUE(request(id));
    // One run was refused and gave the request back; the other's delete went through.
    pqxx::work tx{conn()};
    repo::local_post_removal_failed(tx, id, "HTTP 403", false, clock_.now());
    EXPECT_NO_THROW(repo::local_post_removed(tx, id, clock_.now()));
    tx.commit();
    EXPECT_EQ(post(id, "status"), "removed");
    // And the other way round: a failure reported after the post is gone changes nothing.
    pqxx::work again{conn()};
    repo::local_post_removal_failed(again, id, "HTTP 503", true, clock_.now());
    again.commit();
    EXPECT_EQ(post(id, "status"), "removed");
    EXPECT_EQ(post(id, "status_reason"), "") << "what the refused run said about it no longer holds";
}

TEST_F(LocalPostsRemove, ARemovalAlreadySeenByTheSyncSendsNoDelete) {
    const auto id = at_google(kSpaPosts, 1, "Deleted at Google meanwhile.");
    ASSERT_TRUE(request(id));
    google_.remove(name_of(kSpaPosts, 1));
    sync(spa_);
    ASSERT_EQ(post(id, "status"), "removed");
    run_jobs();
    EXPECT_TRUE(google_.deletes.empty());
    EXPECT_EQ(problems(), 0);
}


TEST_F(LocalPostsRemove, ADeleteThatDidNotGetThroughIsSentAgainBecauseItIsSafeToRepeat) {
    const auto id = at_google(kSpaPosts, 1, "Hard to reach.");
    ASSERT_TRUE(request(id));

    google_.delete_times_out = true;
    EXPECT_THROW(remove(id), std::runtime_error) << "the job runs again";
    EXPECT_EQ(post(id, "status"), "sent");
    google_.delete_times_out = false;
    google_.fail_delete = gbp::ApiError{Kind::unavailable, 503, "HTTP 503 UNAVAILABLE"};
    EXPECT_THROW(remove(id), std::runtime_error);
    google_.fail_delete = gbp::ApiError{Kind::rate_limited, 429, "HTTP 429"};
    EXPECT_THROW(remove(id), std::runtime_error);
    google_.fail_delete.reset();
    remove(id);
    EXPECT_EQ(post(id, "status"), "removed");
    EXPECT_EQ(google_.deletes.size(), 4u);
    EXPECT_EQ(problems(), 0);

    // The connection dropped after Google had deleted the post: the second delete finds it gone.
    const auto other = at_google(kSpaPosts, 2, "Deleted, answer lost.");
    ASSERT_TRUE(request(other));
    google_.delete_times_out = true;
    google_.timeout_after_deleting = true;
    EXPECT_THROW(remove(other), std::runtime_error);
    google_.delete_times_out = false;
    remove(other);
    EXPECT_EQ(post(other, "status"), "removed");
}

TEST_F(LocalPostsRemove, ADeleteThatGoogleRefusesIsGivenBackAndStaffHear) {
    const auto id = at_google(kSpaPosts, 1, "Not ours to delete, says Google.");
    ASSERT_TRUE(request(id));
    google_.fail_delete = gbp::ApiError{Kind::permission_denied, 403, "HTTP 403 PERMISSION_DENIED"};
    run_jobs();
    EXPECT_EQ(google_.deletes.size(), 1u) << "a refusal is not tried again";
    EXPECT_EQ(post(id, "status"), "sent");
    EXPECT_EQ(post(id, "removal_requested_at"), "") << "the request is given back";
    EXPECT_THAT(post(id, "status_reason"), HasSubstr("could not be deleted at Google"));
    EXPECT_EQ(problems(), 1);
    EXPECT_EQ(value<std::string>("SELECT subject FROM notification_deliveries"), "A post could not be removed");
    EXPECT_EQ(audited("local_post.remove_failed"), 1);

    // A person asks again, and that is a new request with a new job.
    google_.fail_delete.reset();
    clock_.advance(1min);
    ASSERT_TRUE(request(id));
    EXPECT_EQ(audited("local_post.remove_requested"), 2);
    run_jobs();
    EXPECT_EQ(post(id, "status"), "removed");

    // Google unreachable until the job's last attempt: the same ending.
    const auto other = at_google(kSpaPosts, 2, "Unreachable.");
    ASSERT_TRUE(request(other));
    google_.fail_delete = gbp::ApiError{Kind::unavailable, 503, "HTTP 503 UNAVAILABLE"};
    remove(other, true);
    EXPECT_EQ(post(other, "status"), "sent");
    EXPECT_EQ(post(other, "removal_requested_at"), "");
    EXPECT_EQ(problems(), 2);
}

TEST_F(LocalPostsRemove, ARequestWhoseJobDiedCanBeMadeAgain) {
    const auto id = at_google(kSpaPosts, 1, "The job vanished.");
    ASSERT_TRUE(request(id));
    exec("DELETE FROM jobs WHERE kind = 'local_posts.remove'");
    clock_.advance(1min);
    ASSERT_TRUE(request(id));
    EXPECT_EQ(count("jobs WHERE kind = 'local_posts.remove'"), 1);
    run_jobs();
    EXPECT_EQ(post(id, "status"), "removed");
}

TEST_F(LocalPostsRemove, OnlyAPostOfALinkedLocationIsDeleted) {
    const auto id = at_google(kBuffetPosts, 1, "Client left.");
    exec(std::format("UPDATE locations SET client_id = NULL WHERE id = {}", buffet_));
    EXPECT_EQ(request(id).error().code, "location_not_synced");
    exec(std::format("UPDATE locations SET client_id = {}, access_status = 'lost' WHERE id = {}", buffet_client_, buffet_));
    EXPECT_EQ(request(id).error().code, "location_not_synced") << "nor one the engine has lost access to";
    exec(std::format("UPDATE locations SET access_status = 'active' WHERE id = {}", buffet_));

    // Unlinked between the request and the job.
    exec(std::format("UPDATE locations SET client_id = {} WHERE id = {}", buffet_client_, buffet_));
    ASSERT_TRUE(request(id));
    exec(std::format("UPDATE clients SET archived = true WHERE id = {}", buffet_client_));
    run_jobs();
    EXPECT_TRUE(google_.deletes.empty());
    EXPECT_EQ(post(id, "status"), "sent");
    EXPECT_EQ(post(id, "removal_requested_at"), "");
    EXPECT_EQ(problems(), 1);
}

TEST_F(LocalPostsRemove, TheDeleteNamesThatPostOfItsOwnLocationAndNothingElse) {
    // A row whose name at Google is another location's is not deleted: something is wrong with it.
    const auto foreign = row("origin, status, content, google_name",
                             "'google', 'sent', " + content_sql(standard("Misfiled.")) + ", 'accounts/111/locations/9002/localPosts/5'");
    ASSERT_TRUE(request(foreign));
    run_jobs();
    EXPECT_TRUE(google_.deletes.empty());
    EXPECT_EQ(post(foreign, "status"), "sent");
    EXPECT_THAT(post(foreign, "status_reason"), HasSubstr("not one of its location's"));

    // A name that is not a post's name at all never becomes a delete.
    for (const auto* name : {"accounts/111/locations/9001", "accounts/111/locations/9001/localPosts/..", "accounts/111/locations/9001/reviews/r1",
                             "accounts/111/locations/9001/localPosts/7/../../reviews/r1"}) {
        const auto odd = row("origin, status, content, google_name",
                             std::format("'google', 'sent', {}, '{}'", content_sql(standard("Odd.")), name));
        ASSERT_TRUE(request(odd)) << name;
        run_jobs();
        EXPECT_TRUE(google_.deletes.empty()) << name;
        EXPECT_EQ(post(odd, "status"), "sent") << name;
    }

    // The account's id as the location has it now: a location that moved to another account is still its location,
    // and its post is deleted under the account that has it.
    const auto moved = at_google(kSpaPosts, 3, "Read under the old account.");
    google_.account_id = "222";
    exec(std::format("UPDATE locations SET gbp_account_id = '222' WHERE id = {}", spa_));
    ASSERT_TRUE(request(moved));
    run_jobs();
    ASSERT_EQ(google_.deletes.size(), 1u);
    EXPECT_EQ(google_.deletes[0], "accounts/222/locations/9001/localPosts/3");
    EXPECT_EQ(post(moved, "status"), "removed");
    EXPECT_EQ(google_.find_post(kSpaPosts, "3"), nullptr) << "and it is really gone there";
}
