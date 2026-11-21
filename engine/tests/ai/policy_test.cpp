// The tiered policy (FR-5): reply mode, target language and the gate.

#include "ai_fixture.hpp"
#include "domain/reply_policy.hpp"
#include "domain/reply_settings.hpp"
#include "domain/review_pipeline.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/value_from.hpp>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <functional>

using namespace poni;
using namespace poni::domain;
using testing::ElementsAre;
using testing::IsEmpty;

namespace {

const PolicyRules kDefault{};

Localization localization(std::vector<std::string> languages, std::string script) {
    return {.reply_languages = std::move(languages),
            .chinese_script = std::move(script),
            .chinese_region = "taiwan",
            .english_variant = "us",
            .fallback_language = "en"};
}

}  // namespace

TEST(ReplyMode, FollowsStarsTextComplaintAndSeverity) {
    EXPECT_EQ(reply_mode(5, true, false, false), "positive");
    EXPECT_EQ(reply_mode(4, true, false, false), "positive");
    EXPECT_EQ(reply_mode(5, true, false, true), "complaint");
    EXPECT_EQ(reply_mode(3, true, false, false), "complaint");
    EXPECT_EQ(reply_mode(1, true, false, true), "complaint");
    EXPECT_EQ(reply_mode(5, false, false, false), "rating_only");
    EXPECT_EQ(reply_mode(4, false, false, false), "rating_only");
    EXPECT_EQ(reply_mode(3, false, false, false), "rating_only_low");
    EXPECT_EQ(reply_mode(1, false, false, false), "rating_only_low");
    // A severity flag decides the mode whatever the stars.
    EXPECT_EQ(reply_mode(5, true, true, false), "severity");
    EXPECT_EQ(reply_mode(1, true, true, true), "severity");
}

TEST(TargetLanguage, RepliesInTheReviewsLanguageWhenTheBusinessDoes) {
    const auto spa = localization({"en", "zh"}, "match");
    EXPECT_EQ(target_language("en", "Latn", spa).language, "en");
    EXPECT_EQ(target_language("zh", "Hans", spa).language, "zh");
    // Spanish and Korean reviews get the fallback language.
    EXPECT_EQ(target_language("es", "Latn", spa).language, "en");
    EXPECT_EQ(target_language("ko", "Hang", spa).language, "en");
    EXPECT_EQ(target_language("none", "none", spa).language, "en");
    EXPECT_FALSE(target_language("en", "Latn", spa).script);

    const auto buffet = localization({"en", "zh", "es"}, "simplified");
    EXPECT_EQ(target_language("es", "Latn", buffet).language, "es");
}

TEST(TargetLanguage, ChineseScriptFollowsThePreference) {
    const auto match = localization({"en", "zh"}, "match");
    EXPECT_EQ(target_language("zh", "Hans", match).script, "Hans");
    EXPECT_EQ(target_language("zh", "Hant", match).script, "Hant");
    // A review whose script can't be told gets Simplified.
    EXPECT_EQ(target_language("zh", "none", match).script, "Hans");
    EXPECT_EQ(target_language("zh", "Hant", localization({"zh"}, "simplified")).script, "Hans");
    EXPECT_EQ(target_language("zh", "Hans", localization({"zh"}, "traditional")).script, "Hant");
    // Chinese as the fallback for another language still gets a script.
    auto fallback = localization({"zh"}, "traditional");
    fallback.fallback_language = "zh";
    EXPECT_EQ(target_language("ko", "Hang", fallback).language, "zh");
    EXPECT_EQ(target_language("ko", "Hang", fallback).script, "Hant");
}

TEST(Gate, PraiseWithCleanChecksWouldAutoPost) {
    const auto five = gate({.stars = 5, .has_text = true}, kDefault);
    EXPECT_EQ(five.route, "auto_post");
    EXPECT_THAT(five.reasons, IsEmpty());
    EXPECT_EQ(gate({.stars = 4, .has_text = true}, kDefault).route, "auto_post");
    EXPECT_EQ(gate({.stars = 5, .has_text = false}, kDefault).route, "auto_post");
}

