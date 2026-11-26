// The go-live test's rules without a database or a model (D-074): what is in
// a fingerprint and what is not, how wide a policy is, the corpus, and every
// pack read through the engine's structs.

#include "ai_fixture.hpp"
#include "domain/go_live.hpp"
#include "domain/settings.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <boost/json/serialize.hpp>
#include <boost/json/value_from.hpp>

#include <algorithm>
#include <set>

using namespace poni;
using namespace poni::domain;
using testing::HasSubstr;
using testing::Not;

namespace {

class GoLiveDomain : public ::testing::Test {
protected:
    test::KnowledgeFiles seed_;
    test::ClientSetup spa_ = seed_.client("wellness");

    ModelSetup model(std::optional<double> temperature) {
        return {.provider = "dashscope",
                .model = "qwen3.7-plus",
                .temperature = temperature,
                .max_tokens = 1200,
                .extras = {{"enable_thinking", false}}};
    }
    Fingerprint of(const test::ClientSetup& client, const ModelSetup& analyzer, const ModelSetup& writer) {
        return fingerprint({.business_name = client.business_name,
                            .industry = client.industry,
                            .settings = client.settings,
                            .analyzer = analyzer,
                            .writer = writer});
    }
    std::string hash(const test::ClientSetup& client) { return of(client, model(0.0), model(0.7)).hash; }
};

}  // namespace

TEST_F(GoLiveDomain, TheSameConfigurationHasTheSameFingerprint) {
    const auto first = of(spa_, model(0.0), model(0.7));
    const auto second = of(seed_.client("wellness"), model(0.0), model(0.7));
    EXPECT_EQ(first.hash, second.hash);
    EXPECT_EQ(first.hash.size(), 16u);
    EXPECT_EQ(first.inputs.at("gate").as_string(), kGateVersion);
    // One writer prompt per reply mode and language the business replies in.
    const auto& prompts = first.inputs.at("prompts").as_object();
    EXPECT_TRUE(prompts.contains("analyzer"));
    EXPECT_TRUE(prompts.contains("writer:positive:en"));
    EXPECT_TRUE(prompts.contains("writer:severity:en"));
    EXPECT_TRUE(prompts.contains("writer:rating_only:en"));
    // The prompts are in it as hashes: no business fact and no example reply is copied into a run's record.
    EXPECT_THAT(boost::json::serialize(first.inputs), Not(HasSubstr(spa_.business_name)));
}

TEST_F(GoLiveDomain, TwoClientsNeverShareAFingerprint) { EXPECT_NE(hash(spa_), hash(seed_.client("food_beverage"))); }

TEST_F(GoLiveDomain, EveryPartThatIsTestedChangesIt) {
    const auto base = hash(spa_);
    std::set<std::string> seen{base};
    const auto differs = [&](const std::string& other, std::string_view what) {
        EXPECT_NE(other, base) << what;
        EXPECT_TRUE(seen.insert(other).second) << what << ": two different changes gave one fingerprint";
    };
    const auto changed = [&](auto&& edit) {
        auto copy = spa_;
        edit(copy);
        return hash(copy);
    };
    differs(changed([](test::ClientSetup& c) { c.settings.content.style_notes.push_back("Keep it very short."); }), "a style note");
    differs(changed([](test::ClientSetup& c) { c.settings.content.business_facts += "\n- Open daily."; }), "a business fact");
    differs(changed([](test::ClientSetup& c) { c.business_name = "Willow Moon"; }), "the name in replies");
    differs(changed([](test::ClientSetup& c) { c.settings.content.base_style = "Brisk. Two sentences at most."; }), "the base style");
    differs(changed([](test::ClientSetup& c) { c.settings.reply.contact_allowed = true; }), "contact details allowed");
    differs(changed([](test::ClientSetup& c) { c.settings.content.emoji_max += 1; }), "the emoji limit");
    differs(changed([](test::ClientSetup& c) { c.settings.reply.length["en"].positive.max += 50; }), "a length limit");
    differs(changed([](test::ClientSetup& c) { c.settings.content.english_variant = "en-CA"; }), "the English variant");
    differs(changed([](test::ClientSetup& c) { c.settings.reply.reply_languages = {"en"}; }), "the reply languages");
    differs(changed([](test::ClientSetup& c) { c.industry.severity_flags.pop_back(); }), "a severity flag");

    auto other = model(0.7);
    other.model = "qwen3.7-plus-2026-05-26";
    differs(of(spa_, model(0.0), other).hash, "the model's name exactly as typed");
    other = model(0.7);
    other.provider = "openai";
    differs(of(spa_, model(0.0), other).hash, "the provider");
    differs(of(spa_, model(0.0), model(0.2)).hash, "the writer's temperature");
    differs(of(spa_, model(0.0), model(std::nullopt)).hash, "no temperature sent");
    differs(of(spa_, model(0.1), model(0.7)).hash, "the analyzer's temperature");
    other = model(0.7);
    other.max_tokens = 900;
    differs(of(spa_, model(0.0), other).hash, "the token limit");
    other = model(0.7);
    other.extras = {{"enable_thinking", true}};
    differs(of(spa_, model(0.0), other).hash, "the request extras");
}

