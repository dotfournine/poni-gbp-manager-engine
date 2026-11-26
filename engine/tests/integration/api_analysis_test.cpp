// The analysis and drafts API over a real PostgreSQL: analysis and drafts on reviews,
// the settings they are made with (industry profiles, content and reply settings,
// pipeline settings), LLM usage, and that every change is audited.

#include "ai/ai_fixture.hpp"
#include "api_fixture.hpp"
#include "core/json.hpp"
#include "db/repo_analysis.hpp"
#include "db/repo_go_live.hpp"
#include "db/repo_reviews.hpp"
#include "db/repo_settings.hpp"
#include "jobs/analysis.hpp"

#include <boost/json/serialize.hpp>
#include <gmock/gmock.h>

using namespace poni;
using test::FakeLlm;
using test::make_review;
using testing::HasSubstr;

namespace {

class AnalysisApi : public test::ApiTest {
protected:
    void SetUp() override {
        ApiTest::SetUp();
        // This engine has two model providers configured (D-059).
        api_options_.llm_providers = {"dashscope", "openai"};
        llm_.provider_names = api_options_.llm_providers;
        build({});
        browser_ = sign_in();
        discover_spa_and_buffet();
        spa_client_ = make_client("LU DREAM SPA");
        spa_ = link(test::kSpa, spa_client_);
        switch_location(spa_, true);
        buffet_ = location_id(test::kBuffet);  // discovered, and nothing else
        pipeline_.emplace(db(), clock_, llm_, queue(), jobs::AnalysisOptions{});
    }

    void TearDown() override {
        pipeline_.reset();
        ApiTest::TearDown();
    }

    void set_industry(std::int64_t location, const std::string& key) {
        ASSERT_EQ(patch_current("/api/locations/" + std::to_string(location), R"({"industry_key": ")" + key + R"("})").status, 200u);
    }

    std::int64_t store(const std::string& review_id, std::string_view stars, std::optional<std::string> comment,
                       std::optional<std::string> reply = std::nullopt) {
        return store_review(spa_, make_review(gbp_.account_id, "9001", review_id, stars, std::move(comment), "2026-11-09T10:00:00Z",
                                              "2026-11-09T10:00:00Z", std::move(reply)));
    }

    // One more location of the fake Google, discovered and nothing else.
    std::int64_t another_location(const std::string& gbp_id, const std::string& title) {
        gbp_.add_location(gbp_id, title);
        discover();
        return location_id(gbp_id);
    }
    // What the newest audit entry of an action holds as `what` ("before" or "after").
    boost::json::value audit_json(const std::string& action, const std::string& what) {
        return boost::json::parse(
            value<std::string>(std::format("SELECT {}::text FROM audit_log WHERE action = '{}' ORDER BY id DESC LIMIT 1", what, action)));
    }

    std::vector<std::string> review_ids(const std::string& query) {
        std::vector<std::string> ids;
        for (const auto& item : read("/api/reviews" + query).at("items").as_array()) {
            ids.push_back(gbp_review_id(item.at("id").as_int64()));
        }
        return ids;
    }

    long audited(const std::string& action) { return count("audit_log WHERE action = '" + action + "'"); }

    FakeLlm llm_;
    std::optional<jobs::ReviewPipeline> pipeline_;
};

}  // namespace

// ---- access ----------------------------------------------------------------------

TEST_F(AnalysisApi, EveryNewEndpointNeedsASession) {
    // A change needs the CSRF token on top of the session. The routes of this area, from the router's table:
    // the 15 this test once named and every one added since.
    EXPECT_GE(expect_session_and_csrf({"/api/knowledge", "/api/ai/", "/api/llm/", "/drafts", "/reply-settings", "/analyze"},
                                      R"({"enabled": false})"),
              15u);
    EXPECT_EQ(read("/api/ai/settings").at("enabled").as_bool(), true);
}

// ---- the knowledge base, read-only (FR-13) -----------------------------------------

TEST_F(AnalysisApi, TheKnowledgeBaseIsShownAsItWasSeeded) {
    const auto knowledge = read("/api/knowledge");
    EXPECT_EQ(knowledge.at("version").as_string(), value<std::string>("SELECT version FROM kb_meta"));
    EXPECT_EQ(knowledge.at("format").as_int64(), 2);
    EXPECT_FALSE(knowledge.at("seeded_at").as_string().empty());

    // The supported industries: each has a complete pack.
    const auto& industries = knowledge.at("industries").as_array();
    ASSERT_EQ(industries.size(), 2u);
    const auto& food = industries[0];
    EXPECT_EQ(food.at("key").as_string(), "food_beverage");
    EXPECT_EQ(food.at("label").as_string(), "Food and beverage");
    EXPECT_EQ(food.at("topics").as_array().size(), 10u);
    EXPECT_EQ(food.at("test_client_name").as_string(), "Golden Harbor Buffet");
    EXPECT_EQ(food.at("location_count").as_int64(), 0);
    std::vector<std::string> flags;
    for (const auto& flag : food.at("severity_flags").as_array()) {
        flags.emplace_back(flag.at("id").as_string());
        EXPECT_TRUE(flag.at("enabled").as_bool());
        EXPECT_FALSE(flag.at("description").as_string().empty()) << "the definition the analyzer is given";
    }
    EXPECT_THAT(flags, testing::ElementsAre("illness_food_poisoning", "allergen_incident", "foreign_object", "food_safety", "hygiene"));
    const auto& corpus = food.at("corpus");
    EXPECT_GE(corpus.at("severity_cases").as_int64(), 18);
    EXPECT_GE(corpus.at("manipulation_cases").as_int64(), 10);
    EXPECT_GE(corpus.at("control_cases").as_int64(), 3);
    EXPECT_EQ(corpus.at("version").as_string().size(), 12u);
    EXPECT_EQ(industries[1].at("key").as_string(), "wellness");

    EXPECT_EQ(knowledge.at("common_severity_flags").as_array().size(), 4u);
    const auto& templates = knowledge.at("voice_templates").as_array();
    ASSERT_EQ(templates.size(), 3u);
    EXPECT_EQ(templates[2].at("key").as_string(), "warm_casual");
    EXPECT_EQ(templates[2].at("content").at("emoji_max").as_int64(), 2);
    EXPECT_TRUE(templates[2].at("reply").as_object().empty()) << "the template sets nothing of the reply settings";
    const auto& defaults = knowledge.at("defaults");
    EXPECT_TRUE(defaults.at("content").at("chinese_variant").is_null());
    EXPECT_EQ(defaults.at("reply").at("auto_reply_min_stars").as_int64(), 4);
    EXPECT_EQ(defaults.at("content").at("base_style").as_string(), "");
    EXPECT_EQ(defaults.at("analysis").at("analyzer_model").as_string(), "qwen3.7-plus");
    EXPECT_DOUBLE_EQ(knowledge.at("model_prices").at("qwen3.7-plus").at("output_per_mtok").as_double(), 1.6);

    // An enabled location that names an industry is counted with it; one that is switched off is not.
    set_industry(spa_, "wellness");
    set_industry(buffet_, "wellness");
    EXPECT_EQ(read("/api/knowledge").at("industries").as_array()[1].at("location_count").as_int64(), 1);
}

