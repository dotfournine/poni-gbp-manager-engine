// The approval-queue API through the router as main assembles it: the approval
// queue (edit, regenerate, approve, reject), escalations, notification
// rules, the delivery log and the switches. Every change is made by a
// signed-in person and lands in the audit log under their name.

#include "api_fixture.hpp"
#include "db/repo_auto_posting.hpp"
#include "db/repo_notifications.hpp"
#include "db/repo_reviews.hpp"
#include "jobs/go_live.hpp"
#include "jobs/notifications.hpp"
#include "jobs/posting.hpp"

#include <gmock/gmock.h>


using namespace poni;
using namespace std::chrono_literals;
using test::FakeLlm;
using test::make_review;
using testing::HasSubstr;

namespace {

constexpr std::string_view kSevere = R"({
    "language": "en", "script": "Latn", "sentiment": "negative", "contains_complaint": true, "topics": [], "items": [],
    "severity_flags": [{"id": "injury", "evidence": "my neck has hurt for a week"}],
    "manipulation_suspected": false, "manipulation_evidence": "", "summary_en": "A guest was hurt."})";

class RepliesApi : public test::ApiTest {
protected:
    void SetUp() override {
        ApiTest::SetUp();
        pipeline_.emplace(db(), clock_, llm_, queue(), jobs::AnalysisOptions{});
        api_pipeline_ = &*pipeline_;
        api_options_.llm_providers = {"anthropic", "dashscope"};
        build({});
        browser_ = sign_in();
        gbp_.add_location("9001", "LU DREAM SPA - Chelsea");
        discover();
        spa_client_ = make_client("LU DREAM SPA");
        spa_ = link("9001", spa_client_);
        // The client is the made-up business of the spa pack in the knowledge base.
        become_test_client(spa_client_, "wellness");
        poster_.emplace(db(), clock_, gbp_, gbp_, jobs::PostingOptions{});
        notifier_.emplace(db(), clock_, jobs::NotifierOptions{.public_url = "http://localhost:5173"});
        notifier_->add_channel(channel_);
        tester_.emplace(db(), clock_, llm_);
        jobs::install(registry_, *tester_);
        jobs::install(registry_, *pipeline_);
        jobs::install(registry_, *poster_);
        jobs::install(registry_, *notifier_);
    }

    void TearDown() override {
        notifier_.reset();
        tester_.reset();
        poster_.reset();
        pipeline_.reset();
        ApiTest::TearDown();
    }

    static std::string body_of(std::string_view key, std::string_view value) {
        return boost::json::serialize(boost::json::object{{key, value}});
    }

    // A review on the fake Google and in the mirror, analyzed and drafted. Returns the draft's id.
    std::int64_t draft_for(const std::string& review_id = "r1", std::string_view stars = "FIVE",
                           std::string comment = "Lovely head spa with Eva, so relaxing!") {
        const auto review = make_review(gbp_.account_id, "9001", review_id, stars, std::move(comment), "2026-11-09T10:00:00Z", "2026-11-09T10:00:00Z");
        gbp_.put(review);
        const auto id = store_review(spa_, review);
        await(pipeline_->process(id, false, false));
        return value<std::int64_t>("SELECT id FROM reply_drafts WHERE review_id = " + std::to_string(id) + " ORDER BY id DESC LIMIT 1");
    }

    std::string text_of(std::int64_t draft_id) { return std::string(read("/api/drafts/" + std::to_string(draft_id)).at("text").as_string()); }
    http::Response approve(std::int64_t draft_id) {
        return post("/api/drafts/" + std::to_string(draft_id) + "/approve", body_of("text", text_of(draft_id)));
    }
    void posting(bool on) { ASSERT_EQ(patch("/api/system/settings", on ? R"({"posting_enabled": true})" : R"({"posting_enabled": false})").status, 200u); }

    // A location's auto-posting settings as the dashboard reads them: from the overview.
    boost::json::value auto_posting_of(std::int64_t location) {
        for (const auto& item : read("/api/auto-posting").at("locations").as_array()) {
            if (item.at("location_id").as_int64() == location) return item;
        }
        ADD_FAILURE() << "location " << location << " is not in the overview";
        return {};
    }

    long audited(const std::string& action) {
        return count("audit_log WHERE action = '" + action + "' AND actor_type = 'user' AND actor = 'staff@xvisionmediaus.com'");
    }

    FakeLlm llm_;
    test::FakeChannel channel_;
    std::optional<jobs::ReviewPipeline> pipeline_;
    std::optional<jobs::ReplyPoster> poster_;
    std::optional<jobs::GoLiveTester> tester_;
    std::optional<jobs::Notifier> notifier_;
};

}  // namespace

TEST_F(RepliesApi, EveryNewEndpointNeedsASessionAndEveryChangeTheCsrfToken) {
    const auto draft_id = std::to_string(draft_for());
    // The routes of this area, from the router's table: the 21 this test once named and every one added since.
    EXPECT_GE(expect_session_and_csrf({"/api/drafts", "/api/escalations", "/api/users", "/api/notification", "/api/system/settings", "/ignore",
                                       "/unignore"},
                                      R"({"text": "x", "posting_enabled": true})"),
              21u);
    EXPECT_FALSE(read("/api/system/settings").at("posting_enabled").as_bool());
    EXPECT_EQ(std::string(read("/api/drafts/" + draft_id).at("status").as_string()), "draft");
}

