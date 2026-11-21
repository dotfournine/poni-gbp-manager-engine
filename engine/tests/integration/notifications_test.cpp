// Escalations and notifications over a real PostgreSQL with a channel that
// keeps what it is given: who is told about what, that an alert goes out
// with the analysis that raised it, and what the delivery log says when a
// message can't be or mustn't be sent.

#include "db/repo_cases.hpp"
#include "db/repo_notifications.hpp"
#include "jobs/scheduler.hpp"
#include "reply_fixture.hpp"

#include <gmock/gmock.h>

using namespace poni;
using namespace std::chrono_literals;
using test::FakeLlm;
using test::kBuffet;
using test::kSpa;
using test::make_review;
using testing::HasSubstr;
using testing::Not;

namespace {

class Notifications : public test::ReplyTest {
protected:
    std::int64_t rule(const std::string& event_type, std::optional<std::int64_t> client_id, std::vector<std::string> recipients) {
        return as_test([&](pqxx::work& tx, audit::Writer& audit) {
            return repo::create_rule(tx, audit, {.event_type = event_type, .client_id = client_id, .recipients = recipients}).value().id;
        });
    }

    // A review of the buffet that the analyzer flags as food poisoning. Returns its id.
    std::int64_t flagged_review(const std::string& review_id = "bad") {
        llm_.analyzer = FakeLlm::always(test::kSevereAnswer);
        llm_.generator = FakeLlm::always(test::kHoldingAnswer);
        return review(kBuffet, review_id, "TWO", "By 2am we were both throwing up. Pretty sure it was the crab legs.");
    }

    std::string delivery(const std::string& column, const std::string& where = "true") {
        return value<std::string>("SELECT coalesce(" + column + "::text, '') FROM notification_deliveries WHERE " + where +
                                  " ORDER BY id DESC LIMIT 1");
    }

    int emit(const repo::NotificationEvent& event) {
        return tx([&](pqxx::work& tx) { return repo::emit(tx, event, clock_.now()); });
    }

    static repo::NotificationEvent problem(std::string key) {
        return {.type = "system_problem", .key = std::move(key), .client_id = std::nullopt, .location_id = std::nullopt,
                .review_id = std::nullopt, .subject = "Something broke", .body = "Details.", .link_path = "/#/status"};
    }
};

}  // namespace

// ---- escalations ---------------------------------------------------------------------

TEST_F(Notifications, ASeverityFlagOpensAnEscalationAndAlertsAtOnce) {
    rule("escalation", std::nullopt, {"owner@xvisionmediaus.com", "manager@xvisionmediaus.com"});
    // The whole path as main runs it: the poll stores the review, which
    // queues the sweep, which queues the analysis, which raises the alert,
    // whose delivery is a job of its own.
    await(sync_->full_sync(buffet_));
    run_jobs();
    llm_.analyzer = FakeLlm::always(test::kSevereAnswer);
    llm_.generator = FakeLlm::always(test::kHoldingAnswer);
    gbp_.put(make_review(gbp_.account_id, kBuffet, "bad", "TWO", "By 2am we were both throwing up. Pretty sure it was the crab legs.",
                         "2026-11-10T11:58:00Z", "2026-11-10T11:58:00Z"));
    const auto synced_at = clock_.now();
    await(sync_->poll_reviews());
    run_jobs();

    ASSERT_EQ(count("cases"), 1);
    EXPECT_EQ(value<std::string>("SELECT status || ' ' || opened_by FROM cases"), "open severity_flag");
    EXPECT_EQ(value<std::string>("SELECT severity_flags->0->>'id' FROM cases"), "illness_food_poisoning");
    EXPECT_EQ(count("audit_log WHERE action = 'escalation.open' AND actor = 'system:review-analysis'"), 1);

    // Both recipients were written to, in the same run of jobs: no clock time passed at all.
    ASSERT_EQ(channel_.sent.size(), 2u);
    EXPECT_EQ(count("notification_deliveries WHERE event_type = 'escalation' AND status = 'sent'"), 2);
    EXPECT_LE(clock_.now() - synced_at, 5min);
    const auto& message = channel_.sent[0];
    EXPECT_EQ(message.recipient, "owner@xvisionmediaus.com");
    EXPECT_THAT(message.subject, HasSubstr("Escalation: Gangnam AYCE Buffet"));
    EXPECT_THAT(message.subject, HasSubstr("Illness or food poisoning"));
    EXPECT_THAT(message.subject, Not(HasSubstr("throwing up"))) << "a reviewer's words never go into a header";
    EXPECT_THAT(message.body, HasSubstr("By 2am we were both throwing up"));
    EXPECT_THAT(message.body, HasSubstr("Gangnam AYCE Buffet"));
    EXPECT_THAT(message.body, HasSubstr("http://localhost:5173/#/escalations/1/show"));

    // The holding reply waits for a person like every other draft, and the escalation says so once.
    EXPECT_EQ(value<std::string>("SELECT route || ' ' || status FROM reply_drafts"), "escalate draft");
    EXPECT_EQ(count("notification_deliveries WHERE event_type = 'approval_needed'"), 0);
    EXPECT_TRUE(gbp_.puts.empty());
}

