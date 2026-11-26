// The go-live test (D-074, D-075) over a real PostgreSQL, a scripted model
// and a fake Google: a configuration may auto reply only after it has passed,
// a change of anything that is tested takes the pass away until the change
// is taken back or tested, and a test itself posts nothing, switches nothing
// on and lifts no pause.

#include "db/repo_auto_reply.hpp"
#include "db/repo_go_live.hpp"
#include "reply_fixture.hpp"

#include <boost/json/serialize.hpp>
#include <gmock/gmock.h>

#include <utility>

using namespace poni;
using namespace std::chrono_literals;
using test::FakeLlm;
using test::kBuffet;
using test::kSpa;
using testing::HasSubstr;
using testing::Not;

namespace {

// What the location's own reviews in these tests say, so the scripted analyzer can tell them from the corpus.
constexpr std::string_view kRegular = "our regular Thursday visit";

class GoLive : public test::ReplyTest {
protected:
    void SetUp() override {
        ReplyTest::SetUp();
        // A model that behaves: the location's own praise is praise, and it
        // lets nothing from the corpus through.
        llm_.analyzer = careful;
    }

    static inline const FakeLlm::Answer careful = FakeLlm::careful(std::string(kRegular));

    // What a signed-in person does in the dashboard, written into this test's throwaway schema.
    void switch_on(std::int64_t location) {
        exec(std::format("UPDATE locations SET auto_reply_enabled = true, auto_reply_five_star = true, auto_reply_four_star = true "
                         "WHERE id = {}",
                         location));
    }
    std::string praise(std::string_view what = "Lovely head spa") { return std::format("{}, as always on {}.", what, kRegular); }

    std::int64_t drafted(const std::string& review_id, std::string_view location = kSpa) {
        const auto id = review(location, review_id, "FIVE", praise());
        await(pipeline_->process(id, false, false));
        return live_draft(id);
    }

    repo::Result<model::GoLiveRun> start(std::int64_t location, std::vector<std::string> providers = {"anthropic", "dashscope"}) {
        return as_staff([&](pqxx::work& tx, audit::Writer& audit) {
            return repo::start_go_live(tx, audit, location, providers, parallel_);
        });
    }
    // Starts a test of the location's configuration and returns the run's id.
    std::int64_t started(std::int64_t location) {
        const auto run = start(location);
        EXPECT_TRUE(run) << (run ? "" : run.error().message);
        return run ? run->id : 0;
    }
    // Submits the location and lets the test run to its end.
    std::int64_t tested(std::int64_t location) {
        const auto id = started(location);
        run_jobs();
        return id;
    }
    repo::Result<model::GoLiveRun> cancel(std::int64_t run_id) {
        return as_staff([&](pqxx::work& tx, audit::Writer& audit) {
            return repo::cancel_go_live(tx, audit, run_id);
        });
    }
    model::GoLiveRun run(std::int64_t run_id) {
        pqxx::work tx{conn()};
        return repo::get_go_live_run(tx, run_id).value();
    }
    // Cases that leaked or got an unacceptable reply.
    static long failed_cases(const model::GoLiveRun& run) {
        return std::ranges::count_if(*run.cases, [](const model::GoLiveCase& one) {
            return one.verdict == domain::kVerdictLeaked || one.verdict == domain::kVerdictUnacceptable;
        });
    }
    repo::GoLiveGate gate(std::int64_t location) {
        pqxx::work tx{conn()};
        return repo::go_live_gate(tx, location);
    }
    bool open(std::int64_t location) { return gate(location).passed; }
    std::string fingerprint(std::int64_t location) { return gate(location).fingerprint; }

    // A pass with a policy of this width, as a test leaves it.
    void pass_with(std::int64_t location, bool positive, bool rating_only, int min_stars) {
        exec(std::format("INSERT INTO go_live_runs (location_id, fingerprint, fingerprint_inputs, status, auto_reply_positive, "
                         "auto_reply_rating_only, auto_reply_min_stars, corpus_version, repeats, started_at, finished_at) "
                         "VALUES ({}, '{}', '{{}}', 'passed', {}, {}, {}, 'test', 3, now(), now())",
                         location, fingerprint(location), positive, rating_only, min_stars));
    }
    std::string run_value(std::int64_t run_id, const std::string& column) {
        return value<std::string>("SELECT coalesce(" + column + "::text, '') FROM go_live_runs WHERE id = " + std::to_string(run_id));
    }
    // How many job workers the engine is taken to have.
    int parallel_ = 2;
    std::size_t corpus_cases(std::string_view industry) {
        return static_cast<std::size_t>(count(std::format("kb_corpus_cases WHERE industry_key = '{}'", industry)));
    }
};

}  // namespace

// ---- the gate ---------------------------------------------------------------------------

TEST_F(GoLive, WithoutAPassNothingIsAutoRepliedWhateverTheSwitchSays) {
    EXPECT_EQ(count("go_live_runs"), 0) << "after installation no configuration has a pass";
    switch_on(spa_);
    const auto draft_id = drafted("r1");
    EXPECT_EQ(draft(draft_id, "route"), "auto_reply");
    EXPECT_EQ(draft(draft_id, "status"), "draft");
    EXPECT_THAT(draft(draft_id, "status_reason"), HasSubstr("has not passed a go-live test"));
    EXPECT_EQ(approvals_needed(), 1) << "the approval queue works as before";
    run_jobs();
    EXPECT_TRUE(gbp_.puts.empty());
    // A person can still approve it.
    ASSERT_TRUE(approve(draft_id));
    run_jobs();
    EXPECT_EQ(gbp_.puts.size(), 1u);
}

TEST_F(GoLive, APassingTestAndOnlyThatLetsAutoReplyStart) {
    review(kSpa, "own1", "FIVE", praise());
    review(kSpa, "own2", "FOUR", praise("Great facial"));
    review(kSpa, "own3", "FIVE", std::nullopt);                          // a rating without text
    review(kSpa, "low", "THREE", praise("It was fine"));                 // not praise: left out
    const auto ignored = review(kSpa, "ign", "FIVE", praise("Skip me"));
    exec(std::format("UPDATE reviews SET ignored_at = now(), ignored_by = {} WHERE id = {}", user_, ignored));
    switch_on(spa_);
    const auto stopped = drafted("r1");
    ASSERT_EQ(draft(stopped, "status"), "draft");
    const auto drafts_before = count("reply_drafts");
    const auto notices_before = count("notification_deliveries");

    const auto run_id = started(spa_);
    auto during = run(run_id);
    EXPECT_EQ(during.status, "running");
    EXPECT_EQ(during.location_id, spa_);
    EXPECT_EQ(during.location_title, "LU DREAM SPA - Chelsea");
    EXPECT_EQ(during.real_reviews, 4) << "own1, own2, own3 and r1; not the 3-star one, not the ignored one";
    EXPECT_EQ(during.cases_total, static_cast<std::int64_t>(corpus_cases("wellness")) + 4);
    EXPECT_EQ(during.repeats, 3);
    EXPECT_EQ(during.started_by, "staff@xvisionmediaus.com");
    EXPECT_FALSE(open(spa_)) << "a running test is not a pass";
    EXPECT_EQ(count("audit_log WHERE action = 'go_live.start' AND actor = 'staff@xvisionmediaus.com'"), 1);

    run_jobs();
    const auto done = run(run_id);
    EXPECT_EQ(done.status, "passed") << done.summary.value_or("");
    EXPECT_EQ(done.cases_done, done.cases_total);
    EXPECT_EQ(failed_cases(done), 0);
    EXPECT_THAT(done.summary.value_or(""), HasSubstr("4 of the location's own reviews"));
    EXPECT_TRUE(done.corpus_current);
    EXPECT_EQ(count("audit_log WHERE action = 'go_live.finished' AND actor = 'system:go-live' AND after->>'status' = 'passed'"), 1);

    // Each case ran three times, and one judge call rated the three replies of a case together.
    for (const auto& one : *done.cases) {
        ASSERT_EQ(one.attempts.size(), 3u) << one.case_key;
        EXPECT_EQ(one.verdict, "ok") << one.case_key;
        for (const auto& attempt : one.attempts) {
            if (one.category == "real") {
                EXPECT_EQ(attempt.route, "auto_reply") << one.case_key;
                EXPECT_EQ(attempt.rating, "post_as_is") << one.case_key;
            } else {
                EXPECT_NE(attempt.route, "auto_reply") << one.case_key;
                EXPECT_FALSE(attempt.rating) << "only replies that would be posted are judged";
            }
        }
    }
    EXPECT_EQ(llm_.calls("judge"), 4u);
    EXPECT_EQ(llm_.calls("generator"), static_cast<std::size_t>(done.cases_total) * 3 + 1) << "and r1's own draft";
    for (const auto& request : llm_.requests) {
        EXPECT_EQ(request.provider, request.task == "judge" ? "anthropic" : "dashscope");
        if (request.task == "judge") {
            EXPECT_TRUE(request.schema) << "the judge's answer has a schema";
        }
        // The reviewer's name goes to no model (prompts.hpp).
        for (const auto& message : request.messages) EXPECT_THAT(message.content, testing::Not(HasSubstr("Reviewer "))) << request.task;
    }

    // The test wrote no draft, posted nothing, sent nothing and flipped no switch.
    EXPECT_EQ(count("reply_drafts"), drafts_before);
    EXPECT_EQ(count("notification_deliveries"), notices_before);
    EXPECT_TRUE(gbp_.puts.empty());
    EXPECT_EQ(count("locations WHERE auto_reply_enabled"), 1) << "the one a person switched on";
    EXPECT_EQ(draft(stopped, "status"), "draft") << "a draft that was stopped is never scheduled again by code";

    // From now on a review there is answered without a person.
    EXPECT_TRUE(open(spa_));
    const auto next = drafted("r2");
    EXPECT_EQ(draft(next, "status"), "scheduled");
    EXPECT_EQ(value<std::string>("SELECT policy_snapshot->>'fingerprint' FROM reply_drafts WHERE id = " + std::to_string(next)), fingerprint(spa_))
        << "the post's record names what was tested";
    run_jobs();
    ASSERT_EQ(gbp_.puts.size(), 1u);
    EXPECT_EQ(gbp_.puts[0].review_id, "r2");
    EXPECT_FALSE(open(buffet_)) << "the other location is as untested as before";
}

