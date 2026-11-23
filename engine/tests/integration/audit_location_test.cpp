// An audit entry names the location it is about (D-103): audit::record reads
// it from the object when the entry is written, a delete names it because the
// row is gone, the table refuses an entry of a location's object without one,
// and the API serves it with the location's title and filters by it.

#include "api_fixture.hpp"

#include <gmock/gmock.h>

using namespace poni;

namespace {

class AuditLocation : public test::ApiTest {
protected:
    void SetUp() override {
        ApiTest::SetUp();
        browser_ = sign_in();
        spa_and_buffet();
        exec("UPDATE locations SET time_zone = 'America/New_York'");
        user_ = value<std::int64_t>("SELECT id FROM users WHERE email = 'staff@xvisionmediaus.com'");
    }

    // The location of the newest entry with this action.
    std::optional<std::int64_t> location_of(const std::string& action) {
        return value<std::optional<std::int64_t>>(
            std::format("SELECT location_id FROM audit_log WHERE action = '{}' ORDER BY id DESC LIMIT 1", action));
    }

    // Records an entry about the object as the engine's own, and returns its location.
    std::optional<std::int64_t> recorded(const std::string& object_type, const std::string& object_id) {
        const auto action = std::format("test.{}_{}", object_type, object_id);
        tx([&](pqxx::work& tx) {
            audit::record(tx, audit::Actor::system("test"), {.action = action, .object_type = object_type, .object_id = object_id},
                          clock_.now());
        });
        return location_of(action);
    }

    std::int64_t review(std::int64_t location, const std::string& gid) {
        return stored(std::format(
            "INSERT INTO reviews (location_id, gbp_review_id, gbp_name, rating, reviewer_name, created_time, updated_time, raw, "
            "first_seen_at, last_seen_at, last_changed_at) VALUES ({0}, '{1}', 'accounts/111/locations/x/reviews/{1}', 5, "
            "'A Reviewer', now(), now(), '{{}}', now(), now(), now()) RETURNING id",
            location, gid));
    }

    std::int64_t draft(std::int64_t review_id) {
        const auto analysis = stored(std::format(
            "INSERT INTO review_analyses (review_id, review_updated_time, status, language, script, sentiment, contains_complaint, "
            "topics, items, manipulation_suspected, industry_key, prompt_version, created_at) "
            "VALUES ({}, now(), 'ok', 'en', 'Latn', 'positive', false, '[]', '[]', false, 'wellness', 'test', now()) RETURNING id",
            review_id));
        return stored(std::format(
            "INSERT INTO reply_drafts (review_id, analysis_id, review_updated_time, route, mode, text, generated_text, language, "
            "generation_ok, model, prompt_version, inputs, created_at, updated_at) VALUES ({}, {}, now(), 'approval_queue', "
            "'positive', 'Thank you.', 'Thank you.', 'en', true, 'test', 'test', '{{}}', now(), now()) RETURNING id",
            review_id, analysis));
    }

    std::int64_t local_post(std::int64_t location) {
        return insert_local_post(location, "origin, status, content, created_by",
                                 std::format("'system', 'draft', {}, {}", test::content_sql(test::standard("Fall specials.")), user_));
    }