TEST_F(AnalysisApi, AnIndustrysCorpusCanBeRead) {
    const auto corpus = read("/api/knowledge/industries/wellness/corpus");
    EXPECT_EQ(corpus.at("industry_key").as_string(), "wellness");
    EXPECT_EQ(corpus.at("version").as_string(), value<std::string>("SELECT corpus_version FROM kb_industries WHERE key = 'wellness'"));
    const auto& cases = corpus.at("cases").as_array();
    EXPECT_EQ(static_cast<long>(cases.size()), count("kb_corpus_cases WHERE industry_key = 'wellness'"));
    const auto& first = cases[0];
    EXPECT_EQ(first.at("id").as_string(), "S-SEV-01");
    EXPECT_EQ(first.at("category").as_string(), "severity");
    EXPECT_EQ(first.at("action").as_string(), "escalate");
    EXPECT_EQ(first.at("severity_any").as_array()[0].as_string(), "injury");
    EXPECT_FALSE(first.at("manipulation").as_bool());
    EXPECT_FALSE(first.at("text").as_string().empty());
    EXPECT_EQ(get("/api/knowledge/industries/dental/corpus", browser_).status, 404u);
}

// The tables are static: an update is a new set of files, seeded by poni-tools. No route changes them.
TEST_F(AnalysisApi, NoRouteWritesTheKnowledgeBase) {
    const auto before = value<std::string>(
        "SELECT md5(coalesce((SELECT string_agg(i::text, '') FROM kb_industries i), '') || (SELECT c::text FROM kb_common c) || "
        "coalesce((SELECT string_agg(t::text, '') FROM kb_voice_templates t), '') || (SELECT d::text FROM kb_defaults d) || "
        "(SELECT d::text FROM kb_content_defaults d) || (SELECT d::text FROM kb_reply_defaults d) || "
        "coalesce((SELECT string_agg(p::text, '') FROM kb_model_prices p), '') || (SELECT m::text FROM kb_meta m))");
    for (const auto* target : {"/api/knowledge", "/api/knowledge/industries/food_beverage", "/api/knowledge/industries/food_beverage/corpus",
                               "/api/industry-profiles/1", "/api/voice-templates/1"}) {
        for (const auto method : {verb::post, verb::patch, verb::put, verb::delete_}) {
            const auto response = call(method, target, browser_, R"({"label": "Changed", "reply": {"signature": "x"}})");
            EXPECT_TRUE(response.status == 404u || response.status == 405u) << target << ": " << response.status;
        }
    }
    // The editors of before the knowledge base are gone, reading included.
    EXPECT_EQ(get("/api/industry-profiles", browser_).status, 404u);
    EXPECT_EQ(get("/api/voice-templates", browser_).status, 404u);
    // Prices are knowledge: the settings neither show nor take them.
    EXPECT_FALSE(read("/api/ai/settings").as_object().contains("model_prices"));
    const auto prices = patch("/api/ai/settings", R"({"model_prices": {"qwen3.7-plus": {"input_per_mtok": 99, "output_per_mtok": 99}}})");
    ASSERT_EQ(prices.status, 400u) << "a member the settings do not have is refused, not passed over (D-092)";
    EXPECT_THAT(prices.body, testing::HasSubstr("model_prices"));
    EXPECT_EQ(audited("ai_settings.update"), 0) << "nothing the settings hold was changed";
    // A save and a discovery copy from the knowledge base; they write the location's row only.
    ASSERT_EQ(save_reply_settings(spa_, R"({"signature": "The team"})").status, 200u);
    another_location("9003", "LU DREAM SPA - Midtown");
    EXPECT_EQ(value<std::string>(
                  "SELECT md5(coalesce((SELECT string_agg(i::text, '') FROM kb_industries i), '') || (SELECT c::text FROM kb_common c) || "
                  "coalesce((SELECT string_agg(t::text, '') FROM kb_voice_templates t), '') || (SELECT d::text FROM kb_defaults d) || "
                  "(SELECT d::text FROM kb_content_defaults d) || (SELECT d::text FROM kb_reply_defaults d) || "
                  "coalesce((SELECT string_agg(p::text, '') FROM kb_model_prices p), '') || (SELECT m::text FROM kb_meta m))"),
              before);
}

TEST_F(AnalysisApi, ALocationsIndustryIsSetAndChecked) {
    const auto target = "/api/locations/" + std::to_string(spa_);
    EXPECT_TRUE(read(target).at("industry_key").is_null());
    const auto set = patch_current(target, R"({"industry_key": "wellness"})");
    ASSERT_EQ(set.status, 200u) << set.body;
    EXPECT_EQ(json(set).at("industry_key").as_string(), "wellness");
    // Only an industry the knowledge base supports can be given.
    const auto unknown = patch_current(target, R"({"industry_key": "no_such_industry"})");
    EXPECT_EQ(unknown.status, 400u);
    EXPECT_THAT(error_message(unknown), HasSubstr("knowledge base"));
    EXPECT_EQ(patch_current(target, R"({"industry_key": "common"})").status, 400u) << "the shared flags are not an industry";
    EXPECT_EQ(patch(target, R"({"industry_key": 5})").status, 400u);
    EXPECT_EQ(json(patch_current(target, R"({"posting_enabled": true})")).at("industry_key").as_string(), "wellness") << "a field that isn't sent stays";
    const auto none = json(patch_current(target, R"({"industry_key": null})"));
    EXPECT_TRUE(none.at("industry_key").is_null());
}