TEST(Gate, LowStarsAndComplaintsWaitForAPerson) {
    for (const int stars : {1, 2, 3}) {
        const auto decision = gate({.stars = stars, .has_text = true}, kDefault);
        EXPECT_EQ(decision.route, "approval_queue") << stars;
        EXPECT_THAT(decision.reasons, ElementsAre(std::to_string(stars) + " stars"));
    }
    const auto complaint = gate({.stars = 5, .has_text = true, .contains_complaint = true}, kDefault);
    EXPECT_EQ(complaint.route, "approval_queue");
    EXPECT_THAT(complaint.reasons, ElementsAre("contains a complaint"));
    EXPECT_EQ(gate({.stars = 2, .has_text = false}, kDefault).route, "approval_queue");
    EXPECT_THAT(gate({.stars = 0, .has_text = true}, kDefault).reasons, ElementsAre("no star rating"));
}

TEST(Gate, ASeverityFlagAlwaysEscalates) {
    const auto decision = gate({.stars = 5, .has_text = true, .severity_flags = {"injury", "hygiene", "injury"}}, kDefault);
    EXPECT_EQ(decision.route, "escalate");
    EXPECT_THAT(decision.reasons, ElementsAre("severity: hygiene, injury"));
    // Nothing else changes that: not a failed check, not a failed model call.
    EXPECT_EQ(gate({.stars = 1, .has_text = true, .analysis_ok = false, .generation_ok = false, .severity_flags = {"injury"},
                    .manipulation_suspected = true, .failed_checks = {"no_url"}},
                   kDefault)
                  .route,
              "escalate");
}

TEST(Gate, AnyDoubtKeepsAReviewFromAutoPost) {
    const auto manipulation = gate({.stars = 5, .has_text = true, .manipulation_suspected = true}, kDefault);
    EXPECT_EQ(manipulation.route, "approval_queue");
    EXPECT_THAT(manipulation.reasons, ElementsAre("manipulation suspected"));

    const auto checks = gate({.stars = 5, .has_text = true, .failed_checks = {"no_url", "no_phone"}}, kDefault);
    EXPECT_EQ(checks.route, "approval_queue");
    EXPECT_THAT(checks.reasons, ElementsAre("check failed: no_phone, no_url"));

    EXPECT_THAT(gate({.stars = 5, .has_text = true, .analysis_ok = false}, kDefault).reasons, ElementsAre("analysis failed"));
    EXPECT_THAT(gate({.stars = 5, .has_text = true, .generation_ok = false}, kDefault).reasons, ElementsAre("generation failed"));

    // Every reason is listed, so the person reviewing sees all of them.
    const auto all = gate({.stars = 2, .has_text = true, .analysis_ok = false, .generation_ok = false, .contains_complaint = true,
                           .manipulation_suspected = true, .failed_checks = {"length"}},
                          kDefault);
    EXPECT_THAT(all.reasons, ElementsAre("analysis failed", "generation failed", "manipulation suspected", "check failed: length", "2 stars"));
}

TEST(Gate, ThePolicyCanTightenOrLoosenAutoPost) {
    EXPECT_EQ(gate({.stars = 4, .has_text = true}, {.auto_post_min_stars = 5}).route, "approval_queue");
    EXPECT_EQ(gate({.stars = 5, .has_text = true}, {.auto_post_min_stars = 5}).route, "auto_post");
    EXPECT_EQ(gate({.stars = 3, .has_text = true}, {.auto_post_min_stars = 3}).route, "auto_post");
    const auto no_positive = gate({.stars = 5, .has_text = true}, {.auto_post_positive = false});
    EXPECT_EQ(no_positive.route, "approval_queue");
    EXPECT_THAT(no_positive.reasons, ElementsAre("auto-post is off for positive reviews"));
    EXPECT_EQ(gate({.stars = 5, .has_text = false}, {.auto_post_positive = false}).route, "auto_post");
    const auto no_rating_only = gate({.stars = 5, .has_text = false}, {.auto_post_rating_only = false});
    EXPECT_EQ(no_rating_only.route, "approval_queue");
    EXPECT_THAT(no_rating_only.reasons, ElementsAre("auto-post is off for rating-only reviews"));
}