TEST_F(RepliesApi, PostingAndSendingAreOffUntilAPersonSwitchesThemOn) {
    const auto settings = read("/api/system/settings");
    EXPECT_FALSE(settings.at("posting_enabled").as_bool());
    EXPECT_FALSE(settings.at("email_sending_enabled").as_bool());

    const auto draft_id = draft_for();
    const auto refused = approve(draft_id);
    EXPECT_EQ(refused.status, 409u);
    EXPECT_EQ(error_code(refused), "posting_disabled");
    EXPECT_EQ(audited("draft.approve"), 0);

    posting(true);
    EXPECT_EQ(audited("posting.enabled"), 1);
    EXPECT_EQ(patch("/api/system/settings", R"({"posting_enabled": true})").status, 200u);
    EXPECT_EQ(audited("posting.enabled"), 1) << "no change, no entry";
    EXPECT_EQ(patch("/api/system/settings", R"({"posting_enabled": "yes"})").status, 400u);
    posting(false);
    EXPECT_EQ(audited("posting.disabled"), 1);

    // The location's own switch.
    posting(true);
    const auto location = "/api/locations/" + std::to_string(spa_);
    EXPECT_TRUE(read(location).at("posting_enabled").as_bool());
    const auto paused = patch(location, R"({"posting_enabled": false})");
    ASSERT_EQ(paused.status, 200u);
    EXPECT_FALSE(json(paused).at("posting_enabled").as_bool());
    EXPECT_EQ(audited("location.posting_paused"), 1);
    EXPECT_EQ(error_code(approve(draft_id)), "posting_disabled");
    EXPECT_EQ(read("/api/system/status").at("posting").at("paused_locations").as_int64(), 1);
}

TEST_F(RepliesApi, ApprovingPostsTheExactTextOnceUnderTheApproversName) {
    posting(true);
    const auto draft_id = draft_for();
    const auto path = "/api/drafts/" + std::to_string(draft_id);

    EXPECT_EQ(post(path + "/approve", "{}").status, 400u);
    const auto other = post(path + "/approve", body_of("text", "Not what is stored."));
    EXPECT_EQ(other.status, 409u);
    EXPECT_EQ(error_code(other), "draft_changed");

    const auto approved = approve(draft_id);
    ASSERT_EQ(approved.status, 202u) << approved.body;
    EXPECT_EQ(json(approved).at("status").as_string(), "approved");
    EXPECT_EQ(json(approved).at("decided_by").as_string(), "staff@xvisionmediaus.com");
    EXPECT_EQ(audited("draft.approve"), 1);
    EXPECT_EQ(error_code(approve(draft_id)), "draft_not_open");
    EXPECT_EQ(error_code(patch(path, body_of("text", "Too late to edit this."))), "draft_not_open");
    EXPECT_EQ(error_code(post(path + "/reject", body_of("reason", "Too late."))), "draft_not_open");

    run_jobs();
    ASSERT_EQ(gbp_.puts.size(), 1u);
    auto draft = read(path);
    EXPECT_EQ(draft.at("status").as_string(), "posted");
    EXPECT_EQ(draft.at("post").at("status").as_string(), "posted");
    EXPECT_TRUE(draft.at("post").at("published_at").is_null());
    EXPECT_EQ(read("/api/system/status").at("posting").at("in_flight").as_int64(), 1);

    gbp_.publish_held();
    await(poster_->confirm());
    draft = read(path);
    EXPECT_EQ(draft.at("status").as_string(), "published");
    EXPECT_EQ(draft.at("post").at("status").as_string(), "published");
    const auto review = read("/api/reviews/" + std::to_string(draft.at("review_id").as_int64()));
    EXPECT_EQ(review.at("reply").at("origin").as_string(), "system");
    EXPECT_EQ(review.at("draft").at("status").as_string(), "published");
    EXPECT_EQ(read("/api/reviews?draft_status=published").at("total").as_int64(), 1);
    const auto status = read("/api/system/status").at("posting");
    EXPECT_EQ(status.at("published").as_int64(), 1);
    EXPECT_EQ(status.at("in_flight").as_int64(), 0);
    EXPECT_FALSE(status.at("last_posted_at").is_null());
    EXPECT_EQ(gbp_.puts.size(), 1u);
}

TEST_F(RepliesApi, AnEditRunsTheChecksAgainAndAFailedCheckBlocksApproval) {
    posting(true);
    const auto draft_id = draft_for();
    const auto path = "/api/drafts/" + std::to_string(draft_id);
    const auto original = text_of(draft_id);

    EXPECT_EQ(patch(path, R"({"text": "   "})").status, 400u);
    EXPECT_EQ(patch(path, R"({"text": 5})").status, 400u);
    EXPECT_EQ(patch("/api/drafts/999999", body_of("text", "Hello there, thank you.")).status, 404u);

    const auto edited = patch(path, body_of("text", "Thank you so much! Call us on 212-555-0143 to book your next head spa with Eva."));
    ASSERT_EQ(edited.status, 200u) << edited.body;
    const auto draft = json(edited);
    EXPECT_EQ(draft.at("edited_by").as_string(), "staff@xvisionmediaus.com");
    EXPECT_EQ(draft.at("generated_text").as_string(), original);
    bool phone_failed = false;
    for (const auto& check : draft.at("checks").as_array()) {
        if (check.at("name").as_string() == "no_phone") phone_failed = !check.at("passed").as_bool();
    }
    EXPECT_TRUE(phone_failed);
    const auto blocked = approve(draft_id);
    EXPECT_EQ(blocked.status, 409u);
    EXPECT_EQ(error_code(blocked), "checks_failed");
    EXPECT_EQ(audited("draft.edit"), 1);

    ASSERT_EQ(patch(path, body_of("text", "Thank you so much for visiting us! Eva will be delighted, and we hope to see you again soon.")).status, 200u);
    ASSERT_EQ(approve(draft_id).status, 202u);
    run_jobs();
    ASSERT_EQ(gbp_.puts.size(), 1u);
    EXPECT_EQ(gbp_.puts[0].comment, "Thank you so much for visiting us! Eva will be delighted, and we hope to see you again soon.");
}

TEST_F(RepliesApi, RejectingNeedsAReasonAndEndsTheDraft) {
    posting(true);
    const auto draft_id = draft_for();
    const auto path = "/api/drafts/" + std::to_string(draft_id);
    EXPECT_EQ(post(path + "/reject", "{}").status, 400u);
    EXPECT_EQ(post(path + "/reject", body_of("reason", "  ")).status, 400u);
    const auto rejected = post(path + "/reject", body_of("reason", "The owner answers this guest herself."));
    ASSERT_EQ(rejected.status, 200u) << rejected.body;
    EXPECT_EQ(json(rejected).at("status").as_string(), "rejected");
    EXPECT_EQ(json(rejected).at("status_reason").as_string(), "The owner answers this guest herself.");
    EXPECT_EQ(audited("draft.reject"), 1);
    EXPECT_EQ(error_code(approve(draft_id)), "draft_not_open");
    run_jobs();
    EXPECT_TRUE(gbp_.puts.empty());
    EXPECT_EQ(read("/api/reviews?draft_status=draft").at("total").as_int64(), 0);
    EXPECT_EQ(read("/api/reviews?draft_status=rejected").at("total").as_int64(), 1);
}