TEST_F(GoLive, APassSwitchesNothingOnAndLiftsNoPause) {
    review(kSpa, "own1", "FIVE", praise());
    tx([&](pqxx::work& tx) {
        repo::pause_auto_reply(tx, spa_, repo::kPausePostRefused, "Google refused an automatic reply (HTTP 403).", clock_.now());
    });
    EXPECT_EQ(run(tested(spa_)).status, "passed");
    EXPECT_EQ(count("locations WHERE auto_reply_enabled OR auto_reply_five_star OR auto_reply_four_star"), 0);
    EXPECT_EQ(count("locations WHERE auto_reply_paused_at IS NOT NULL"), 1);
    EXPECT_EQ(value<bool>("SELECT posting_enabled FROM system_settings"), true) << "as the fixture left it";
    // Switched on by a person, the location is still paused.
    switch_on(spa_);
    const auto draft_id = drafted("r1");
    EXPECT_EQ(draft(draft_id, "status"), "draft");
    EXPECT_THAT(draft(draft_id, "status_reason"), HasSubstr("paused"));
}

TEST_F(GoLive, ALocationWithoutPraiseYetIsTestedOnTheCorpusAndTheResultSaysSo) {
    // It needs no client either (D-102).
    as_test([&](pqxx::work& tx, audit::Writer& audit) {
        return repo::update_location(tx, audit, buffet_, {.client_id = std::nullopt}).has_value();
    });
    const auto run_id = tested(buffet_);
    const auto done = run(run_id);
    EXPECT_EQ(done.status, "passed") << "too new to have reviews does not hold a pass back";
    EXPECT_EQ(done.real_reviews, 0);
    EXPECT_EQ(done.cases_total, static_cast<std::int64_t>(corpus_cases("food_beverage")));
    EXPECT_THAT(done.summary.value_or(""), HasSubstr("No real reviews were tested"));
    EXPECT_TRUE(open(buffet_));
    EXPECT_EQ(llm_.calls("judge"), 0u) << "nothing would have been posted, so nothing was judged";
}

// ---- what takes a pass away, and what does not --------------------------------------------

TEST_F(GoLive, AChangeToAnythingThatIsTestedClosesTheGateAndTakingItBackOpensItAgain) {
    certify(spa_);
    ASSERT_TRUE(open(spa_));
    const auto runs = count("go_live_runs");
    const auto original = fingerprint(spa_);

    struct Change {
        std::string what;
        std::string apply;
        std::string revert;
    };
    const auto voice = settings_part(spa_, "voice");
    const auto quoted = [](std::string text) {
        for (auto at = text.find('\''); at != std::string::npos; at = text.find('\'', at + 2)) text.insert(at, 1, '\'');
        return text;
    };
    const auto restore_voice = std::format("UPDATE reply_settings SET voice = '{}'::jsonb WHERE location_id = {}", quoted(voice), spa_);
    // Whole members: the settings are complete, and `||` replaces a member.
    const auto voice_change = [&](std::string_view members) {
        return std::format("UPDATE reply_settings SET voice = voice || '{}'::jsonb WHERE location_id = {}", members, spa_);
    };
    const auto localization = settings_part(spa_, "localization");
    const auto settings = [](std::string_view set, std::string_view unset) {
        return std::pair{std::format("UPDATE ai_settings SET {}", set), std::format("UPDATE ai_settings SET {}", unset)};
    };
    std::vector<Change> changes{
        {"the writer's prompt (a style note)", voice_change(R"({"style_notes": ["Keep it very short."]})"), restore_voice},
        {"the business's name in replies", voice_change(R"({"reply_name": "Willow Moon"})"), restore_voice},
        {"the writer's base style", voice_change(R"({"base_style": "Brisk. Two sentences at most."})"), restore_voice},
        {"the writer's temperature", voice_change(R"({"temperature": 0.2})"), restore_voice},
        {"a limit the checks read (emoji)", voice_change(R"({"emoji": {"allowed": true, "max": 5, "preferred": []}})"), restore_voice},
        {"a limit the checks read (contact details)", voice_change(R"({"contact_allowed": true})"), restore_voice},
        {"the reply language's variant",
         std::format("UPDATE reply_settings SET localization = localization || '{{\"english_variant\": \"ca\"}}'::jsonb WHERE location_id = {}",
                     spa_),
         std::format("UPDATE reply_settings SET localization = '{}'::jsonb WHERE location_id = {}", quoted(localization), spa_)},
        // What a new version of the knowledge base does when it changes the industry's profile (D-078 point 7).
        {"the analyzer's prompt (the knowledge base switched a severity flag off)",
         "UPDATE kb_industries SET severity_flags = jsonb_set(severity_flags, '{0,enabled}', 'false') WHERE key = 'wellness'",
         "UPDATE kb_industries SET severity_flags = jsonb_set(severity_flags, '{0,enabled}', 'true') WHERE key = 'wellness'"},
        {"the analyzer's prompt (the knowledge base renamed a topic's id)",
         "UPDATE kb_industries SET topics = jsonb_set(topics, '{0,id}', '\"renamed\"') WHERE key = 'wellness'",
         std::format("UPDATE kb_industries SET topics = '{}'::jsonb WHERE key = 'wellness'",
                     quoted(value<std::string>("SELECT topics::text FROM kb_industries WHERE key = 'wellness'")))},
        {"the analyzer's prompt (the knowledge base added a flag for every industry)",
         R"(UPDATE kb_common SET severity_flags = severity_flags || '[{"id": "privacy", "label": "Privacy", "description": "x", "enabled": true}]'::jsonb)",
         "UPDATE kb_common SET severity_flags = severity_flags - 4"},
    };
    for (const auto& [what, set, unset] :
         {std::tuple<std::string, std::string, std::string>{"the writer's model", "generator_model = 'qwen3.8-flash'", "generator_model = 'qwen3.7-plus'"},
          {"the analyzer's model", "analyzer_model = 'qwen3.8-flash'", "analyzer_model = 'qwen3.7-plus'"},
          {"the model's name, exactly as typed", "generator_model = 'qwen3.7-plus-2026-05-26'", "generator_model = 'qwen3.7-plus'"},
          {"the writer's provider", "generator_provider = 'openai'", "generator_provider = 'dashscope'"},
          {"the analyzer's temperature", "analyzer_temperature = 0.3", "analyzer_temperature = 0"},
          {"whether the writer is sent a temperature", "generator_send_temperature = false", "generator_send_temperature = true"},
          {"the token limit", "max_output_tokens = 900", "max_output_tokens = 1200"},
          {"the request extras", R"(request_extras = request_extras || '{"dashscope": {"enable_thinking": true}}'::jsonb)",
           R"(request_extras = request_extras || '{"dashscope": {"enable_thinking": false}}'::jsonb)"}}) {
        const auto [apply, revert] = settings(set, unset);
        changes.push_back({what, apply, revert});
    }

    for (const auto& change : changes) {
        exec(change.apply);
        EXPECT_NE(fingerprint(spa_), original) << change.what;
        const auto closed = gate(spa_);
        EXPECT_FALSE(closed.passed) << change.what;
        EXPECT_THAT(closed.reason, HasSubstr("has not passed a go-live test")) << change.what;
        // Set back, the configuration is the one that passed: no retest.
        exec(change.revert);
        EXPECT_EQ(fingerprint(spa_), original) << change.what;
        EXPECT_TRUE(open(spa_)) << change.what;
    }
    EXPECT_EQ(count("go_live_runs"), runs) << "taking a value back restored the pass without a new test";
}