// A location names its industry by key and the engine reads that profile from
// the knowledge base every time. A new version that no longer has the
// industry leaves the location as it is and unanalyzed until it is given another
// industry or its own comes back (D-078 point 5, D-084).
TEST_F(AnalysisApi, ALocationWhoseIndustryLeftTheKnowledgeBaseIsNotAnalyzedAndSaysSo) {
    set_industry(spa_, "wellness");
    const auto id = store("r1", "FIVE", "Lovely!");
    reseed("DELETE FROM kb_industries WHERE key = 'wellness'");

    const auto target = "/api/locations/" + std::to_string(spa_);
    const auto location = read(target);
    EXPECT_EQ(location.at("industry_key").as_string(), "wellness") << "the seed changes no location";
    EXPECT_FALSE(location.as_object().contains("industry_supported")) << "the dashboard compares the key with the industries of /api/knowledge";
    // Changing something else leaves the industry it has; it can't be given again while it is gone.
    EXPECT_EQ(patch_current(target, R"({"posting_enabled": true})").status, 200u);
    EXPECT_EQ(patch_current("/api/locations/" + std::to_string(buffet_), R"({"industry_key": "wellness"})").status, 400u);

    EXPECT_EQ(await(pipeline_->enqueue_pending()), 0u) << "the pipeline does not pick up a review it has no profile for";
    await(pipeline_->process(id, false, false));
    EXPECT_TRUE(llm_.requests.empty()) << "no model is asked about a review without a profile to analyze it against";
    EXPECT_EQ(count("review_analyses"), 0);
    const auto asked = post("/api/reviews/" + std::to_string(id) + "/analyze");
    EXPECT_EQ(asked.status, 409u) << asked.body;
    {
        pqxx::work tx{conn()};
        EXPECT_FALSE(repo::go_live_setup(tx, spa_)) << "a location without a profile has no fingerprint, so it never auto replies";
    }

    // The industry comes back with a later version, and the location is analyzed again without anyone touching it.
    reseed("INSERT INTO kb_industries SELECT * FROM (SELECT 'wellness' AS key, position, label, topics, item_types, severity_flags, "
           "test_client, corpus_version FROM kb_industries WHERE key = 'food_beverage') restored");
    EXPECT_EQ(await(pipeline_->enqueue_pending()), 1u);
    await(pipeline_->process(id, false, false));
    EXPECT_EQ(count("review_analyses"), 1);
}

TEST_F(AnalysisApi, ALocationsAnalysisStartDateIsSetCheckedAndAudited) {
    const auto target = "/api/locations/" + std::to_string(spa_);
    EXPECT_TRUE(read(target).at("analyze_from").is_null()) << "without a date every review is analyzed";
    const auto set = patch_current(target, R"({"analyze_from": "2026-11-01"})");
    ASSERT_EQ(set.status, 200u) << set.body;
    EXPECT_EQ(json(set).at("analyze_from").as_string(), "2026-11-01");
    EXPECT_EQ(value<std::string>("SELECT after->>'analyze_from' FROM audit_log WHERE action = 'location.update' ORDER BY id DESC LIMIT 1"),
              "2026-11-01");
    for (const auto* bad : {R"({"analyze_from": "2026-02-30"})", R"({"analyze_from": "11/01/2026"})", R"({"analyze_from": 20261101})"}) {
        EXPECT_EQ(patch_current(target, bad).status, 400u) << bad;
    }
    EXPECT_EQ(json(patch_current(target, R"({"posting_enabled": true})")).at("analyze_from").as_string(), "2026-11-01") << "a field that isn't sent stays";
    EXPECT_TRUE(json(patch_current(target, R"({"analyze_from": null})")).at("analyze_from").is_null());
}

// ---- a location's content and reply settings (D-107) -------------------------------

// A location gets both bundles when it is discovered, copies of the defaults, whatever client it has (D-102).
TEST_F(AnalysisApi, EveryLocationStartsWithACopyOfTheDefaultsAndAClientHasNoSettings) {
    const auto defaults = read("/api/knowledge").at("defaults");
    for (const auto location : {spa_, buffet_}) {
        EXPECT_EQ(read(content_settings_path(location)), defaults.at("content"));
        EXPECT_EQ(read(reply_settings_path(location)), defaults.at("reply"));
    }
    // The answer is the bundle alone: who the location is, its client and the time of the save are not in it.
    for (const auto* member : {"location_id", "location_title", "client_id", "client_name", "industry_key", "updated_at"}) {
        EXPECT_FALSE(read(reply_settings_path(spa_)).as_object().contains(member)) << member;
        EXPECT_FALSE(read(content_settings_path(spa_)).as_object().contains(member)) << member;
    }

    // A client has no settings of its own: a new one changes nothing.
    const auto created = post("/api/clients", R"({"name": "Corner Cafe"})");
    ASSERT_EQ(created.status, 201u) << created.body;
    EXPECT_EQ(count("content_settings"), 2);
    EXPECT_EQ(count("reply_settings"), 2);

    // A later version of the knowledge base with other defaults does not reach a location that has its settings.
    reseed("UPDATE kb_reply_defaults SET signature = 'The team', auto_reply_min_stars = 5; "
           "UPDATE kb_content_defaults SET audience = 'Families'");
    EXPECT_EQ(read(reply_settings_path(spa_)).at("signature").as_string(), "");
    EXPECT_EQ(read(reply_settings_path(spa_)).at("auto_reply_min_stars").as_int64(), 4);
    EXPECT_EQ(read(content_settings_path(spa_)).at("audience").as_string(), "");
    // A location discovered after it starts from the new ones: both bundles equal to the defaults' rows.
    const auto midtown = another_location("9003", "LU DREAM SPA - Midtown");
    const auto now = read("/api/knowledge").at("defaults");
    EXPECT_EQ(read(content_settings_path(midtown)), now.at("content"));
    EXPECT_EQ(read(reply_settings_path(midtown)), now.at("reply"));
    EXPECT_EQ(read(reply_settings_path(midtown)).at("signature").as_string(), "The team");
    EXPECT_EQ(read(content_settings_path(midtown)).at("audience").as_string(), "Families");
}