TEST_F(RepliesApi, RegeneratingWritesANewDraftWithTheStaffInstructions) {
    posting(true);
    const auto draft_id = draft_for();
    const auto path = "/api/drafts/" + std::to_string(draft_id);
    EXPECT_EQ(post(path + "/regenerate", "{}").status, 400u);
    EXPECT_EQ(post(path + "/regenerate", body_of("instructions", " ")).status, 400u);

    llm_.requests.clear();
    llm_.generator = FakeLlm::always(R"({"reply": "Thank you! Eva says hello, and your next head spa is waiting whenever you are.",
        "language": "en", "script": "Latn", "manipulation_suspected": false, "notes_for_staff": ""})");
    const auto regenerated = post(path + "/regenerate", body_of("instructions", "Mention Eva by name and keep it to two sentences."));
    ASSERT_EQ(regenerated.status, 201u) << regenerated.body;
    const auto fresh = json(regenerated);
    EXPECT_NE(fresh.at("id").as_int64(), draft_id);
    EXPECT_EQ(fresh.at("status").as_string(), "draft");
    EXPECT_EQ(fresh.at("text").as_string(), "Thank you! Eva says hello, and your next head spa is waiting whenever you are.");
    EXPECT_EQ(fresh.at("instructions").as_string(), "Mention Eva by name and keep it to two sentences.");
    EXPECT_EQ(fresh.at("created_by").as_string(), "staff@xvisionmediaus.com");
    // A draft shaped by a person's words always waits for a person, also once auto-posting exists.
    EXPECT_EQ(fresh.at("route").as_string(), "approval_queue");
    EXPECT_EQ(read(path).at("status").as_string(), "superseded");
    EXPECT_EQ(audited("draft.regenerate"), 1);

    // Only the generator ran, the instructions are in the system message, and the review is still only in the data block.
    ASSERT_EQ(llm_.requests.size(), 1u);
    EXPECT_EQ(llm_.requests[0].task, "generator");
    EXPECT_THAT(llm_.requests[0].messages[0].content, HasSubstr("Mention Eva by name and keep it to two sentences."));
    EXPECT_THAT(llm_.requests[0].messages[0].content, testing::Not(HasSubstr("Lovely head spa")));
    EXPECT_THAT(llm_.requests[0].messages[1].content, HasSubstr("Lovely head spa"));
    EXPECT_EQ(count("notification_deliveries WHERE event_type = 'approval_needed'"), 1) << "the person who asked needs no mail about it";

    // The old draft can't be acted on any more; the new one can.
    EXPECT_EQ(error_code(post(path + "/regenerate", body_of("instructions", "Again."))), "draft_not_open");
    ASSERT_EQ(approve(fresh.at("id").as_int64()).status, 202u);
    run_jobs();
    ASSERT_EQ(gbp_.puts.size(), 1u);
    EXPECT_EQ(gbp_.puts[0].comment, "Thank you! Eva says hello, and your next head spa is waiting whenever you are.");
}

TEST_F(RepliesApi, AModelThatDoesNotAnswerLeavesTheDraftAsItWas) {
    const auto draft_id = draft_for();
    const auto path = "/api/drafts/" + std::to_string(draft_id);
    const auto before = text_of(draft_id);
    llm_.generator = [](const llm::ChatRequest&) { return FakeLlm::failure(llm::Error::Kind::unavailable, "connect to dashscope.example failed"); };
    const auto failed = post(path + "/regenerate", body_of("instructions", "Shorter."));
    EXPECT_EQ(failed.status, 502u);
    EXPECT_EQ(error_code(failed), "model_unavailable");
    EXPECT_THAT(failed.body, testing::Not(HasSubstr("dashscope.example"))) << "host names stay in the log";
    EXPECT_EQ(read(path).at("status").as_string(), "draft");
    EXPECT_EQ(text_of(draft_id), before);
    EXPECT_EQ(count("reply_drafts"), 1);
}

// ---- not every review has to be answered (D-068) -------------------------------------

TEST_F(RepliesApi, AReviewCanBeMarkedAsNeedingNoReplyAndTheDecisionCanBeUndone) {
    posting(true);
    const auto draft_id = draft_for("meh", "THREE", "It was fine.");
    const auto review_id = read("/api/drafts/" + std::to_string(draft_id)).at("review_id").as_int64();
    const auto path = "/api/reviews/" + std::to_string(review_id);
    EXPECT_TRUE(read(path).at("ignored").is_null());
    EXPECT_EQ(read("/api/reviews?draft_status=draft").at("total").as_int64(), 1);

    EXPECT_EQ(post(path + "/ignore", R"({"reason": 5})").status, 400u);
    EXPECT_EQ(post("/api/reviews/999999/ignore").status, 404u);
    const auto ignored = post(path + "/ignore", "");  // no body at all: one click
    ASSERT_EQ(ignored.status, 200u) << ignored.body;
    const auto decision = json(ignored).at("ignored");
    EXPECT_EQ(decision.at("by").as_string(), "staff@xvisionmediaus.com");
    EXPECT_EQ(decision.at("reason").as_string(), "");
    EXPECT_EQ(json(ignored).at("draft").at("status").as_string(), "cancelled");
    EXPECT_EQ(json(ignored).at("draft").at("status_reason").as_string(), "a person decided that this review needs no reply");
    EXPECT_EQ(audited("review.ignore"), 1);
    EXPECT_EQ(post(path + "/ignore", R"({"reason": "Again."})").status, 200u);
    EXPECT_EQ(audited("review.ignore"), 1) << "doing it twice changes nothing";

    // Out of the queue, not drafted again, and its old draft can't be approved.
    EXPECT_EQ(read("/api/reviews?draft_status=draft").at("total").as_int64(), 0);
    EXPECT_EQ(read("/api/reviews?ignored=true").at("total").as_int64(), 1);
    EXPECT_EQ(read("/api/reviews?ignored=false").at("total").as_int64(), 0);
    EXPECT_EQ(await(pipeline_->enqueue_pending()), 0u);
    await(pipeline_->process(review_id, true, false));  // even "analyze again" writes no draft
    EXPECT_EQ(count("reply_drafts"), 1);
    EXPECT_EQ(error_code(approve(draft_id)), "draft_not_open");
    run_jobs();
    EXPECT_TRUE(gbp_.puts.empty());

    // Taken back: the next pass drafts for it again.
    const auto restored = post(path + "/unignore");
    ASSERT_EQ(restored.status, 200u) << restored.body;
    EXPECT_TRUE(json(restored).at("ignored").is_null());
    EXPECT_EQ(audited("review.unignore"), 1);
    EXPECT_EQ(await(pipeline_->enqueue_pending()), 1u);
    run_jobs();
    EXPECT_EQ(read("/api/reviews?draft_status=draft").at("total").as_int64(), 1);
    EXPECT_EQ(post("/api/reviews/999999/unignore").status, 404u);
}

