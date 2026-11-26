// fixtures/reply_settings_golden.json was captured on 2026-11-20 with the
// engine as it was before reply settings became one complete row per
// location: made-up clients and locations, and for each location the
// settings it resolved to and the fingerprint of that. Its settings follow
// the domain types: they were converted once to the content and reply
// bundles of D-107, each case naming the location's title, which is how the
// business is named now, and its fingerprints were recaptured once for the
// wording of D-107 points 9 to 11. Its fingerprints are recaptured only by a change
// that alters a reply prompt on purpose, named in the decision record. It
// holds everything it needs (the industries, the models), so a later version
// of the knowledge base does not touch it.
//
// The test below: the captured settings still give the captured
// fingerprints, so no prompt, model request or check limit changed. That a
// code change which alters a prompt for unchanged settings fails a test
// rests on this file (D-091 point 10).
//
// fixtures/reply_language_signoff_golden.json holds one made-up spa for each
// branch of the writer's language text and of its sign-off line (D-107
// points 9 and 11), with the sentence the branch renders, captured with that
// wording; its fingerprints are recaptured under the same rule.

#include "ai_fixture.hpp"
#include "domain/go_live.hpp"
#include "domain/prompts.hpp"
#include "domain/settings.hpp"

#include <gtest/gtest.h>

#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>
#include <boost/json/value_from.hpp>
#include <boost/json/value_to.hpp>

#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace poni;

namespace {

const boost::json::value& row_with_id(const boost::json::value& golden, std::string_view table, const boost::json::value& id) {
    for (const auto& row : golden.at("rows").at(table).as_array()) {
        if (row.at("id") == id) return row;
    }
    throw std::runtime_error("the golden file has no such row in " + std::string(table));
}

domain::ModelSetup model_of(const boost::json::value& json, std::optional<double> temperature) {
    return {.provider = std::string(json.at("provider").as_string()),
            .model = std::string(json.at("model").as_string()),
            .temperature = temperature,
            .max_tokens = json.at("max_tokens").to_number<std::int64_t>(),
            .extras = json.at("extras").as_object()};
}

domain::IndustryRules industry_of(const boost::json::value& told) {
    return {.key = std::string(told.at("key").as_string()),
            .label = std::string(told.at("label").as_string()),
            .topics = boost::json::value_to<std::vector<domain::Topic>>(told.at("topics")),
            .item_types = boost::json::value_to<std::vector<domain::ItemType>>(told.at("item_types")),
            .severity_flags = boost::json::value_to<std::vector<domain::SeverityFlag>>(told.at("severity_flags"))};
}

// The fingerprint of a location with these settings, built as
// repo::go_live_setup builds it: the business is named by the location's
// title, and the writer is asked with the reply settings' temperature.
domain::Fingerprint fingerprint_of(const boost::json::value& golden, const domain::IndustryRules& industry, std::string_view title,
                                   const domain::ResolvedSettings& settings) {
    return domain::fingerprint({.business_name = std::string(title),
                                .industry = industry,
                                .settings = settings,
                                .analyzer = model_of(golden.at("analyzer"), golden.at("analyzer").at("temperature").to_number<double>()),
                                .writer = model_of(golden.at("writer"), settings.reply.temperature)});
}

}  // namespace

TEST(ReplySettingsGolden, TheCapturedSettingsGiveTheCapturedFingerprints) {
    const auto golden = test::load_json_fixture("reply_settings_golden.json");
    ASSERT_EQ(golden.at("cases").as_array().size(), 15u);
    for (const auto& captured : golden.at("cases").as_array()) {
        const auto name = std::string(captured.at("name").as_string());
        const auto& resolved = captured.at("resolved");
        const auto settings = test::read_bundles(resolved.at("content"), resolved.at("reply"));
        ASSERT_TRUE(settings) << name << ": " << settings.error();
        // Complete settings are read as they are: nothing is added, dropped or reordered.
        EXPECT_EQ(boost::json::serialize(boost::json::value_from(*settings)), boost::json::serialize(resolved)) << name;

        // When the file was captured, a location without a client had no configuration to test.
        const auto& location = row_with_id(golden, "locations", captured.at("location_id"));
        if (location.at("client_id").is_null()) {
            EXPECT_TRUE(captured.at("fingerprint").is_null()) << name;
            continue;
        }
        const auto& client = row_with_id(golden, "clients", location.at("client_id"));
        const auto now = fingerprint_of(golden, industry_of(golden.at("industries").at(client.at("industry_key").as_string())),
                                        captured.at("title").as_string(), *settings);
        EXPECT_EQ(now.inputs, captured.at("fingerprint_inputs").as_object()) << name;
        EXPECT_EQ(now.hash, captured.at("fingerprint").as_string()) << name;
    }
}

TEST(ReplySettingsGolden, EachLanguageAndSignOffBranchGivesItsCapturedFingerprint) {
    const auto golden = test::load_json_fixture("reply_language_signoff_golden.json");
    const auto industry = industry_of(golden.at("industry"));
    ASSERT_EQ(golden.at("cases").as_array().size(), 9u);
    for (const auto& captured : golden.at("cases").as_array()) {
        const auto name = std::string(captured.at("name").as_string());
        const auto& stored = captured.at("settings");
        const auto settings = test::read_bundles(stored.at("content"), stored.at("reply"));
        ASSERT_TRUE(settings) << name << ": " << settings.error();
        EXPECT_EQ(domain::check_content_settings(settings->content), std::nullopt) << name;
        EXPECT_EQ(domain::check_reply_settings(settings->reply), std::nullopt) << name;
        const std::string title(captured.at("title").as_string());

        // The case renders the sentence of the branch it is for, in the writer's prompt for one of its languages.
        std::set<std::string> languages(settings->reply.reply_languages.begin(), settings->reply.reply_languages.end());
        languages.insert(settings->reply.fallback_language);
        std::string prompts;
        for (const auto& language : languages) {
            prompts += domain::generator_messages({.business_name = title,
                                                   .settings = *settings,
                                                   .stars = 5,
                                                   .review_text = "placeholder",
                                                   .mode = "positive",
                                                   .target_language = language,
                                                   .severity_labels = {},
                                                   .items = {},
                                                   .recent_replies = {},
                                                   .staff_instructions = {}},
                                                  "")
                           .front()
                           .content;
        }
        for (const auto& sentence : captured.at("renders").as_array()) {
            EXPECT_NE(prompts.find(std::string(sentence.as_string()) + "\n"), std::string::npos) << name << ": " << sentence;
        }

        const auto now = fingerprint_of(golden, industry, title, *settings);
        EXPECT_EQ(boost::json::value(now.inputs), captured.at("fingerprint_inputs")) << name << ": " << boost::json::serialize(now.inputs);
        EXPECT_EQ(boost::json::value(now.hash), captured.at("fingerprint")) << name << ": " << now.hash;
    }
}
