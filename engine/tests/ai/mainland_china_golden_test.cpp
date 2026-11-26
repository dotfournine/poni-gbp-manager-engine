// fixtures/mainland_china_golden.json holds one made-up location whose
// replies may be Chinese with Mainland China phrasing, its Chinese variant
// zh-CN (D-107), and the fingerprint captured for it, recaptured once for the
// wording of D-107 points 9 to 11. Its fingerprint is
// recaptured only by a change that alters a reply prompt on purpose, as
// ReplySettingsGolden's are; like that file it holds its own industry and
// model setups, so the knowledge base does not touch it.
//
// The other branch no golden renders, a reply language other than en, zh
// and es, cannot be reached: the settings' check accepts only those three
// as reply and fallback languages, and the reply's language is always one
// of them (domain::target_language).

#include "ai_fixture.hpp"
#include "domain/go_live.hpp"
#include "domain/settings.hpp"

#include <gtest/gtest.h>

#include <boost/json/serialize.hpp>
#include <boost/json/value_to.hpp>

#include <string>
#include <vector>

using namespace poni;

namespace {

domain::ModelSetup model_of(const boost::json::value& json, std::optional<double> temperature) {
    return {.provider = std::string(json.at("provider").as_string()),
            .model = std::string(json.at("model").as_string()),
            .temperature = temperature,
            .max_tokens = json.at("max_tokens").to_number<std::int64_t>(),
            .extras = json.at("extras").as_object()};
}

}  // namespace

TEST(MainlandChinaGolden, TheCapturedSettingsGiveTheCapturedFingerprint) {
    const auto golden = test::load_json_fixture("mainland_china_golden.json");
    const auto& stored = golden.at("settings");
    const auto settings = test::read_bundles(stored.at("content"), stored.at("reply"));
    ASSERT_TRUE(settings) << settings.error();
    // Settings a person could save, and the branch this file is for.
    EXPECT_EQ(domain::check_content_settings(settings->content), std::nullopt);
    EXPECT_EQ(domain::check_reply_settings(settings->reply), std::nullopt);
    ASSERT_EQ(settings->content.chinese_variant, "zh-CN");

    const auto& told = golden.at("industry");
    const domain::IndustryRules industry{.key = std::string(told.at("key").as_string()),
                                         .label = std::string(told.at("label").as_string()),
                                         .topics = boost::json::value_to<std::vector<domain::Topic>>(told.at("topics")),
                                         .item_types = boost::json::value_to<std::vector<domain::ItemType>>(told.at("item_types")),
                                         .severity_flags = boost::json::value_to<std::vector<domain::SeverityFlag>>(told.at("severity_flags"))};
    const auto& analyzer = golden.at("analyzer");
    const auto now = domain::fingerprint({.business_name = std::string(golden.at("title").as_string()),
                                          .industry = industry,
                                          .settings = *settings,
                                          .analyzer = model_of(analyzer, analyzer.at("temperature").to_number<double>()),
                                          .writer = model_of(golden.at("writer"), settings->reply.temperature)});

    // One Chinese target, which zh-CN renders with Simplified characters and the Mainland phrasing.
    const auto& prompts = golden.at("fingerprint_inputs").at("prompts").as_object();
    EXPECT_TRUE(prompts.contains("writer:positive:zh"));
    EXPECT_FALSE(prompts.contains("writer:positive:zh-Hans"));
    EXPECT_EQ(now.inputs, golden.at("fingerprint_inputs").as_object()) << boost::json::serialize(now.inputs);
    EXPECT_EQ(now.hash, golden.at("fingerprint").as_string()) << now.hash;
}