TEST_F(RepliesApi, NoReplyNeededEndsWhenTheReviewerEditsTheReview) {
    const auto draft_id = draft_for("meh", "THREE", "It was fine.");
    const auto review_id = read("/api/drafts/" + std::to_string(draft_id)).at("review_id").as_int64();
    const auto path = "/api/reviews/" + std::to_string(review_id);
    ASSERT_EQ(post(path + "/ignore", R"({"reason": "Nothing to answer."})").status, 200u);
    EXPECT_EQ(read(path).at("ignored").at("reason").as_string(), "Nothing to answer.");

    // The reviewer comes back and writes something that does want an answer.
    const auto edited = make_review(gbp_.account_id, "9001", "meh", "ONE", "Update: I was charged twice and nobody picks up the phone.",
                                    "2026-11-09T10:00:00Z", "2026-11-10T11:00:00Z");
    ASSERT_TRUE(tx([&](pqxx::work& tx) { return repo::apply_review(tx, spa_, edited, repo::kSourcePoll, clock_.now()); }));
    EXPECT_TRUE(read(path).at("ignored").is_null());
    EXPECT_EQ(await(pipeline_->enqueue_pending()), 1u);
    run_jobs();
    EXPECT_EQ(read(path).at("draft").at("status").as_string(), "draft");
}

TEST_F(RepliesApi, AReviewWithAReplyOrAReplyOnItsWayCannotBeIgnored) {
    posting(true);
    const auto draft_id = draft_for();
    const auto path = "/api/reviews/" + std::to_string(read("/api/drafts/" + std::to_string(draft_id)).at("review_id").as_int64());
    ASSERT_EQ(approve(draft_id).status, 202u);
    EXPECT_EQ(error_code(post(path + "/ignore")), "review_not_ignorable") << "approved: a reply is on its way";
    run_jobs();
    EXPECT_EQ(error_code(post(path + "/ignore")), "review_not_ignorable") << "posted, not yet visible";
    gbp_.publish_held();
    await(poster_->confirm());
    EXPECT_EQ(error_code(post(path + "/ignore")), "review_not_ignorable") << "it has a reply";
    EXPECT_EQ(audited("review.ignore"), 0);
    EXPECT_EQ(gbp_.puts.size(), 1u);
}

TEST_F(RepliesApi, EscalationsAreListedAssignedAndClosed) {
    llm_.analyzer = FakeLlm::always(kSevere);
    draft_for("hurt", "ONE", "The therapist pressed so hard that my neck has hurt for a week.");
    draft_for("fine");  // the scripted analyzer flags this one too; two escalations
    const auto list = read("/api/escalations");
    ASSERT_EQ(list.at("total").as_int64(), 2);
    const auto& first = list.at("items").as_array().back();
    const auto id = first.at("id").as_int64();
    const auto path = "/api/escalations/" + std::to_string(id);
    EXPECT_EQ(first.at("status").as_string(), "open");
    EXPECT_EQ(first.at("severity_flags").as_array().at(0).at("id").as_string(), "injury");
    EXPECT_EQ(gbp_review_id(first.at("review_id").as_int64()), "hurt");
    EXPECT_EQ(first.at("review").at("draft").at("route").as_string(), "escalate");
    EXPECT_TRUE(first.at("assignee").is_null());
    EXPECT_EQ(read("/api/system/status").at("posting").at("open_escalations").as_int64(), 2);

    const auto users = read("/api/users").at("items").as_array();
    ASSERT_EQ(users.size(), 1u);
    const auto me = users[0].at("id").as_int64();
    EXPECT_EQ(post(path + "/assign", "{}").status, 400u);
    EXPECT_EQ(error_code(post(path + "/assign", R"({"assignee_id": 424242})")), "user_not_found");
    const auto assigned = post(path + "/assign", boost::json::serialize(boost::json::object{{"assignee_id", me}}));
    ASSERT_EQ(assigned.status, 200u) << assigned.body;
    EXPECT_EQ(json(assigned).at("assignee").at("email").as_string(), "staff@xvisionmediaus.com");
    EXPECT_EQ(read("/api/escalations?assignee_id=" + std::to_string(me)).at("total").as_int64(), 1);
    EXPECT_EQ(json(post(path + "/assign", R"({"assignee_id": null})")).at("assignee").is_null(), true);

    EXPECT_EQ(post(path + "/close", R"({"reason": "bored"})").status, 400u);
    const auto closed = post(path + "/close", R"({"reason": "no_response", "note": "Called twice, no answer."})");
    ASSERT_EQ(closed.status, 200u) << closed.body;
    EXPECT_EQ(json(closed).at("status").as_string(), "closed");
    EXPECT_EQ(json(closed).at("close_reason").as_string(), "no_response");
    EXPECT_EQ(json(closed).at("closed_by").as_string(), "staff@xvisionmediaus.com");
    EXPECT_EQ(error_code(post(path + "/close", R"({"reason": "resolved"})")), "escalation_closed");
    EXPECT_EQ(error_code(post(path + "/assign", R"({"assignee_id": null})")), "escalation_closed");
    EXPECT_EQ(read("/api/escalations?status=open").at("total").as_int64(), 1);
    EXPECT_EQ(read("/api/escalations?status=closed").at("total").as_int64(), 1);
    EXPECT_EQ(get("/api/escalations?status=odd", browser_).status, 400u);
    EXPECT_EQ(get("/api/escalations/9999", browser_).status, 404u);
    EXPECT_EQ(audited("escalation.assign"), 2);
    EXPECT_EQ(audited("escalation.close"), 1);
}