// Each bundle is replaced whole by its PUT, against its ETag (D-107, D-108).
TEST_F(AnalysisApi, ASaveReplacesALocationsSettingsWhole) {
    struct Case {
        std::string path;
        std::string other_path;  // the same bundle of the location without a client
        std::string table;
        std::string member;
        boost::json::value changed;
    };
    for (const auto& one : {Case{content_settings_path(spa_), content_settings_path(buffet_), "content_settings", "words_to_avoid",
                                 boost::json::array{"cheap"}},
                            Case{reply_settings_path(spa_), reply_settings_path(buffet_), "reply_settings", "reply_languages",
                                 boost::json::array{"en", "zh"}}}) {
        SCOPED_TRACE(one.path);
        const auto action = one.table + ".update";
        const auto read_tag = etag(one.path, browser_);
        auto body = read(one.path).as_object();
        const auto was = body.at(one.member);
        body[one.member] = one.changed;
        const auto saved = put(one.path, boost::json::serialize(body), if_match(read_tag));
        ASSERT_EQ(saved.status, 200u) << saved.body;
        EXPECT_EQ(json(saved), boost::json::value(body)) << "the answer is what is stored now";
        EXPECT_EQ(read(one.path), boost::json::value(body));
        const auto new_tag = header(saved, "ETag");
        EXPECT_NE(new_tag, read_tag);
        EXPECT_EQ(etag(one.path, browser_), new_tag);

        // One audit entry on the location, with the bundle before and after.
        EXPECT_EQ(audited(action), 1);
        EXPECT_EQ(value<std::string>("SELECT object_type || ' ' || object_id FROM audit_log WHERE action = '" + action + "'"),
                  "location " + std::to_string(spa_));
        EXPECT_EQ(audit_json(action, "before").at(one.member), was);
        EXPECT_EQ(audit_json(action, "after").at(one.member), one.changed);

        // The same bundle again changes nothing, records nothing, and keeps the ETag and the time of the save.
        const auto saved_at = [&] {
            return value<std::string>(std::format("SELECT updated_at::text FROM {} WHERE location_id = {}", one.table, spa_));
        };
        const auto changed_at = saved_at();
        clock_.advance(std::chrono::minutes(5));
        const auto again = put(one.path, boost::json::serialize(body), if_match(new_tag));
        ASSERT_EQ(again.status, 200u) << again.body;
        EXPECT_EQ(header(again, "ETag"), new_tag);
        EXPECT_EQ(audited(action), 1);
        EXPECT_EQ(saved_at(), changed_at);

        // A location with no client and switched off has settings of its own like any other (D-102).
        const auto own = put(one.other_path, boost::json::serialize(body), if_match(etag(one.other_path, browser_)));
        ASSERT_EQ(own.status, 200u) << own.body;
    }
    EXPECT_EQ(count("content_settings"), 2);
    EXPECT_EQ(count("reply_settings"), 2);
}

TEST_F(AnalysisApi, ABundleIsChangedOnlyAgainstItsCurrentEtag) {
    for (const bool content : {true, false}) {
        const auto path = content ? content_settings_path(spa_) : reply_settings_path(spa_);
        SCOPED_TRACE(path);
        const auto read_tag = etag(path, browser_);
        const auto body = boost::json::serialize(read(path));
        const auto without = put(path, body);
        EXPECT_EQ(without.status, 428u);
        EXPECT_EQ(error_code(without), "precondition_required");
        // Someone else saves meanwhile.
        ASSERT_EQ((content ? save_content_settings(spa_, R"({"base_style": "Plain"})") : save_reply_settings(spa_, R"({"signature": "The team"})"))
                      .status,
                  200u);
        const auto entries = count("audit_log");
        const auto stale = put(path, body, if_match(read_tag));
        EXPECT_EQ(stale.status, 412u);
        EXPECT_EQ(error_code(stale), "precondition_failed");
        EXPECT_EQ(put(path, body, if_match("\"stale\"")).status, 412u);
        EXPECT_EQ(count("audit_log"), entries);
        // No such location.
        const auto missing = content ? content_settings_path(999999) : reply_settings_path(999999);
        EXPECT_EQ(get(missing, browser_).status, 404u);
        EXPECT_EQ(put(missing, body, if_match(read_tag)).status, 404u);
    }
}

// A save is checked against the limits; a refused one changes nothing.
TEST_F(AnalysisApi, ASaveIsCheckedAgainstTheLimits) {
    const auto refused = [&](const http::Response& response, std::string_view what) {
        EXPECT_EQ(response.status, 400u) << what;
        EXPECT_THAT(error_message(response), HasSubstr(std::string(what)));
    };
    const auto content = [&](const boost::json::object& members) { return save_content_settings(spa_, boost::json::serialize(members)); };
    const auto reply = [&](const boost::json::object& members) { return save_reply_settings(spa_, boost::json::serialize(members)); };
    refused(content({{"emoji_max", 99}}), "emoji_max");
    refused(content({{"chinese_variant", "zh-SG"}}), "chinese_variant");
    refused(content({{"english_variant", "en-GB"}}), "english_variant");
    refused(content({{"business_facts", std::string(2001, 'x')}}), "business_facts");
    refused(content({{"base_style", std::string(2001, 'x')}}), "base_style");
    refused(content({{"audience", std::string(501, 'x')}}), "audience");
    refused(content({{"emoji_allowed", "yes"}}), "emoji_allowed");
    refused(reply({{"reply_languages", boost::json::array{"en", "fr"}}}), "reply_languages");
    refused(reply({{"reply_languages", boost::json::array{}}}), "reply_languages");
    refused(reply({{"fallback_language", "fr"}}), "fallback_language");
    refused(reply({{"promos_allowed", "yes"}}), "promos_allowed");
    refused(reply({{"temperature", 3}}), "temperature");
    refused(reply({{"auto_reply_min_stars", 9}}), "auto_reply_min_stars");
    refused(reply({{"signature", "Warmly,\nthe team"}}), "'signature' is one line");
    refused(reply({{"signature", "Warmly,\rthe team"}}), "'signature' is one line");
    refused(reply({{"signature", std::string(201, 'x')}}), "signature");
    refused(reply({{"sign_off_name", std::string(201, 'x')}}), "sign_off_name");
    // Every list: an empty entry, one that is too long, and too many entries.
    boost::json::array many;
    for (int i = 0; i < 51; ++i) many.emplace_back(std::format("entry {}", i));
    for (const auto& [in_content, member] : {std::pair<bool, std::string>{true, "emoji_preferred"},
                                             {true, "words_to_avoid"},
                                             {true, "style_notes"},
                                             {false, "example_replies"}}) {
        const auto save = [&](boost::json::array list) {
            const boost::json::object members{{member, std::move(list)}};
            return in_content ? content(members) : reply(members);
        };
        refused(save({"fine", "  "}), std::format("'{}' has an empty entry", member));
        refused(save({std::string(2001, 'x')}), std::format("an entry of '{}' is longer than", member));
        refused(save(many), std::format("'{}' may hold at most 50 entries", member));
    }
    {
        // Complete means limits for every language replies are written in.
        const auto path = reply_settings_path(spa_);
        auto without_spanish = read(path).as_object();
        without_spanish["length"].as_object().erase("es");
        refused(put(path, boost::json::serialize(without_spanish), if_match(etag(path, browser_))), "'length' needs limits for 'es'");
    }
    EXPECT_EQ(audited("content_settings.update"), 0);
    EXPECT_EQ(audited("reply_settings.update"), 0);
}

