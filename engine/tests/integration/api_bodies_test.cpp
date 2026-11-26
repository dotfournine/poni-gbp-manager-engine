// Request bodies are read as api/openapi.yaml describes them, the same on
// every route (D-092): a member the schema does not list, a null for a member
// that can't be empty and a missing required member are each a 400 that names
// the member, and a refused body changes nothing.

#include "api_fixture.hpp"

#include <gmock/gmock.h>

#include <map>
#include <string>
#include <vector>

using namespace poni;
using testing::HasSubstr;

namespace {

class Bodies : public test::ApiTest {
protected:
    void SetUp() override {
        ApiTest::SetUp();
        browser_ = sign_in();
        spa_and_buffet();
    }

    // The 400 the reader answers with, naming `member`.
    void expect_refused(const http::Response& response, std::string_view member, const std::string& what) {
        ASSERT_EQ(response.status, 400u) << what << ": " << response.body;
        EXPECT_EQ(error_code(response), "invalid_request") << what;
        EXPECT_THAT(error_message(response), HasSubstr(std::string(member))) << what;
    }

    std::string spa_path(std::string_view rest = "") { return std::format("/api/locations/{}{}", spa_, rest); }
    std::string client_path() { return std::format("/api/clients/{}", spa_client_); }
};

const std::string kPost = R"({"topic_type": "standard", "summary": "x", "language_code": "en", "call_to_action": null,
                              "event": null, "offer": null, "media_ids": [], "scheduled_time": null})";

// Every route that takes a JSON body, as the spec lists them, each with a member of its body that can't be
// null ("" where every member can). It is the body's first member, so that nothing is missing before it.
const std::map<std::string, std::string> kRoutesWithBody = {
    {"POST /api/clients", "name"},
    {"PATCH /api/clients/{id}", "name"},
    {"PATCH /api/locations/{id}", "enabled"},
    {"POST /api/reviews/{id}/ignore", "reason"},
    {"PUT /api/locations/{id}/content-settings", "business_facts"},
    {"PUT /api/locations/{id}/reply-settings", "reply_languages"},
    {"PATCH /api/ai/settings", "enabled"},
    {"PATCH /api/drafts/{id}", "text"},
    {"POST /api/drafts/{id}/approve", "text"},
    {"POST /api/drafts/{id}/reject", "reason"},
    {"POST /api/drafts/{id}/regenerate", "instructions"},
    {"POST /api/locations/{id}/reports", "period_start"},
    {"POST /api/reports/{id}/send", "recipients"},
    {"POST /api/report-schedules", "location_id"},
    {"PATCH /api/report-schedules/{id}", "frequency"},
    {"PUT /api/local-posts/{id}", "topic_type"},
    {"POST /api/local-posts/{id}/approve", "topic_type"},
    {"PUT /api/local-posts/{id}/edit", "content"},
    {"POST /api/local-posts/{id}/edit/approve", "topic_type"},
    {"POST /api/local-posts/{id}/reject", "reason"},
    {"POST /api/locations/{id}/local-posts", "topic_type"},
    {"POST /api/locations/{id}/local-post-drafts", "topic_type"},
    {"POST /api/escalations/{id}/assign", ""},
    {"POST /api/escalations/{id}/close", "reason"},
    {"POST /api/users", "directory_id"},
    {"POST /api/notification-rules", "event_type"},
    {"PATCH /api/notification-rules/{id}", "recipients"},
    {"POST /api/notifications/test", "recipient"},
    {"POST /api/devices", "apns_token"},
    {"PATCH /api/system/settings", "posting_enabled"},
};

// The changing routes whose schema has no JSON body: nothing is read from one.
const std::vector<std::string> kRoutesWithoutBody = {
    "POST /api/auth/logout",
    "POST /api/locations/discover",
    "POST /api/reviews/{id}/unignore",
    "POST /api/reviews/{id}/analyze",
    "POST /api/drafts/{id}/unschedule",
    "POST /api/system/auto-reply/resume",
    "POST /api/locations/{id}/auto-reply/resume",
    "POST /api/locations/{id}/go-live-test",
    "POST /api/go-live-runs/{id}/cancel",
    "POST /api/local-posts/{id}/remove",
    "POST /api/local-posts/{id}/copy",
    "POST /api/local-post-media",  // the body is the file
    "POST /api/locations/{id}/local-posts/refresh",
    "POST /api/locations/{id}/local-post-ideas",
    "POST /api/reports/{id}/discard",
    "DELETE /api/clients/{id}",
    "DELETE /api/notification-rules/{id}",
    "DELETE /api/devices/{id}",
    "DELETE /api/report-schedules/{id}",
    "DELETE /api/local-posts/{id}",
    "DELETE /api/local-posts/{id}/edit",
};

}  // namespace

