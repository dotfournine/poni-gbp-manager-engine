// fixtures/reply_settings_golden.json was captured on 2026-11-20 with the
// engine as it was before reply settings became one complete row per
// location (D-093): made-up clients and locations with voice and policy
// layers at every level, exactly as the engine stored them, and for each
// location what the layers resolved to and the fingerprint of that. Giving
// every location one row must change none of them. The file is never
// regenerated: it holds everything it needs (the defaults, the industries,
// the models), so a later version of the knowledge base does not touch it.
//
// The test below: the captured settings still give the captured
// fingerprints, so no prompt, model request or check limit changed. That a
// code change which alters a prompt for unchanged settings fails a test
// rests on this file (D-091 point 10). That the migration (0026, now part of
// 0001_baseline) gave every location these settings was proved once, by a
// test that left with the old migration files.

#include "ai_fixture.hpp"
#include "domain/go_live.hpp"
#include "domain/reply_settings.hpp"

#include <gtest/gtest.h>

#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>
#include <boost/json/value_from.hpp>
#include <boost/json/value_to.hpp>

#include <optional>
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

// The fingerprint of a client's location with these settings, built as
// repo::go_live_setup builds it: the business is named by the voice or,
// where its reply_name is empty, by the name the file holds for it, and the
// writer is asked with the voice's temperature.
domain::Fingerprint fingerprint_of(const boost::json::value& golden, const boost::json::value& client,
                                   const domain::ResolvedSettings& settings) {
    const auto& told = golden.at("industries").at(client.at("industry_key").as_string());
    const domain::IndustryRules industry{.key = std::string(told.at("key").as_string()),
                                         .label = std::string(told.at("label").as_string()),
                                         .topics = boost::json::value_to<std::vector<domain::Topic>>(told.at("topics")),
                                         .item_types = boost::json::value_to<std::vector<domain::ItemType>>(told.at("item_types")),
                                         .severity_flags = boost::json::value_to<std::vector<domain::SeverityFlag>>(told.at("severity_flags"))};
    return domain::fingerprint({.business_name = domain::business_name(settings.voice, client.at("name").as_string()),
                                .industry = industry,
                                .settings = settings,
                                .analyzer = model_of(golden.at("analyzer"), golden.at("analyzer").at("temperature").to_number<double>()),
                                .writer = model_of(golden.at("writer"), settings.voice.temperature)});
}

}  // namespace

TEST(ReplySettingsGolden, TheCapturedSettingsGiveTheCapturedFingerprints) {
    const auto golden = test::load_json_fixture("reply_settings_golden.json");
    ASSERT_EQ(golden.at("cases").as_array().size(), 15u);
    for (const auto& captured : golden.at("cases").as_array()) {
        const auto name = std::string(captured.at("name").as_string());
        const auto& resolved = captured.at("resolved");
        const auto settings = domain::read_settings(resolved.at("voice"), resolved.at("localization"), resolved.at("policy"));
        ASSERT_TRUE(settings) << name << ": " << settings.error();
        // Complete settings are read as they are: nothing is added, dropped or reordered.
        EXPECT_EQ(boost::json::serialize(boost::json::value_from(*settings)), boost::json::serialize(resolved)) << name;

        // When the file was captured, a location without a client had no configuration to test.
        const auto& location = row_with_id(golden, "locations", captured.at("location_id"));
        if (location.at("client_id").is_null()) {
            EXPECT_TRUE(captured.at("fingerprint").is_null()) << name;
            continue;
        }
        const auto now = fingerprint_of(golden, row_with_id(golden, "clients", location.at("client_id")), *settings);
        EXPECT_EQ(now.inputs, captured.at("fingerprint_inputs").as_object()) << name;
        EXPECT_EQ(now.hash, captured.at("fingerprint").as_string()) << name;
    }
}
