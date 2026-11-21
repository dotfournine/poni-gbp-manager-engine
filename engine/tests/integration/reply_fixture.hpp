// What the reply tests start from: two linked locations with their seed
// voices, a scripted model, an in-memory Google that takes replies and holds
// them back like the real one, a notification channel that keeps what it is
// given, a staff member, and the jobs wired as main wires them.
#pragma once

#include "db/repo_analysis.hpp"
#include "db/repo_posting.hpp"
#include "db/repo_reviews.hpp"
#include "engine_fixture.hpp"
#include "jobs/analysis.hpp"
#include "jobs/go_live.hpp"
#include "jobs/notifications.hpp"
#include "jobs/posting.hpp"

#include <optional>
#include <string>

namespace poni::test {

inline constexpr std::string_view kSevereAnswer = R"({
    "language": "en", "script": "Latn", "sentiment": "negative", "contains_complaint": true,
    "topics": [], "items": [],
    "severity_flags": [{"id": "illness_food_poisoning", "evidence": "we were both throwing up"}],
    "manipulation_suspected": false, "manipulation_evidence": "", "summary_en": "Two guests fell ill after dinner."})";
inline constexpr std::string_view kHoldingAnswer = R"({
    "reply": "Thank you for letting us know. We are concerned to hear this and take it seriously. Please reach out to us directly so our team can look into what happened.",
    "language": "en", "script": "Latn", "manipulation_suspected": false, "notes_for_staff": ""})";

class ReplyTest : public EngineTest {
protected:
    void SetUp() override {
        EngineTest::SetUp();
        spa_and_buffet();
        become_test_clients();
        add_staff();
        // Both switches are off after installation; most tests are about what happens when they are on.
        exec("UPDATE system_settings SET posting_enabled = true, email_sending_enabled = true");

        // The point in the policy's delay range is fixed, so a test knows when an auto-post is due.
        pipeline_.emplace(db(), clock_, llm_, queue(), jobs::AnalysisOptions{.delay_roll = [this] { return roll_; }});
        poster_.emplace(db(), clock_, gbp_, gbp_, jobs::PostingOptions{});
        tester_.emplace(db(), clock_, llm_);
        notifier_.emplace(db(), clock_, jobs::NotifierOptions{.public_url = "http://localhost:5173", .lifetimes = {}});
        notifier_->add_channel(channel_);
        notifier_->set_push(push_);
        sync_.emplace(db(), clock_, gbp_, queue(), jobs::SyncOptions{});
        jobs::install(registry_, *sync_, nullptr);
        jobs::install(registry_, *pipeline_);
        jobs::install(registry_, *poster_);
        jobs::install(registry_, *tester_);
        jobs::install(registry_, *notifier_);
    }

    void TearDown() override {
        sync_.reset();
        notifier_.reset();
        tester_.reset();
        poster_.reset();
        pipeline_.reset();
        EngineTest::TearDown();
    }

    // Puts a review on the fake Google and in the mirror, as a sync would. Returns its id.
    std::int64_t review(std::string_view location, const std::string& review_id, std::string_view stars, std::optional<std::string> comment,
                        std::optional<std::string> reply = std::nullopt, std::string_view updated = "2026-11-09T10:00:00Z") {
        auto incoming = make_review(gbp_.account_id, location, review_id, stars, std::move(comment), "2026-11-09T10:00:00Z", updated,
                                    std::move(reply));
        gbp_.put(incoming);
        return store_review(location == kSpa ? spa_ : buffet_, incoming);
    }

    // A happy five-star review with a waiting draft. Returns the draft's id.
    std::int64_t waiting_draft(const std::string& review_id = "r1", std::string_view location = kSpa) {
        const auto id = review(location, review_id, "FIVE", "Lovely head spa with Eva, so relaxing!");
        await(pipeline_->process(id, false, false));
        return live_draft(id);
    }

    std::int64_t live_draft(std::int64_t review_id) {
        return value<std::int64_t>("SELECT id FROM reply_drafts WHERE review_id = " + std::to_string(review_id) + " ORDER BY id DESC LIMIT 1");
    }

    // A signed-in person approves the draft's stored text.
    repo::Result<model::ReplyDraft> approve(std::int64_t draft_id) {
        const auto text = draft(draft_id, "text");
        return as_staff([&](pqxx::work& tx, audit::Writer& audit) {
            return repo::approve_draft(tx, audit, draft_id, text);
        });
    }

    // The poster's pass that confirms posts Google now shows; returns how many it confirmed.
    std::size_t confirm() { return await(poster_->confirm()); }
    long approvals_needed() { return count("notification_deliveries WHERE event_type = 'approval_needed'"); }
    std::string draft(std::int64_t draft_id, const std::string& column) {
        return value<std::string>("SELECT coalesce(" + column + "::text, '') FROM reply_drafts WHERE id = " + std::to_string(draft_id));
    }
    // A column of the draft's newest post.
    std::string post(std::int64_t draft_id, const std::string& column) {
        return value<std::string>("SELECT coalesce(" + column + "::text, '') FROM reply_posts WHERE draft_id = " + std::to_string(draft_id) +
                                  " ORDER BY id DESC LIMIT 1");
    }
    std::string review_value(std::int64_t review_id, const std::string& column) {
        return value<std::string>("SELECT coalesce(" + column + "::text, '') FROM reviews WHERE id = " + std::to_string(review_id));
    }

    FakeLlm llm_;
    // Where in its delay range an auto-post falls, in [0, 1).
    double roll_ = 0.0;
    FakeChannel channel_;
    test::FakePushSender push_;
    std::optional<jobs::ReviewPipeline> pipeline_;
    std::optional<jobs::ReplyPoster> poster_;
    std::optional<jobs::GoLiveTester> tester_;
    std::optional<jobs::Notifier> notifier_;
    std::optional<jobs::Sync> sync_;
};

}  // namespace poni::test