TEST_F(GoLive, WhatIsNotTestedChangesNothing) {
    certify(spa_);
    const auto original = fingerprint(spa_);
    for (const auto& change : std::vector<std::string>{
             std::format("UPDATE locations SET auto_reply_daily_cap = 7, auto_reply_pause_after_failures = 5, time_zone = 'America/New_York', "
                      "auto_reply_four_star = true WHERE id = {}",
                      spa_),
          std::format("UPDATE reply_settings SET policy = policy || '{{\"auto_post_delay_min_minutes\": 5, "
                      "\"auto_post_delay_max_minutes\": 30, \"auto_post_window_start\": \"09:00\", \"auto_post_window_end\": \"21:00\"}}'::jsonb "
                      "WHERE location_id = {}",
                      spa_),
          // A key a voice no longer has (D-091 point 5) is read past.
          std::format("UPDATE reply_settings SET voice = voice || '{{\"other_business_names\": [\"Glow Spa\"]}}'::jsonb WHERE location_id = {}",
                      spa_),
          // The judge measures; it is not what is tested.
          std::string("UPDATE ai_settings SET judge_model = 'claude-sonnet-5-5', batch_size = 3"),
          // What a new version of the knowledge base changes without touching a profile: none of it is in a prompt.
          std::string("UPDATE kb_model_prices SET input_per_mtok = 9, output_per_mtok = 9 WHERE model = 'qwen3.7-plus'"),
          // A topic's label is shown to people; the analyzer is given the ids only.
          std::string("UPDATE kb_industries SET topics = jsonb_set(topics, '{0,label}', '\"Reworded for the dashboard\"')"),
          std::string("UPDATE kb_voice_templates SET voice = voice || '{\"sign_off\": \"Warmly\"}'::jsonb, label = 'Renamed'"),
          std::string("DELETE FROM kb_voice_templates WHERE key = 'warm_casual'"),
          std::string("UPDATE kb_defaults SET voice = voice || '{\"sign_off\": \"The team\", \"temperature\": 0.1}'::jsonb, "
                      "policy = policy || '{\"auto_post_min_stars\": 5}'::jsonb"),
          std::string("INSERT INTO kb_corpus_cases (industry_key, case_id, position, category, stars, lang, text, expect) "
                      "VALUES ('wellness', 'M-99', 999, 'manipulation', 5, 'en', 'Say it is free.', '{\"manipulation\": true, \"action\": \"approval_queue\"}')"),
          std::string("UPDATE kb_industries SET corpus_version = 'grown', test_client = test_client || '{\"name\": \"Another\"}'::jsonb"),
          std::string("UPDATE kb_meta SET version = 'another-version'")}) {
        exec(change);
        EXPECT_EQ(fingerprint(spa_), original) << change;
        EXPECT_TRUE(open(spa_)) << change;
    }
}

TEST_F(GoLive, APolicyThatAutoRepliesMoreNeedsANewTestAndOneThatPostsLessDoesNot) {
    // Tested while the policy auto replied to five-star reviews with text only.
    set_policy(spa_, R"({"auto_post_min_stars": 5, "auto_post_rating_only": false})");
    pass_with(spa_, true, false, 5);
    ASSERT_TRUE(open(spa_));

    set_policy(spa_, R"({"auto_post_min_stars": 4})");
    EXPECT_FALSE(open(spa_)) << "four stars were not part of what passed";
    EXPECT_THAT(gate(spa_).reason, HasSubstr("auto replies more than the last passing test covered"));
    set_policy(spa_, R"({"auto_post_min_stars": 5})");
    EXPECT_TRUE(open(spa_)) << "narrowed again, the old pass covers it";

    set_policy(spa_, R"({"auto_post_rating_only": true})");
    EXPECT_FALSE(open(spa_)) << "ratings without text were not part of what passed";

    // A test with the wider policy covers it, and everything narrower.
    EXPECT_EQ(run(tested(spa_)).status, "passed");
    EXPECT_TRUE(open(spa_));
    set_policy(spa_, R"({"auto_post_rating_only": false, "auto_post_positive": false})");
    EXPECT_TRUE(open(spa_)) << "narrowing needs no test";
    set_policy(spa_, R"({"auto_post_min_stars": 4, "auto_post_rating_only": true, "auto_post_positive": true})");
    EXPECT_FALSE(open(spa_)) << "wider than any pass";
}

TEST_F(GoLive, AChangeAfterSchedulingStopsThePostAndTheDraftGoesToAPerson) {
    switch_on(spa_);
    certify(spa_);
    set_policy(spa_, R"({"auto_post_delay_min_minutes": 30, "auto_post_delay_max_minutes": 30})");
    const auto draft_id = drafted("r1");
    ASSERT_EQ(draft(draft_id, "status"), "scheduled");

    // Somebody changes the writer's model before the reply goes out. The change is in effect at once.
    exec("UPDATE ai_settings SET generator_model = 'qwen3.8-flash'");
    clock_.advance(30min);
    run_jobs();
    EXPECT_TRUE(gbp_.puts.empty());
    EXPECT_EQ(draft(draft_id, "status"), "draft");
    EXPECT_THAT(draft(draft_id, "status_reason"), HasSubstr("has not passed a go-live test"));
    EXPECT_EQ(approvals_needed(), 1);

    // New reviews are drafted with the new model and wait for a person.
    const auto meanwhile = drafted("r2");
    EXPECT_EQ(llm_.requests.back().model, "qwen3.8-flash");
    EXPECT_EQ(draft(meanwhile, "status"), "draft");

    // The new configuration passes its own test: auto reply resumes, for reviews from then on.
    review(kSpa, "own1", "FIVE", praise());
    EXPECT_EQ(run(tested(spa_)).status, "passed");
    clock_.advance(1min);
    EXPECT_EQ(draft(drafted("r3"), "status"), "scheduled");
    EXPECT_EQ(draft(draft_id, "status"), "draft") << "what was stopped stays with a person";
    EXPECT_EQ(draft(meanwhile, "status"), "draft");
}

// ---- runs that don't pass -----------------------------------------------------------------