TEST_F(Notifications, AnEscalationAlertsOncePerOpening) {
    rule("escalation", std::nullopt, {"owner@xvisionmediaus.com"});
    const auto id = flagged_review();
    await(pipeline_->process(id, false, false));
    await(pipeline_->process(id, false, false));  // the job ran twice
    run_jobs();
    EXPECT_EQ(count("cases"), 1);
    EXPECT_EQ(channel_.sent.size(), 1u);

    // Analyzed again while still open: the flags are refreshed, nobody is mailed again.
    await(pipeline_->process(id, true, false));
    run_jobs();
    EXPECT_EQ(channel_.sent.size(), 1u);
}

TEST_F(Notifications, AnEscalationIsAssignedAndClosedWithAReasonAndOpensAgainIfTheReviewIsFlaggedAgain) {
    rule("escalation", std::nullopt, {"owner@xvisionmediaus.com"});
    const auto id = flagged_review();
    await(pipeline_->process(id, false, false));
    const auto case_id = value<std::int64_t>("SELECT id FROM cases");
    {
        const auto assigned = as_staff_without_ip([&](pqxx::work& tx, audit::Writer& audit) {
            return repo::assign_escalation(tx, audit, case_id, user_);
        });
        ASSERT_TRUE(assigned);
        EXPECT_EQ(assigned->assignee->email, "staff@xvisionmediaus.com");
        EXPECT_EQ(assigned->review.id, id);
        const auto nobody = as_staff_without_ip([&](pqxx::work& tx, audit::Writer& audit) {
            auto result = repo::assign_escalation(tx, audit, case_id, 99999);
            audit.nothing_changed();
            return result;
        });
        EXPECT_EQ(nobody.error().code, "user_not_found");
        const auto bad_reason = as_staff_without_ip([&](pqxx::work& tx, audit::Writer& audit) {
            auto result = repo::close_escalation(tx, audit, case_id, "because", "");
            audit.nothing_changed();
            return result;
        });
        EXPECT_EQ(bad_reason.error().kind, repo::Failure::Kind::invalid);
        const auto closed = as_staff_without_ip([&](pqxx::work& tx, audit::Writer& audit) {
            return repo::close_escalation(tx, audit, case_id, "resolved", "Called the guest; refunded by the owner.");
        });
        ASSERT_TRUE(closed);
        EXPECT_EQ(closed->status, "closed");
        EXPECT_EQ(closed->close_reason, "resolved");
        EXPECT_EQ(closed->closed_by, "staff@xvisionmediaus.com");
        const auto again = as_staff_without_ip([&](pqxx::work& tx, audit::Writer& audit) {
            auto result = repo::close_escalation(tx, audit, case_id, "resolved", "");
            audit.nothing_changed();
            return result;
        });
        EXPECT_EQ(again.error().code, "escalation_closed");
    }
    EXPECT_EQ(count("audit_log WHERE action IN ('escalation.assign', 'escalation.close') AND actor = 'staff@xvisionmediaus.com'"), 2);
    run_jobs();
    ASSERT_EQ(channel_.sent.size(), 1u);

    // The same analysis again changes nothing: the case stays closed.
    await(pipeline_->process(id, false, false));
    EXPECT_EQ(value<std::string>("SELECT status FROM cases"), "closed");
    // The reviewer edits the review and it is flagged again: one review, one case, opened again, and a new alert.
    review(kBuffet, "bad", "ONE", "Update: my husband went to the ER. We are calling the health department.", std::nullopt,
           "2026-11-10T11:00:00Z");
    await(pipeline_->process(id, false, false));
    run_jobs();
    EXPECT_EQ(count("cases"), 1);
    EXPECT_EQ(value<std::string>("SELECT status FROM cases"), "open");
    EXPECT_EQ(count("audit_log WHERE action = 'escalation.reopen'"), 1);
    EXPECT_EQ(channel_.sent.size(), 2u);
}