TEST_F(RepliesApi, NotificationRulesAreManagedAndTheLogShowsWhatHappened) {
    const auto client = std::to_string(spa_client_);
    EXPECT_EQ(post("/api/notification-rules", R"({"event_type": "escalation", "recipients": []})").status, 400u);
    EXPECT_EQ(post("/api/notification-rules", R"({"event_type": "escalation", "recipients": ["not an address"]})").status, 400u);
    EXPECT_EQ(error_code(post("/api/notification-rules", R"({"event_type": "escalation", "client_id": 4242, "recipients": ["a@example.com"]})")),
              "client_not_found");
    const auto created = post("/api/notification-rules",
                              R"({"event_type": "approval_needed", "client_id": )" + client + R"(, "recipients": ["Team@XVisionMediaUS.com"]})");
    ASSERT_EQ(created.status, 201u) << created.body;
    const auto rule = json(created);
    const auto path = "/api/notification-rules/" + std::to_string(rule.at("id").as_int64());
    EXPECT_EQ(rule.at("client_name").as_string(), "LU DREAM SPA");
    EXPECT_EQ(rule.at("recipients").as_array().at(0).as_string(), "team@xvisionmediaus.com");
    EXPECT_EQ(error_code(post("/api/notification-rules",
                              R"({"event_type": "approval_needed", "client_id": )" + client + R"(, "recipients": ["b@example.com"]})")),
              "rule_exists");
    EXPECT_EQ(read("/api/notification-rules").at("total").as_int64(), 1);
    EXPECT_EQ(read(path).at("event_type").as_string(), "approval_needed");

    // Sending is off: the draft's notification is logged, not sent.
    draft_for();
    run_jobs();
    auto log = read("/api/notification-deliveries?event_type=approval_needed");
    ASSERT_EQ(log.at("total").as_int64(), 1);
    EXPECT_EQ(log.at("items").as_array().at(0).at("status").as_string(), "not_sent");
    EXPECT_EQ(log.at("items").as_array().at(0).at("status_reason").as_string(), "email sending is switched off");
    EXPECT_EQ(log.at("items").as_array().at(0).at("recipient").as_string(), "team@xvisionmediaus.com");
    EXPECT_TRUE(log.at("items").as_array().at(0).at("escalation_id").is_null()) << "a draft's event is not about an escalation";
    EXPECT_EQ(read("/api/notification-deliveries?escalation_id=1").at("total").as_int64(), 0);
    EXPECT_EQ(get("/api/notification-deliveries?escalation_id=first", browser_).status, 400u);
    EXPECT_TRUE(channel_.sent.empty());

    const auto changed = patch(path, R"({"recipients": ["team@xvisionmediaus.com", "owner@xvisionmediaus.com"], "enabled": false})");
    ASSERT_EQ(changed.status, 200u) << changed.body;
    EXPECT_FALSE(json(changed).at("enabled").as_bool());
    EXPECT_EQ(patch(path, R"({"recipients": ["broken@"]})").status, 400u);

    // Switched on: the test message goes out through the same channel and log.
    ASSERT_EQ(patch("/api/system/settings", R"({"email_sending_enabled": true})").status, 200u);
    EXPECT_EQ(audited("email_sending.enabled"), 1);
    EXPECT_EQ(post("/api/notifications/test", R"({"recipient": "nope"})").status, 400u);
    const auto test_message = post("/api/notifications/test", R"({"recipient": "owner@xvisionmediaus.com"})");
    ASSERT_EQ(test_message.status, 202u) << test_message.body;
    EXPECT_EQ(json(test_message).at("status").as_string(), "pending");
    run_jobs();
    ASSERT_EQ(channel_.sent.size(), 1u);
    EXPECT_EQ(channel_.sent[0].recipient, "owner@xvisionmediaus.com");
    EXPECT_EQ(read("/api/notification-deliveries?status=sent").at("total").as_int64(), 1);
    EXPECT_EQ(get("/api/notification-deliveries?status=lost", browser_).status, 400u);

    const auto email = read("/api/system/status").at("email");
    EXPECT_TRUE(email.at("sending_enabled").as_bool());
    EXPECT_FALSE(email.at("connected").as_bool()) << "no sending account has signed in";
    EXPECT_TRUE(email.at("account_email").is_null());
    EXPECT_FALSE(email.at("last_ok_at").is_null());

    EXPECT_EQ(del(path).status, 204u);
    EXPECT_EQ(del(path).status, 404u);
    EXPECT_EQ(read("/api/notification-rules").at("total").as_int64(), 0);
    EXPECT_EQ(read("/api/notification-deliveries").at("total").as_int64(), 2) << "the log outlives the rule";
    EXPECT_EQ(audited("notification_rule.create") + audited("notification_rule.update") + audited("notification_rule.delete"), 3);
}

// ---- auto-posting ------------------------------------------------------

TEST_F(RepliesApi, ALocationShowsItsTimeZoneInTheListAndWhenRead) {
    const auto zone_in_list = [&](std::int64_t id) -> boost::json::value {
        for (const auto& item : read("/api/locations").at("items").as_array()) {
            if (item.at("id").as_int64() == id) return item.at("time_zone");
        }
        return "not listed";
    };
    const auto target = "/api/locations/" + std::to_string(spa_) + "/auto-posting";
    EXPECT_TRUE(read("/api/locations/" + std::to_string(spa_)).at("time_zone").is_null()) << "Google gives none";
    EXPECT_TRUE(zone_in_list(spa_).is_null());

    // It is set where auto-posting's own settings are, and nowhere else: one value, shown in both places.
    ASSERT_EQ(patch(target, R"({"time_zone": "America/New_York"})").status, 200u);
    EXPECT_EQ(read("/api/locations/" + std::to_string(spa_)).at("time_zone").as_string(), "America/New_York");
    EXPECT_EQ(zone_in_list(spa_).as_string(), "America/New_York");
    EXPECT_EQ(auto_posting_of(spa_).at("time_zone").as_string(), "America/New_York");
    EXPECT_EQ(patch("/api/locations/" + std::to_string(spa_), R"({"time_zone": "Europe/Paris"})").status, 400u)
        << "the location's own update does not take it";
    EXPECT_EQ(count("locations WHERE auto_post_enabled"), 0);

    ASSERT_EQ(patch(target, R"({"time_zone": null})").status, 200u);
    EXPECT_TRUE(read("/api/locations/" + std::to_string(spa_)).at("time_zone").is_null());
}

