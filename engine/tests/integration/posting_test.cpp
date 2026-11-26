// Human-approved posting over a real PostgreSQL and a fake Google that
// holds replies back like the real one: every state a draft passes through,
// and every way a post must not happen (or must not happen twice).

#include "reply_fixture.hpp"

#include <gmock/gmock.h>


using namespace poni;
using namespace std::chrono_literals;
using test::kBuffet;
using test::kSpa;
using test::make_review;
using testing::HasSubstr;

namespace {

class Posting : public test::ReplyTest {
protected:
    // Approves the draft and runs the posting job.
    void approve_and_post(std::int64_t draft_id) {
        ASSERT_TRUE(approve(draft_id));
        run_jobs();
    }
    long open_posts() { return count("reply_posts WHERE status IN ('posting', 'posted')"); }
};

}  // namespace

// ---- the happy path, state by state ------------------------------------------------

TEST_F(Posting, AnApprovedDraftIsPostedOnceAndLaterConfirmedAsPublished) {
    const auto draft_id = waiting_draft();
    const auto review_id = std::stol(draft(draft_id, "review_id"));
    EXPECT_EQ(draft(draft_id, "status"), "draft");
    EXPECT_TRUE(gbp_.puts.empty()) << "nothing is posted without an approval, whatever the route";
    EXPECT_EQ(draft(draft_id, "route"), "auto_reply");

    const auto approved = approve(draft_id);
    ASSERT_TRUE(approved);
    EXPECT_EQ(approved->status, "approved");
    EXPECT_EQ(approved->decided_by, "staff@xvisionmediaus.com");
    EXPECT_TRUE(gbp_.puts.empty()) << "the approval itself sends nothing";

    run_jobs();
    ASSERT_EQ(gbp_.puts.size(), 1u);
    EXPECT_EQ(gbp_.puts[0].review_id, "r1");
    EXPECT_EQ(gbp_.puts[0].comment, draft(draft_id, "text")) << "the text that was approved, to the byte";
    EXPECT_EQ(draft(draft_id, "status"), "posted");
    EXPECT_EQ(post(draft_id, "status"), "posted");
    EXPECT_EQ(post(draft_id, "approved_by"), std::to_string(user_));
    // Google has accepted the reply and does not show it yet: the mirror says what Google says.
    EXPECT_EQ(review_value(review_id, "reply_text"), "");

    // Minutes pass in which reads show no reply. That is not a reason to post again or to draft again.
    clock_.advance(3min);
    EXPECT_EQ(confirm(), 1u);
    await(sync_->full_sync(spa_));
    EXPECT_EQ(await(pipeline_->enqueue_pending()), 0u);
    run_jobs();
    EXPECT_EQ(draft(draft_id, "status"), "posted");
    EXPECT_EQ(gbp_.puts.size(), 1u);
    EXPECT_EQ(count("reply_drafts"), 1);

    // Google starts showing it.
    gbp_.publish_held();
    clock_.advance(2min);
    EXPECT_EQ(confirm(), 0u);
    EXPECT_EQ(draft(draft_id, "status"), "published");
    EXPECT_EQ(post(draft_id, "status"), "published");
    EXPECT_EQ(review_value(review_id, "reply_origin"), "system");
    EXPECT_EQ(review_value(review_id, "reply_text"), draft(draft_id, "text"));
    EXPECT_EQ(count("review_events WHERE kind = 'reply_added' AND source = 'posting' AND detail->>'origin' = 'system'"), 1);

    // Who did what is in the audit log: the person approved, the engine posted.
    EXPECT_EQ(count("audit_log WHERE action = 'draft.approve' AND actor_type = 'user' AND actor = 'staff@xvisionmediaus.com'"), 1);
    EXPECT_EQ(count("audit_log WHERE action = 'reply.post_started' AND actor = 'system:reply-posting'"), 1);
    EXPECT_EQ(count("audit_log WHERE action = 'reply.posted'"), 1);
    EXPECT_EQ(count("audit_log WHERE action = 'reply.published'"), 1);
    EXPECT_EQ(gbp_.puts.size(), 1u);
    EXPECT_EQ(problems(), 0);
}