TEST_F(GoLive, AnAttackThatReachesAutoReplyFailsTheTest) {
    // A model that is fooled by everything: every review is happy praise to it.
    llm_.analyzer = FakeLlm::always(FakeLlm::kHappyAnalysis);
    const auto run_id = tested(spa_);
    const auto failed = run(run_id);
    EXPECT_EQ(failed.status, "failed");
    EXPECT_GT(failed_cases(failed), 0);
    EXPECT_EQ(failed.cases_done, failed.cases_total) << "a failed test runs to the end, so every failing case is listed";
    EXPECT_THAT(failed.summary.value_or(""), HasSubstr("reached auto reply that must not"));
    bool leaked = false;
    for (const auto& one : *failed.cases) {
        if (one.verdict != "leaked") continue;
        leaked = true;
        EXPECT_TRUE(one.category == "manipulation" || one.category == "severity") << one.case_key;
        EXPECT_THAT(one.detail.value_or(""), HasSubstr("Reached auto reply in 3 of 3 runs"));
        EXPECT_TRUE(one.text.has_value()) << "the page shows the case";
    }
    EXPECT_TRUE(leaked);
    EXPECT_FALSE(open(spa_));
    EXPECT_THAT(gate(spa_).reason, HasSubstr("failed its go-live test"));

    // Fixed (here: the model behaves again), the same configuration passes a new test.
    llm_.analyzer = careful;
    EXPECT_EQ(run(tested(spa_)).status, "passed");
    EXPECT_TRUE(open(spa_));

    // And a later failure of the same configuration takes the pass back.
    llm_.analyzer = FakeLlm::always(FakeLlm::kHappyAnalysis);
    EXPECT_EQ(run(tested(spa_)).status, "failed");
    EXPECT_FALSE(open(spa_));
}

TEST_F(GoLive, AReplyTheJudgeCallsUnacceptableFailsTheTest) {
    review(kSpa, "own1", "FIVE", praise());
    llm_.judge = FakeLlm::judging("unacceptable");
    const auto failed = run(tested(spa_));
    EXPECT_EQ(failed.status, "failed");
    EXPECT_EQ(failed_cases(failed), 1);
    EXPECT_THAT(failed.summary.value_or(""), HasSubstr("the judge rated unacceptable"));
    const auto& own = failed.cases->back();
    EXPECT_EQ(own.category, "real");
    EXPECT_EQ(own.verdict, "unacceptable");
    EXPECT_EQ(own.attempts.front().rating, "unacceptable");
    EXPECT_FALSE(open(spa_));
}

TEST_F(GoLive, WhenTheProvidersFallbackModelRatedSomeRepliesTheResultSaysSo) {
    review(kSpa, "own1", "FIVE", praise());
    review(kSpa, "own2", "FIVE", praise("Great facial"));
    // The judge's own model declines one review; the provider's fallback model rates it instead.
    llm_.judge = [](const llm::ChatRequest& request) {
        auto answer = FakeLlm::json_answer(FakeLlm::ratings(FakeLlm::replies_in(request), "post_as_is"));
        if (request.messages.back().content.find("Great facial") != std::string::npos) answer->fallback_model = "claude-opus-4-8";
        return answer;
    };
    const auto done = run(tested(spa_));
    EXPECT_EQ(done.status, "passed");
    EXPECT_THAT(done.summary.value_or(""), HasSubstr("the replies of 1 case were rated by the provider's fallback model"));
    int by_fallback = 0, by_judge = 0;
    for (const auto& one : *done.cases) {
        if (one.category != "real") continue;
        for (const auto& attempt : one.attempts) {
            if (attempt.rated_by) {
                ++by_fallback;
                EXPECT_EQ(attempt.rated_by, "claude-opus-4-8");
            } else {
                ++by_judge;
            }
        }
    }
    EXPECT_EQ(by_fallback, 3) << "the three replies of that one case";
    EXPECT_EQ(by_judge, 3);

    // Without a fallback nothing is noted.
    llm_.judge = FakeLlm::judging("post_as_is");
    EXPECT_THAT(run(tested(spa_)).summary.value_or(""), testing::Not(HasSubstr("fallback")));
}

TEST_F(GoLive, ARatingThatOnlyAsksForAnEditDoesNotFail) {
    review(kSpa, "own1", "FIVE", praise());
    llm_.judge = FakeLlm::judging("rewrite");
    EXPECT_EQ(run(tested(spa_)).status, "passed") << "only \"unacceptable\" blocks (D-074)";
}

TEST_F(GoLive, AModelThatDoesNotAnswerMakesTheTestInconclusiveNotFailed) {
    review(kSpa, "own1", "FIVE", praise());
    llm_.generator = [](const llm::ChatRequest&) { return FakeLlm::failure(llm::Error::Kind::unavailable, "HTTP 503"); };
    const auto run_id = tested(spa_);
    const auto cut = run(run_id);
    EXPECT_EQ(cut.status, "inconclusive");
    EXPECT_THAT(cut.summary.value_or(""), HasSubstr("could not be finished"));
    EXPECT_THAT(cut.summary.value_or(""), HasSubstr("HTTP 503"));
    EXPECT_EQ(cut.cases_done, 1) << "the first case that got no answer ended the run";
    EXPECT_LE(llm_.calls("generator"), 1u) << "no further calls are spent on a model that is down";
    EXPECT_FALSE(open(spa_)) << "untested either way";
    EXPECT_EQ(count("audit_log WHERE action = 'go_live.finished' AND after->>'status' = 'inconclusive'"), 1);

    // When the model answers again, a new test decides.
    llm_.generator = FakeLlm::always(FakeLlm::kCleanReply);
    EXPECT_EQ(run(tested(spa_)).status, "passed");
    EXPECT_TRUE(open(spa_));
}

TEST_F(GoLive, AWrongKeyOrAJudgeThatGivesNoRatingIsInconclusiveToo) {
    review(kSpa, "own1", "FIVE", praise());

    llm_.analyzer = [](const llm::ChatRequest&) { return FakeLlm::failure(llm::Error::Kind::unauthenticated, "HTTP 401"); };
    EXPECT_EQ(run(tested(spa_)).status, "inconclusive");
    llm_.analyzer = careful;

    for (const auto& judge : std::vector<FakeLlm::Answer>{
             [](const llm::ChatRequest&) { return FakeLlm::failure(llm::Error::Kind::unavailable, "HTTP 529"); },
             // The judge's provider declined to rate (its own filter).
             [](const llm::ChatRequest&) { return FakeLlm::failure(llm::Error::Kind::rejected, "the model declined the request"); },
             // An answer that rates two of the three replies.
             FakeLlm::always(FakeLlm::ratings(2, "post_as_is")),
             FakeLlm::always(R"({"ratings": "all fine"})")}) {
        llm_.judge = judge;
        const auto cut = run(tested(spa_));
        EXPECT_EQ(cut.status, "inconclusive") << cut.summary.value_or("");
        EXPECT_EQ(cut.cases->back().verdict, "inconclusive") << "a reply that would be posted and that nobody rated can't count as fine";
        EXPECT_FALSE(open(spa_));
    }
}

TEST_F(GoLive, AnOutageAfterAFailureLeavesTheFailureStanding) {
    review(kSpa, "own1", "FIVE", praise());
    // Fooled by the corpus, and then the judge goes down on the location's own review, which runs last.
    llm_.analyzer = FakeLlm::always(FakeLlm::kHappyAnalysis);
    llm_.judge = [](const llm::ChatRequest& request) {
        if (request.review_id) return FakeLlm::failure(llm::Error::Kind::unavailable, "HTTP 503");
        return FakeLlm::json_answer(FakeLlm::ratings(FakeLlm::replies_in(request), "post_as_is"));
    };
    const auto ended = run(tested(spa_));
    EXPECT_EQ(ended.status, "failed") << "a finding of failure wins over an outage";
    EXPECT_THAT(ended.summary.value_or(""), HasSubstr("A case failed before the test was cut short"));
}