TEST_F(Notifications, AFlaggedDraftFromBeforeEscalationsExistedGetsOneOnTheNextPass) {
    rule("escalation", std::nullopt, {"owner@xvisionmediaus.com"});
    const auto id = flagged_review();
    await(pipeline_->process(id, false, false));
    run_jobs();
    channel_.sent.clear();
    // As the pipeline leaves it: the analysis and the holding reply, and no case.
    exec("DELETE FROM notification_deliveries");
    exec("DELETE FROM cases");
    EXPECT_EQ(await(pipeline_->enqueue_pending()), 1u);
    run_jobs();
    EXPECT_EQ(count("cases WHERE status = 'open'"), 1);
    EXPECT_EQ(channel_.sent.size(), 1u);
    EXPECT_EQ(count("review_analyses"), 1) << "nothing was analyzed or drafted again";
    EXPECT_EQ(count("reply_drafts"), 1);
    EXPECT_EQ(await(pipeline_->enqueue_pending()), 0u);
}

TEST_F(Notifications, AFlaggedReviewThatAlreadyHasAReplyOpensNothing) {
    llm_.analyzer = FakeLlm::always(test::kSevereAnswer);
    const auto id = review(kBuffet, "old", "TWO", "We were both throwing up.", "We are so sorry, please call us.");
    await(pipeline_->process(id, false, false));
    EXPECT_EQ(count("review_analyses"), 1);
    EXPECT_EQ(count("cases"), 0) << "the business answered this one long ago";
    EXPECT_EQ(count("notification_deliveries"), 0);
}

// ---- approval needed ------------------------------------------------------------------

TEST_F(Notifications, ANewDraftTellsTheClientsRecipientsThatApprovalIsNeeded) {
    rule("approval_needed", spa_client_, {"spa-team@xvisionmediaus.com"});
    rule("approval_needed", std::nullopt, {"everyone@xvisionmediaus.com", "spa-team@xvisionmediaus.com"});
    rule("escalation", std::nullopt, {"owner@xvisionmediaus.com"});
    waiting_draft("r1", kSpa);
    run_jobs();
    // The client's rule and the rule for all clients both apply; an address named twice is written to once.
    ASSERT_EQ(channel_.sent.size(), 2u);
    EXPECT_EQ(count("notification_deliveries WHERE event_type = 'approval_needed' AND status = 'sent'"), 2);
    const auto& message = channel_.sent[0];
    EXPECT_THAT(message.subject, HasSubstr("Approval needed: LU DREAM SPA - Chelsea"));
    EXPECT_THAT(message.body, HasSubstr("Lovely head spa with Eva"));
    EXPECT_THAT(message.body, HasSubstr("Thank you so much for your kind words"));
    EXPECT_THAT(message.body, HasSubstr("What the policy would do: auto_post"));
    EXPECT_THAT(message.body, HasSubstr("/#/drafts/1/show"));

    channel_.sent.clear();
    waiting_draft("r2", kBuffet);
    run_jobs();
    ASSERT_EQ(channel_.sent.size(), 2u) << "the spa's own rule does not apply to the buffet";
    EXPECT_EQ(channel_.sent[0].recipient, "everyone@xvisionmediaus.com");
}