TEST_F(RepliesApi, AutoPostingEndpointsNeedASessionAndEveryChangeTheCsrfToken) {
    draft_for();
    // The routes of this area, from the router's table: the 6 this test names and every one added since.
    EXPECT_GE(expect_session_and_csrf({"auto-post", "/unschedule"}, R"({"enabled": true, "five_star": true})"), 6u);
    EXPECT_EQ(count("locations WHERE auto_post_enabled"), 0);
}

TEST_F(RepliesApi, AutoPostingIsOffAndNothingGoesOutBeforeTheGoLiveTestPassed) {
    const auto target = "/api/locations/" + std::to_string(spa_) + "/auto-posting";
    const auto before = auto_posting_of(spa_);
    EXPECT_FALSE(before.at("enabled").as_bool());
    EXPECT_FALSE(before.at("five_star").as_bool());
    EXPECT_FALSE(before.at("four_star").as_bool());
    EXPECT_EQ(before.at("daily_cap").as_int64(), 100);
    EXPECT_EQ(before.at("pause_after_failures").as_int64(), 3);
    EXPECT_EQ(before.at("go_live_state").as_string(), "not_tested");
    EXPECT_TRUE(before.at("time_zone").is_null());
    EXPECT_FALSE(before.at("paused").as_bool());
    const auto overview = read("/api/auto-posting");
    EXPECT_FALSE(overview.at("posting_enabled").as_bool());
    ASSERT_EQ(overview.at("locations").as_array().size(), 1u);

    EXPECT_EQ(patch(target, R"({"enabled": true})").status, 400u) << "no star rating chosen";
    EXPECT_EQ(patch(target, R"({"enabled": "yes"})").status, 400u);
    EXPECT_EQ(patch(target, R"({"daily_cap": 0})").status, 400u);
    EXPECT_EQ(patch(target, R"({"pause_after_failures": 0})").status, 400u);
    EXPECT_EQ(patch(target, R"({"time_zone": "Mars/Olympus"})").status, 400u);
    EXPECT_EQ(patch("/api/locations/999999/auto-posting", R"({"daily_cap": 3})").status, 404u);
    EXPECT_FALSE(auto_posting_of(spa_).at("enabled").as_bool());

    const auto prepared = patch(target, R"({"five_star": true, "daily_cap": 5, "pause_after_failures": 2, "time_zone": "America/New_York"})");
    ASSERT_EQ(prepared.status, 200u) << prepared.body;
    EXPECT_EQ(json(prepared).at("time_zone").as_string(), "America/New_York");
    EXPECT_EQ(json(prepared).at("pause_after_failures").as_int64(), 2);
    EXPECT_EQ(audited("location.auto_post_settings"), 1);
    EXPECT_EQ(patch(target, R"({"time_zone": null})").status, 200u);
    EXPECT_TRUE(auto_posting_of(spa_).at("time_zone").is_null());

    // A person switches auto-posting on. No reply approved by a person has been
    // published, and it is not refused (D-076): the go-live test is the safeguard.
    posting(true);
    const auto on = patch(target, R"({"enabled": true})");
    ASSERT_EQ(on.status, 200u) << on.body;
    EXPECT_TRUE(json(on).at("enabled").as_bool());
    EXPECT_EQ(json(on).at("go_live_state").as_string(), "not_tested");
    EXPECT_EQ(audited("location.auto_post_enabled"), 1);
    const auto first = draft_for("r1");
    EXPECT_EQ(read("/api/drafts/" + std::to_string(first)).at("status").as_string(), "draft") << "switched on, untested: a person decides";

    // ---- the go-live test, through the API (D-074) ----
    // A model that behaves: the corpus gets nothing past it.
    llm_.analyzer = FakeLlm::careful("Lovely head spa");
    const auto untested = read("/api/go-live");
    EXPECT_TRUE(untested.at("providers_configured").as_bool());
    EXPECT_EQ(untested.at("judge_model").as_string(), "claude-opus-5-5");
    ASSERT_EQ(untested.at("clients").as_array().size(), 1u);
    const auto& configuration = untested.at("clients").as_array()[0].at("configurations").as_array()[0];
    EXPECT_EQ(configuration.at("state").as_string(), "not_tested");
    EXPECT_TRUE(configuration.at("pass").is_null());
    EXPECT_EQ(configuration.at("estimate").at("real_reviews").as_int64(), 1);
    EXPECT_GT(configuration.at("estimate").at("model_calls").as_int64(), 100);

    const auto submit = "/api/clients/" + std::to_string(spa_client_) + "/go-live-test";
    EXPECT_EQ(post("/api/clients/999999/go-live-test").status, 404u);
    const auto submitted = post(submit);
    ASSERT_EQ(submitted.status, 201u) << submitted.body;
    ASSERT_EQ(json(submitted).at("total").as_int64(), 1);
    const auto run_id = json(submitted).at("items").as_array()[0].at("id").as_int64();
    EXPECT_EQ(json(submitted).at("items").as_array()[0].at("status").as_string(), "running");
    EXPECT_EQ(audited("go_live.start"), 1);
    EXPECT_EQ(error_code(post(submit)), "go_live_running") << "one test of a configuration at a time";
    EXPECT_EQ(auto_posting_of(spa_).at("go_live_state").as_string(), "running");

    run_jobs();
    const auto run_target = "/api/go-live-runs/" + std::to_string(run_id);
    const auto done = read(run_target);
    EXPECT_EQ(done.at("status").as_string(), "passed") << done.at("summary");
    EXPECT_EQ(done.at("real_reviews").as_int64(), 1);
    EXPECT_EQ(done.at("cases_done").as_int64(), done.at("cases_total").as_int64());
    EXPECT_EQ(done.at("started_by").as_string(), "staff@xvisionmediaus.com");
    const auto& cases = done.at("cases").as_array();
    ASSERT_FALSE(cases.empty());
    const auto& own = cases.back();
    EXPECT_EQ(own.at("category").as_string(), "real");
    EXPECT_EQ(own.at("text").as_string(), "Lovely head spa with Eva, so relaxing!");
    ASSERT_EQ(own.at("attempts").as_array().size(), 3u);
    EXPECT_EQ(own.at("attempts").as_array()[0].at("rating").as_string(), "post_as_is");
    EXPECT_EQ(error_code(post(run_target + "/cancel")), "go_live_not_running");
    EXPECT_EQ(post("/api/go-live-runs/999999/cancel").status, 404u);
    EXPECT_EQ(get("/api/go-live-runs/999999", browser_).status, 404u);
    EXPECT_TRUE(json(submitted).at("items").as_array()[0].at("cases").is_null()) << "the runs a start returns carry no cases";
    const auto tested = read("/api/go-live").at("clients").as_array()[0].at("configurations").as_array()[0];
    EXPECT_EQ(tested.at("state").as_string(), "passed");
    EXPECT_EQ(tested.at("pass").at("id").as_int64(), run_id);
    EXPECT_EQ(auto_posting_of(spa_).at("go_live_state").as_string(), "passed");
    EXPECT_TRUE(gbp_.puts.empty()) << "a test posts nothing";
    EXPECT_EQ(read("/api/drafts/" + std::to_string(first)).at("status").as_string(), "draft") << "and re-arms nothing";

    // From now on a five-star review there is answered without a person, and the list shows it.
    const auto second = draft_for("r2");
    const auto scheduled = read("/api/drafts/" + std::to_string(second));
    EXPECT_EQ(scheduled.at("status").as_string(), "scheduled");
    EXPECT_FALSE(scheduled.at("post_at").is_null());
    EXPECT_EQ(scheduled.at("policy_version").as_string().size(), 12u);
    EXPECT_EQ(auto_posting_of(spa_).at("scheduled").as_int64(), 1);
    EXPECT_EQ(read("/api/reviews?draft_status=scheduled").at("total").as_int64(), 1);
    run_jobs();
    ASSERT_EQ(gbp_.puts.size(), 1u);
    const auto list = read("/api/auto-posts");
    ASSERT_EQ(list.at("total").as_int64(), 1);
    const auto& item = list.at("items").as_array()[0];
    EXPECT_EQ(item.at("review_id").as_int64(), value<std::int64_t>("SELECT review_id FROM reply_drafts WHERE id = " + std::to_string(second)));
    EXPECT_EQ(item.at("policy_version").as_string(), scheduled.at("policy_version").as_string());
    EXPECT_EQ(item.at("status").as_string(), "posted");
    EXPECT_EQ(item.at("seconds_to_post").as_int64(), 0);
    EXPECT_EQ(auto_posting_of(spa_).at("posted_last_24h").as_int64(), 1);

    const auto off = patch(target, R"({"enabled": false})");
    ASSERT_EQ(off.status, 200u);
    EXPECT_EQ(audited("location.auto_post_disabled"), 1);
}