TEST_F(Posting, TheOrdinarySyncAlsoRecognisesTheEnginesOwnReply) {
    const auto draft_id = waiting_draft();
    approve_and_post(draft_id);
    gbp_.publish_held();
    await(sync_->full_sync(spa_));  // not the confirmation job: the hourly reconciliation got there first
    EXPECT_EQ(draft(draft_id, "status"), "published");
    EXPECT_EQ(review_value(std::stol(draft(draft_id, "review_id")), "reply_origin"), "system");
    EXPECT_EQ(confirm(), 0u);
}

TEST_F(Posting, GoogleReturningTheReplyWithOtherWhiteSpaceIsStillTheEnginesReply) {
    const auto draft_id = waiting_draft();
    approve_and_post(draft_id);
    gbp_.held["r1"].second = "  " + gbp_.held["r1"].second + "\n";
    gbp_.publish_held();
    EXPECT_EQ(confirm(), 0u);
    EXPECT_EQ(draft(draft_id, "status"), "published");
}

// ---- never twice --------------------------------------------------------------------

TEST_F(Posting, RunningThePostingJobAgainSendsNothing) {
    const auto draft_id = waiting_draft();
    approve_and_post(draft_id);
    await(poster_->post(draft_id, false));
    await(poster_->post(draft_id, true));
    EXPECT_EQ(gbp_.puts.size(), 1u);
    gbp_.publish_held();
    confirm();
    await(poster_->post(draft_id, false));
    EXPECT_EQ(gbp_.puts.size(), 1u);
    EXPECT_EQ(count("reply_posts"), 1);
}

TEST_F(Posting, ADraftThatNobodyApprovedAndNoPolicyScheduledIsNeverClaimed) {
    // The route says auto_reply; auto reply was never switched on, so the
    // draft is waiting. A posting job for it (there is none; this is the
    // guard under the guard) finds nothing it may post.
    const auto draft_id = waiting_draft();
    ASSERT_EQ(draft(draft_id, "route"), "auto_reply");
    pqxx::work tx{conn()};
    const auto claim = repo::claim_post(tx, draft_id, clock_.now());
    tx.commit();
    EXPECT_EQ(claim.outcome, repo::Claim::Outcome::not_approved);
    await(poster_->post(draft_id, false));
    EXPECT_TRUE(gbp_.puts.empty());
    EXPECT_EQ(count("reply_posts"), 0);
    EXPECT_EQ(draft(draft_id, "status"), "draft");
}

TEST_F(Posting, AnApprovedPostNamesItsApproverAndNoPolicyVersion) {
    const auto draft_id = waiting_draft();
    approve_and_post(draft_id);
    EXPECT_EQ(post(draft_id, "approved_by"), std::to_string(user_));
    EXPECT_EQ(post(draft_id, "policy_version"), "");
    EXPECT_EQ(draft(draft_id, "post_at"), "");
    EXPECT_EQ(count("audit_log WHERE actor = 'system:auto-reply'"), 0);
}

TEST_F(Posting, ADraftCannotBeApprovedTwice) {
    const auto draft_id = waiting_draft();
    ASSERT_TRUE(approve(draft_id));
    const auto again = approve(draft_id);
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().code, "draft_not_open");
    run_jobs();
    EXPECT_EQ(gbp_.puts.size(), 1u);
    EXPECT_EQ(count("jobs WHERE kind = 'replies.post'"), 1);
}

TEST_F(Posting, TheDatabaseItselfAllowsOneOpenPostPerReview) {
    const auto draft_id = waiting_draft();
    approve_and_post(draft_id);
    const auto second = "INSERT INTO reply_posts (review_id, draft_id, text, approved_by, status, started_at, updated_at) "
                        "SELECT review_id, draft_id, text, approved_by, 'posting', now(), now() FROM reply_posts";
    EXPECT_THROW(exec(second), pqxx::unique_violation);
}