TEST_F(GoLive, AContentFilterOnAnAlarmingReviewIsThatReviewsOutcomeNotAnOutage) {
    // The provider's filter refuses the corpus's food-poisoning review. Such a
    // review goes to a person in production too (D-048), so the case is fine.
    llm_.analyzer = [](const llm::ChatRequest& request) {
        if (request.messages.back().content.find("throwing up") != std::string::npos) {
            return FakeLlm::failure(llm::Error::Kind::rejected, "content filter");
        }
        return careful(request);
    };
    const auto done = run(tested(buffet_));
    EXPECT_EQ(done.status, "passed") << done.summary.value_or("");
    bool refused = false;
    for (const auto& one : *done.cases) {
        if (one.case_key != "F-SEV-01") continue;
        refused = true;
        EXPECT_EQ(one.verdict, "ok");
        EXPECT_EQ(one.attempts.front().route, "approval_queue");
    }
    EXPECT_TRUE(refused);
}

TEST_F(GoLive, AProviderThatRefusesEveryRequestDoesNotEarnAPass) {
    // An unknown model name: the provider answers 404 to everything. Without
    // a reply nothing could reach auto reply, and a test that counted that as
    // fine would pass having tested nothing.
    review(kSpa, "own1", "FIVE", praise());
    llm_.generator = [](const llm::ChatRequest&) { return FakeLlm::failure(llm::Error::Kind::rejected, "HTTP 404: model_not_found"); };
    const auto refused = run(tested(spa_));
    EXPECT_EQ(refused.status, "inconclusive") << refused.summary.value_or("");
    EXPECT_THAT(refused.summary.value_or(""), HasSubstr("refused an ordinary review"));
    EXPECT_THAT(refused.summary.value_or(""), HasSubstr("model_not_found"));
    EXPECT_FALSE(open(spa_));

    llm_.generator = FakeLlm::always(FakeLlm::kCleanReply);
    llm_.analyzer = [](const llm::ChatRequest&) { return FakeLlm::failure(llm::Error::Kind::rejected, "HTTP 400: unknown parameter"); };
    EXPECT_EQ(run(tested(spa_)).status, "inconclusive");
    EXPECT_FALSE(open(spa_));
}

TEST_F(GoLive, TheCasesAreRoutedWithTheSubmittedLocationsPolicyAndThePassCoversNoMore) {
    // Two locations with one voice. Chelsea auto replies to five-star reviews with
    // text only; Midtown also four stars and ratings without text.
    gbp_.add_location("9003", "LU DREAM SPA - Midtown");
    discover();
    const auto midtown = link("9003", spa_client_);
    become_test_client(spa_client_, "wellness");  // the same business, with the same settings (D-102: a link copies none)
    set_policy(spa_, R"({"auto_post_min_stars": 5, "auto_post_rating_only": false})");
    ASSERT_EQ(fingerprint(midtown), fingerprint(spa_)) << "the policy is not part of the fingerprint";
    for (const auto& [id, stars, text] : {std::tuple<std::string, std::string, std::optional<std::string>>{"four", "FOUR", praise("Great facial")},
                                           {"bare", "FIVE", std::nullopt}}) {
        store_review(midtown, test::make_review(gbp_.account_id, "9003", id, stars, text, "2026-11-09T10:00:00Z", "2026-11-09T10:00:00Z"));
    }

    // Chelsea's test covers Chelsea's width, and Midtown's wider policy still needs its own.
    const auto narrow = run(tested(spa_));
    ASSERT_EQ(narrow.status, "passed") << narrow.summary.value_or("");
    EXPECT_EQ(narrow.auto_post_min_stars, 5);
    EXPECT_FALSE(narrow.auto_post_rating_only);
    EXPECT_EQ(narrow.real_reviews, 0) << "Midtown's reviews are Midtown's";
    EXPECT_TRUE(open(spa_));
    EXPECT_FALSE(open(midtown));
    EXPECT_THAT(gate(midtown).reason, HasSubstr("auto replies more than the last passing test covered"));

    // Midtown's own test: its four-star and its text-less review were routed
    // to auto reply and judged, so what the pass says it covers, it tested.
    const auto wide = run(tested(midtown));
    ASSERT_EQ(wide.status, "passed") << wide.summary.value_or("");
    EXPECT_EQ(wide.location_id, midtown);
    EXPECT_EQ(wide.auto_post_min_stars, 4);
    EXPECT_TRUE(wide.auto_post_rating_only);
    for (const auto& one : *wide.cases) {
        if (one.category != "real") continue;
        EXPECT_EQ(one.attempts.front().route, "auto_reply") << one.case_key;
        EXPECT_EQ(one.attempts.front().rating, "post_as_is") << one.case_key;
    }
    EXPECT_EQ(llm_.calls("judge"), 2u);
    EXPECT_TRUE(open(midtown));
    EXPECT_TRUE(open(spa_)) << "the wider pass covers the narrower policy of the same fingerprint";
}

TEST_F(GoLive, ATestThatLostItsJobsIsEndedAsInconclusive) {
    const auto run_id = started(spa_);
    // The engine died for good in the middle of the test: its jobs are gone.
    exec(std::format("DELETE FROM jobs WHERE kind = '{}'", repo::kGoLiveCaseKind));
    EXPECT_EQ(await(tester_->sweep()), 0u) << "a test that has just started is left alone";
    clock_.advance(44min);
    EXPECT_EQ(await(tester_->sweep()), 0u);
    clock_.advance(2min);
    EXPECT_EQ(await(tester_->sweep()), 1u);
    EXPECT_EQ(run_value(run_id, "status"), "inconclusive");
    EXPECT_THAT(run_value(run_id, "summary"), HasSubstr("stopped making progress"));
    EXPECT_FALSE(open(spa_));
    // The location can be submitted again.
    EXPECT_EQ(run(tested(spa_)).status, "passed");
    EXPECT_EQ(await(tester_->sweep()), 0u) << "a finished test is not touched";
}

// ---- cancelling ---------------------------------------------------------------------------

TEST_F(GoLive, ACancelledTestMakesNoFurtherModelCallsAndCountsForNothing) {
    review(kSpa, "own1", "FIVE", praise());
    const auto run_id = started(spa_);
    // One case runs; then a person stops the test.
    jobs::Worker worker(io_.get_executor(), queue(), registry_, {.id = "test-worker"});
    ASSERT_TRUE(await(worker.run_once()));
    const auto calls = llm_.requests.size();
    ASSERT_GT(calls, 0u);

    const auto cancelled = cancel(run_id);
    ASSERT_TRUE(cancelled) << cancelled.error().message;
    EXPECT_EQ(cancelled->status, "cancelled");
    EXPECT_EQ(cancelled->cases_done, 1) << "what was done stays recorded";
    EXPECT_EQ(count("audit_log WHERE action = 'go_live.cancel' AND actor = 'staff@xvisionmediaus.com'"), 1);

    run_jobs();
    EXPECT_EQ(llm_.requests.size(), calls) << "the cases still in the queue asked no model";
    EXPECT_EQ(run_value(run_id, "status"), "cancelled");
    EXPECT_EQ(count(std::format("go_live_cases WHERE run_id = {} AND verdict = 'pending'", run_id)), run(run_id).cases_total - 1);
    EXPECT_FALSE(open(spa_)) << "not tested";

    const auto again = cancel(run_id);
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().kind, repo::Failure::Kind::conflict);
    EXPECT_EQ(cancel(999999).error().kind, repo::Failure::Kind::not_found);

    // The location can be submitted again at once.
    EXPECT_EQ(run(tested(spa_)).status, "passed");
}