TEST_F(Bodies, EveryRouteWithABodyRefusesWhatItsSchemaDoesNotAllow) {
    const auto audited = count("audit_log");
    const auto queued = count("jobs");
    std::size_t checked = 0;
    // An id nothing has: a body is refused before anything is looked up.
    for (const auto& route : user_routes("999999999")) {
        if (route.method == verb::get) continue;
        const auto with_body = kRoutesWithBody.find(route.name);
        if (with_body == kRoutesWithBody.end()) {
            EXPECT_THAT(kRoutesWithoutBody, testing::Contains(route.name)) << "a new route: say here whether it takes a JSON body";
            continue;
        }
        expect_refused(call(route.method, route.path, browser_, R"({"not_in_the_spec": true})"), "not_in_the_spec", route.name);
        if (const auto& firm = with_body->second; !firm.empty()) {
            const auto refused = call(route.method, route.path, browser_, boost::json::serialize(boost::json::object{{firm, nullptr}}));
            expect_refused(refused, firm, route.name + " with null");
            EXPECT_THAT(error_message(refused), HasSubstr("must not be null")) << route.name;
        }
        for (const auto* not_an_object : {"[]", "null", "{not json"}) {
            const auto refused = call(route.method, route.path, browser_, not_an_object);
            ASSERT_EQ(refused.status, 400u) << route.name << " " << not_an_object;
            EXPECT_EQ(error_message(refused), "The body must be a JSON object.") << route.name;
        }
        ++checked;
    }
    EXPECT_EQ(checked, kRoutesWithBody.size()) << "a route with a body is gone, or its pattern changed";
    EXPECT_EQ(count("audit_log"), audited) << "a refused body records nothing";
    EXPECT_EQ(count("jobs"), queued) << "and queues nothing";
}

TEST_F(Bodies, ARefusedAutoReplyBodySwitchesNothingOn) {
    const auto target = reply_settings_path(spa_);
    const auto on = [&] { return count(std::format("reply_settings WHERE location_id = {} AND auto_reply_enabled", spa_)); };
    const auto audited = count("audit_log");
    // The stored reply settings, switched on, with `member` set to a JSON value.
    const auto switched_on_with = [&](std::string_view member, std::string_view value) {
        auto body = read(target).as_object();
        body["auto_reply_enabled"] = true;
        body[member] = boost::json::parse(value);
        return put_current(target, boost::json::serialize(body));
    };

    expect_refused(switched_on_with("not_in_the_spec", "1"), "not_in_the_spec", "an unknown member");
    expect_refused(switched_on_with("auto_reply_daily_cap", "null"), "auto_reply_daily_cap", "null for a number");
    expect_refused(switched_on_with("auto_reply_enabled", "1"), "auto_reply_enabled", "a number for a switch");
    expect_refused(switched_on_with("auto_reply_enabled", R"("true")"), "auto_reply_enabled", "a string for a switch");
    expect_refused(switched_on_with("five_star", "true"), "five_star", "the per-star switch of before (D-110)");
    expect_refused(switched_on_with("auto_reply_min_stars", "3"), "auto_reply_min_stars", "three stars");
    EXPECT_EQ(on(), 0);
    EXPECT_EQ(count("audit_log"), audited);

    // The same switch without what the spec refuses is what a person sends.
    const auto taken = switched_on_with("auto_reply_min_stars", "4");
    ASSERT_EQ(taken.status, 200u) << taken.body;
    EXPECT_EQ(on(), 1);
}