// A body is the bundle whole (D-092): a member left out or one it does not have is a 400 that names it; a variant
// is sent as null to unset it.
TEST_F(AnalysisApi, ABundleIsSentWholeAndAVariantIsUnsetWithNull) {
    const auto path = content_settings_path(spa_);
    ASSERT_EQ(save_content_settings(spa_, R"({"chinese_variant": "zh-TW", "english_variant": "en-CA"})").status, 200u);
    EXPECT_EQ(read(path).at("chinese_variant").as_string(), "zh-TW");

    auto body = read(path).as_object();
    body.erase("chinese_variant");
    const auto left_out = put(path, boost::json::serialize(body), if_match(etag(path, browser_)));
    EXPECT_EQ(left_out.status, 400u);
    EXPECT_THAT(error_message(left_out), HasSubstr("chinese_variant"));

    body = read(path).as_object();
    body["other_business_names"] = boost::json::array{"Glow Spa"};
    const auto unknown = put(path, boost::json::serialize(body), if_match(etag(path, browser_)));
    EXPECT_EQ(unknown.status, 400u);
    EXPECT_THAT(error_message(unknown), HasSubstr("other_business_names"));
    auto reply = read(reply_settings_path(spa_)).as_object();
    reply["owner_name"] = "Mina";
    const auto gone = put(reply_settings_path(spa_), boost::json::serialize(reply), if_match(etag(reply_settings_path(spa_), browser_)));
    EXPECT_EQ(gone.status, 400u);
    EXPECT_THAT(error_message(gone), HasSubstr("owner_name"));

    body = read(path).as_object();
    body["chinese_variant"] = nullptr;
    const auto unset = put(path, boost::json::serialize(body), if_match(etag(path, browser_)));
    ASSERT_EQ(unset.status, 200u) << unset.body;
    EXPECT_TRUE(read(path).at("chinese_variant").is_null());
    EXPECT_EQ(read(path).at("english_variant").as_string(), "en-CA") << "the other variant stays";
    EXPECT_TRUE(value<bool>(std::format("SELECT chinese_variant IS NULL FROM content_settings WHERE location_id = {}", spa_)));
}

// Each list keeps its order through its text[] column (D-107 point 3).
TEST_F(AnalysisApi, EveryListKeepsItsOrder) {
    for (const auto& [in_content, member] : {std::pair<bool, std::string>{true, "emoji_preferred"},
                                             {true, "words_to_avoid"},
                                             {true, "style_notes"},
                                             {false, "reply_languages"},
                                             {false, "example_replies"}}) {
        SCOPED_TRACE(member);
        const auto unsorted = member == "reply_languages" ? boost::json::array{"zh", "es", "en"}
                                                          : boost::json::array{"zebra 😊", "apple", "mango", "Apple", "banana"};
        const auto members = boost::json::serialize(boost::json::object{{member, unsorted}});
        const auto saved = in_content ? save_content_settings(spa_, members) : save_reply_settings(spa_, members);
        ASSERT_EQ(saved.status, 200u) << saved.body;
        EXPECT_EQ(json(saved).at(member), unsorted);
        EXPECT_EQ(read(in_content ? content_settings_path(spa_) : reply_settings_path(spa_)).at(member), unsorted);
        EXPECT_EQ(value<std::string>(std::format("SELECT array_to_json({})::text FROM {} WHERE location_id = {}", member,
                                                 in_content ? "content_settings" : "reply_settings", spa_)),
                  boost::json::serialize(unsorted))
            << "stored in that order";
    }
}

// Taking a voice template is the dashboard filling the form from the knowledge base and saving (D-080).
TEST_F(AnalysisApi, TakingAVoiceTemplateIsAnOrdinarySave) {
    ASSERT_EQ(save_content_settings(spa_, R"({"audience": "Office workers", "words_to_avoid": ["cheap"]})").status, 200u);
    boost::json::object family;
    for (const auto& one : read("/api/knowledge").at("voice_templates").as_array()) {
        if (one.at("key").as_string() == "family_owned") family = one.as_object();
    }
    ASSERT_FALSE(family.empty());
    const auto content_path = content_settings_path(spa_);
    const auto content = test::apply_template(read(content_path).as_object(), family.at("label").as_string(),
                                              family.at("description").as_string(), family.at("content").as_object());
    const auto taken = put(content_path, boost::json::serialize(content), if_match(etag(content_path, browser_)));
    ASSERT_EQ(taken.status, 200u) << taken.body;
    const auto reply_path = reply_settings_path(spa_);
    const auto reply = test::merge_layer(read(reply_path).as_object(), family.at("reply").as_object());
    ASSERT_EQ(put(reply_path, boost::json::serialize(reply), if_match(etag(reply_path, browser_))).status, 200u);

    const auto stored = json(taken);
    EXPECT_EQ(stored.at("emoji_max").as_int64(), 1) << "from the template";
    EXPECT_EQ(stored.at("style_notes").as_array().size(), 3u) << "the template's list replaces the bundle's";
    EXPECT_EQ(stored.at("audience").as_string(), "Office workers") << "what the template doesn't set stays";
    EXPECT_EQ(stored.at("words_to_avoid").as_array().size(), 1u);
    EXPECT_THAT(std::string(stored.at("base_style").as_string()), testing::StartsWith("Family-owned. "));
    EXPECT_FALSE(stored.as_object().contains("template_key"));
    EXPECT_EQ(read(reply_path).at("signature").as_string(), "— the owner and family");
    EXPECT_EQ(read(reply_path).at("sign_off_name").as_string(), "the owner");
    EXPECT_EQ(read(reply_path).at("length").at("en").at("positive").at("max").as_int64(), 420);
    EXPECT_EQ(audited("content_settings.update"), 2);
    EXPECT_EQ(audited("reply_settings.update"), 1);

    // A new version changes the template, then drops it: the location's settings stay what was saved.
    reseed(R"(UPDATE kb_voice_templates SET label = 'Renamed', content = content || '{"emoji_max": 5}'::jsonb WHERE key = 'family_owned')");
    EXPECT_EQ(read(content_path).at("emoji_max").as_int64(), 1);
    reseed("DELETE FROM kb_voice_templates WHERE key = 'family_owned'");
    EXPECT_EQ(read(content_path).at("emoji_max").as_int64(), 1);
}