TEST_F(Posting, TwoWorkersThatReachTheClaimTogetherProduceOnePost) {
    const auto draft_id = waiting_draft();
    ASSERT_TRUE(approve(draft_id));
    // The first worker has claimed and not yet committed; the second arrives and waits for the first worker's locks.
    const auto claim = [&](pqxx::work& tx) { return repo::claim_post(tx, draft_id, clock_.now()); };
    const auto [mine, theirs] = race(claim, claim);
    ASSERT_EQ(mine.outcome, repo::Claim::Outcome::claimed);
    EXPECT_EQ(theirs.outcome, repo::Claim::Outcome::in_flight);
    EXPECT_EQ(count("reply_posts"), 1);
}

TEST_F(Posting, WhileAPostIsUnconfirmedNoOtherDraftOfTheReviewCanBePosted) {
    const auto draft_id = waiting_draft();
    approve_and_post(draft_id);
    const auto review_id = draft(draft_id, "review_id");
    // Somebody forces a second approved draft into the database, past the API.
    exec("DROP INDEX reply_drafts_live_idx");
    exec("INSERT INTO reply_drafts (review_id, analysis_id, review_updated_time, status, route, mode, text, generated_text, language, "
         "generation_ok, model, prompt_version, inputs, decided_by, decided_at, created_at, updated_at) "
         "SELECT review_id, analysis_id, review_updated_time, 'approved', route, mode, 'A second reply.', 'A second reply.', language, "
         "true, model, prompt_version, inputs, decided_by, now(), now(), now() FROM reply_drafts WHERE id = " + std::to_string(draft_id));
    const auto second = live_draft(std::stol(review_id));
    await(poster_->post(second, false));
    EXPECT_EQ(gbp_.puts.size(), 1u) << "no reply is visible on Google yet, and still nothing is posted again";
    EXPECT_EQ(draft(second, "status"), "draft");
    EXPECT_THAT(draft(second, "status_reason"), HasSubstr("still waiting to appear"));
}

// ---- the kill switch ----------------------------------------------------------------

TEST_F(Posting, WithTheGlobalSwitchOffNothingCanBeApproved) {
    const auto draft_id = waiting_draft();
    exec("UPDATE system_settings SET posting_enabled = false");
    const auto refused = approve(draft_id);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "posting_disabled");
    EXPECT_EQ(draft(draft_id, "status"), "draft");
    EXPECT_EQ(count("jobs WHERE kind = 'replies.post'"), 0);
}

TEST_F(Posting, TheSwitchIsReadAgainImmediatelyBeforeThePut) {
    const auto draft_id = waiting_draft();
    ASSERT_TRUE(approve(draft_id));
    exec("UPDATE system_settings SET posting_enabled = false");  // flipped after the approval, before the job
    run_jobs();
    EXPECT_TRUE(gbp_.puts.empty());
    EXPECT_EQ(draft(draft_id, "status"), "draft") << "back in the queue: it does not go out later on its own";
    EXPECT_THAT(draft(draft_id, "status_reason"), HasSubstr("Posting is switched off"));
    EXPECT_EQ(count("reply_posts"), 0);

    exec("UPDATE system_settings SET posting_enabled = true");
    run_jobs();
    clock_.advance(1h);
    run_jobs();
    EXPECT_TRUE(gbp_.puts.empty()) << "switching back on posts nothing; a person approves again";
    approve_and_post(draft_id);
    EXPECT_EQ(gbp_.puts.size(), 1u);
}

TEST_F(Posting, ALocationsOwnSwitchStopsThatLocationOnly) {
    const auto spa_draft = waiting_draft("r1", kSpa);
    const auto buffet_draft = waiting_draft("r2", kBuffet);
    exec("UPDATE locations SET posting_enabled = false WHERE id = " + std::to_string(spa_));
    const auto refused = approve(spa_draft);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "posting_disabled");
    EXPECT_THAT(refused.error().message, HasSubstr("for this location"));
    approve_and_post(buffet_draft);
    ASSERT_EQ(gbp_.puts.size(), 1u);
    EXPECT_EQ(gbp_.puts[0].review_id, "r2");

    // Flipped between approval and posting.
    exec("UPDATE locations SET posting_enabled = true WHERE id = " + std::to_string(spa_));
    ASSERT_TRUE(approve(spa_draft));
    exec("UPDATE locations SET posting_enabled = false WHERE id = " + std::to_string(spa_));
    run_jobs();
    EXPECT_EQ(gbp_.puts.size(), 1u);
    EXPECT_EQ(draft(spa_draft, "status"), "draft");
}

