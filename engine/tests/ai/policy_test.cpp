// The tiered policy (FR-5): reply mode, target language and the gate.

#include "ai_fixture.hpp"
#include "domain/reply_policy.hpp"
#include "domain/settings.hpp"
#include "domain/review_pipeline.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/value_from.hpp>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <functional>
#include <optional>
#include <string>

using namespace poni;
using namespace poni::domain;
using testing::ElementsAre;
using testing::IsEmpty;

namespace {

const ReplySettings kDefault{};

// Settings that reply in these languages, with this Chinese variant (none: the review's script).
ResolvedSettings replying_in(std::vector<std::string> languages, std::optional<std::string> chinese_variant) {
    ResolvedSettings settings;
    settings.reply.reply_languages = std::move(languages);
    settings.reply.fallback_language = "en";
    settings.content.chinese_variant = std::move(chinese_variant);
    return settings;
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
    const auto spa = replying_in({"en", "zh"}, std::nullopt);
    EXPECT_EQ(target_language("en", "Latn", spa).language, "en");
    EXPECT_EQ(target_language("zh", "Hans", spa).language, "zh");
    // Spanish and Korean reviews get the fallback language.
    EXPECT_EQ(target_language("es", "Latn", spa).language, "en");
    EXPECT_EQ(target_language("ko", "Hang", spa).language, "en");
    EXPECT_EQ(target_language("none", "none", spa).language, "en");
    EXPECT_FALSE(target_language("en", "Latn", spa).script);

    const auto buffet = replying_in({"en", "zh", "es"}, "zh-CN");
    EXPECT_EQ(target_language("es", "Latn", buffet).language, "es");
}

TEST(TargetLanguage, ChineseScriptFollowsTheVariant) {
    const auto match = replying_in({"en", "zh"}, std::nullopt);
    EXPECT_EQ(target_language("zh", "Hans", match).script, "Hans");
    EXPECT_EQ(target_language("zh", "Hant", match).script, "Hant");
    // A review whose script can't be told gets Simplified.
    EXPECT_EQ(target_language("zh", "none", match).script, "Hans");
    EXPECT_EQ(target_language("zh", "Hant", replying_in({"zh"}, "zh-CN")).script, "Hans");
    EXPECT_EQ(target_language("zh", "Hans", replying_in({"zh"}, "zh-TW")).script, "Hant");
    EXPECT_EQ(target_language("zh", "Hans", replying_in({"zh"}, "zh-HK")).script, "Hant");
    // Chinese as the fallback for another language still gets a script.
    auto fallback = replying_in({"zh"}, "zh-TW");
    fallback.reply.fallback_language = "zh";
    EXPECT_EQ(target_language("ko", "Hang", fallback).language, "zh");
    EXPECT_EQ(target_language("ko", "Hang", fallback).script, "Hant");
}

TEST(Gate, PraiseWithCleanChecksWouldAutoReply) {
    const auto five = gate({.stars = 5, .has_text = true}, kDefault);
    EXPECT_EQ(five.route, "auto_reply");
    EXPECT_THAT(five.reasons, IsEmpty());
    // Five stars only, unless a person chooses four (D-110).
    EXPECT_THAT(gate({.stars = 4, .has_text = true}, kDefault).reasons, ElementsAre("4 stars"));
    EXPECT_EQ(gate({.stars = 4, .has_text = true}, {.auto_reply_min_stars = 4}).route, "auto_reply");
}

// Auto reply is for reviews with text (D-110): a rating alone waits for a
// person, with a plain reason, at any number of stars and any minimum.
TEST(Gate, AReviewWithoutTextWaitsForAPerson) {
    for (const std::int64_t min_stars : {4, 5}) {
        const auto five = gate({.stars = 5, .has_text = false}, {.auto_reply_min_stars = min_stars});
        EXPECT_EQ(five.route, "approval_queue") << min_stars;
        EXPECT_THAT(five.reasons, ElementsAre("auto reply is for reviews with text")) << min_stars;
    }
    const auto four = gate({.stars = 4, .has_text = false}, {.auto_reply_min_stars = 4});
    EXPECT_EQ(four.route, "approval_queue");
    EXPECT_THAT(four.reasons, ElementsAre("auto reply is for reviews with text"));
    EXPECT_THAT(gate({.stars = 4, .has_text = false}, kDefault).reasons, ElementsAre("4 stars")) << "the first reason that applies";
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

TEST(Gate, AnyDoubtKeepsAReviewFromAutoReply) {
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

TEST(Gate, ThePolicyCanTightenOrLoosenAutoReply) {
    EXPECT_EQ(gate({.stars = 4, .has_text = true}, {.auto_reply_min_stars = 5}).route, "approval_queue");
    EXPECT_EQ(gate({.stars = 5, .has_text = true}, {.auto_reply_min_stars = 5}).route, "auto_reply");
    EXPECT_EQ(gate({.stars = 4, .has_text = true}, {.auto_reply_min_stars = 4}).route, "auto_reply");
    EXPECT_EQ(gate({.stars = 5, .has_text = true}, {.auto_reply_min_stars = 4}).route, "auto_reply");
}

// The star check auto_verdict runs at scheduling and at the claim. Its own 4
// holds even under a minimum no save and no column allows (D-110).
TEST(Gate, ThreeStarsOrFewerNeverAutoReplyWhateverTheMinimum) {
    for (const std::int64_t min_stars : {1, 2, 3, 4}) {
        for (const std::int64_t stars : {0, 1, 2, 3}) EXPECT_FALSE(stars_may_auto_reply(stars, min_stars)) << stars << " of " << min_stars;
        EXPECT_TRUE(stars_may_auto_reply(4, min_stars)) << min_stars;
        EXPECT_TRUE(stars_may_auto_reply(5, min_stars)) << min_stars;
    }
    EXPECT_FALSE(stars_may_auto_reply(4, 5));
    EXPECT_TRUE(stars_may_auto_reply(5, 5));
}

// ---- content and reply settings (D-107) -------------------------------------------

TEST(Settings, ATestClientIsTheDefaultsItsTemplateAndWhatItSets) {
    const test::KnowledgeFiles seed;
    const auto spa = seed.client("wellness");
    // Taking the template left its label and description as the writer's base style (D-080).
    EXPECT_THAT(spa.settings.content.base_style, testing::StartsWith("Warm and casual. Friendly and upbeat"));
    // The business is named by its title (D-107).
    EXPECT_EQ(spa.business_name, "Willow Moon Head Spa");
    // From the template: the business's own notes replaced the template's, as a list does.
    EXPECT_EQ(spa.settings.content.style_notes.size(), 5u);
    EXPECT_TRUE(spa.settings.content.emoji_allowed);
    EXPECT_EQ(spa.settings.content.emoji_max, 2);
    // The business's list replaces the template's.
    EXPECT_EQ(spa.settings.content.emoji_preferred.size(), 4u);
    EXPECT_THAT(spa.settings.content.business_facts, testing::StartsWith("- A head spa and wellness studio"));
    EXPECT_FALSE(spa.settings.reply.promos_allowed);
    EXPECT_EQ(spa.settings.reply.length.at("zh").positive.max, 160);
    EXPECT_THAT(spa.settings.reply.reply_languages, ElementsAre("en", "zh"));
    // Not set: a Chinese reply follows the review's script.
    EXPECT_EQ(spa.settings.content.chinese_variant, std::nullopt);
    EXPECT_EQ(spa.settings.content.english_variant, std::nullopt);
    EXPECT_EQ(spa.settings.reply.auto_reply_min_stars, 5);
    EXPECT_FALSE(spa.settings.reply.auto_reply_enabled) << "no file switches auto reply on";
    EXPECT_DOUBLE_EQ(spa.settings.reply.temperature, 0.7);

    const auto buffet = seed.client("food_beverage");
    EXPECT_THAT(buffet.settings.content.base_style, testing::StartsWith("Polished hospitality. "));
    EXPECT_FALSE(buffet.settings.content.emoji_allowed);
    EXPECT_THAT(buffet.settings.reply.reply_languages, ElementsAre("en", "zh", "es"));
    EXPECT_EQ(buffet.settings.content.chinese_variant, "zh-CN");
    EXPECT_EQ(buffet.settings.reply.fallback_language, "en");
}

// What is stored is what a location has: a bundle that lacks a member is an error, and no default fills it.
TEST(Settings, StoredSettingsAreReadAsTheyAreAndNothingIsFilledIn) {
    const test::KnowledgeFiles seed;
    const auto& defaults = seed.defaults();
    const auto whole = test::read_bundles(defaults.at("content"), defaults.at("reply"));
    ASSERT_TRUE(whole) << "the defaults are complete: " << whole.error();
    EXPECT_EQ(whole->content.base_style, "") << "the defaults name no base style";
    EXPECT_EQ(whole->content.business_facts, "");
    EXPECT_DOUBLE_EQ(whole->reply.temperature, 0.7);

    auto reply = defaults.at("reply").as_object();
    reply.erase("signature");
    const auto without_signature = test::read_bundles(defaults.at("content"), reply);
    ASSERT_FALSE(without_signature);
    EXPECT_EQ(without_signature.error(), "missing field 'reply.signature'");

    auto content = defaults.at("content").as_object();
    content.erase("audience");
    EXPECT_EQ(test::read_bundles(content, defaults.at("reply")).error(), "missing field 'content.audience'");
    reply = defaults.at("reply").as_object();
    reply.erase("auto_reply_min_stars");
    EXPECT_EQ(test::read_bundles(defaults.at("content"), reply).error(), "missing field 'reply.auto_reply_min_stars'");
    EXPECT_FALSE(test::read_bundles(boost::json::value(nullptr), defaults.at("reply")));

    // A member the code no longer knows is ignored; it is gone with the next save.
    content = defaults.at("content").as_object();
    content["legacy_note"] = "kept";
    EXPECT_TRUE(test::read_bundles(content, defaults.at("reply")));
}

TEST(Settings, ASaveIsCheckedAgainstItsLimits) {
    const test::KnowledgeFiles seed;
    const auto defaults = seed.default_settings().value();
    EXPECT_EQ(check_content_settings(defaults.content), std::nullopt) << "the knowledge base's defaults pass the engine's own rules";
    EXPECT_EQ(check_reply_settings(defaults.reply), std::nullopt) << "the knowledge base's defaults pass the engine's own rules";
    for (const auto& key : seed.industries()) {
        EXPECT_EQ(check_content_settings(seed.client(key).settings.content), std::nullopt) << key;
        EXPECT_EQ(check_reply_settings(seed.client(key).settings.reply), std::nullopt) << key;
    }

    // What is wrong with the defaults' content settings after `change`.
    const auto content = [&](const std::function<void(ContentSettings&)>& change) {
        auto settings = defaults.content;
        change(settings);
        return check_content_settings(settings).value_or("fine");
    };
    // And with their reply settings.
    const auto reply = [&](const std::function<void(ReplySettings&)>& change) {
        auto settings = defaults.reply;
        change(settings);
        return check_reply_settings(settings).value_or("fine");
    };
    const std::string long_text(2001, 'a');
    const std::string line_201(201, 'a');
    // A code point is one character, however many bytes it takes.
    std::string chinese_2000;
    for (int i = 0; i < 2000; ++i) chinese_2000 += "字";

    EXPECT_EQ(content([&](auto& s) { s.business_facts = long_text; }), "'business_facts' may be up to 2000 characters");
    EXPECT_EQ(content([&](auto& s) { s.business_facts = chinese_2000; }), "fine");
    EXPECT_EQ(content([&](auto& s) { s.base_style = long_text; }), "'base_style' may be up to 2000 characters");
    EXPECT_EQ(content([](auto& s) { s.emoji_max = 50; }), "'emoji_max' must be between 0 and 10");
    EXPECT_EQ(content([](auto& s) { s.emoji_max = -1; }), "'emoji_max' must be between 0 and 10");
    EXPECT_EQ(content([&](auto& s) { s.audience = std::string(501, 'a'); }), "'audience' may be up to 500 characters");
    EXPECT_EQ(content([&](auto& s) { s.audience = std::string(500, 'a'); }), "fine");
    for (const auto* variant : {"zh-CN", "zh-HK", "zh-TW"}) {
        EXPECT_EQ(content([&](auto& s) { s.chinese_variant = variant; }), "fine") << variant;
    }
    EXPECT_EQ(content([](auto& s) { s.chinese_variant = std::nullopt; }), "fine");
    EXPECT_EQ(content([](auto& s) { s.chinese_variant = "zh-SG"; }), "'chinese_variant' must be zh-CN, zh-HK, zh-TW or null");
    for (const auto* variant : {"en-US", "en-CA"}) {
        EXPECT_EQ(content([&](auto& s) { s.english_variant = variant; }), "fine") << variant;
    }
    EXPECT_EQ(content([](auto& s) { s.english_variant = std::nullopt; }), "fine");
    EXPECT_EQ(content([](auto& s) { s.english_variant = "en-GB"; }), "'english_variant' must be en-US, en-CA or null");

    // Every list: at most 50 entries, none empty, none longer than 2000.
    using ContentList = std::vector<std::string> ContentSettings::*;
    for (const auto& [name, list] : {std::pair<std::string, ContentList>{"emoji_preferred", &ContentSettings::emoji_preferred},
                                     {"words_to_avoid", &ContentSettings::words_to_avoid},
                                     {"style_notes", &ContentSettings::style_notes}}) {
        EXPECT_EQ(content([&](auto& s) { s.*list = std::vector<std::string>(51, "x"); }), "'" + name + "' may hold at most 50 entries");
        EXPECT_EQ(content([&](auto& s) { s.*list = std::vector<std::string>(50, "x"); }), "fine") << name;
        EXPECT_EQ(content([&](auto& s) { s.*list = {"x", " "}; }), "'" + name + "' has an empty entry");
        EXPECT_EQ(content([&](auto& s) { s.*list = {""}; }), "'" + name + "' has an empty entry");
        EXPECT_EQ(content([&](auto& s) { s.*list = {long_text}; }), "an entry of '" + name + "' is longer than 2000 characters");
    }
    EXPECT_EQ(reply([](auto& s) { s.example_replies = std::vector<std::string>(51, "x"); }), "'example_replies' may hold at most 50 entries");
    EXPECT_EQ(reply([](auto& s) { s.example_replies = {"\t"}; }), "'example_replies' has an empty entry");
    EXPECT_EQ(reply([&](auto& s) { s.example_replies = {long_text}; }), "an entry of 'example_replies' is longer than 2000 characters");

    EXPECT_EQ(reply([&](auto& s) { s.signature = line_201; }), "'signature' may be up to 200 characters");
    EXPECT_EQ(reply([](auto& s) { s.signature = "Warmly,\nthe team"; }), "'signature' is one line");
    EXPECT_EQ(reply([](auto& s) { s.signature = "Warmly,\rthe team"; }), "'signature' is one line");
    // A trailing space is kept as typed.
    EXPECT_EQ(reply([](auto& s) { s.signature = "Warmly, "; }), "fine");
    EXPECT_EQ(reply([&](auto& s) { s.sign_off_name = line_201; }), "'sign_off_name' may be up to 200 characters");
    EXPECT_EQ(reply([](auto& s) { s.sign_off_name = "Mina"; }), "fine");
    EXPECT_EQ(reply([](auto& s) { s.temperature = 2.5; }), "'temperature' must be between 0 and 2");
    EXPECT_EQ(reply([](auto& s) { s.length["en"].positive = {.min = 500, .max = 100}; }),
              "'length.en.positive' needs 0 <= min <= max <= 4000");
    // A language the checks can't verify can't be configured.
    EXPECT_EQ(reply([](auto& s) { s.length["fr"] = s.length["en"]; }), "'length' has limits for 'fr'; replies are written in en, zh or es");
    // Complete means every language replies are written in.
    EXPECT_EQ(reply([](auto& s) { s.length.erase("es"); }), "'length' needs limits for 'es'");
    EXPECT_EQ(reply([](auto& s) { s.reply_languages = {}; }), "'reply_languages' needs at least one language");
    EXPECT_EQ(reply([](auto& s) { s.reply_languages = {"en", "fr"}; }), "'reply_languages' may hold en, zh and es");
    EXPECT_EQ(reply([](auto& s) { s.fallback_language = "ko"; }), "'fallback_language' must be en, zh or es");

    EXPECT_EQ(reply([](auto& s) { s.auto_reply_min_stars = 5; }), "fine");
    EXPECT_EQ(reply([](auto& s) { s.auto_reply_min_stars = 4; }), "fine");
    for (const std::int64_t stars : {0, 1, 3, 6}) {
        EXPECT_EQ(reply([&](auto& s) { s.auto_reply_min_stars = stars; }), "'auto_reply_min_stars' must be 4 or 5") << stars;
    }
    EXPECT_EQ(reply([](auto& s) { s.auto_reply_daily_cap = 0; }), "'auto_reply_daily_cap' must be between 1 and 1000");
    EXPECT_EQ(reply([](auto& s) { s.auto_reply_daily_cap = 1001; }), "'auto_reply_daily_cap' must be between 1 and 1000");
    EXPECT_EQ(reply([](auto& s) { s.auto_reply_daily_cap = 1000; }), "fine");
    EXPECT_EQ(reply([](auto& s) { s.auto_reply_pause_after_failures = 0; }), "'auto_reply_pause_after_failures' must be between 1 and 20");
    EXPECT_EQ(reply([](auto& s) { s.auto_reply_pause_after_failures = 21; }), "'auto_reply_pause_after_failures' must be between 1 and 20");
    EXPECT_EQ(reply([](auto& s) { s.auto_reply_enabled = true; }), "fine") << "what the location must be is the save's to check";
    EXPECT_EQ(reply([](auto& s) { s.auto_reply_delay_max_minutes = 1441; }),
              "'auto_reply_delay_min_minutes' and 'auto_reply_delay_max_minutes' must be between 0 and 1440");
    EXPECT_EQ(reply([](auto& s) { s.auto_reply_window_start = "9am"; }),
              "'auto_reply_window_start' and 'auto_reply_window_end' must be a time like \"09:00\", or empty");
    // The delay and the window as pairs: the rule of domain::posting_timing, which a post asks again.
    EXPECT_EQ(reply([](auto& s) {
                  s.auto_reply_delay_min_minutes = 30;
                  s.auto_reply_delay_max_minutes = 10;
              }),
              "the delay's minimum is greater than its maximum");
    EXPECT_EQ(reply([](auto& s) { s.auto_reply_window_start = "09:00"; }), "the posting window has only one end");
    EXPECT_EQ(reply([](auto& s) {
                  s.auto_reply_window_start = "09:00";
                  s.auto_reply_window_end = "09:00";
              }),
              "the posting window is not a span of time");
    EXPECT_EQ(reply([](auto& s) {
                  s.auto_reply_window_start = "09:00";
                  s.auto_reply_window_end = "21:30";
              }),
              "fine");
}