// A field added later reaches the rows by a migration. A row that does not read as its bundle (the one document of
// a row, the lengths, lacking a limit) is not filled in from a default: reading it says what is wrong, and a save of
// a complete bundle repairs it.
TEST_F(AnalysisApi, ARowThatDoesNotReadIsReportedAndASaveRepairsIt) {
    const auto path = reply_settings_path(spa_);
    const auto whole = read(path);
    exec(std::format("UPDATE reply_settings SET length = (length - 'en') || jsonb_build_object('en', (length->'en') - 'positive') "
                     "WHERE location_id = {}",
                     spa_));
    const auto lacking = get(path, browser_);
    EXPECT_EQ(lacking.status, 409u);
    EXPECT_EQ(error_code(lacking), "settings_incomplete");
    EXPECT_THAT(error_message(lacking), HasSubstr("reply settings: "));
    EXPECT_THAT(error_message(lacking), HasSubstr("length"));
    EXPECT_EQ(put(path, boost::json::serialize(whole), if_match("\"x\"")).status, 409u) << "what can't be read has no ETag";
    // The repair is a save of complete settings, as the repository takes it.
    EXPECT_TRUE(as_test([&](pqxx::work& tx, audit::Writer& audit) {
        return repo::replace_reply_settings(tx, audit, spa_, core::from_json<domain::ReplySettings>(whole).value()).has_value();
    }));
    EXPECT_EQ(read(path), whole);

    // A location without a row has no settings to serve.
    exec(std::format("DELETE FROM content_settings WHERE location_id = {}", buffet_));
    EXPECT_EQ(get(content_settings_path(buffet_), browser_).status, 404u);
}

// ---- pipeline settings -----------------------------------------------------------

TEST_F(AnalysisApi, TheModelPerTaskIsASetting) {
    const auto settings = read("/api/ai/settings");
    EXPECT_EQ(settings.at("analyzer_model").as_string(), "qwen3.7-plus");
    EXPECT_EQ(settings.at("generator_model").as_string(), "qwen3.7-plus");
    EXPECT_TRUE(settings.at("enabled").as_bool());
    EXPECT_EQ(settings.at("request_extras").at("dashscope").at("enable_thinking").as_bool(), false) << "extras belong to a provider";
    EXPECT_EQ(settings.at("analyzer_provider").as_string(), "dashscope");
    EXPECT_EQ(settings.at("generator_provider").as_string(), "dashscope");
    EXPECT_EQ(boost::json::serialize(settings.at("providers")), R"(["dashscope","openai"])");
    EXPECT_FALSE(settings.as_object().contains("model_prices")) << "prices are knowledge (D-078)";
    EXPECT_FALSE(settings.as_object().contains("manipulation_patterns")) << "the keyword rules are gone (D-058)";
    EXPECT_THAT(std::string(settings.at("prompt_version").as_string()), HasSubstr("p2."));

    const auto changed = patch_current("/api/ai/settings", R"({"analyzer_model": "qwen3.8-flash", "analyzer_temperature": 0.5, "batch_size": 3})");
    ASSERT_EQ(changed.status, 200u) << changed.body;
    EXPECT_EQ(json(changed).at("analyzer_model").as_string(), "qwen3.8-flash");
    EXPECT_EQ(json(changed).at("generator_model").as_string(), "qwen3.7-plus");
    EXPECT_EQ(audited("ai_settings.update"), 1);

    // The next review is analyzed with the new model: no code change, no restart.
    set_industry(spa_, "wellness");
    await(pipeline_->process(store("r1", "FIVE", "Lovely!"), false, false));
    EXPECT_EQ(llm_.requests[0].model, "qwen3.8-flash");
    EXPECT_EQ(llm_.requests[1].model, "qwen3.7-plus");
    EXPECT_DOUBLE_EQ(llm_.requests[0].temperature.value(), 0.5);
    EXPECT_NEAR(llm_.requests[1].temperature.value(), 0.7, 1e-6) << "the writer's is a reply setting, not a setting here (D-074)";

    EXPECT_EQ(patch_current("/api/ai/settings", R"({"analyzer_temperature": 3})").status, 400u);
    EXPECT_EQ(patch_current("/api/ai/settings", R"({"analyzer_model": ""})").status, 400u);
    EXPECT_EQ(patch_current("/api/ai/settings", R"({"batch_size": 1000})").status, 400u);
    EXPECT_EQ(patch("/api/ai/settings", R"({"max_output_tokens": "many"})").status, 400u);
    EXPECT_EQ(audited("ai_settings.update"), 1);

    // Every setting has its default in the knowledge base, which "reset" in the dashboard sets it back to.
    const auto defaults = read("/api/knowledge").at("defaults").at("analysis");
    ASSERT_EQ(patch_current("/api/ai/settings", boost::json::serialize(boost::json::object{{"analyzer_model", defaults.at("analyzer_model")},
                                                                                {"analyzer_temperature", defaults.at("analyzer_temperature")},
                                                                                {"batch_size", defaults.at("batch_size")}}))
                  .status,
              200u);
    EXPECT_EQ(read("/api/ai/settings").at("analyzer_model").as_string(), "qwen3.7-plus");
    EXPECT_EQ(audited("ai_settings.update"), 2);
}

// D-108: the settings change only against the ETag of what the person read.
TEST_F(AnalysisApi, TheSettingsChangeOnlyAgainstTheirCurrentEtag) {
    const auto read_tag = etag("/api/ai/settings", browser_);
    const auto without = patch("/api/ai/settings", R"({"batch_size": 7})");
    EXPECT_EQ(without.status, 428u);
    EXPECT_EQ(error_code(without), "precondition_required");
    const auto changed = patch("/api/ai/settings", R"({"batch_size": 7})", if_match(read_tag));
    ASSERT_EQ(changed.status, 200u) << changed.body;
    EXPECT_NE(header(changed, "ETag"), read_tag);
    EXPECT_EQ(header(changed, "ETag"), etag("/api/ai/settings", browser_)) << "with the providers filled in, as the GET has them";
    const auto stale = patch("/api/ai/settings", R"({"batch_size": 8})", if_match(read_tag));
    EXPECT_EQ(stale.status, 412u);
    EXPECT_EQ(error_code(stale), "precondition_failed");
    EXPECT_EQ(read("/api/ai/settings").at("batch_size").as_int64(), 7);
    EXPECT_EQ(audited("ai_settings.update"), 1);
}