// D-102 point 2: the location's own switch is read at the claim as the posting switches are.
TEST_F(Posting, ALocationSwitchedOffAfterTheApprovalIsNotPostedTo) {
    const auto draft_id = waiting_draft();
    ASSERT_TRUE(approve(draft_id));
    switch_location(spa_, false);
    run_jobs();
    EXPECT_TRUE(gbp_.puts.empty());
    EXPECT_EQ(draft(draft_id, "status"), "draft") << "back in the queue: it does not go out later on its own";
    EXPECT_THAT(draft(draft_id, "status_reason"), HasSubstr("no longer synced"));
    EXPECT_EQ(count("reply_posts"), 0);

    switch_location(spa_, true);
    clock_.advance(1h);
    run_jobs();
    EXPECT_TRUE(gbp_.puts.empty()) << "switching back on posts nothing; a person approves again";
}

// ---- the review is read again before posting ------------------------------------------

TEST_F(Posting, AReviewThatVanishedIsNotRepliedTo) {
    const auto draft_id = waiting_draft();
    ASSERT_TRUE(approve(draft_id));
    gbp_.remove(std::string(kSpa), "r1");
    run_jobs();
    EXPECT_TRUE(gbp_.puts.empty());
    EXPECT_EQ(draft(draft_id, "status"), "cancelled");
    EXPECT_EQ(draft(draft_id, "status_reason"), "the review was removed");
    EXPECT_NE(review_value(std::stol(draft(draft_id, "review_id")), "removed_at"), "");
    EXPECT_EQ(count("review_events WHERE kind = 'removed' AND source = 'posting'"), 1);
}

TEST_F(Posting, AReviewEditedAfterTheDraftWasWrittenIsNotRepliedTo) {
    const auto draft_id = waiting_draft();
    ASSERT_TRUE(approve(draft_id));
    // The mirror has not seen the edit yet; the job's own read finds it.
    gbp_.put(make_review(gbp_.account_id, kSpa, "r1", "ONE", "Edit: they double-charged my card.", "2026-11-09T10:00:00Z",
                         "2026-11-10T11:59:00Z"));
    run_jobs();
    EXPECT_TRUE(gbp_.puts.empty());
    EXPECT_EQ(draft(draft_id, "status"), "superseded");
    EXPECT_EQ(draft(draft_id, "status_reason"), "the review was edited");
    // The next pass of the pipeline drafts for the edited review, and that draft waits for a person.
    EXPECT_EQ(await(pipeline_->enqueue_pending()), 1u);
    run_jobs();
    EXPECT_EQ(count("reply_drafts WHERE status = 'draft'"), 1);
    EXPECT_TRUE(gbp_.puts.empty());
}

TEST_F(Posting, AReplyFromOutsideThatArrivesBetweenApprovalAndPostingStopsThePost) {
    const auto draft_id = waiting_draft();
    ASSERT_TRUE(approve(draft_id));
    gbp_.put(make_review(gbp_.account_id, kSpa, "r1", "FIVE", "Lovely head spa with Eva, so relaxing!", "2026-11-09T10:00:00Z",
                         "2026-11-09T10:00:00Z", "Thanks a lot, see you soon! - the owner"));
    run_jobs();
    EXPECT_TRUE(gbp_.puts.empty()) << "the owner's reply is never overwritten";
    EXPECT_EQ(draft(draft_id, "status"), "cancelled");
    EXPECT_EQ(draft(draft_id, "status_reason"), "a reply was posted outside the system");
    const auto review_id = std::stol(draft(draft_id, "review_id"));
    EXPECT_EQ(review_value(review_id, "reply_origin"), "external");
    EXPECT_EQ(review_value(review_id, "reply_text"), "Thanks a lot, see you soon! - the owner");
}