// ---- rules and the delivery log -------------------------------------------------------

TEST_F(Notifications, AnEventNobodyIsToldAboutIsStillInTheLog) {
    EXPECT_EQ(emit(problem("p1")), 1);
    EXPECT_EQ(emit(problem("p1")), 0) << "the same event is recorded once";
    EXPECT_EQ(delivery("status || ' ' || status_reason"), "not_sent no notification rule names a recipient for this event");
    run_jobs();
    EXPECT_TRUE(channel_.sent.empty());
}

TEST_F(Notifications, ADisabledRuleAndARuleOfAnotherEventDoNotApply) {
    const auto id = rule("system_problem", std::nullopt, {"ops@xvisionmediaus.com"});
    rule("escalation", std::nullopt, {"owner@xvisionmediaus.com"});
    exec("UPDATE notification_rules SET enabled = false WHERE id = " + std::to_string(id));
    emit(problem("p1"));
    EXPECT_EQ(delivery("recipient"), "");
}

TEST_F(Notifications, WithSendingSwitchedOffNothingLeavesAndTheLogSaysWhy) {
    rule("system_problem", std::nullopt, {"ops@xvisionmediaus.com"});
    exec("UPDATE system_settings SET email_sending_enabled = false");
    emit(problem("p1"));
    run_jobs();
    EXPECT_TRUE(channel_.sent.empty());
    EXPECT_EQ(delivery("status || ' ' || status_reason"), "not_sent email sending is switched off");
    // Switching it on later does not send what was held back: a stale alert is worse than a missing one.
    exec("UPDATE system_settings SET email_sending_enabled = true");
    clock_.advance(2h);
    run_jobs();
    EXPECT_TRUE(channel_.sent.empty());
}

TEST_F(Notifications, WithoutASignedInSenderTheDeliveryIsNotSent) {
    rule("system_problem", std::nullopt, {"ops@xvisionmediaus.com"});
    channel_.failures.push_back({notify::SendError::Kind::not_configured, "the sending mailbox is not signed in"});
    emit(problem("p1"));
    run_jobs();
    EXPECT_EQ(delivery("status"), "not_sent");
    EXPECT_THAT(delivery("status_reason"), HasSubstr("not signed in"));
    EXPECT_EQ(count("jobs WHERE kind = 'notifications.deliver' AND status = 'succeeded'"), 1);
}

TEST_F(Notifications, AFailureThatMayPassIsRetriedWithBackoff) {
    rule("system_problem", std::nullopt, {"ops@xvisionmediaus.com"});
    channel_.failures.push_back({notify::SendError::Kind::transient, "HTTP 503"});
    channel_.failures.push_back({notify::SendError::Kind::transient, "network error: timed out"});
    emit(problem("p1"));
    run_jobs();
    EXPECT_EQ(delivery("status || ' ' || attempts || ' ' || status_reason"), "pending 1 HTTP 503");
    run_jobs();
    EXPECT_EQ(delivery("attempts"), "1") << "not before the backoff has passed";
    clock_.advance(31s);
    run_jobs();
    EXPECT_EQ(delivery("attempts"), "2");
    clock_.advance(61s);
    run_jobs();
    EXPECT_EQ(delivery("status || ' ' || attempts"), "sent 3");
    ASSERT_EQ(channel_.sent.size(), 1u);
    EXPECT_THAT(channel_.sent[0].body, HasSubstr("Open in the dashboard: http://localhost:5173/#/status"));
    EXPECT_EQ(value<std::string>("SELECT coalesce(last_error, 'none') FROM integration_status WHERE name = 'email'"), "none");
}