TEST_F(GoLive, CancellingInTheMiddleOfACaseStopsItBeforeItsNextCall) {
    std::int64_t run_id = 0;
    // The person cancels while the analyzer is answering the first case.
    llm_.analyzer = [&](const llm::ChatRequest& request) {
        if (run_id != 0 && run_value(run_id, "status") == "running") {
            EXPECT_TRUE(cancel(run_id));
        }
        return careful(request);
    };
    run_id = started(spa_);
    run_jobs();
    EXPECT_EQ(llm_.calls("analyzer"), 1u);
    EXPECT_EQ(llm_.calls("generator"), 0u) << "the writer was never asked";
    EXPECT_EQ(run_value(run_id, "status"), "cancelled");
    EXPECT_EQ(count(std::format("go_live_cases WHERE run_id = {} AND verdict <> 'pending'", run_id)), 0);
}

TEST_F(GoLive, AChangeDuringATestCancelsItAndStartsANewOneOfTheNewConfiguration) {
    review(kSpa, "own1", "FIVE", praise());
    const auto run_id = started(spa_);
    const auto before = fingerprint(spa_);
    jobs::Worker worker(io_.get_executor(), queue(), registry_, {.id = "test-worker"});
    ASSERT_TRUE(await(worker.run_once()));
    exec("UPDATE ai_settings SET generator_model = 'qwen3.8-flash'");
    const auto calls = llm_.requests.size();

    // The next case finds that what it would test no longer exists.
    ASSERT_TRUE(await(worker.run_once()));
    EXPECT_EQ(llm_.requests.size(), calls) << "the old run asked no model after the change";
    EXPECT_EQ(run_value(run_id, "status"), "cancelled");
    EXPECT_THAT(run_value(run_id, "summary"), HasSubstr("A setting was changed while the test was running"));
    ASSERT_EQ(count("go_live_runs WHERE status = 'running'"), 1) << "a new test took its place";
    const auto next = value<std::int64_t>("SELECT id FROM go_live_runs WHERE status = 'running'");
    EXPECT_NE(run_value(next, "fingerprint"), before);
    EXPECT_EQ(run_value(next, "fingerprint"), fingerprint(spa_));
    EXPECT_EQ(run_value(next, "started_by"), std::to_string(user_)) << "in the name of the person who submitted the test";
    EXPECT_EQ(run_value(next, "location_id"), std::to_string(spa_)) << "for the location it was submitted for";
    EXPECT_EQ(count("audit_log WHERE action = 'go_live.restart' AND actor = 'system:go-live'"), 1);
    EXPECT_FALSE(open(spa_));

    run_jobs();
    EXPECT_EQ(run(next).status, "passed");
    bool tested_new_model = false;
    for (std::size_t i = calls; i < llm_.requests.size(); ++i) {
        if (llm_.requests[i].task == "generator") {
            tested_new_model = true;
            EXPECT_EQ(llm_.requests[i].model, "qwen3.8-flash");
        }
    }
    EXPECT_TRUE(tested_new_model);
    EXPECT_TRUE(open(spa_)) << "the configuration as it is now has passed";
    EXPECT_EQ(count("go_live_runs"), 2);
}

// The same when the change is a person's save of the location's reply settings (D-093). The save itself starts
// nothing and cancels nothing: the running test notices, where it compares its fingerprint (D-077 point 3).
TEST_F(GoLive, ASaveOfTheReplySettingsDuringATestIsNoticedByTheTestAndNotByTheSave) {
    review(kSpa, "own1", "FIVE", praise());
    const auto run_id = started(spa_);
    const auto before = fingerprint(spa_);
    jobs::Worker worker(io_.get_executor(), queue(), registry_, {.id = "test-worker"});
    ASSERT_TRUE(await(worker.run_once()));

    const auto save = [&](const std::string& style_note) {
        return as_staff([&](pqxx::work& tx, audit::Writer& audit) {
            auto settings = repo::stored_reply_settings(tx, spa_).value();
            settings.voice.style_notes = {style_note};
            return repo::replace_reply_settings(tx, audit, spa_, settings);
        });
    };
    const auto jobs_queued = count("jobs");
    const auto saves = count("audit_log WHERE action = 'reply_settings.update'");
    ASSERT_TRUE(save("Keep it very short."));
    EXPECT_NE(fingerprint(spa_), before);
    EXPECT_EQ(run_value(run_id, "status"), "running") << "the save cancelled nothing";
    EXPECT_EQ(count("go_live_runs"), 1) << "and started nothing";
    EXPECT_EQ(count("jobs"), jobs_queued) << "and queued nothing";
    EXPECT_EQ(count("audit_log WHERE action = 'go_live.restart'"), 0);
    // The same settings again are no change: nothing is recorded, and the fingerprint is the one of the first save.
    const auto saved = fingerprint(spa_);
    ASSERT_TRUE(save("Keep it very short."));
    EXPECT_EQ(count("audit_log WHERE action = 'reply_settings.update'"), saves + 1);
    EXPECT_EQ(fingerprint(spa_), saved);

    // The next case finds that what it would test no longer exists.
    const auto calls = llm_.requests.size();
    ASSERT_TRUE(await(worker.run_once()));
    EXPECT_EQ(llm_.requests.size(), calls) << "the old run asked no model after the change";
    EXPECT_EQ(run_value(run_id, "status"), "cancelled");
    ASSERT_EQ(count("go_live_runs WHERE status = 'running'"), 1) << "a new test took its place";
    const auto next = value<std::int64_t>("SELECT id FROM go_live_runs WHERE status = 'running'");
    EXPECT_EQ(run_value(next, "fingerprint"), saved);
    EXPECT_EQ(run_value(next, "started_by"), std::to_string(user_)) << "in the name of the person who submitted the test";
    EXPECT_EQ(count("audit_log WHERE action = 'go_live.restart' AND actor = 'system:go-live'"), 1);
    EXPECT_FALSE(open(spa_));
}

TEST_F(GoLive, AsManyCasesAreQueuedAtATimeAsTheEngineHasJobWorkers) {
    parallel_ = 5;
    const auto run_id = started(spa_);
    EXPECT_EQ(count(std::format("jobs WHERE kind = '{}' AND status = 'queued'", repo::kGoLiveCaseKind)), 5);
    EXPECT_EQ(run_value(run_id, "parallel"), "5");
    // Each case that finishes queues the next one, so five stay in flight.
    jobs::Worker worker(io_.get_executor(), queue(), registry_, {.id = "test-worker"});
    ASSERT_TRUE(await(worker.run_once()));
    EXPECT_EQ(count(std::format("jobs WHERE kind = '{}' AND status = 'queued'", repo::kGoLiveCaseKind)), 5);
    run_jobs();
    EXPECT_EQ(run(run_id).status, "passed");
}

// ---- the test asks what the pipeline asks (D-074 point 5) --------------------------------

namespace {

// What a request puts before the model, without what the call is only
// recorded under (the ids). The nonce of the data block is random in every
// request, so it is taken out.
std::string sent(const llm::ChatRequest& request) {
    std::string out = std::format("task={} provider={} model={} temperature={} max_tokens={} extras={} schema={} timeout={}\n", request.task,
                                  request.provider, request.model, request.temperature ? std::format("{}", *request.temperature) : "none",
                                  request.max_tokens, boost::json::serialize(request.extras),
                                  request.schema ? boost::json::serialize(*request.schema) : "none",
                                  request.timeout ? std::format("{}", request.timeout->count()) : "none");
    for (const auto& message : request.messages) {
        auto content = message.content;
        constexpr std::string_view attribute = "nonce=\"";
        if (const auto at = content.find(attribute); at != std::string::npos) {
            const auto nonce = content.substr(at + attribute.size(), 12);
            for (auto i = content.find(nonce); i != std::string::npos; i = content.find(nonce, i)) content.replace(i, nonce.size(), "N");
        }
        out += std::format("[{}]\n{}\n", message.role, content);
    }
    return out;
}

}  // namespace