TEST_F(GoLiveDomain, WhatIsNotTestedIsNotInIt) {
    const auto base = hash(spa_);
    auto copy = spa_;
    // The auto reply's timing and the fields no prompt and no check reads.
    copy.settings.reply.auto_reply_delay_min_minutes = 5;
    copy.settings.reply.auto_reply_delay_max_minutes = 45;
    copy.settings.reply.auto_reply_window_start = "09:00";
    copy.settings.reply.auto_reply_window_end = "21:00";
    copy.settings.reply.auto_reply_min_stars = 5;
    // Stored for the sign-off wording to come, and read by no prompt yet.
    copy.settings.reply.sign_off_name = "Mina";
    copy.name = "Willow Moon Head Spa LLC";  // the client's name: the business is named by its title
    EXPECT_EQ(hash(copy), base);
}

// What the reply writer reads of the content settings is in the fingerprint; the audience,
// which only the help with local posts reads, is not (D-107 point 7).
TEST_F(GoLiveDomain, TheWritersContentSettingsAreInItAndTheAudienceIsNot) {
    const auto base = of(spa_, model(0.0), model(0.7));
    const auto changed = [&](auto&& edit) {
        auto copy = spa_;
        edit(copy.settings.content);
        return of(copy, model(0.0), model(0.7));
    };
    const auto audience = changed([](ContentSettings& c) { c.audience = "Office workers on their lunch break"; });
    EXPECT_EQ(audience.inputs, base.inputs);
    EXPECT_EQ(audience.hash, base.hash);

    EXPECT_NE(changed([](ContentSettings& c) { c.business_facts = "- Open daily."; }).hash, base.hash) << "the business facts";
    EXPECT_NE(changed([](ContentSettings& c) { c.base_style = "Brisk."; }).hash, base.hash) << "the base style";
    EXPECT_NE(changed([](ContentSettings& c) { c.words_to_avoid.push_back("amazing"); }).hash, base.hash) << "the words to avoid";
    EXPECT_NE(changed([](ContentSettings& c) { c.emoji_allowed = false; }).hash, base.hash) << "emoji allowed";
    EXPECT_NE(changed([](ContentSettings& c) { c.emoji_preferred = {"🌸"}; }).hash, base.hash) << "the preferred emoji";

    // The variants: a change that renders other text. (Not set and en-US render the same US spelling.)
    const auto us = changed([](ContentSettings& c) { c.english_variant = "en-US"; });
    EXPECT_NE(changed([](ContentSettings& c) { c.english_variant = "en-CA"; }).hash, us.hash) << "en-US to en-CA";
    ASSERT_TRUE(std::ranges::contains(spa_.settings.reply.reply_languages, "zh"));
    ASSERT_EQ(spa_.settings.content.chinese_variant, std::nullopt);
    EXPECT_NE(changed([](ContentSettings& c) { c.chinese_variant = "zh-TW"; }).hash, base.hash) << "not set to zh-TW";
}

TEST_F(GoLiveDomain, AWiderPolicyIsNotCoveredAndANarrowerOneIs) {
    ReplySettings policy;
    policy.auto_reply_positive = true;
    policy.auto_reply_rating_only = false;
    policy.auto_reply_min_stars = 5;
    const auto tested = scope_of(policy);
    EXPECT_TRUE(tested.covers(tested));

    auto wider = policy;
    wider.auto_reply_min_stars = 4;
    EXPECT_FALSE(tested.covers(scope_of(wider))) << "5 to 4 stars";
    EXPECT_TRUE(scope_of(wider).covers(tested));
    wider = policy;
    wider.auto_reply_rating_only = true;
    EXPECT_FALSE(tested.covers(scope_of(wider)));
    auto narrower = policy;
    narrower.auto_reply_positive = false;
    EXPECT_TRUE(tested.covers(scope_of(narrower)));

    // Three stars and fewer never auto reply, so a policy that names them is no wider than four.
    policy.auto_reply_min_stars = 4;
    auto low = policy;
    low.auto_reply_min_stars = 1;
    EXPECT_TRUE(scope_of(policy).covers(scope_of(low)));
    EXPECT_EQ(scope_of(low).min_stars, 4);
}