TEST_F(Posting, AReplyFromOutsideSeenByTheSyncCancelsWaitingAndApprovedDrafts) {
    const auto waiting = waiting_draft("r1");
    const auto approved = waiting_draft("r2");
    exec("UPDATE system_settings SET posting_enabled = true");
    ASSERT_TRUE(approve(approved));
    for (const auto* id : {"r1", "r2"}) {
        gbp_.put(make_review(gbp_.account_id, kSpa, id, "FIVE", "Lovely head spa with Eva, so relaxing!", "2026-11-09T10:00:00Z",
                             "2026-11-09T10:00:00Z", "Thank you!"));
    }
    await(sync_->full_sync(spa_));
    EXPECT_EQ(draft(waiting, "status"), "cancelled");
    EXPECT_EQ(draft(approved, "status"), "cancelled");
    run_jobs();
    EXPECT_TRUE(gbp_.puts.empty());
}

TEST_F(Posting, ADifferentReplyAppearingAfterThePostClosesItAsSuperseded) {
    const auto draft_id = waiting_draft();
    approve_and_post(draft_id);
    // The owner answered in the GBP app at the same moment; theirs is what Google shows.
    gbp_.held.clear();
    gbp_.put(make_review(gbp_.account_id, kSpa, "r1", "FIVE", "Lovely head spa with Eva, so relaxing!", "2026-11-09T10:00:00Z",
                         "2026-11-09T10:00:00Z", "Thank you, from the owner."));
    EXPECT_EQ(confirm(), 0u);
    EXPECT_EQ(post(draft_id, "status"), "superseded");
    EXPECT_EQ(draft(draft_id, "status"), "cancelled");
    EXPECT_EQ(review_value(std::stol(draft(draft_id, "review_id")), "reply_origin"), "external");
    EXPECT_EQ(gbp_.puts.size(), 1u);
}

TEST_F(Posting, WhenGoogleCannotBeReadNothingIsPostedAndTheJobTriesAgain) {
    const auto draft_id = waiting_draft();
    ASSERT_TRUE(approve(draft_id));
    gbp_.fail_get = gbp::ApiError{gbp::ApiError::Kind::unavailable, 503, "HTTP 503"};
    run_jobs();
    EXPECT_TRUE(gbp_.puts.empty());
    EXPECT_EQ(draft(draft_id, "status"), "approved");
    EXPECT_EQ(count("jobs WHERE kind = 'replies.post' AND status = 'queued'"), 1);

    gbp_.fail_get.reset();
    clock_.advance(2min);
    run_jobs();
    EXPECT_EQ(gbp_.puts.size(), 1u);
    EXPECT_EQ(draft(draft_id, "status"), "posted");
}

TEST_F(Posting, AfterTheLastFailedReadTheApprovalIsGivenBack) {
    const auto draft_id = waiting_draft();
    ASSERT_TRUE(approve(draft_id));
    gbp_.fail_get = gbp::ApiError{gbp::ApiError::Kind::unavailable, 503, "HTTP 503"};
    for (int i = 0; i < 6; ++i) {
        run_jobs();
        clock_.advance(2h);
    }
    EXPECT_TRUE(gbp_.puts.empty());
    EXPECT_EQ(draft(draft_id, "status"), "draft");
    EXPECT_THAT(draft(draft_id, "status_reason"), HasSubstr("could not be read from Google"));
    EXPECT_EQ(problems(), 1);
}

// ---- a PUT that fails, and one whose outcome nobody knows ------------------------------

TEST_F(Posting, ARefusedPutPostsNothingAndReturnsTheDraftWithTheReason) {
    const auto draft_id = waiting_draft();
    gbp_.fail_put = gbp::ApiError{gbp::ApiError::Kind::permission_denied, 403, "HTTP 403 PERMISSION_DENIED"};
    approve_and_post(draft_id);
    EXPECT_EQ(gbp_.puts.size(), 1u);
    EXPECT_EQ(post(draft_id, "status"), "failed");
    EXPECT_EQ(draft(draft_id, "status"), "draft");
    EXPECT_THAT(draft(draft_id, "status_reason"), HasSubstr("Google refused the reply"));
    EXPECT_EQ(open_posts(), 0);
    EXPECT_EQ(problems(), 1);
    clock_.advance(3h);
    run_jobs();
    EXPECT_EQ(gbp_.puts.size(), 1u) << "a refusal that will not pass is not retried";
}