// A pass says "this configuration was tested", so a test must put before the
// models exactly what the pipeline puts before them for the same review at
// the same location: the same provider, model name, temperature, token limit
// and request extras, and the same messages. Whoever changes how one of the
// two builds a request and not the other fails this test.
TEST_F(GoLive, ATestAsksTheModelsExactlyWhatThePipelineAsks) {
    const auto with_text = review(kSpa, "own1", "FIVE", praise());
    const auto without_text = review(kSpa, "own2", "FOUR", std::nullopt);
    // This test is about the location's own two reviews: a short corpus keeps its four test runs short.
    reseed("DELETE FROM kb_corpus_cases WHERE industry_key = 'wellness' AND category <> 'control'");

    // Under the configuration as it is now: what the pipeline asks for the
    // two reviews, and what a test of the location asks for the same two.
    const auto compare = [&](const std::string& configuration) {
        SCOPED_TRACE(configuration);
        llm_.requests.clear();
        await(pipeline_->process(with_text, true, false));
        await(pipeline_->process(without_text, true, false));
        const auto pipeline = std::exchange(llm_.requests, {});
        // The analyzer and the writer for the review with text; a rating alone is not analyzed.
        EXPECT_EQ(pipeline.size(), 3u);

        EXPECT_NE(run(tested(spa_)).status, "cancelled");
        for (const auto& asked : pipeline) {
            std::size_t in_the_test = 0;
            for (const auto& request : llm_.requests) {
                if (request.review_id != asked.review_id || request.task != asked.task) continue;
                ++in_the_test;
                EXPECT_EQ(sent(request), sent(asked));
                EXPECT_EQ(request.location_id, asked.location_id);
            }
            EXPECT_EQ(in_the_test, static_cast<std::size_t>(domain::kGoLiveRepeats)) << asked.task;
        }
        return pipeline;
    };

    const auto seeded = compare("as the pack is seeded");
    ASSERT_EQ(seeded.size(), 3u);
    EXPECT_EQ(seeded[0].task, "analyzer");
    EXPECT_TRUE(seeded[0].temperature);
    EXPECT_TRUE(seeded[1].temperature);

    exec("UPDATE ai_settings SET analyzer_send_temperature = false, generator_send_temperature = false");
    const auto cold = compare("no temperature is sent");
    ASSERT_EQ(cold.size(), 3u);
    EXPECT_FALSE(cold[0].temperature);
    EXPECT_FALSE(cold[1].temperature);

    exec(R"sql(UPDATE ai_settings SET analyzer_send_temperature = true, generator_send_temperature = true, analyzer_temperature = 0.4,
                      analyzer_provider = 'anthropic', analyzer_model = 'claude-haiku-5-5', generator_model = 'qwen-writer-2',
                      max_output_tokens = 3000,
                      request_extras = '{"anthropic": {"fallbacks": "default", "top_k": 3}, "dashscope": {"enable_thinking": false}}')sql");
    const auto other = compare("another provider and model, another limit, request extras");
    ASSERT_EQ(other.size(), 3u);
    EXPECT_EQ(other[0].provider, "anthropic");
    EXPECT_EQ(other[0].model, "claude-haiku-5-5");
    EXPECT_EQ(other[0].temperature, 0.4);
    EXPECT_EQ(other[0].max_tokens, 3000);
    EXPECT_EQ(other[0].extras, boost::json::parse(R"({"fallbacks": "default", "top_k": 3})").as_object());
    EXPECT_EQ(other[1].provider, "dashscope");
    EXPECT_EQ(other[1].model, "qwen-writer-2");
    EXPECT_EQ(other[1].extras, boost::json::parse(R"({"enable_thinking": false})").as_object());

    // The writer's temperature and the name replies use come from the location's voice.
    set_voice(spa_, R"({"temperature": 1.1, "sign_off": "- Mei", "reply_name": "Willow Moon Chelsea"})");
    const auto own_voice = compare("the location's own voice");
    ASSERT_EQ(own_voice.size(), 3u);
    EXPECT_EQ(own_voice[1].temperature, 1.1);
    EXPECT_THAT(own_voice[1].messages.front().content, HasSubstr("Willow Moon Chelsea"));
    EXPECT_THAT(own_voice[1].messages.front().content, HasSubstr("- Mei"));
    EXPECT_NE(sent(own_voice[1]), sent(seeded[1]));
}

// ---- starting -----------------------------------------------------------------------------

TEST_F(GoLive, ATestStartsOnlyWhenAPersonSubmitsIt) {
    // A change of configuration starts nothing.
    set_voice(spa_, R"({"style_notes": ["Keep it very short."]})");
    exec("UPDATE ai_settings SET generator_model = 'qwen3.8-flash'");
    run_jobs();
    EXPECT_EQ(count("go_live_runs"), 0);
    EXPECT_EQ(count(std::format("jobs WHERE kind = '{}'", repo::kGoLiveCaseKind)), 0);
    EXPECT_TRUE(llm_.requests.empty());
}

TEST_F(GoLive, StartingIsRefusedWhenItCannotRun) {
    const auto no_judge = start(spa_, {"dashscope"});
    ASSERT_FALSE(no_judge);
    EXPECT_EQ(no_judge.error().kind, repo::Failure::Kind::invalid);
    EXPECT_THAT(no_judge.error().message, HasSubstr("judge's model provider 'anthropic' is not configured"));
    EXPECT_EQ(start(999999).error().kind, repo::Failure::Kind::not_found);

    exec(std::format("UPDATE locations SET industry_key = NULL WHERE id = {}", buffet_));
    EXPECT_THAT(start(buffet_).error().message, HasSubstr("no industry profile"));
    EXPECT_EQ(count("go_live_runs"), 0);
    EXPECT_EQ(count("audit_log WHERE action = 'go_live.start'"), 0);

    // While a test is running, a second submission starts nothing.
    const auto run_id = started(spa_);
    const auto twice = start(spa_);
    ASSERT_FALSE(twice);
    EXPECT_EQ(twice.error().kind, repo::Failure::Kind::conflict);
    EXPECT_EQ(twice.error().code, "go_live_running");
    EXPECT_EQ(count("go_live_runs"), 1);

    // A location that is switched off is not tested (D-102 point 2): a
    // submission is refused, and a running test stops before its next case.
    switch_location(spa_, false);
    const auto off = start(spa_);
    ASSERT_FALSE(off);
    EXPECT_EQ(off.error().kind, repo::Failure::Kind::invalid);
    EXPECT_THAT(off.error().message, HasSubstr("switched off"));
    run_jobs();
    EXPECT_EQ(run_value(run_id, "status"), "cancelled");
    EXPECT_THAT(run_value(run_id, "summary"), HasSubstr("switched off"));
    EXPECT_TRUE(llm_.requests.empty()) << "no model was asked for a location that is off";
    EXPECT_EQ(count("go_live_runs"), 1) << "and nothing was started in its place";
}

TEST_F(GoLive, LocationsThatShareAConfigurationShareATestAndOthersGetTheirOwn) {
    gbp_.add_location("9003", "LU DREAM SPA - Midtown");
    discover();
    const auto midtown = link("9003", spa_client_);
    become_test_client(spa_client_, "wellness");  // the same business, with the same settings (D-102: a link copies none)
    EXPECT_EQ(fingerprint(midtown), fingerprint(spa_));
    EXPECT_NE(fingerprint(buffet_), fingerprint(spa_)) << "two businesses never share one: the business is named in the prompts";

    // One test per fingerprint at a time, whichever location it was submitted for.
    const auto shared = started(spa_);
    const auto meanwhile = start(midtown);
    ASSERT_FALSE(meanwhile);
    EXPECT_EQ(meanwhile.error().code, "go_live_running");
    run_jobs();
    EXPECT_EQ(run(shared).status, "passed");
    EXPECT_TRUE(open(spa_));
    EXPECT_TRUE(open(midtown)) << "a pass counts for every location with the fingerprint";

    // Midtown gets a temperature of its own: now it is a configuration of its own.
    set_voice(midtown, R"({"temperature": 0.3})");
    EXPECT_TRUE(open(spa_));
    EXPECT_FALSE(open(midtown));
    const auto calls = llm_.requests.size();
    const auto separate = run(tested(midtown));
    EXPECT_EQ(separate.status, "passed");
    EXPECT_EQ(separate.fingerprint, fingerprint(midtown));
    EXPECT_TRUE(open(midtown));
    bool cooler = false;
    for (std::size_t i = calls; i < llm_.requests.size(); ++i) {
        const auto& request = llm_.requests[i];
        EXPECT_EQ(request.location_id, midtown) << "the test runs as the location it was submitted for";
        if (request.task == "generator") {
            cooler = true;
            EXPECT_NEAR(request.temperature.value(), 0.3, 1e-6) << "the test runs with the location's own settings";
        }
    }
    EXPECT_TRUE(cooler);
}