TEST_F(Bodies, NullClearsWhatCanBeEmptyAndAMemberLeftOutStays) {
    // A location's industry and the day its analysis starts from.
    ASSERT_EQ(patch_current(spa_path(), R"({"industry_key": "wellness", "analyze_from": "2026-06-01"})").status, 200u);
    const auto kept = json(patch_current(spa_path(), R"({"posting_enabled": true})"));
    EXPECT_EQ(kept.at("industry_key").as_string(), "wellness");
    EXPECT_EQ(kept.at("analyze_from").as_string(), "2026-06-01");
    const auto cleared = json(patch_current(spa_path(), R"({"industry_key": null, "analyze_from": null})"));
    EXPECT_TRUE(cleared.at("industry_key").is_null());
    EXPECT_TRUE(cleared.at("analyze_from").is_null());
    // What can't be empty can't be cleared.
    expect_refused(patch(spa_path(), R"({"audience": "x"})"), "audience", "who the customers are, a content setting now");
    expect_refused(patch(spa_path(), R"({"enabled": null})"), "enabled", "a location's switch");
    expect_refused(patch(client_path(), R"({"notes": null})"), "notes", "a client's notes");
    expect_refused(patch(client_path(), R"({"archived": null})"), "archived", "a client's archived");
    // A day the calendar lacks is no day.
    EXPECT_EQ(patch_current(spa_path(), R"({"analyze_from": "2026-02-30"})").status, 400u);
    EXPECT_EQ(patch_current(spa_path(), R"({"analyze_from": "June"})").status, 400u);
    // A client has none of these any more (D-102).
    expect_refused(patch(client_path(), R"({"industry_key": "wellness"})"), "industry_key", "a client's industry");
    expect_refused(post("/api/clients", R"({"name": "Another", "analyze_from": null})"), "analyze_from", "a client's start day");

    // A location's client.
    EXPECT_EQ(json(patch_current(spa_path(), R"({"posting_enabled": true})")).at("client_id").as_int64(), spa_client_);
    EXPECT_TRUE(json(patch_current(spa_path(), R"({"client_id": null})")).at("client_id").is_null());
    expect_refused(patch(spa_path(), R"({"posting_enabled": null})"), "posting_enabled", "the kill switch");
    // How often reviews are polled is the engine's setting, not the location's (D-112).
    expect_refused(patch(spa_path(), R"({"review_poll_interval_s": 600})"), "review_poll_interval_s", "the old poll interval");
}

TEST_F(Bodies, AWholeNumberMayBeWrittenWithAFractionOfZero) {
    const auto linked = patch_current(spa_path(), std::format(R"({{"client_id": {}.0}})", buffet_client_));
    ASSERT_EQ(linked.status, 200u) << linked.body;
    EXPECT_EQ(json(linked).at("client_id").as_int64(), buffet_client_);
    expect_refused(patch(spa_path(), std::format(R"({{"client_id": {}.5}})", spa_client_)), "client_id", "a fraction");
    expect_refused(patch(spa_path(), R"({"client_id": "7"})"), "client_id", "a string");
}

TEST_F(Bodies, SettingsTravelWholeAndTakeOnlyTheirOwnMembersAtEveryDepth) {
    const auto content_path = content_settings_path(spa_);
    const auto reply_path = reply_settings_path(spa_);
    const auto stored = boost::json::serialize(read(content_path)) + boost::json::serialize(read(reply_path));
    // What the engine itself stores is a body it takes.
    for (const auto& path : {content_path, reply_path}) {
        const auto same = put_current(path, boost::json::serialize(read(path)));
        ASSERT_EQ(same.status, 200u) << path << ": " << same.body;
    }

    // The stored bundle with `member` replaced (a JSON value), or with it left out.
    const auto with = [&](const std::string& path, std::string_view member, std::string_view value) {
        auto body = read(path).as_object();
        body[member] = boost::json::parse(value);
        return put_current(path, boost::json::serialize(body));
    };
    const auto without = [&](const std::string& path, std::string_view member) {
        auto body = read(path).as_object();
        body.erase(member);
        return put_current(path, boost::json::serialize(body));
    };
    // Every member is sent: a save is the whole of the bundle (D-107).
    expect_refused(without(content_path, "audience"), "audience", "a member left out");
    expect_refused(without(content_path, "english_variant"), "english_variant", "a variant left out");
    expect_refused(without(reply_path, "signature"), "signature", "a member left out");
    expect_refused(without(reply_path, "auto_reply_min_stars"), "auto_reply_min_stars", "a rule left out");
    expect_refused(with(reply_path, "length", R"({"en": {"positive": {"min": 1, "max": 9}}})"), "length.en.rating_only",
                   "a kind of reply left out");
    // No member the schema does not have, at any depth, and no null.
    expect_refused(with(reply_path, "signatur", R"("Thanks")"), "signatur", "a misspelled member");
    expect_refused(with(reply_path, "length",
                        R"({"en": {"rating_only": {"min": 1, "max": 9}, "positive": {"min": 1, "max": 9}, "complaint": {"min": 1, "max": 9},
                                   "severity": {"min": 1, "max": 9, "size": 2}}})"),
                   "length.en.severity.size", "inside the lengths");
    expect_refused(with(reply_path, "signature", "null"), "signature", "a null member");
    expect_refused(with(content_path, "emoji", R"({"allowed": true})"), "emoji", "the emoji rule of before");
    expect_refused(with(content_path, "emoji_preferred", R"(["ok", null])"), "emoji_preferred", "a null entry");
    expect_refused(with(reply_path, "auto_reply", "true"), "auto_reply", "a rule it does not have");
    expect_refused(with(reply_path, "auto_reply_min_stars", "null"), "auto_reply_min_stars", "a null rule");
    expect_refused(with(content_path, "location_title", R"("Willow")"), "location_title", "what the old answer held");
    expect_refused(with(content_path, "client_id", std::to_string(spa_client_)), "client_id", "what the location holds");
    EXPECT_EQ(boost::json::serialize(read(content_path)) + boost::json::serialize(read(reply_path)), stored);
}