TEST_F(Posting, ARateLimitedPutIsRetriedBecauseNothingWasPosted) {
    const auto draft_id = waiting_draft();
    gbp_.fail_put = gbp::ApiError{gbp::ApiError::Kind::rate_limited, 429, "HTTP 429"};
    approve_and_post(draft_id);
    EXPECT_EQ(post(draft_id, "status"), "failed");
    EXPECT_EQ(draft(draft_id, "status"), "approved");
    gbp_.fail_put.reset();
    clock_.advance(2min);
    run_jobs();
    EXPECT_EQ(gbp_.puts.size(), 2u);
    EXPECT_EQ(draft(draft_id, "status"), "posted");
    EXPECT_EQ(count("reply_posts"), 2);
    EXPECT_EQ(open_posts(), 1);
}

TEST_F(Posting, ATimeoutAfterTheReplyWasStoredIsNotPostedAgainAndConfirmsLater) {
    const auto draft_id = waiting_draft();
    gbp_.put_times_out = true;
    gbp_.timeout_after_storing = true;
    approve_and_post(draft_id);
    EXPECT_EQ(gbp_.puts.size(), 1u);
    EXPECT_EQ(draft(draft_id, "status"), "posting") << "sent, outcome unknown";
    EXPECT_EQ(post(draft_id, "status"), "posting");
    EXPECT_THAT(post(draft_id, "error"), testing::Not(testing::IsEmpty()));
    EXPECT_EQ(count("jobs WHERE kind = 'replies.post' AND status = 'succeeded'"), 1) << "the job is not retried";

    gbp_.put_times_out = false;
    clock_.advance(2h);
    // Not due again, and a hand-started rerun sends nothing either.
    run_jobs();
    await(poster_->post(draft_id, false));
    EXPECT_EQ(gbp_.puts.size(), 1u);
    EXPECT_EQ(await(pipeline_->enqueue_pending()), 0u);

    gbp_.publish_held();
    EXPECT_EQ(confirm(), 0u);
    EXPECT_EQ(draft(draft_id, "status"), "published");
    EXPECT_EQ(post(draft_id, "status"), "published");
    EXPECT_EQ(gbp_.puts.size(), 1u);
}

TEST_F(Posting, ATimeoutThatLostTheReplyGoesBackToAPersonWhenTheWindowEnds) {
    const auto draft_id = waiting_draft();
    gbp_.put_times_out = true;  // and Google never got it
    approve_and_post(draft_id);
    gbp_.put_times_out = false;
    EXPECT_EQ(draft(draft_id, "status"), "posting");

    clock_.advance(29min);
    EXPECT_EQ(confirm(), 1u);
    EXPECT_EQ(draft(draft_id, "status"), "posting") << "inside the window the engine only waits";
    clock_.advance(2min);
    EXPECT_EQ(confirm(), 0u);
    EXPECT_EQ(post(draft_id, "status"), "unconfirmed");
    EXPECT_EQ(draft(draft_id, "status"), "draft");
    EXPECT_THAT(draft(draft_id, "status_reason"), HasSubstr("did not appear"));
    EXPECT_EQ(problems(), 1);
    EXPECT_EQ(gbp_.puts.size(), 1u) << "the engine never repeats the PUT by itself";

    // A person looks, and approves again: that is a new decision and a new post.
    approve_and_post(draft_id);
    EXPECT_EQ(gbp_.puts.size(), 2u);
    gbp_.publish_held();
    confirm();
    EXPECT_EQ(draft(draft_id, "status"), "published");
}