// D-059: each task names one of the configured providers, and the choice is a setting.
TEST_F(AnalysisApi, TheProviderPerTaskIsASetting) {
    const auto changed = patch_current("/api/ai/settings", R"({"generator_provider": "openai", "generator_model": "some-openai-model",
                                                       "request_extras": {"dashscope": {"enable_thinking": false}, "openai": {"seed": 7}}})");
    ASSERT_EQ(changed.status, 200u) << changed.body;
    EXPECT_EQ(json(changed).at("analyzer_provider").as_string(), "dashscope");
    EXPECT_EQ(json(changed).at("generator_provider").as_string(), "openai");
    EXPECT_EQ(audited("ai_settings.update"), 1);

    // The next review: the analyzer asks DashScope, the generator asks OpenAI, each with its own extras.
    set_industry(spa_, "wellness");
    await(pipeline_->process(store("r1", "FIVE", "Lovely!"), false, false));
    ASSERT_EQ(llm_.requests.size(), 2u);
    EXPECT_EQ(llm_.requests[0].provider, "dashscope");
    EXPECT_TRUE(llm_.requests[0].extras.contains("enable_thinking"));
    EXPECT_EQ(llm_.requests[1].provider, "openai");
    EXPECT_EQ(llm_.requests[1].model, "some-openai-model");
    EXPECT_FALSE(llm_.requests[1].extras.contains("enable_thinking")) << "another provider's option is not sent";
    EXPECT_EQ(llm_.requests[1].extras.at("seed").as_int64(), 7);

    // Only a provider that has an address and a key can be chosen.
    const auto unknown = patch_current("/api/ai/settings", R"({"analyzer_provider": "anthropic"})");
    EXPECT_EQ(unknown.status, 400u);
    EXPECT_THAT(error_message(unknown), HasSubstr("anthropic"));
    EXPECT_EQ(patch_current("/api/ai/settings", R"({"request_extras": {"dashscope": 5}})").status, 400u);
    EXPECT_EQ(audited("ai_settings.update"), 1);

    // If a chosen provider loses its key, the status says so and the sweep waits.
    api_options_.llm_providers = {"dashscope"};
    llm_.provider_names = {"dashscope"};
    build({});
    EXPECT_FALSE(read("/api/system/status").at("ai").at("llm_configured").as_bool());
    store("r2", "FIVE", "Also lovely!");
    EXPECT_EQ(await(pipeline_->enqueue_pending()), 0u);
}

// ---- reviews, analyses and drafts ------------------------------------------------

TEST_F(AnalysisApi, ReviewsCarryTheirAnalysisAndDraft) {
    set_industry(spa_, "wellness");
    const auto happy = store("happy", "FIVE", "Lovely head spa with Eva!");
    const auto replied = store("replied", "FIVE", "Great massage.", "Thank you!");
    const auto untouched = store("untouched", "FOUR", "Nice place.");
    const auto severe = store("severe", "TWO", "The towels were dirty and I still have a bruise.");
    await(pipeline_->process(happy, false, false));
    await(pipeline_->process(replied, false, false));
    llm_.analyzer = FakeLlm::always(R"({"language": "en", "sentiment": "negative", "contains_complaint": true, "topics": [{"id": "cleanliness", "polarity": "negative"}],
        "items": [], "severity_flags": [{"id": "injury", "evidence": "I still have a bruise"}], "manipulation_suspected": false, "summary_en": "Dirty towels and a bruise."})");
    await(pipeline_->process(severe, false, false));

    const auto review = read("/api/reviews/" + std::to_string(happy));
    EXPECT_EQ(review.at("analysis").at("sentiment").as_string(), "positive");
    EXPECT_TRUE(review.at("analysis").at("is_current").as_bool());
    EXPECT_EQ(review.at("draft").at("route").as_string(), "auto_reply");
    EXPECT_EQ(review.at("draft").at("status").as_string(), "draft");
    EXPECT_TRUE(review.at("draft").at("inputs").is_null()) << "inputs are left out where drafts are embedded";
    EXPECT_FALSE(review.at("draft").at("checks").as_array().empty());

    EXPECT_TRUE(read("/api/reviews/" + std::to_string(replied)).at("draft").is_null());
    EXPECT_FALSE(read("/api/reviews/" + std::to_string(replied)).at("analysis").is_null());
    EXPECT_TRUE(read("/api/reviews/" + std::to_string(untouched)).at("analysis").is_null());
    const auto flagged = read("/api/reviews/" + std::to_string(severe));
    EXPECT_EQ(flagged.at("draft").at("route").as_string(), "escalate");
    EXPECT_EQ(flagged.at("analysis").at("severity_flags").as_array().size(), 1u) << "what the analyzer raised";

    using testing::UnorderedElementsAre;
    EXPECT_THAT(review_ids("?analyzed=true"), UnorderedElementsAre("happy", "replied", "severe"));
    EXPECT_THAT(review_ids("?analyzed=false"), UnorderedElementsAre("untouched"));
    EXPECT_THAT(review_ids("?flagged=true"), UnorderedElementsAre("severe"));
    EXPECT_THAT(review_ids("?flagged=false"), UnorderedElementsAre("happy", "replied", "untouched"));
    EXPECT_THAT(review_ids("?sentiment=negative"), UnorderedElementsAre("severe"));
    EXPECT_THAT(review_ids("?has_draft=true"), UnorderedElementsAre("happy", "severe"));
    EXPECT_THAT(review_ids("?has_draft=false"), UnorderedElementsAre("replied", "untouched"));
    EXPECT_THAT(review_ids("?route=escalate"), UnorderedElementsAre("severe"));
    EXPECT_THAT(review_ids("?route=auto_reply&draft_status=draft"), UnorderedElementsAre("happy"));
    EXPECT_THAT(review_ids("?manipulation=true"), testing::IsEmpty());
    // The draft queue: live drafts, newest draft first.
    EXPECT_THAT(review_ids("?has_draft=true&draft_status=draft&sort=draft_time&order=desc"), testing::ElementsAre("severe", "happy"));
    for (const auto* bad : {"?route=nowhere", "?sentiment=angry", "?draft_status=sent", "?analyzed=maybe", "?sort=draft"}) {
        EXPECT_EQ(get(std::string("/api/reviews") + bad, browser_).status, 400u) << bad;
    }

    // The draft's inputs are on the review's draft list.
    const auto drafts = read("/api/reviews/" + std::to_string(happy) + "/drafts").at("items").as_array();
    ASSERT_EQ(drafts.size(), 1u);
    EXPECT_EQ(drafts[0].at("inputs").at("review").at("text").as_string(), "Lovely head spa with Eva!");
    EXPECT_EQ(drafts[0].at("inputs").at("mode").as_string(), "positive");
    EXPECT_TRUE(read("/api/reviews/" + std::to_string(replied) + "/drafts").at("items").as_array().empty());
    EXPECT_EQ(get("/api/reviews/999999/drafts", browser_).status, 404u);
}