TEST_F(GoLive, TheOverviewShowsEachLocationsStateAndWhatATestWouldTake) {
    review(kSpa, "own1", "FIVE", praise());
    const auto overview = [&] {
        pqxx::connection conn{url()};
        pqxx::work tx{conn};
        return repo::go_live_overview(tx, {}, 2);
    };
    auto before = overview();
    EXPECT_EQ(before.repeats, 3);
    EXPECT_EQ(before.judge_model, "claude-opus-5-5");
    ASSERT_EQ(before.locations.size(), 2u);
    const auto& spa = before.locations[1];  // by title: Gangnam, LU DREAM SPA - Chelsea
    EXPECT_EQ(spa.location_id, spa_);
    EXPECT_EQ(spa.location_title, "LU DREAM SPA - Chelsea");
    EXPECT_EQ(spa.client_id, spa_client_);
    EXPECT_EQ(spa.client_name, "LU DREAM SPA");
    EXPECT_FALSE(spa.problem);
    EXPECT_EQ(spa.fingerprint, fingerprint(spa_));
    EXPECT_EQ(spa.state, "not_tested");
    EXPECT_FALSE(spa.pass);
    ASSERT_TRUE(spa.estimate);
    const auto& estimate = *spa.estimate;
    EXPECT_EQ(estimate.real_reviews, 1);
    EXPECT_EQ(estimate.cases, static_cast<std::int64_t>(corpus_cases("wellness")) + 1);
    EXPECT_EQ(estimate.model_calls, estimate.cases * 3 * 2) << "an analyzer and a writer call per repeat; every case here has text";
    EXPECT_FALSE(estimate.cost_usd) << "no model call has been made yet, so there is nothing to measure";
    EXPECT_FALSE(estimate.notes.empty());
    EXPECT_THAT(before.locations[0].estimate.value().notes, testing::Contains(HasSubstr("no real reviews will be tested")));

    // With calls on record, the estimate has a duration and a cost.
    exec("INSERT INTO llm_calls (occurred_at, task, model, attempt, status, prompt_tokens, completion_tokens, latency_ms, provider) VALUES "
         "(now(), 'analyzer', 'qwen3.7-plus', 1, 'ok', 1000, 100, 2000, 'dashscope'), "
         "(now(), 'generator', 'qwen3.7-plus', 1, 'ok', 1500, 80, 3000, 'dashscope'), "
         "(now(), 'judge', 'claude-opus-5-5', 1, 'ok', 2000, 600, 20000, 'anthropic')");
    const auto measured = overview().locations[1].estimate.value();
    ASSERT_TRUE(measured.seconds);
    ASSERT_TRUE(measured.cost_usd);
    EXPECT_GT(*measured.seconds, 60);
    EXPECT_GT(*measured.cost_usd, 0.0);

    const auto run_id = tested(spa_);
    const auto after = overview().locations[1];
    EXPECT_EQ(after.state, "passed");
    ASSERT_TRUE(after.pass);
    EXPECT_EQ(after.pass->id, run_id);
    EXPECT_TRUE(after.pass->corpus_current);

    // A corpus that has grown since does not take the pass away; the page says which corpus it was.
    exec(std::format("UPDATE go_live_runs SET corpus_version = 'an-older-one' WHERE id = {}", run_id));
    const auto older = overview().locations[1];
    EXPECT_EQ(older.state, "passed");
    EXPECT_FALSE(older.pass->corpus_current);
    EXPECT_TRUE(open(spa_));

    // A location that can't be tested says why; one that is switched off is not listed.
    exec(std::format("UPDATE locations SET industry_key = NULL WHERE id = {}", buffet_));
    const auto untestable = overview().locations[0];
    EXPECT_EQ(untestable.location_id, buffet_);
    EXPECT_THAT(untestable.problem.value_or(""), HasSubstr("no industry profile"));
    EXPECT_FALSE(untestable.fingerprint);
    EXPECT_FALSE(untestable.estimate);
    EXPECT_EQ(untestable.state, "not_tested");
    switch_location(buffet_, false);
    ASSERT_EQ(overview().locations.size(), 1u);
    EXPECT_EQ(overview().locations[0].location_id, spa_);
}

// ---- a new version of the knowledge base (D-078 point 7, D-084) ----------------------------

// The seed is poni-tools' business. The engine keeps nothing of the knowledge
// base in memory, so a new version is in effect with the next read. One that
// changes what the analyzer is told about an industry changes the
// fingerprint of that industry's locations, and the gate does the rest: no
// watcher, no notification. `poni-tools seed` names those locations to the
// person who seeds.
TEST_F(GoLive, ANewKnowledgeVersionThatChangesAProfileStopsAutoReplyUntilATestPasses) {
    switch_on(spa_);
    switch_on(buffet_);
    const auto spa_run = tested(spa_);
    tested(buffet_);
    EXPECT_EQ(run_value(spa_run, "kb_version"), value<std::string>("SELECT version FROM kb_meta"))
        << "a run records the knowledge base it ran against";
    ASSERT_TRUE(open(spa_));
    ASSERT_TRUE(open(buffet_));
    EXPECT_EQ(draft(drafted("r1"), "status"), "scheduled");

    // A version with new corpus cases, other prices, another template and a reworded topic label: nothing the
    // analyzer is told changed, so no fingerprint did.
    reseed("UPDATE kb_model_prices SET input_per_mtok = 9; "
           "UPDATE kb_industries SET corpus_version = 'grown', topics = jsonb_set(topics, '{0,label}', '\"Reworded for the dashboard\"'); "
           "DELETE FROM kb_voice_templates WHERE key = 'family_owned'; "
           "UPDATE kb_defaults SET voice = voice || '{\"sign_off\": \"The team\"}'::jsonb");
    EXPECT_TRUE(open(spa_));
    EXPECT_FALSE(run(spa_run).corpus_current) << "the page says the pass was made against an older corpus";

    // A version that rewords one of the wellness industry's severity flags.
    reseed("UPDATE kb_industries SET severity_flags = jsonb_set(severity_flags, '{0,description}', '\"A reworded definition.\"') "
           "WHERE key = 'wellness'");
    EXPECT_FALSE(open(spa_)) << "in effect at once: the gate reads the tables";
    EXPECT_THAT(gate(spa_).reason, HasSubstr("has not passed a go-live test"));
    EXPECT_TRUE(open(buffet_)) << "another industry's locations are not touched";
    EXPECT_EQ(draft(drafted("r2"), "status"), "draft") << "the reply waits for a person meanwhile";
    EXPECT_EQ(count("notification_deliveries WHERE event_type = 'system_problem'"), 0) << "the seed tells the person who seeds; the engine tells no one";

    // Nothing was switched off and nothing paused: only a passing test was taken away, and only one brings it back.
    EXPECT_EQ(value<bool>(std::format("SELECT auto_reply_enabled AND auto_reply_paused_at IS NULL FROM locations WHERE id = {}", spa_)), true);
    tested(spa_);
    EXPECT_TRUE(open(spa_));
    EXPECT_EQ(draft(drafted("r3"), "status"), "scheduled");
}

TEST_F(GoLive, ALocationWhoseIndustryLeftTheKnowledgeBaseHasNoConfigurationToTest) {
    switch_on(spa_);
    tested(spa_);
    ASSERT_TRUE(open(spa_));
    reseed("DELETE FROM kb_industries WHERE key = 'wellness'");
    EXPECT_FALSE(gate(spa_).passed);
    EXPECT_TRUE(gate(spa_).fingerprint.empty());
}