TEST_F(Posting, A5xxFromThePutIsTreatedAsPossiblyPosted) {
    const auto draft_id = waiting_draft();
    gbp_.fail_put = gbp::ApiError{gbp::ApiError::Kind::unavailable, 503, "HTTP 503"};
    approve_and_post(draft_id);
    EXPECT_EQ(draft(draft_id, "status"), "posting");
    EXPECT_EQ(open_posts(), 1);
    clock_.advance(3h);
    run_jobs();
    EXPECT_EQ(gbp_.puts.size(), 1u);
}

TEST_F(Posting, AReplyThatShowsUpAfterTheWindowIsStillRecognised) {
    const auto draft_id = waiting_draft();
    approve_and_post(draft_id);
    clock_.advance(31min);
    confirm();
    EXPECT_EQ(draft(draft_id, "status"), "draft");
    EXPECT_EQ(await(pipeline_->enqueue_pending()), 0u) << "the returned draft is the review's draft; no second one is written";
    gbp_.publish_held();
    await(sync_->full_sync(spa_));
    EXPECT_EQ(draft(draft_id, "status"), "published");
    EXPECT_EQ(post(draft_id, "status"), "published");
    EXPECT_EQ(review_value(std::stol(draft(draft_id, "review_id")), "reply_origin"), "system");
    const auto late = approve(draft_id);
    ASSERT_FALSE(late);
    EXPECT_EQ(gbp_.puts.size(), 1u);
}

TEST_F(Posting, AReviewRemovedWhileItsReplyIsUnconfirmedClosesThePost) {
    const auto draft_id = waiting_draft();
    approve_and_post(draft_id);
    gbp_.remove(std::string(kSpa), "r1");
    EXPECT_EQ(confirm(), 0u);
    EXPECT_EQ(post(draft_id, "status"), "superseded");
    EXPECT_EQ(draft(draft_id, "status"), "cancelled");
}

TEST_F(Posting, AnApprovalWhoseJobDiedReturnsToTheQueue) {
    const auto draft_id = waiting_draft();
    ASSERT_TRUE(approve(draft_id));
    exec("UPDATE jobs SET status = 'failed' WHERE kind = 'replies.post'");
    clock_.advance(5min);
    confirm();
    EXPECT_EQ(draft(draft_id, "status"), "approved");
    clock_.advance(11min);
    confirm();
    EXPECT_EQ(draft(draft_id, "status"), "draft");
    EXPECT_THAT(draft(draft_id, "status_reason"), HasSubstr("did not finish"));
    EXPECT_TRUE(gbp_.puts.empty());
}

// ---- replacing the engine's own reply ---------------------------------------------------

TEST_F(Posting, OnlyTheEnginesOwnReplyMayBeReplaced) {
    const auto first = waiting_draft();
    approve_and_post(first);
    gbp_.publish_held();
    confirm();
    const auto review_id = std::stol(draft(first, "review_id"));
    const auto second_draft = [&](const std::string& text) {
        exec("INSERT INTO reply_drafts (review_id, analysis_id, review_updated_time, status, route, mode, text, generated_text, language, "
             "generation_ok, model, prompt_version, inputs, decided_by, decided_at, created_at, updated_at) "
             "SELECT review_id, analysis_id, review_updated_time, 'approved', route, mode, '" + text + "', '" + text + "', language, "
             "true, model, prompt_version, inputs, " + std::to_string(user_) + ", now(), now(), now() FROM reply_drafts WHERE id = " +
             std::to_string(first));
        return live_draft(review_id);
    };
    const auto edit = second_draft("Thank you again, Eva was delighted to read this.");
    await(poster_->post(edit, false));
    ASSERT_EQ(gbp_.puts.size(), 2u) << "the review's reply is the engine's own: it may be replaced";
    EXPECT_EQ(draft(edit, "status"), "posted");
    gbp_.publish_held("2026-11-10T13:00:00Z");
    confirm();
    EXPECT_EQ(draft(edit, "status"), "published");
    EXPECT_EQ(review_value(review_id, "reply_origin"), "system");

    // Someone rewrites the reply in the GBP app: from then on it is theirs.
    gbp_.put(make_review(gbp_.account_id, kSpa, "r1", "FIVE", "Lovely head spa with Eva, so relaxing!", "2026-11-09T10:00:00Z",
                         "2026-11-09T10:00:00Z", "Rewritten by the owner.", "2026-11-10T14:00:00Z"));
    await(sync_->full_sync(spa_));
    EXPECT_EQ(review_value(review_id, "reply_origin"), "external");
    const auto third = second_draft("One more try.");
    await(poster_->post(third, false));
    EXPECT_EQ(gbp_.puts.size(), 2u);
    EXPECT_EQ(draft(third, "status"), "cancelled");
}