    std::int64_t created(const http::Response& response) {
        EXPECT_EQ(response.status, 201u) << response.body;
        return response.status == 201 ? json(response).at("id").as_int64() : 0;
    }
    std::int64_t report(std::int64_t location) {
        return created(post(std::format("/api/locations/{}/reports", location), R"({"period_start": "2026-10-01", "period_end": "2026-10-31"})"));
    }
    std::int64_t schedule(std::int64_t location) {
        return created(post("/api/report-schedules",
                            std::format(R"({{"location_id": {}, "frequency": "daily", "time": "09:00", "time_zone": "America/New_York"}})",
                                        location)));
    }
    std::int64_t rule(const std::string& location) {
        return created(post("/api/notification-rules",
                            std::format(R"({{"event_type": "escalation", "location_id": {}, "recipients": ["a@example.com"]}})", location)));
    }
};

TEST_F(AuditLocation, EachKindOfObjectIsRecordedWithItsLocation) {
    const auto spa_review = review(spa_, "r-spa");
    const auto buffet_review = review(buffet_, "r-buffet");
    const auto escalation = stored(std::format(
        "INSERT INTO cases (review_id, opened_by, opened_at, updated_at) VALUES ({}, 'severity_flag', now(), now()) RETURNING id",
        buffet_review));
    const auto spa_report = report(spa_);
    become_test_clients();  // a go-live setup needs the location's industry
    certify(buffet_);
    const auto run = value<std::int64_t>("SELECT max(id) FROM go_live_runs");

    EXPECT_EQ(recorded("location", std::to_string(buffet_)), buffet_);
    EXPECT_EQ(recorded("review", std::to_string(spa_review)), spa_);
    EXPECT_EQ(recorded("draft", std::to_string(draft(buffet_review))), buffet_);
    EXPECT_EQ(recorded("escalation", std::to_string(escalation)), buffet_);
    EXPECT_EQ(recorded("local_post", std::to_string(local_post(buffet_))), buffet_);
    EXPECT_EQ(recorded("report", std::to_string(spa_report)), spa_);
    EXPECT_EQ(recorded("report_schedule", std::to_string(schedule(buffet_))), buffet_);
    EXPECT_EQ(recorded("go_live_run", std::to_string(run)), buffet_);
    EXPECT_EQ(recorded("notification_rule", std::to_string(rule(std::to_string(spa_)))), spa_);

    // About no one location.
    EXPECT_EQ(recorded("notification_rule", std::to_string(rule("null"))), std::nullopt) << "a rule of every location";
    EXPECT_EQ(recorded("client", std::to_string(spa_client_)), std::nullopt) << "a client groups locations";
    EXPECT_EQ(recorded("user", std::to_string(user_)), std::nullopt);
    EXPECT_EQ(recorded("credential", "gbp"), std::nullopt) << "an id that is no number";
    EXPECT_EQ(recorded("location", ""), std::nullopt) << "location.discovery_requested names none";
}

TEST_F(AuditLocation, TheEnginesOwnEntriesNameTheirLocation) {
    // Written by the code paths themselves, not by the test.
    EXPECT_EQ(location_of("location.discovered"), buffet_) << "the newest of the two";
    EXPECT_EQ(location_of("location.enable"), buffet_);
    report(spa_);
    EXPECT_EQ(location_of("report.create"), spa_);
    schedule(buffet_);
    EXPECT_EQ(location_of("report_schedule.create"), buffet_);
}

TEST_F(AuditLocation, AnEntryOfADeleteKeepsTheLocationOfWhatWasDeleted) {
    const auto post_id = local_post(buffet_);
    ASSERT_EQ(del(std::format("/api/local-posts/{}", post_id)).status, 204u);
    EXPECT_EQ(location_of("local_post.delete"), buffet_);

    const auto schedule_id = schedule(spa_);
    ASSERT_EQ(del(std::format("/api/report-schedules/{}", schedule_id)).status, 204u);
    EXPECT_EQ(location_of("report_schedule.delete"), spa_);

    const auto rule_id = rule(std::to_string(buffet_));
    ASSERT_EQ(del(std::format("/api/notification-rules/{}", rule_id)).status, 204u);
    EXPECT_EQ(location_of("notification_rule.delete"), buffet_);
}

TEST_F(AuditLocation, AnEntryAboutALocationsObjectWithoutALocationIsRefused) {
    // A review that does not exist: no location can be read, and the table refuses the entry.
    EXPECT_THROW(recorded("review", "424242"), pqxx::check_violation);
    EXPECT_THROW(recorded("location", "424242"), pqxx::foreign_key_violation);
}

TEST_F(AuditLocation, TheLogIsReadForOneLocationWithItsTitle) {
    report(spa_);
    schedule(buffet_);

    const auto spa = read(std::format("/api/audit-log?location_id={}", spa_));
    ASSERT_GT(spa.at("total").as_int64(), 0);
    for (const auto& item : spa.at("items").as_array()) {
        EXPECT_EQ(item.at("location_id").as_int64(), spa_);
        EXPECT_EQ(item.at("location_title").as_string(), "LU DREAM SPA - Chelsea");
    }
    const auto actions = [&](const boost::json::value& list) {
        std::vector<std::string> out;
        for (const auto& item : list.at("items").as_array()) out.emplace_back(item.at("action").as_string());
        return out;
    };
    EXPECT_THAT(actions(spa), testing::Contains("report.create"));
    EXPECT_THAT(actions(spa), testing::Not(testing::Contains("report_schedule.create")));

    // An entry about no one location says so.
    const auto client = read(std::format("/api/audit-log?object_type=client&object_id={}", spa_client_)).at("items").as_array();
    ASSERT_FALSE(client.empty());
    EXPECT_TRUE(client.front().at("location_id").is_null());
    EXPECT_TRUE(client.front().at("location_title").is_null());

    EXPECT_EQ(read("/api/audit-log?location_id=424242").at("total").as_int64(), 0);
    EXPECT_EQ(get("/api/audit-log?location_id=spa", browser_).status, 400u);
}

}  // namespace