TEST_F(Bodies, APostsContentTravelsWhole) {
    const auto target = spa_path("/local-posts");
    const auto created = post(target, kPost);
    ASSERT_EQ(created.status, 201u) << created.body;
    const auto id = json(created).at("id").as_int64();

    auto content = boost::json::parse(kPost).as_object();
    content.erase("media_ids");
    expect_refused(post(target, boost::json::serialize(content)), "media_ids", "a member left out");
    // The approval carries every member too: a part of the content is no approval of it.
    expect_refused(post(std::format("/api/local-posts/{}/approve", id), boost::json::serialize(content)), "media_ids", "an approval in part");
    expect_refused(put(std::format("/api/local-posts/{}", id), boost::json::serialize(content)), "media_ids", "an edit in part");

    content = boost::json::parse(kPost).as_object();
    content["call_to_action"] = boost::json::object{{"action_type", "call"}};
    expect_refused(post(target, boost::json::serialize(content)), "call_to_action.url", "a button without its address member");
    content["call_to_action"] = boost::json::object{{"action_type", "call"}, {"url", nullptr}, {"label", "Call"}};
    expect_refused(post(target, boost::json::serialize(content)), "call_to_action.label", "a member the button does not have");
    content["call_to_action"] = boost::json::object{{"action_type", "call"}, {"url", nullptr}};
    EXPECT_EQ(post(target, boost::json::serialize(content)).status, 201u);

    // The model's help: the title is sent, as null where there is none.
    expect_refused(post(spa_path("/local-post-drafts"), R"({"topic_type": "standard", "language_code": "en", "brief": "x", "summary": ""})"),
                   "title", "a draft request without its title member");
}

TEST_F(Bodies, MembersOfAnotherBodyAreNotTaken) {
    // Each of these was ignored until D-092: the caller believed it was saved.
    expect_refused(post("/api/drafts/999999999/reject", R"({"reason": "x", "note": "y"})"), "note", "a note on a draft's rejection");
    expect_refused(patch("/api/notification-rules/999999999", R"({"enabled": true, "location_id": 3})"), "location_id", "a rule's location");
    expect_refused(patch("/api/notification-rules/999999999", R"({"event_type": "escalation"})"), "event_type", "a rule's event");
    expect_refused(patch("/api/report-schedules/999999999", R"({"location_id": 3})"), "location_id", "a schedule's location");
    // A schedule's weekday is dropped by the engine when the frequency does not use it; a caller can't clear it.
    expect_refused(patch("/api/report-schedules/999999999", R"({"weekday": null})"), "weekday", "a null weekday");
    // On a new schedule null says "none", as leaving it out does.
    const auto schedule = post("/api/report-schedules",
                               std::format(R"({{"location_id": {}, "frequency": "daily", "weekday": null, "month_day": null, "time": "09:00",
                                               "time_zone": "America/New_York"}})",
                                           spa_));
    EXPECT_EQ(schedule.status, 201u) << schedule.body;
    // The body may be left out where the spec says so, and is read like any other where it is sent.
    EXPECT_EQ(post("/api/reviews/999999999/ignore", "").status, 404u);
    expect_refused(post("/api/reviews/999999999/ignore", R"({"reason": 5})"), "reason", "a reason that is no text");
    expect_refused(post("/api/escalations/999999999/assign", "{}"), "assignee_id", "an assignment that names nobody and not null");
    EXPECT_EQ(post("/api/escalations/999999999/assign", R"({"assignee_id": null})").status, 404u);
}