// ---- reply settings (D-093) -------------------------------------------------------

TEST(Settings, ATestClientIsTheDefaultsItsTemplateAndWhatItSets) {
    const test::KnowledgeFiles seed;
    const auto spa = seed.client("wellness");
    // Taking the template left its label and description as the writer's base style (D-080).
    EXPECT_THAT(spa.settings.voice.base_style, testing::StartsWith("Warm and casual. Friendly and upbeat"));
    EXPECT_EQ(spa.settings.voice.reply_name, "Willow Moon Head Spa");
    // From the template: the business's own notes replaced the template's, as a list does.
    EXPECT_EQ(spa.settings.voice.style_notes.size(), 5u);
    EXPECT_TRUE(spa.settings.voice.emoji.allowed);
    EXPECT_EQ(spa.settings.voice.emoji.max, 2);
    // The business's list replaces the template's.
    EXPECT_EQ(spa.settings.voice.emoji.preferred.size(), 4u);
    EXPECT_FALSE(spa.settings.voice.promos_allowed);
    EXPECT_EQ(spa.settings.voice.length.at("zh").positive.max, 160);
    EXPECT_THAT(spa.settings.localization.reply_languages, ElementsAre("en", "zh"));
    EXPECT_EQ(spa.settings.localization.chinese_script, "match");
    // The default: Traditional replies use Taiwan phrasing.
    EXPECT_EQ(spa.settings.localization.chinese_region, "taiwan");
    EXPECT_EQ(spa.settings.policy.auto_post_min_stars, 4);
    EXPECT_DOUBLE_EQ(spa.settings.voice.temperature, 0.7);

    const auto buffet = seed.client("food_beverage");
    EXPECT_THAT(buffet.settings.voice.base_style, testing::StartsWith("Polished hospitality. "));
    EXPECT_FALSE(buffet.settings.voice.emoji.allowed);
    EXPECT_THAT(buffet.settings.localization.reply_languages, ElementsAre("en", "zh", "es"));
    EXPECT_EQ(buffet.settings.localization.chinese_script, "simplified");
    EXPECT_EQ(buffet.settings.localization.fallback_language, "en");
}

// What is stored is what a location has: a part that lacks a field is an error, and no default fills it.
TEST(Settings, StoredSettingsAreReadAsTheyAreAndNothingIsFilledIn) {
    const test::KnowledgeFiles seed;
    const auto& defaults = seed.defaults();
    const auto whole = read_settings(defaults.at("voice"), defaults.at("localization"), defaults.at("policy"));
    ASSERT_TRUE(whole) << "the defaults are complete: " << whole.error();
    EXPECT_EQ(whole->voice.base_style, "") << "the defaults name no base style";
    EXPECT_DOUBLE_EQ(whole->voice.temperature, 0.7);

    auto voice = defaults.at("voice").as_object();
    voice.erase("sign_off");
    const auto without_sign_off = read_settings(voice, defaults.at("localization"), defaults.at("policy"));
    ASSERT_FALSE(without_sign_off);
    EXPECT_EQ(without_sign_off.error(), "voice: missing field 'sign_off'");

    auto localization = defaults.at("localization").as_object();
    localization.erase("english_variant");
    EXPECT_EQ(read_settings(defaults.at("voice"), localization, defaults.at("policy")).error(), "localization: missing field 'english_variant'");
    auto policy = defaults.at("policy").as_object();
    policy.erase("auto_post_min_stars");
    EXPECT_EQ(read_settings(defaults.at("voice"), defaults.at("localization"), policy).error(), "policy: missing field 'auto_post_min_stars'");
    EXPECT_FALSE(read_settings(boost::json::value(nullptr), defaults.at("localization"), defaults.at("policy")));

    // A member the code no longer knows is ignored; it is gone with the next save.
    voice = defaults.at("voice").as_object();
    voice["legacy_note"] = "kept";
    EXPECT_TRUE(read_settings(voice, defaults.at("localization"), defaults.at("policy")));
}