TEST_F(Notifications, ARefusalAndTheLastFailedAttemptEndAsFailed) {
    rule("system_problem", std::nullopt, {"ops@xvisionmediaus.com"});
    channel_.failures.push_back({notify::SendError::Kind::permanent, "HTTP 403 PERMISSION_DENIED"});
    emit(problem("p1"));
    run_jobs();
    EXPECT_EQ(delivery("status || ' ' || status_reason"), "failed HTTP 403 PERMISSION_DENIED");

    for (int i = 0; i < 20; ++i) channel_.failures.push_back({notify::SendError::Kind::transient, "HTTP 503"});
    emit(problem("p2"));
    for (int i = 0; i < 12; ++i) {
        run_jobs();
        clock_.advance(2h);
    }
    EXPECT_EQ(delivery("status || ' ' || attempts", "event_key = 'p2'"), "failed 8");
    EXPECT_TRUE(channel_.sent.empty());
    EXPECT_EQ(value<std::string>("SELECT last_error FROM integration_status WHERE name = 'email'"), "HTTP 503");
}

TEST_F(Notifications, RulesTakeOnlyRealAddressesAndOneRulePerEventAndClient) {
    const auto create = [&](repo::NewRule rule) {
        return as_test([&](pqxx::work& tx, audit::Writer& audit) {
            auto result = repo::create_rule(tx, audit, rule);
            if (!result) audit.nothing_changed();
            return result;
        });
    };
    const auto made = create({.event_type = "escalation", .client_id = std::nullopt, .recipients = {" Owner@XVisionMediaUS.com ", "owner@xvisionmediaus.com"}});
    ASSERT_TRUE(made);
    EXPECT_THAT(made->recipients, testing::ElementsAre("owner@xvisionmediaus.com"));
    EXPECT_EQ(create({.event_type = "escalation", .client_id = std::nullopt, .recipients = {"x@example.com"}}).error().code, "rule_exists");
    EXPECT_EQ(create({.event_type = "escalation", .client_id = 99999, .recipients = {"x@example.com"}}).error().code,
              "client_not_found");
    EXPECT_EQ(create({.event_type = "test", .client_id = std::nullopt, .recipients = {"x@example.com"}}).error().kind, repo::Failure::Kind::invalid);
    for (const auto* bad : {"nobody", "a@b", "a b@example.com", "x@example.com\r\nBcc: evil@example.com", "<x@example.com>", "x@@example.com"}) {
        EXPECT_FALSE(repo::valid_email(bad)) << bad;
        EXPECT_EQ(create({.event_type = "approval_needed", .client_id = std::nullopt, .recipients = {bad}}).error().kind, repo::Failure::Kind::invalid);
    }
    EXPECT_TRUE(repo::valid_email("host@xvisionmediaus.com"));
}

// ---- system problems ------------------------------------------------------------------

TEST_F(Notifications, LosingAccessToAClientsLocationIsReported) {
    rule("system_problem", std::nullopt, {"ops@xvisionmediaus.com"});
    std::erase_if(gbp_.locations, [](const gbp::Location& location) { return location.name == "locations/9002"; });
    await(sync_->discover_locations());
    EXPECT_EQ(count("notification_deliveries"), 0) << "one miss can be a hiccup";
    await(sync_->discover_locations());
    run_jobs();
    ASSERT_EQ(channel_.sent.size(), 1u);
    EXPECT_EQ(channel_.sent[0].subject, "Lost access to a location");
    EXPECT_THAT(channel_.sent[0].body, HasSubstr("Gangnam AYCE Buffet"));
}

TEST_F(Notifications, AJobThatFailsForGoodCallsTheFailureHook) {
    jobs::Registry registry;
    registry.add("always.fails", [](const jobs::Job&) -> boost::asio::awaitable<void> {
        throw std::runtime_error("broken");
        co_return;
    });
    std::vector<std::string> reported;
    jobs::Worker worker(io_.get_executor(), queue(), registry,
                        {.id = "w", .on_permanent_failure = [&](jobs::Job job, std::string error) -> boost::asio::awaitable<void> {
                             reported.push_back(job.kind + ": " + error);
                             co_return;
                         }});
    await(queue().enqueue({.kind = "always.fails", .max_attempts = 2}));
    EXPECT_TRUE(await(worker.run_once()));
    EXPECT_TRUE(reported.empty()) << "it will be retried";
    clock_.advance(1min);
    EXPECT_TRUE(await(worker.run_once()));
    EXPECT_THAT(reported, testing::ElementsAre("always.fails: broken"));
}