TEST_F(RepliesApi, AGoLiveTestCanBeCancelledAndNeedsTheJudgesProvider) {
    const auto submit = "/api/clients/" + std::to_string(spa_client_) + "/go-live-test";
    const auto submitted = post(submit);
    ASSERT_EQ(submitted.status, 201u) << submitted.body;
    const auto run_target = "/api/go-live-runs/" + std::to_string(json(submitted).at("items").as_array()[0].at("id").as_int64());
    const auto cancelled = post(run_target + "/cancel");
    ASSERT_EQ(cancelled.status, 200u) << cancelled.body;
    EXPECT_EQ(json(cancelled).at("status").as_string(), "cancelled");
    EXPECT_EQ(audited("go_live.cancel"), 1);
    run_jobs();
    EXPECT_TRUE(llm_.requests.empty()) << "cancelled before a case ran: no model was asked";
    EXPECT_EQ(read("/api/go-live").at("clients").as_array()[0].at("configurations").as_array()[0].at("state").as_string(), "cancelled");

    // An engine without the judge's provider can't run a test, and says so.
    api_options_.llm_providers = {"dashscope"};
    build({});
    browser_ = sign_in();
    const auto refused = post(submit);
    EXPECT_EQ(refused.status, 400u);
    EXPECT_THAT(std::string(json(refused).at("error").at("message").as_string()), HasSubstr("judge"));
    EXPECT_FALSE(read("/api/go-live").at("providers_configured").as_bool());
}

TEST_F(RepliesApi, AScheduledReplyCanBeTakenBackThroughTheApi) {
    posting(true);
    // Switched on in this test's schema, with a delay so the reply is still waiting.
    exec("UPDATE locations SET auto_post_enabled = true, auto_post_five_star = true");
    certify(spa_);
    set_policy(spa_, R"({"auto_post_delay_min_minutes": 30, "auto_post_delay_max_minutes": 30})");
    const auto draft_id = draft_for("r1");
    const auto target = "/api/drafts/" + std::to_string(draft_id);
    ASSERT_EQ(read(target).at("status").as_string(), "scheduled");
    // A scheduled draft is not in the approval queue: it can't be edited, approved or rejected there.
    EXPECT_EQ(error_code(approve(draft_id)), "draft_not_open");
    EXPECT_EQ(error_code(patch(target, body_of("text", "Thanks!"))), "draft_not_open");

    const auto taken = post(target + "/unschedule");
    ASSERT_EQ(taken.status, 200u) << taken.body;
    EXPECT_EQ(json(taken).at("status").as_string(), "draft");
    EXPECT_EQ(audited("draft.unschedule"), 1);
    EXPECT_EQ(error_code(post(target + "/unschedule")), "draft_not_scheduled");
    EXPECT_EQ(post("/api/drafts/999999/unschedule").status, 404u);
    clock_.advance(31min);
    run_jobs();
    EXPECT_TRUE(gbp_.puts.empty());
}