TEST(Settings, ASaveIsCheckedAgainstItsLimits) {
    const test::KnowledgeFiles seed;
    const auto defaults = seed.default_settings().value();
    EXPECT_EQ(check_settings(defaults), std::nullopt) << "the knowledge base's defaults pass the engine's own rules";
    for (const auto& key : seed.industries()) EXPECT_EQ(check_settings(seed.client(key).settings), std::nullopt) << key;

    // What is wrong with the defaults after `change`.
    const auto with = [&](const std::function<void(ResolvedSettings&)>& change) {
        auto settings = defaults;
        change(settings);
        return check_settings(settings).value_or("fine");
    };
    EXPECT_EQ(with([](auto& s) { s.voice.emoji.max = 50; }), "voice: 'emoji.max' must be between 0 and 10");
    EXPECT_EQ(with([](auto& s) { s.voice.temperature = 2.5; }), "voice: 'temperature' must be between 0 and 2");
    EXPECT_EQ(with([](auto& s) { s.voice.words_to_avoid = {" "}; }), "voice: 'words_to_avoid' has an empty entry");
    EXPECT_EQ(with([](auto& s) { s.voice.length["en"].positive = {.min = 500, .max = 100}; }),
              "voice: 'length.en.positive' needs 0 <= min <= max <= 4000");
    // A language the checks can't verify can't be configured.
    EXPECT_EQ(with([](auto& s) { s.voice.length["fr"] = s.voice.length["en"]; }),
              "voice: 'length' has limits for 'fr'; replies are written in en, zh or es");
    // Complete means every language replies are written in.
    EXPECT_EQ(with([](auto& s) { s.voice.length.erase("es"); }), "voice: 'length' needs limits for 'es'");

    EXPECT_EQ(with([](auto& s) { s.localization.chinese_script = "traditional"; }), "fine");
    EXPECT_EQ(with([](auto& s) { s.localization.chinese_script = "cursive"; }),
              "localization: 'chinese_script' must be match, simplified or traditional");
    EXPECT_EQ(with([](auto& s) { s.localization.chinese_region = "mars"; }),
              "localization: 'chinese_region' must be mainland, taiwan or hong_kong");
    EXPECT_EQ(with([](auto& s) { s.localization.english_variant = "uk"; }), "localization: 'english_variant' must be us or ca");
    EXPECT_EQ(with([](auto& s) { s.localization.reply_languages = {}; }), "localization: 'reply_languages' needs at least one language");
    EXPECT_EQ(with([](auto& s) { s.localization.reply_languages = {"en", "fr"}; }), "localization: 'reply_languages' may hold en, zh and es");
    EXPECT_EQ(with([](auto& s) { s.localization.fallback_language = "ko"; }), "localization: 'fallback_language' must be en, zh or es");

    EXPECT_EQ(with([](auto& s) { s.policy.auto_post_min_stars = 5; }), "fine");
    EXPECT_EQ(with([](auto& s) { s.policy.auto_post_min_stars = 0; }), "policy: 'auto_post_min_stars' must be between 1 and 5");
    // The delay and the window as pairs: the rule of domain::posting_timing, which a post asks again.
    EXPECT_EQ(with([](auto& s) {
                  s.policy.auto_post_delay_min_minutes = 30;
                  s.policy.auto_post_delay_max_minutes = 10;
              }),
              "policy: the delay's minimum is greater than its maximum");
    EXPECT_EQ(with([](auto& s) { s.policy.auto_post_window_start = "09:00"; }),
              "policy: the posting window has only one end");
    EXPECT_EQ(with([](auto& s) {
                  s.policy.auto_post_window_start = "09:00";
                  s.policy.auto_post_window_end = "09:00";
              }),
              "policy: the posting window is not a span of time");
    EXPECT_EQ(with([](auto& s) {
                  s.policy.auto_post_window_start = "09:00";
                  s.policy.auto_post_window_end = "21:30";
              }),
              "fine");
}