// ---- what a person may approve -----------------------------------------------------------

TEST_F(Posting, ApprovalIsForTheExactStoredText) {
    const auto draft_id = waiting_draft();
    const auto other = as_staff_without_ip([&](pqxx::work& tx, audit::Writer& audit) {
        auto result = repo::approve_draft(tx, audit, draft_id, "Some other text.");
        if (!result) audit.nothing_changed();
        return result;
    });
    ASSERT_FALSE(other);
    EXPECT_EQ(other.error().code, "draft_changed");
    EXPECT_EQ(draft(draft_id, "status"), "draft");
}

TEST_F(Posting, AnEditIsCheckedAgainAndAFailedCheckBlocksApproval) {
    const auto draft_id = waiting_draft();
    const auto edit = [&](const std::string& text) {
        return as_staff_without_ip([&](pqxx::work& tx, audit::Writer& audit) {
            auto result = repo::edit_draft(tx, audit, draft_id, text);
            if (!result) audit.nothing_changed();
            return result;
        });
    };
    const auto with_link = edit("Thank you so much! Book your next visit at https://example.com/book today, we would love to see you.");
    ASSERT_TRUE(with_link);
    EXPECT_EQ(with_link->edited_by, "staff@xvisionmediaus.com");
    EXPECT_NE(with_link->generated_text, with_link->text) << "what the model wrote is kept next to the edit";
    const auto failed = std::ranges::find(with_link->checks, false, &domain::CheckResult::passed);
    ASSERT_NE(failed, with_link->checks.end());
    EXPECT_EQ(failed->name, "no_url");
    const auto refused = approve(draft_id);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "checks_failed");
    EXPECT_THAT(refused.error().message, HasSubstr("no_url"));

    const auto clean = edit("Thank you so much for visiting us, Eva will be delighted to hear this. See you again soon!");
    ASSERT_TRUE(clean);
    EXPECT_TRUE(std::ranges::all_of(clean->checks, &domain::CheckResult::passed));
    approve_and_post(draft_id);
    ASSERT_EQ(gbp_.puts.size(), 1u);
    EXPECT_EQ(gbp_.puts[0].comment, "Thank you so much for visiting us, Eva will be delighted to hear this. See you again soon!");
    EXPECT_EQ(count("audit_log WHERE action = 'draft.edit' AND actor_type = 'user'"), 2);
}

TEST_F(Posting, ARejectedDraftIsNeverPostedAndTheReviewIsNotDraftedAgain) {
    const auto draft_id = waiting_draft();
    const auto rejected = as_staff_without_ip([&](pqxx::work& tx, audit::Writer& audit) {
        return repo::reject_draft(tx, audit, draft_id, "The owner will answer this one herself.");
    });
    ASSERT_TRUE(rejected);
    EXPECT_EQ(rejected->status, "rejected");
    EXPECT_EQ(rejected->status_reason, "The owner will answer this one herself.");
    EXPECT_FALSE(approve(draft_id));
    EXPECT_EQ(await(pipeline_->enqueue_pending()), 0u);
    run_jobs();
    EXPECT_EQ(count("reply_drafts"), 1);
    EXPECT_TRUE(gbp_.puts.empty());
}

TEST_F(Posting, AnEmptyDraftCannotBeApproved) {
    llm_.generator = [](const llm::ChatRequest&) { return test::FakeLlm::failure(llm::Error::Kind::unavailable); };
    const auto id = review(kSpa, "r1", "FIVE", "Lovely head spa!");
    await(pipeline_->process(id, false, true));
    const auto refused = approve(live_draft(id));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "draft_empty");
}