TEST_F(RepliesApi, APersonResumesAPausedLocationAndAPauseOnEverything) {
    const auto target = "/api/locations/" + std::to_string(spa_) + "/auto-posting";
    EXPECT_EQ(post(target + "/resume").status, 200u) << "resuming what is not paused changes nothing";
    EXPECT_EQ(audited("location.auto_post_resumed"), 0);
    tx([&](pqxx::work& tx) {
        repo::pause_auto_posting(tx, spa_, repo::kPausePostRefused, "Google refused an automatic reply (HTTP 403).", clock_.now());
        repo::pause_auto_posting(tx, std::nullopt, repo::kPausePostRefused, "Google refused an automatic reply (HTTP 401).", clock_.now());
    });
    const auto paused = auto_posting_of(spa_);
    EXPECT_TRUE(paused.at("paused").as_bool());
    EXPECT_EQ(paused.at("pause_reason").as_string(), "post_refused");
    EXPECT_THAT(std::string(paused.at("pause_detail").as_string()), HasSubstr("HTTP 403"));
    EXPECT_TRUE(read("/api/auto-posting").at("paused").as_bool());

    const auto resumed = post(target + "/resume");
    ASSERT_EQ(resumed.status, 200u);
    EXPECT_FALSE(json(resumed).at("paused").as_bool());
    EXPECT_TRUE(json(resumed).at("pause_reason").is_null());
    EXPECT_EQ(audited("location.auto_post_resumed"), 1);
    EXPECT_TRUE(read("/api/auto-posting").at("paused").as_bool()) << "the pause on everything is lifted separately";
    const auto all = post("/api/auto-posting/resume");
    ASSERT_EQ(all.status, 200u);
    EXPECT_FALSE(json(all).at("paused").as_bool());
    EXPECT_EQ(audited("auto_posting.resumed"), 1);
    EXPECT_EQ(post("/api/locations/999999/auto-posting/resume").status, 404u);
}

// A location that lost its client lost its reply settings with it (D-093): a draft that waited there can't be
// checked against a voice any more, and is refused for what is the matter, not for unreadable settings.
TEST_F(RepliesApi, ADraftOfALocationThatLostItsClientCanNeitherBeEditedNorApproved) {
    posting(true);
    const auto draft_id = draft_for("r1");
    const auto target = "/api/drafts/" + std::to_string(draft_id);
    ASSERT_EQ(patch("/api/locations/" + std::to_string(spa_), R"({"client_id": null})").status, 200u);
    for (const auto& refused : {patch(target, body_of("text", "Thanks!")), approve(draft_id)}) {
        EXPECT_EQ(refused.status, 409u);
        EXPECT_EQ(error_code(refused), "review_not_postable");
        EXPECT_THAT(error_message(refused), HasSubstr("no longer synced"));
    }
    EXPECT_TRUE(gbp_.puts.empty());
}

TEST_F(RepliesApi, ThePolicyCarriesTheDelayAndTheWindow) {
    const auto stored = read(settings_path(spa_)).at("policy");
    EXPECT_EQ(stored.at("auto_post_delay_min_minutes").as_int64(), 0);
    EXPECT_EQ(stored.at("auto_post_window_start").as_string(), "");

    const auto saved = save_settings(spa_, "policy", R"({"auto_post_delay_min_minutes": 20, "auto_post_delay_max_minutes": 120,
                                                          "auto_post_window_start": "09:00", "auto_post_window_end": "21:00"})");
    ASSERT_EQ(saved.status, 200u) << saved.body;
    const auto settings = auto_posting_of(spa_);
    EXPECT_EQ(settings.at("delay_max_minutes").as_int64(), 120);
    EXPECT_EQ(settings.at("window_end").as_string(), "21:00");
    EXPECT_THAT(std::string(settings.at("timing_problem").as_string()), HasSubstr("time zone"));

    const auto message = [&](std::string_view rules) { return error_message(save_settings(spa_, "policy", rules)); };
    EXPECT_THAT(message(R"({"auto_post_delay_min_minutes": 200, "auto_post_delay_max_minutes": 100})"), HasSubstr("minimum is greater"));
    EXPECT_THAT(message(R"({"auto_post_delay_max_minutes": 5000})"), HasSubstr("between 0 and 1440"));
    EXPECT_THAT(message(R"({"auto_post_window_start": "9am", "auto_post_window_end": "21:00"})"), HasSubstr("09:00"));
    EXPECT_THAT(message(R"({"auto_post_window_start": "09:00", "auto_post_window_end": ""})"), HasSubstr("only one end"));
    EXPECT_THAT(message(R"({"auto_post_window_start": "09:00", "auto_post_window_end": "09:00"})"), HasSubstr("not a span of time"));
    // An empty pair removes the window.
    EXPECT_EQ(save_settings(spa_, "policy", R"({"auto_post_window_start": "", "auto_post_window_end": ""})").status, 200u);
    EXPECT_TRUE(auto_posting_of(spa_).at("timing_problem").is_null());
}

// The delivery log's filter takes every event type a delivery can have, the
// client's report among them (the handler once kept a list of its own, which lacked it).
TEST_F(RepliesApi, TheDeliveryLogFiltersByEveryEventType) {
    tx([&](pqxx::work& tx) {
        ASSERT_TRUE(repo::emit_to(tx,
                                  {.type = std::string(repo::kEventClientReport),
                                   .key = "report:1",
                                   .client_id = spa_client_,
                                   .location_id = std::nullopt,
                                   .review_id = std::nullopt,
                                   .subject = "Your report",
                                   .body = "A made-up report.",
                                   .link_path = "/#/reports/1"},
                                  "owner@example.com", clock_.now()));
    });
    const auto reports = read("/api/notification-deliveries?event_type=client_report");
    ASSERT_EQ(reports.at("total").as_int64(), 1);
    EXPECT_EQ(reports.at("items").as_array().at(0).at("event_type").as_string(), "client_report");
    for (const auto* type : {"escalation", "approval_needed", "system_problem", "test"}) {
        const auto others = get(std::format("/api/notification-deliveries?event_type={}", type), browser_);
        ASSERT_EQ(others.status, 200u) << type;
        EXPECT_EQ(json(others).at("total").as_int64(), 0) << type;
    }
    EXPECT_EQ(get("/api/notification-deliveries?event_type=newsletter", browser_).status, 400u);
}