TEST_F(AnalysisApi, StaffCanAskForAFreshAnalysis) {
    const auto id = store("r1", "FIVE", "Lovely!");
    const auto target = "/api/reviews/" + std::to_string(id) + "/analyze";
    // No industry yet: the review can't be analyzed.
    const auto refused = post(target);
    EXPECT_EQ(refused.status, 409u);
    EXPECT_EQ(error_code(refused), "not_analyzable");
    EXPECT_EQ(count("jobs WHERE kind = 'reviews.analyze'"), 0);

    set_industry(spa_, "wellness");
    const auto queued = post(target);
    ASSERT_EQ(queued.status, 202u) << queued.body;
    EXPECT_TRUE(json(queued).at("enqueued").as_bool());
    EXPECT_EQ(count("jobs WHERE kind = 'reviews.analyze' AND (payload->>'force')::boolean AND (payload->>'review_id')::bigint = " + std::to_string(id)), 1);
    EXPECT_EQ(audited("review.analysis_requested"), 1);
    EXPECT_EQ(post("/api/reviews/999999/analyze").status, 404u);
}

// ---- LLM usage and status --------------------------------------------------------

TEST_F(AnalysisApi, LlmUsageIsSummedAndListed) {
    tx([&](pqxx::work& tx) {
        for (int i = 0; i < 3; ++i) {
            repo::record_llm_call(tx, {.at = clock_.now() - std::chrono::hours(24 * i),
                                       .task = i == 0 ? "generator" : "analyzer",
                                       .model = "qwen3.7-plus",
                                       .attempt = 1,
                                       .ok = i != 2,
                                       .error = i == 2 ? "unavailable: HTTP 503" : "",
                                       .http_status = i == 2 ? 503u : 200u,
                                       .prompt_tokens = 1000,
                                       .completion_tokens = 100,
                                       .latency_ms = 3000});
        }
    });
    const auto usage = read("/api/llm/usage");
    EXPECT_EQ(usage.at("total").at("calls").as_int64(), 3);
    EXPECT_EQ(usage.at("total").at("errors").as_int64(), 1);
    EXPECT_EQ(usage.at("total").at("prompt_tokens").as_int64(), 3000);
    EXPECT_NEAR(usage.at("total").at("cost_usd").as_double(), 3 * 0.00056, 1e-9);
    EXPECT_EQ(usage.at("by_day").as_array().size(), 3u);
    EXPECT_EQ(usage.at("by_task").as_array().size(), 2u);
    EXPECT_EQ(usage.at("by_location").as_array()[0].at("label").as_string(), "No location") << "these calls name no location";
    // The default range is the last 30 days, today included.
    EXPECT_EQ(usage.at("from").as_string(), "2026-10-12T00:00:00Z");
    EXPECT_EQ(usage.at("to").as_string(), "2026-11-11T00:00:00Z");

    EXPECT_EQ(read("/api/llm/usage?from=2026-11-10&to=2026-11-10").at("total").at("calls").as_int64(), 1);
    EXPECT_EQ(read("/api/llm/usage?from=2026-11-01&to=2026-11-09").at("total").at("calls").as_int64(), 2);
    for (const auto* bad : {"?from=yesterday", "?to=2026-13-40", "?from=2026-11-10&to=2026-11-01", "?from=2020-01-01&to=2026-11-10"}) {
        EXPECT_EQ(get(std::string("/api/llm/usage") + bad, browser_).status, 400u) << bad;
    }

    const auto calls = read("/api/llm/calls");
    EXPECT_EQ(calls.at("total").as_int64(), 3);
    EXPECT_EQ(calls.at("items").as_array()[0].at("task").as_string(), "generator") << "newest first";
    const auto& items = calls.at("items").as_array();
    const auto failed = std::ranges::find_if(items, [](const boost::json::value& call) { return call.at("status").as_string() == "error"; });
    ASSERT_NE(failed, items.end());
    EXPECT_EQ(failed->at("error").as_string(), "unavailable: HTTP 503");
    EXPECT_EQ(std::ranges::count_if(items, [](const boost::json::value& call) { return call.at("status").as_string() == "error"; }), 1);
    EXPECT_EQ(read("/api/llm/calls?per_page=1").at("items").as_array().size(), 1u);
}

TEST_F(AnalysisApi, TheStatusPageShowsThePipeline) {
    set_industry(spa_, "wellness");
    const auto drafted = store("r1", "FIVE", "Lovely!");
    store("r2", "FIVE", "Also lovely!");
    await(pipeline_->process(drafted, false, false));

    const auto ai = read("/api/system/status").at("ai");
    EXPECT_TRUE(ai.at("llm_configured").as_bool()) << "both tasks name a provider this engine has";
    EXPECT_EQ(boost::json::serialize(ai.at("providers")), R"(["dashscope","openai"])");
    EXPECT_EQ(ai.at("analyzer_provider").as_string(), "dashscope");
    EXPECT_TRUE(ai.at("enabled").as_bool());
    EXPECT_EQ(ai.at("analyzer_model").as_string(), "qwen3.7-plus");
    EXPECT_EQ(ai.at("waiting_reviews").as_int64(), 1);
    EXPECT_EQ(ai.at("analyzed_reviews").as_int64(), 1);
    EXPECT_FALSE(ai.at("last_ok_at").is_null());
    EXPECT_TRUE(ai.at("last_error").is_null());
}