TEST(Corpus, OnlyOrdinaryReviewsMayReachAutoReply) {
    EXPECT_FALSE(may_auto_reply("manipulation"));
    EXPECT_FALSE(may_auto_reply("severity"));
    EXPECT_TRUE(may_auto_reply(kCaseControl));
    EXPECT_TRUE(may_auto_reply(kCaseReal));
}

// ---- every pack, through the engine's own structs (D-078 point 9) ---------------------

// The schemas in knowledge/schema/ are what poni-tools checks the files
// against; these are the structs the engine reads the seeded tables with.
// Reading every file of every pack through them here means the two cannot
// drift apart unnoticed: a file the schemas accept and the engine cannot
// use fails this test, in CI, before it can be seeded anywhere.
TEST(KnowledgePacks, EveryPackReadsThroughTheEnginesStructs) {
    const test::KnowledgeFiles knowledge;
    ASSERT_GE(knowledge.industries().size(), 2u);
    std::set<std::string> case_ids;
    for (const auto& key : knowledge.industries()) {
        SCOPED_TRACE(key);
        const auto rules = knowledge.industry(key);
        EXPECT_EQ(rules.key, key);
        EXPECT_FALSE(rules.topics.empty());
        EXPECT_FALSE(rules.severity_flags.empty());

        // The made-up business has complete settings and a fingerprint, like any location.
        const auto client = knowledge.client(key);
        EXPECT_FALSE(client.business_name.empty());
        const ModelSetup model{.provider = "p", .model = "m", .temperature = 0.0, .max_tokens = 1200, .extras = {}};
        EXPECT_EQ(fingerprint({.business_name = client.business_name, .industry = rules, .settings = client.settings, .analyzer = model, .writer = model})
                      .hash.size(),
                  16u);
    }
    for (const auto& one : knowledge.cases()) {
        EXPECT_TRUE(case_ids.insert(one.id).second) << one.id << " appears twice";
        EXPECT_TRUE(one.category == "manipulation" || one.category == "severity" || one.category == kCaseControl) << one.id;
        EXPECT_TRUE(one.stars >= 1 && one.stars <= 5) << one.id;
        EXPECT_FALSE(one.text.empty()) << one.id;
        // A flag a case expects is one the analyzer can raise for that industry.
        const auto rules = knowledge.industry(one.industry);
        for (const auto& flag : one.severity_any) {
            EXPECT_NE(std::ranges::find(rules.severity_flags, flag, &SeverityFlag::id), rules.severity_flags.end()) << one.id << ": " << flag;
        }
    }
    EXPECT_GE(case_ids.size(), 71u);

    // The defaults are complete and pass the engine's rules, and so do they with any template taken:
    // a template holds no field the engine does not know.
    const auto defaults = knowledge.default_settings();
    ASSERT_TRUE(defaults) << "the defaults alone are complete settings: " << defaults.error();
    EXPECT_EQ(check_content_settings(defaults->content), std::nullopt);
    EXPECT_EQ(check_reply_settings(defaults->reply), std::nullopt);
    for (const auto* key : {"warm_casual", "polished_hospitality", "family_owned"}) {
        const std::optional taken = knowledge.voice_template(key);
        const boost::json::object nothing_set{{"content", boost::json::object{}}, {"reply", boost::json::object{}}};
        const auto settings = test::business_settings(knowledge.defaults().as_object(), taken, nothing_set);
        // Every member a template sets is a member of its bundle, with the value the template gives it.
        const auto content = boost::json::value_from(settings.content).as_object();
        for (const auto& member : taken->content.as_object()) {
            ASSERT_TRUE(content.contains(member.key())) << key << ": content." << member.key();
            EXPECT_EQ(content.at(member.key()), member.value()) << key << ": content." << member.key();
        }
        const auto reply = boost::json::value_from(settings.reply).as_object();
        for (const auto& member : taken->reply.as_object()) {
            ASSERT_TRUE(reply.contains(member.key())) << key << ": reply." << member.key();
            EXPECT_EQ(reply.at(member.key()), member.value()) << key << ": reply." << member.key();
        }
    }
    for (const auto& [name, price] : knowledge.models().at("prices").as_object()) {
        EXPECT_TRUE(price.at("input_per_mtok").is_number() && price.at("output_per_mtok").is_number()) << name;
    }
}