// X4: a job whose lock expires on its last attempt is given up on by the scheduler, and reported the same way.
TEST_F(Notifications, AJobWhoseLockExpiresOnItsLastAttemptCallsTheFailureHook) {
    std::vector<std::string> reported;
    const jobs::FailureHook hook = [&](jobs::Job job, std::string error) -> boost::asio::awaitable<void> {
        reported.push_back(job.kind + ": " + error);
        co_return;
    };
    jobs::Scheduler scheduler(io_.get_executor(), queue(), {}, {.job_lock_timeout = 15min, .on_permanent_failure = hook});
    await(queue().enqueue({.kind = "runs.long", .max_attempts = 2}));
    ASSERT_TRUE(await(queue().claim("crashed-worker")));
    clock_.advance(16min);
    await(scheduler.tick());
    EXPECT_TRUE(reported.empty()) << "it has an attempt left";
    ASSERT_TRUE(await(queue().claim("another-worker")));
    clock_.advance(16min);
    await(scheduler.tick());
    EXPECT_THAT(reported, testing::ElementsAre("runs.long: lock expired: the worker stopped or the job ran too long"));
}

// CORE-3: what the engine raises for a job given up on.
TEST_F(Notifications, AJobGivenUpOnIsASystemProblemOncePerKindAndDay) {
    rule("system_problem", std::nullopt, {"ops@xvisionmediaus.com"});
    const auto give_up = [&](std::string kind, std::int64_t id) {
        tx([&](pqxx::work& tx) {
            repo::emit_job_failed(tx, {.id = id, .kind = std::move(kind), .payload = {}, .attempts = 4, .max_attempts = 4}, "broken", clock_.now());
        });
    };
    give_up("reports.build", 7);
    give_up("reports.build", 8);
    give_up(std::string(repo::kDeliverNotificationKind), 9);
    run_jobs();
    ASSERT_EQ(channel_.sent.size(), 1u) << "once per kind and day, and failed deliveries are left out";
    EXPECT_EQ(channel_.sent[0].subject, "A background job failed: reports.build");
    EXPECT_THAT(channel_.sent[0].body, HasSubstr("(id 7) failed 4 time(s)"));
    EXPECT_THAT(channel_.sent[0].body, HasSubstr("Last error: broken"));
    clock_.advance(24h);
    give_up("reports.build", 10);
    run_jobs();
    EXPECT_EQ(channel_.sent.size(), 2u) << "the next day it is reported again";
}

// CORE-3: what a refresh of the GBP token comes to.
TEST_F(Notifications, AGbpTokenThatStopsWorkingIsOnTheStatusPageAndReportedOnceADay) {
    rule("system_problem", std::nullopt, {"ops@xvisionmediaus.com"});
    const auto refreshed = [&](std::optional<std::string> error) {
        tx([&](pqxx::work& tx) { repo::record_gbp_token_health(tx, error, clock_.now()); });
    };
    refreshed("invalid_grant");
    refreshed("invalid_grant");
    run_jobs();
    ASSERT_EQ(channel_.sent.size(), 1u);
    EXPECT_EQ(channel_.sent[0].subject, "The Google Business Profile token stopped working");
    EXPECT_THAT(channel_.sent[0].body, HasSubstr("(invalid_grant)"));
    EXPECT_EQ(value<std::string>("SELECT last_error FROM integration_status WHERE name = 'gbp_token'"), "invalid_grant");

    refreshed(std::nullopt);
    run_jobs();
    EXPECT_EQ(channel_.sent.size(), 1u) << "a success tells nobody";
    EXPECT_EQ(count("integration_status WHERE name = 'gbp_token' AND last_error IS NULL AND last_ok_at IS NOT NULL"), 1);
}
