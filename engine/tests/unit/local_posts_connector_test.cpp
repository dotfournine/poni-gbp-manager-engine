// The local posts connector against a scripted HTTP client and sanitized
// answers of Google's (fixtures/local-post*.json, from the spike on PONI's
// own profile): what it asks, how it reads the answers, and that a create
// and an update are never repeated by the connector. No update was ever sent
// to Google (D-099): its answers here are a post as Google returns one.

#include "connectors/gbp/local_posts.hpp"
#include "core/json.hpp"
#include "unit/connector_fixture.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/system/system_error.hpp>
#include <gmock/gmock.h>

#include <algorithm>
#include <format>
#include <functional>
#include <stdexcept>

using namespace poni;
using google::ApiError;
using test::standard;
using testing::HasSubstr;
using testing::Not;

using test::kTokenOk;

namespace {

class LocalPostsConnector : public test::ConnectorTest {
protected:
    LocalPostsConnector()
        : ConnectorTest({gbp::kLocalPostsApi, google::kOAuthApi}),
          tokens_(clock_, google::refresh_fetch(http_, limiter_, test::stored_credentials())),
          reader_(api_),
          writer_(api_) {}

    static boost::json::value fixture_json(const std::string& name) { return boost::json::parse(test::fixture(name)); }
    static std::string error_fixture(std::string_view which) {
        return boost::json::serialize(fixture_json("local-post-errors.json").at(which));
    }

    google::CachedTokenSource tokens_;
    google::Api api_{http_, tokens_, limiter_};
    gbp::HttpLocalPostReader reader_;
    gbp::HttpLocalPostWriter writer_;
};

}  // namespace

// ---- reading ---------------------------------------------------------------------------

TEST_F(LocalPostsConnector, AListIsOneGetOfTheLocationsPosts) {
    http_.reply(200, std::string(kTokenOk));
    http_.reply(200, test::fixture("local-posts-list.json"));
    const auto page = await(reader_.list_local_posts("111", "9001", std::string("next-1")));
    ASSERT_TRUE(page) << page.error().message;
    ASSERT_EQ(http_.requests.size(), 2u);
    const auto& request = http_.requests[1];
    EXPECT_EQ(request.method, http::ClientMethod::get);
    EXPECT_EQ(std::string_view(request.url.buffer()),
              "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts?pageSize=100&pageToken=next-1");
    EXPECT_EQ(http_.header(1, "Authorization"), "Bearer token-1");

    ASSERT_EQ(page->posts.size(), 5u);
    EXPECT_EQ(page->skipped, 0u);
    EXPECT_FALSE(page->next_page_token);
    EXPECT_EQ(page->posts[0].name, "accounts/111/locations/9001/localPosts/7002");
    EXPECT_EQ(page->posts[0].state, "SCHEDULED");
    EXPECT_EQ(page->posts[0].raw.at("topicType").as_string(), "OFFER") << "the payload is kept as Google sent it";
    ASSERT_TRUE(page->posts[3].media);
    EXPECT_EQ(page->posts[3].media->size(), 3u) << "a post made in Google's interface can carry several photos";
}

TEST_F(LocalPostsConnector, APostWithoutAUsableNameIsSkippedAndTheNextPageIsNamed) {
    http_.reply(200, std::string(kTokenOk));
    http_.reply(200, R"({"localPosts": [{"name": "accounts/111/locations/9001/localPosts/1", "state": "LIVE"},
                                         {"summary": "no name"},
                                         {"name": "accounts/111/locations/9001/localPosts/../../9002", "state": "LIVE"},
                                         {"name": "accounts/111/locations/9001/reviews/r1"}],
                         "nextPageToken": "page-2"})");
    const auto page = await(reader_.list_local_posts("111", "9001", std::nullopt));
    ASSERT_TRUE(page);
    EXPECT_EQ(page->posts.size(), 1u);
    EXPECT_EQ(page->skipped, 3u) << "one odd entry does not block a location's list";
    EXPECT_EQ(page->next_page_token, "page-2");
    EXPECT_EQ(std::string_view(http_.requests[1].url.buffer()),
              "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts?pageSize=100");

    // A location without posts answers {}.
    http_.reply(200, "{}");
    const auto empty = await(reader_.list_local_posts("111", "9001", std::nullopt));
    ASSERT_TRUE(empty);
    EXPECT_TRUE(empty->posts.empty());
}

TEST_F(LocalPostsConnector, OnePostIsReadByItsNameAndNotFoundMeansGone) {
    http_.reply(200, std::string(kTokenOk));
    http_.reply(200, test::fixture("local-post-created.json"));
    const auto post = await(reader_.get_local_post({"111", "9001", "7001"}));
    ASSERT_TRUE(post);
    EXPECT_EQ(std::string_view(http_.requests[1].url.buffer()), "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/7001");
    EXPECT_EQ(post->state, "PROCESSING");

    http_.reply(404, error_fixture("not_found"));
    const auto gone = await(reader_.get_local_post({"111", "9001", "7001"}));
    ASSERT_FALSE(gone);
    EXPECT_EQ(gone.error().kind, ApiError::Kind::not_found);

    http_.reply(503, "{}");
    const auto unavailable = await(reader_.get_local_post({"111", "9001", "7001"}));
    ASSERT_FALSE(unavailable);
    EXPECT_EQ(unavailable.error().kind, ApiError::Kind::unavailable) << "an error that is not NOT_FOUND says nothing about the post";
}

// ---- Google's spelling and the engine's ---------------------------------------------------

TEST_F(LocalPostsConnector, GooglesPostsAreReadIntoTheEnginesContent) {
    const auto posts = fixture_json("local-posts-list.json").at("localPosts").as_array();
    const auto read = [&](std::size_t i) { return *core::from_json<gbp::LocalPost>(posts[i]); };

    const auto offer = gbp::to_content(read(0));
    EXPECT_EQ(offer.topic_type, "offer");
    EXPECT_EQ(offer.summary, "Ten percent off lunch for the opening week.");
    EXPECT_EQ(offer.language_code, "en");
    ASSERT_TRUE(offer.event);
    EXPECT_EQ(offer.event->title, "Opening week lunch");
    EXPECT_EQ(offer.event->schedule, (domain::LocalPostSchedule{"2026-12-19", "18:00", "2026-12-19", "20:00"}));
    EXPECT_FALSE(offer.event->recurrence);
    ASSERT_TRUE(offer.offer);
    EXPECT_EQ(offer.offer->coupon_code, "FALL10");
    EXPECT_FALSE(offer.offer->redeem_online_url);
    EXPECT_EQ(offer.offer->terms_conditions, "Dine-in only. One per table.");
    EXPECT_EQ(offer.scheduled_time, "2026-12-19T17:00:00.000000Z");
    EXPECT_FALSE(offer.media_id) << "Google knows nothing of the engine's uploads";
    EXPECT_EQ(gbp::to_google_state(read(0)), "scheduled");

    const auto recurring = gbp::to_content(read(1));
    EXPECT_EQ(recurring.topic_type, "event");
    ASSERT_TRUE(recurring.event && recurring.event->recurrence);
    EXPECT_EQ(recurring.event->recurrence->frequency, "weekly");
    EXPECT_EQ(recurring.event->recurrence->until, "2027-01-31T00:00:00.000000Z");
    EXPECT_FALSE(recurring.offer) << "only an offer has the offer's fields";
    EXPECT_EQ(gbp::to_google_state(read(1)), "processing");

    const auto several_photos = gbp::to_content(read(3));
    EXPECT_EQ(several_photos.topic_type, "standard");
    ASSERT_TRUE(several_photos.call_to_action);
    EXPECT_EQ(several_photos.call_to_action->action_type, "call");
    EXPECT_FALSE(several_photos.call_to_action->url);
    EXPECT_FALSE(several_photos.event);
    EXPECT_EQ(gbp::to_google_state(read(3)), "live");

    // Made in Google's interface: no `offer` object, and a start time of {} (midnight).
    const auto made_at_google = gbp::to_content(read(4));
    EXPECT_EQ(made_at_google.topic_type, "offer");
    ASSERT_TRUE(made_at_google.event);
    EXPECT_EQ(made_at_google.event->schedule, (domain::LocalPostSchedule{"2026-08-28", "00:00", "2026-09-06", "22:00"}));
    ASSERT_TRUE(made_at_google.offer) << "an offer always has its three fields, set or not";
    EXPECT_FALSE(made_at_google.offer->coupon_code);
    EXPECT_FALSE(made_at_google.scheduled_time);
}

TEST_F(LocalPostsConnector, WhatGoogleLeavesOutOrTheEngineDoesNotKnowIsReadWithoutGuessing) {
    gbp::LocalPost bare;
    bare.name = "accounts/111/locations/9001/localPosts/1";
    const auto content = gbp::to_content(bare);
    EXPECT_EQ(content.topic_type, "standard");
    EXPECT_EQ(content.summary, "");
    EXPECT_FALSE(content.call_to_action);
    EXPECT_FALSE(gbp::to_google_state(bare));

    gbp::LocalPost odd = bare;
    odd.state = "LOCAL_POST_STATE_UNSPECIFIED";
    odd.topicType = "ALERT";
    odd.callToAction = gbp::CallToAction{.actionType = "GET_OFFER", .url = "https://example.com/x"};
    // An event whose dates Google did not send in full is not an event the engine can show.
    odd.event = gbp::PostEvent{.title = "Half an event",
                               .schedule = gbp::TimeInterval{.startDate = gbp::Date{2026, 12, 19}, .startTime = {}, .endDate = {}, .endTime = {}},
                               .recurrenceInfo = {}};
    const auto read = gbp::to_content(odd);
    EXPECT_FALSE(gbp::to_google_state(odd)) << "a state the engine does not know is no state";
    EXPECT_EQ(read.topic_type, "alert");
    EXPECT_EQ(read.call_to_action->action_type, "get_offer") << "as Google has it, in lower case";
    EXPECT_FALSE(read.event);
    EXPECT_FALSE(read.offer);
}

TEST_F(LocalPostsConnector, TheCreateBodySpellsTheContentGooglesWay) {
    auto content = standard();
    content.call_to_action = domain::LocalPostCallToAction{.action_type = "learn_more", .url = "https://example.com/fall"};
    content.scheduled_time = "2026-12-19T17:00:00.000000Z";
    EXPECT_EQ(gbp::create_body(content, gbp::MediaSource{"photo", "https://storage.googleapis.com/poni-public/p/abc.jpg"}),
              boost::json::parse(R"({
                  "languageCode": "en", "summary": "Fall specials start Monday.", "topicType": "STANDARD",
                  "callToAction": {"actionType": "LEARN_MORE", "url": "https://example.com/fall"},
                  "media": [{"mediaFormat": "PHOTO", "sourceUrl": "https://storage.googleapis.com/poni-public/p/abc.jpg"}],
                  "scheduledTime": "2026-12-19T17:00:00.000000Z"})")
                  .as_object());

    // The call button has no address; a post without media, button or time has none of those members.
    auto call = standard();
    call.call_to_action = domain::LocalPostCallToAction{.action_type = "call", .url = std::nullopt};
    EXPECT_EQ(gbp::create_body(call, std::nullopt),
              boost::json::parse(R"({"languageCode": "en", "summary": "Fall specials start Monday.", "topicType": "STANDARD",
                                     "callToAction": {"actionType": "CALL"}})")
                  .as_object());

    auto offer = standard("Ten percent off lunch.");
    offer.topic_type = "offer";
    offer.event = domain::LocalPostEvent{.title = "Lunch week",
                                         .schedule = {"2026-12-19", "11:00", "2026-12-26", "15:30"},
                                         .recurrence = domain::LocalPostRecurrence{"weekly", "2027-01-31T00:00:00.000000Z"}};
    offer.offer = domain::LocalPostOffer{.coupon_code = "LUNCH10", .redeem_online_url = std::nullopt, .terms_conditions = "Dine-in only."};
    EXPECT_EQ(gbp::create_body(offer, std::nullopt),
              boost::json::parse(R"({
                  "languageCode": "en", "summary": "Ten percent off lunch.", "topicType": "OFFER",
                  "event": {"title": "Lunch week",
                            "schedule": {"startDate": {"year": 2026, "month": 12, "day": 19}, "startTime": {"hours": 11, "minutes": 0},
                                         "endDate": {"year": 2026, "month": 12, "day": 26}, "endTime": {"hours": 15, "minutes": 30}},
                            "recurrenceInfo": {"weeklyPattern": {}, "seriesEndTime": "2027-01-31T00:00:00.000000Z"}},
                  "offer": {"couponCode": "LUNCH10", "termsConditions": "Dine-in only."}})")
                  .as_object());

    // Daily, and monthly on the day the event starts on.
    offer.event->recurrence->frequency = "daily";
    EXPECT_EQ(gbp::create_body(offer, std::nullopt).at("event").at("recurrenceInfo").at("dailyPattern"), boost::json::object{});
    offer.event->recurrence->frequency = "monthly";
    EXPECT_EQ(gbp::create_body(offer, std::nullopt).at("event").at("recurrenceInfo").at("monthlyPattern"),
              boost::json::parse(R"({"dayOfMonth": 19})"));
    // An offer with none of its fields set has no offer object, as at Google.
    offer.offer = domain::LocalPostOffer{};
    EXPECT_FALSE(gbp::create_body(offer, std::nullopt).contains("offer"));
}

TEST_F(LocalPostsConnector, WhatIsSentIsWhatIsReadBack) {
    auto event = standard("Live music every Friday.");
    event.topic_type = "event";
    event.event = domain::LocalPostEvent{.title = "Friday live music",
                                         .schedule = {"2026-12-19", "18:00", "2026-12-19", "20:00"},
                                         .recurrence = domain::LocalPostRecurrence{"weekly", "2027-01-31T00:00:00.000000Z"}};
    event.scheduled_time = "2026-12-19T17:00:00.000000Z";
    auto echoed = boost::json::value(gbp::create_body(event, std::nullopt));
    echoed.as_object()["name"] = "accounts/111/locations/9001/localPosts/1";
    EXPECT_EQ(gbp::to_content(*core::from_json<gbp::LocalPost>(echoed)), event);
}

TEST_F(LocalPostsConnector, APostsNameHasThreePlainIds) {
    const auto name = gbp::parse_local_post_name("accounts/111/locations/9001/localPosts/7856477975681504943");
    ASSERT_TRUE(name);
    EXPECT_EQ(name->account_id, "111");
    EXPECT_EQ(name->location_id, "9001");
    EXPECT_EQ(name->post_id, "7856477975681504943");
    EXPECT_EQ(name->str(), "accounts/111/locations/9001/localPosts/7856477975681504943");
    for (const auto* other : {"", "accounts/111/locations/9001/localPosts", "accounts/111/locations/9001/localPosts/",
                              "accounts/111/locations/9001/localPosts/1/x", "accounts/111/locations/9001/reviews/1",
                              "accounts/111/locations/9001/localPosts/..", "accounts/../locations/9001/localPosts/1",
                              "accounts/111/locations/9001/localPosts/1?x", "accounts/111/locations/9001/localPosts/a b",
                              "accounts/111/locations/9001/localPosts/1%2F2", "locations/9001/localPosts/1",
                              "accounts//locations/9001/localPosts/1"}) {
        EXPECT_FALSE(gbp::parse_local_post_name(other)) << other;
    }
}

// ---- the create ---------------------------------------------------------------------------

TEST_F(LocalPostsConnector, ACreateIsOnePostToTheLocationsPosts) {
    http_.reply(200, std::string(kTokenOk));
    http_.reply(200, test::fixture("local-post-created.json"));
    const auto body = gbp::create_body(standard(), std::nullopt);
    const auto created = await(writer_.create_local_post("111", "9001", body));
    ASSERT_TRUE(created) << created.error().message;
    EXPECT_EQ(created->name, "accounts/111/locations/9001/localPosts/7001");
    EXPECT_EQ(created->state, "PROCESSING") << "Google takes a post in before it shows or schedules it";

    ASSERT_EQ(http_.requests.size(), 2u);
    const auto& request = http_.requests[1];
    EXPECT_EQ(request.method, http::ClientMethod::post);
    EXPECT_EQ(std::string_view(request.url.buffer()), "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts");
    EXPECT_TRUE(http::is_local_post_collection_url(request.url));
    EXPECT_EQ(boost::json::parse(request.body).as_object(), body);
    EXPECT_EQ(request.content_type, "application/json");
    EXPECT_EQ(http_.header(1, "Authorization"), "Bearer token-1");
}

TEST_F(LocalPostsConnector, AValidationErrorNamesTheFieldAndTheLimitAndNotTheText) {
    http_.reply(200, std::string(kTokenOk));
    http_.reply(400, error_fixture("summary_too_long"));
    const auto too_long = await(writer_.create_local_post("111", "9001", gbp::create_body(standard(), std::nullopt)));
    ASSERT_FALSE(too_long);
    EXPECT_EQ(too_long.error().kind, ApiError::Kind::other) << "a refusal: nothing was created";
    EXPECT_THAT(too_long.error().message, HasSubstr("summary: Must be at most 1500 characters"));
    EXPECT_THAT(too_long.error().message, Not(HasSubstr("the text that was sent"))) << "Google's echo of the text is left out";

    http_.reply(400, error_fixture("title_too_long"));
    EXPECT_THAT(await(writer_.create_local_post("111", "9001", {})).error().message, HasSubstr("event.title: Must be at most 58 characters"));
    http_.reply(400, error_fixture("scheduled_in_the_past"));
    EXPECT_THAT(await(writer_.create_local_post("111", "9001", {})).error().message,
                HasSubstr("scheduled_time: Scheduled time cannot be in the past."));
    http_.reply(400, error_fixture("too_many_photos"));
    EXPECT_THAT(await(writer_.create_local_post("111", "9001", {})).error().message, HasSubstr("media: Too many photos."));
    EXPECT_EQ(http_.requests.size(), 5u) << "one request per call: a refusal is not tried again here";
}

TEST_F(LocalPostsConnector, ACreateIsNeverSentTwiceByTheConnector) {
    const auto posts_sent = [&] {
        return std::ranges::count_if(http_.requests, [](const auto& r) { return r.method == http::ClientMethod::post && http::is_gbp_host(r.url); });
    };
    http_.reply(200, std::string(kTokenOk));

    // A 5xx says nothing about whether the post exists. It is reported; whether anything follows is the caller's call.
    http_.reply(503, "{}");
    const auto unavailable = await(writer_.create_local_post("111", "9001", {}));
    ASSERT_FALSE(unavailable);
    EXPECT_EQ(unavailable.error().kind, ApiError::Kind::unavailable);
    EXPECT_EQ(posts_sent(), 1);

    // Accepted, with an answer that can't be read: the post exists, and the caller is told it can't know its name.
    http_.reply(200, "<html>proxy</html>");
    const auto unreadable = await(writer_.create_local_post("111", "9001", {}));
    ASSERT_FALSE(unreadable);
    EXPECT_EQ(unreadable.error().kind, ApiError::Kind::invalid_response);
    http_.reply(200, R"({"state": "PROCESSING"})");
    EXPECT_EQ(await(writer_.create_local_post("111", "9001", {})).error().kind, ApiError::Kind::invalid_response);
    EXPECT_EQ(posts_sent(), 3);

    // Rate limited: nothing was created. Still one request; the job decides about another.
    http_.reply(429, R"({"error": {"status": "RESOURCE_EXHAUSTED", "message": "Quota exceeded"}})");
    EXPECT_EQ(await(writer_.create_local_post("111", "9001", {})).error().kind, ApiError::Kind::rate_limited);
    EXPECT_EQ(posts_sent(), 4);

    // 401: Google did nothing, so one more try with a new token is safe.
    limiter_.configure(std::string(gbp::kLocalPostsApi), {.per_minute = 6000, .burst = 100});
    clock_.advance(std::chrono::seconds(61));
    http_.reply(401, "{}");
    http_.reply(200, R"({"access_token": "token-2", "expires_in": 3600})");
    http_.reply(200, test::fixture("local-post-created.json"));
    ASSERT_TRUE(await(writer_.create_local_post("111", "9001", {})));
    EXPECT_EQ(posts_sent(), 6);
    EXPECT_EQ(http_.header(http_.requests.size() - 1, "Authorization"), "Bearer token-2");
}

TEST_F(LocalPostsConnector, ACreateOrADeleteWhoseAnswerNeverComesThrows) {
    const auto posts_sent = [&] {
        return std::ranges::count_if(http_.requests, [](const auto& r) { return r.method == http::ClientMethod::post && http::is_gbp_host(r.url); });
    };
    http_.reply(200, std::string(kTokenOk));
    // Not an ApiError: jobs/local_posts.cpp takes a throw for "the outcome is unknown" and never sends the create again.
    http_.fail();
    EXPECT_THROW(await(writer_.create_local_post("111", "9001", gbp::create_body(standard(), std::nullopt))), boost::system::system_error);
    EXPECT_EQ(posts_sent(), 1);
    // After a refused token the second create is the one in doubt, and there is no third.
    http_.reply(401, "{}");
    http_.reply(200, R"({"access_token": "token-2", "expires_in": 3600})");
    http_.fail();
    EXPECT_THROW(await(writer_.create_local_post("111", "9001", gbp::create_body(standard(), std::nullopt))), boost::system::system_error);
    EXPECT_EQ(posts_sent(), 3);
    // Twice refused for its token: Google did nothing, and the caller is told so with the status it decides from.
    http_.reply(401, "{}");
    http_.reply(200, R"({"access_token": "token-3", "expires_in": 3600})");
    http_.reply(401, "{}");
    const auto refused = await(writer_.create_local_post("111", "9001", gbp::create_body(standard(), std::nullopt)));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().http_status, 401u);
    EXPECT_EQ(posts_sent(), 5);

    http_.fail();
    EXPECT_THROW(await(writer_.delete_local_post({"111", "9001", "7001"})), boost::system::system_error);
    http_.fail();
    EXPECT_THROW(await(reader_.get_local_post({"111", "9001", "7001"})), boost::system::system_error);
}

// ---- the delete ---------------------------------------------------------------------------

TEST_F(LocalPostsConnector, ADeleteIsOneDeleteOfThatPostAndCarriesNothing) {
    http_.reply(200, std::string(kTokenOk));
    http_.reply(200, "{}");
    const auto removed = await(writer_.delete_local_post({"111", "9001", "7001"}));
    ASSERT_TRUE(removed) << removed.error().message;
    ASSERT_EQ(http_.requests.size(), 2u);
    const auto& request = http_.requests[1];
    EXPECT_EQ(request.method, http::ClientMethod::del);
    EXPECT_EQ(std::string_view(request.url.buffer()), "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/7001");
    EXPECT_TRUE(http::is_local_post_url(request.url));
    EXPECT_TRUE(request.body.empty());

    // Already gone: the caller sees not_found and takes it as done.
    http_.reply(404, error_fixture("not_found"));
    const auto gone = await(writer_.delete_local_post({"111", "9001", "7001"}));
    ASSERT_FALSE(gone);
    EXPECT_EQ(gone.error().kind, ApiError::Kind::not_found);

    http_.reply(403, R"({"error": {"status": "PERMISSION_DENIED", "message": "no"}})");
    EXPECT_EQ(await(writer_.delete_local_post({"111", "9001", "7001"})).error().kind, ApiError::Kind::permission_denied);
}

TEST_F(LocalPostsConnector, AnIdCannotChangeWhatIsDeletedOrWhereAPostIsCreated) {
    http_.reply(200, std::string(kTokenOk));
    // Whatever a caller hands over, the address is built segment by segment and the outbound client's rule refuses the result.
    for (const auto& name : {gbp::LocalPostName{"111", "9001", ".."}, gbp::LocalPostName{"111", "9001", "7001/../../../locations/9002"},
                             gbp::LocalPostName{"111", "..", "7001"}, gbp::LocalPostName{"111", "9001", ""},
                             gbp::LocalPostName{"111", "9001", "7001?force=true"}}) {
        EXPECT_THROW(await(writer_.delete_local_post(name)), std::invalid_argument) << name.str();
    }
    EXPECT_THROW(await(writer_.create_local_post("111", "9001/../9002", {})), std::invalid_argument);
    EXPECT_THROW(await(writer_.create_local_post("111", "..", {})), std::invalid_argument);
    for (const auto& request : http_.requests) {
        EXPECT_EQ(request.method, http::ClientMethod::post) << "only the token request went out";
        EXPECT_FALSE(http::is_gbp_host(request.url));
    }
    EXPECT_EQ(http_.refused.size(), 7u);
}

// ---- the update (D-099) -------------------------------------------------------------------------

namespace {

// The post of the list fixture with that id, as the reader maps it.
gbp::LocalPost listed(std::string_view post_id) {
    for (const auto& value : boost::json::parse(test::fixture("local-posts-list.json")).at("localPosts").as_array()) {
        auto post = core::from_json<gbp::LocalPost>(value).value();
        if (!post.name.ends_with(std::string("/") + std::string(post_id))) continue;
        post.raw = value;
        return post;
    }
    throw std::logic_error("the fixture has no such post");
}

// The post's content as the engine holds it, with `change` made: an edit a person wrote.
domain::LocalPostContent edit_of(const gbp::LocalPost& post, const std::function<void(domain::LocalPostContent&)>& change) {
    auto content = gbp::to_content(post);
    change(content);
    return domain::normalize_local_post(content).value();
}

boost::json::object body_for(const gbp::LocalPost& post, const domain::LocalPostContent& edit, const std::optional<gbp::MediaSource>& media = {}) {
    return gbp::update_body(post, edit, domain::local_post_edit_mask(gbp::to_content(post), edit).value(), media);
}

}  // namespace

TEST_F(LocalPostsConnector, AnUpdateIsOnePatchOfThatPostWithTheFieldsItsMaskNames) {
    http_.reply(200, std::string(kTokenOk));
    http_.reply(200, test::fixture("local-post-created.json"));
    const boost::json::object body{{"summary", "Live music every Friday evening through February."}, {"callToAction", {{"actionType", "CALL"}}}};
    const auto updated = await(writer_.update_local_post({"111", "9001", "7001"}, {"summary", "callToAction"}, body));
    ASSERT_TRUE(updated) << updated.error().message;
    EXPECT_EQ(updated->name, "accounts/111/locations/9001/localPosts/7001") << "the post as Google has it after the edit";
    EXPECT_EQ(updated->raw.at("topicType").as_string(), "EVENT") << "the payload is kept as Google sent it";

    ASSERT_EQ(http_.requests.size(), 2u);
    const auto& request = http_.requests[1];
    EXPECT_EQ(request.method, http::ClientMethod::patch);
    EXPECT_EQ(std::string_view(request.url.buffer()),
              "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/7001?updateMask=summary,callToAction");
    EXPECT_TRUE(http::is_local_post_update_url(request.url));
    EXPECT_EQ(boost::json::parse(request.body).as_object(), body) << "what the mask names, and nothing else";
    EXPECT_EQ(request.content_type, "application/json");
    EXPECT_EQ(http_.header(1, "Authorization"), "Bearer token-1");

    // Each field alone travels under its own name.
    for (const auto field : domain::kLocalPostEditFields) {
        http_.reply(200, test::fixture("local-post-created.json"));
        ASSERT_TRUE(await(writer_.update_local_post({"111", "9001", "7001"}, {field}, {})));
        EXPECT_EQ(std::string_view(http_.requests.back().url.buffer()),
                  std::format("https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/7001?updateMask={}", field));
    }
}

TEST_F(LocalPostsConnector, TheUpdateBodyHoldsTheFieldsTheMaskNamesAndNoOther) {
    // A live post made at Google, with a call button and three photos of Google's own.
    const auto post = listed("7004");
    EXPECT_EQ(body_for(post, edit_of(post, [](auto& c) { c.summary = "Now open late every day."; })),
              boost::json::parse(R"({"summary": "Now open late every day."})").as_object())
        << "the button and the photos are not in the request: they stay as they are";
    EXPECT_EQ(body_for(post, edit_of(post, [](auto& c) { c.language_code = "es"; })), boost::json::parse(R"({"languageCode": "es"})").as_object());
    EXPECT_EQ(body_for(post, edit_of(post, [](auto& c) {
                  c.call_to_action = domain::LocalPostCallToAction{.action_type = "book", .url = "https://example.com/book"};
              })),
              boost::json::parse(R"({"callToAction": {"actionType": "BOOK", "url": "https://example.com/book"}})").as_object());
    // The button taken away: the mask names it and the body leaves it out, which clears it.
    const auto no_button = edit_of(post, [](auto& c) { c.call_to_action.reset(); });
    EXPECT_THAT(domain::local_post_edit_mask(gbp::to_content(post), no_button).value(), testing::ElementsAre("callToAction"));
    EXPECT_TRUE(body_for(post, no_button).empty());

    // An upload replaces what the post shows; taking the post's own upload away sends the media empty.
    auto with_upload = gbp::to_content(post);
    with_upload.media_id = 7;
    EXPECT_EQ(gbp::update_body(post, with_upload, {"media"}, gbp::MediaSource{"photo", "https://storage.googleapis.com/poni-public/p/abc.jpg"}),
              boost::json::parse(R"({"media": [{"mediaFormat": "PHOTO", "sourceUrl": "https://storage.googleapis.com/poni-public/p/abc.jpg"}]})")
                  .as_object());
    EXPECT_EQ(gbp::update_body(post, gbp::to_content(post), {"media"}, std::nullopt), boost::json::parse(R"({"media": []})").as_object());

    // Two fields at once, and an empty mask sends nothing at all.
    EXPECT_EQ(body_for(post, edit_of(post, [](auto& c) {
                  c.summary = "Now open late every day.";
                  c.language_code = "es";
              })),
              boost::json::parse(R"({"summary": "Now open late every day.", "languageCode": "es"})").as_object());
    EXPECT_TRUE(gbp::update_body(post, gbp::to_content(post), {}, std::nullopt).empty());
}

TEST_F(LocalPostsConnector, AnEventGoesBackWithItsRepeatAndItsUnchangedTimesAsGoogleHoldsThem) {
    // A repeating event: its repeat is Google's, with whatever the pattern holds.
    auto repeating = listed("7001");
    repeating.raw.as_object()["event"].as_object()["recurrenceInfo"] =
        boost::json::parse(R"({"weeklyPattern": {"daysOfWeek": ["FRIDAY", "SATURDAY"]}, "seriesEndTime": "2027-01-31T00:00:00Z"})");
    repeating = [&] {
        auto mapped = core::from_json<gbp::LocalPost>(repeating.raw).value();
        mapped.raw = repeating.raw;
        return mapped;
    }();
    const auto retitled = body_for(repeating, edit_of(repeating, [](auto& c) { c.event->title = "Friday and Saturday jazz"; }));
    EXPECT_EQ(retitled, boost::json::parse(R"({"event": {
                  "title": "Friday and Saturday jazz",
                  "schedule": {"startDate": {"year": 2026, "month": 12, "day": 19}, "startTime": {"hours": 18},
                               "endDate": {"year": 2026, "month": 12, "day": 19}, "endTime": {"hours": 20}},
                  "recurrenceInfo": {"weeklyPattern": {"daysOfWeek": ["FRIDAY", "SATURDAY"]}, "seriesEndTime": "2027-01-31T00:00:00Z"}}})")
                            .as_object())
        << "the weekdays the engine does not hold, and the times as Google wrote them, are sent back as they came";

    // New days and times are the edit's; the repeat still is not.
    const auto moved = body_for(repeating, edit_of(repeating, [](auto& c) { c.event->schedule.end_time = "21:30"; }));
    EXPECT_EQ(moved.at("event").at("schedule"),
              boost::json::parse(R"({"startDate": {"year": 2026, "month": 12, "day": 19}, "startTime": {"hours": 18, "minutes": 0},
                                     "endDate": {"year": 2026, "month": 12, "day": 19}, "endTime": {"hours": 21, "minutes": 30}})"));
    EXPECT_EQ(moved.at("event").at("recurrenceInfo"), repeating.raw.at("event").at("recurrenceInfo"));
    EXPECT_EQ(moved.at("event").at("title").as_string(), "Friday live music");

    // An offer whose start has no time at Google: a new title does not give it one.
    const auto offer = listed("7005");
    const auto renamed = body_for(offer, edit_of(offer, [](auto& c) { c.event->title = "Weekend special for four"; }));
    EXPECT_EQ(renamed.at("event").at("schedule"), offer.raw.at("event").at("schedule"));
    EXPECT_EQ(renamed.at("event").at("schedule").at("startTime"), boost::json::object{});
    EXPECT_FALSE(renamed.at("event").as_object().contains("recurrenceInfo")) << "nor a repeat";
    // An offer's three parts are written whole, and one that says nothing is left out, which clears it.
    EXPECT_EQ(body_for(offer, edit_of(offer, [](auto& c) { c.offer->coupon_code = "WEEKEND"; })),
              boost::json::parse(R"({"offer": {"couponCode": "WEEKEND"}})").as_object());
    const auto with_terms = listed("7002");
    EXPECT_EQ(body_for(with_terms, edit_of(with_terms, [](auto& c) { c.offer->coupon_code.reset(); })),
              boost::json::parse(R"({"offer": {"termsConditions": "Dine-in only. One per table."}})").as_object());
    EXPECT_TRUE(body_for(with_terms, edit_of(with_terms, [](auto& c) { c.offer = domain::LocalPostOffer{}; })).empty());
}

TEST_F(LocalPostsConnector, AnUpdateIsNeverSentTwiceByTheConnector) {
    const auto patches_sent = [&] {
        return std::ranges::count_if(http_.requests, [](const auto& r) { return r.method == http::ClientMethod::patch; });
    };
    const auto update = [&] { return await(writer_.update_local_post({"111", "9001", "7001"}, {"summary"}, {{"summary", "New text."}})); };
    http_.reply(200, std::string(kTokenOk));

    // A refusal says what Google would not take; nothing changed.
    http_.reply(400, error_fixture("summary_too_long"));
    const auto too_long = update();
    ASSERT_FALSE(too_long);
    EXPECT_EQ(too_long.error().http_status, 400u);
    EXPECT_THAT(too_long.error().message, HasSubstr("summary: Must be at most 1500 characters"));
    http_.reply(404, error_fixture("not_found"));
    EXPECT_EQ(update().error().kind, ApiError::Kind::not_found);
    http_.reply(403, R"({"error": {"status": "PERMISSION_DENIED", "message": "no"}})");
    EXPECT_EQ(update().error().kind, ApiError::Kind::permission_denied);
    EXPECT_EQ(patches_sent(), 3);

    // A 5xx says nothing about whether the post changed. It is reported; whether anything follows is the caller's call.
    http_.reply(503, "{}");
    const auto unavailable = update();
    ASSERT_FALSE(unavailable);
    EXPECT_EQ(unavailable.error().kind, ApiError::Kind::unavailable);
    EXPECT_EQ(patches_sent(), 4);

    // Accepted, with an answer that can't be read: the post changed, and the caller is told it can't see how.
    http_.reply(200, "<html>proxy</html>");
    EXPECT_EQ(update().error().kind, ApiError::Kind::invalid_response);
    http_.reply(200, R"({"state": "PROCESSING"})");
    EXPECT_EQ(update().error().kind, ApiError::Kind::invalid_response);
    EXPECT_EQ(patches_sent(), 6);

    // Rate limited: nothing changed. Still one request; the job decides about another.
    http_.reply(429, R"({"error": {"status": "RESOURCE_EXHAUSTED", "message": "Quota exceeded"}})");
    EXPECT_EQ(update().error().kind, ApiError::Kind::rate_limited);
    EXPECT_EQ(patches_sent(), 7);

    // 401: Google did nothing, so one more try with a new token is safe. A second 401 is the answer.
    limiter_.configure(std::string(gbp::kLocalPostsApi), {.per_minute = 6000, .burst = 100});
    clock_.advance(std::chrono::seconds(61));
    http_.reply(401, "{}");
    http_.reply(200, R"({"access_token": "token-2", "expires_in": 3600})");
    http_.reply(200, test::fixture("local-post-created.json"));
    ASSERT_TRUE(update());
    EXPECT_EQ(patches_sent(), 9);
    EXPECT_EQ(http_.header(http_.requests.size() - 1, "Authorization"), "Bearer token-2");

    // No answer at all throws: jobs/local_posts.cpp takes that for "the outcome is unknown" and sends nothing again.
    http_.fail();
    EXPECT_THROW(update(), boost::system::system_error);
    EXPECT_EQ(patches_sent(), 10);
}

TEST_F(LocalPostsConnector, NeitherAnIdNorAMaskCanChangeWhatAnUpdateIs) {
    http_.reply(200, std::string(kTokenOk));
    // Whatever a caller hands over, the address is built part by part and the outbound client's rule refuses the result.
    for (const auto& name : {gbp::LocalPostName{"111", "9001", ".."}, gbp::LocalPostName{"111", "9001", "7001/../../../locations/9002"},
                             gbp::LocalPostName{"111", "..", "7001"}, gbp::LocalPostName{"111", "9001", ""},
                             gbp::LocalPostName{"111", "9001", "7001?updateMask=topicType"}}) {
        EXPECT_THROW(await(writer_.update_local_post(name, {"summary"}, {})), std::invalid_argument) << name.str();
    }
    // A mask of anything but the six names, each once: the kind, the time, nothing, a second parameter.
    for (const std::vector<std::string_view>& mask : std::vector<std::vector<std::string_view>>{
             {},
             {""},
             {"topicType"},
             {"scheduledTime"},
             {"summary", "scheduledTime"},
             {"summary", "summary"},
             {"*"},
             {"summary&validateOnly=true"},
             {"summary#x"},
             {"summary,topicType"},
             {"summary "},
             {"state"},
         }) {
        EXPECT_THROW(await(writer_.update_local_post({"111", "9001", "7001"}, mask, {})), std::invalid_argument);
    }
    for (const auto& request : http_.requests) {
        EXPECT_EQ(request.method, http::ClientMethod::post) << "only the token request went out";
        EXPECT_FALSE(http::is_gbp_host(request.url));
    }
    EXPECT_EQ(http_.refused.size(), 17u);
    for (const auto& refused : http_.refused) EXPECT_EQ(refused.url.segments().size(), 7u) << "nothing a caller gave became another part of the address";
}

// ---- the stand-in for Google that the integration tests use ---------------------------------

TEST_F(LocalPostsConnector, TheFakeHoldsANewPostInProcessingAsGoogleDoes) {
    test::FakeLocalPosts google;
    auto scheduled = standard("Opening soon.");
    scheduled.scheduled_time = "2026-12-19T17:00:00.000000Z";
    const auto created = await(google.create_local_post("111", "9001", gbp::create_body(scheduled, gbp::MediaSource{"photo", "https://example.com/p/a.jpg"})));
    ASSERT_TRUE(created);
    EXPECT_EQ(created->name, "accounts/111/locations/9001/localPosts/7001");
    EXPECT_EQ(created->state, "PROCESSING");
    EXPECT_EQ(created->createTime, google.now);
    ASSERT_TRUE(created->media);
    EXPECT_THAT(created->media->at(0).googleUrl.value(), HasSubstr("googleusercontent.com")) << "Google serves the file from its own address";
    ASSERT_TRUE(await(google.create_local_post("111", "9001", gbp::create_body(standard("Now."), std::nullopt))));

    // Listed and readable while it is processing, newest first.
    auto page = await(google.list_local_posts("111", "9001", std::nullopt));
    ASSERT_EQ(page->posts.size(), 2u);
    EXPECT_EQ(page->posts[0].summary, "Now.");
    EXPECT_EQ(gbp::to_content(page->posts[1]).scheduled_time, "2026-12-19T17:00:00.000000Z");
    EXPECT_EQ(await(google.get_local_post({"111", "9001", "7001"}))->state, "PROCESSING");

    google.finish_processing();
    EXPECT_EQ(google.find("accounts/111/locations/9001/localPosts/7001")->state, "SCHEDULED");
    EXPECT_EQ(google.find("accounts/111/locations/9001/localPosts/7002")->state, "LIVE");
    google.reject("accounts/111/locations/9001/localPosts/7002");
    EXPECT_EQ(await(google.get_local_post({"111", "9001", "7002"}))->raw.at("state").as_string(), "REJECTED");

    // Pages, and a post that no list shows.
    google.page_size = 1;
    page = await(google.list_local_posts("111", "9001", std::nullopt));
    ASSERT_TRUE(page->next_page_token);
    EXPECT_EQ(await(google.list_local_posts("111", "9001", page->next_page_token))->posts.size(), 1u);
    google.page_size = 100;
    google.hidden_from_lists.insert("accounts/111/locations/9001/localPosts/7001");
    EXPECT_EQ(await(google.list_local_posts("111", "9001", std::nullopt))->posts.size(), 1u);
    EXPECT_TRUE(await(google.get_local_post({"111", "9001", "7001"}))) << "a list that lacks a post says nothing about it";

    // A delete removes it; a second one answers NOT_FOUND, as Google does.
    ASSERT_TRUE(await(google.delete_local_post({"111", "9001", "7001"})));
    EXPECT_EQ(await(google.delete_local_post({"111", "9001", "7001"})).error().kind, ApiError::Kind::not_found);
    EXPECT_EQ(await(google.get_local_post({"111", "9001", "7001"})).error().kind, ApiError::Kind::not_found);
    EXPECT_EQ(google.deletes.size(), 2u);

    // A create that is refused or lost stores nothing; one that times out after arriving does.
    google.fail_create = ApiError{ApiError::Kind::other, 400, "HTTP 400 INVALID_ARGUMENT"};
    EXPECT_FALSE(await(google.create_local_post("111", "9001", {})));
    google.fail_create.reset();
    google.create_times_out = true;
    EXPECT_THROW(await(google.create_local_post("111", "9001", gbp::create_body(standard("Lost."), std::nullopt))), boost::system::system_error);
    EXPECT_EQ(google.posts["9001"].size(), 1u);
    google.timeout_after_storing = true;
    EXPECT_THROW(await(google.create_local_post("111", "9001", gbp::create_body(standard("Arrived."), std::nullopt))), boost::system::system_error);
    EXPECT_EQ(google.posts["9001"].size(), 2u);
    EXPECT_EQ(google.creates.size(), 5u) << "every create that reached Google is counted, whatever became of it";
}

TEST_F(LocalPostsConnector, TheFakeChangesTheFieldsAnUpdateNamesAndNoOther) {
    test::FakeLocalPosts google;
    auto content = standard("Open late on Fridays.");
    content.call_to_action = domain::LocalPostCallToAction{.action_type = "book", .url = "https://example.com/book"};
    ASSERT_TRUE(await(google.create_local_post("111", "9001", gbp::create_body(content, gbp::MediaSource{"photo", "https://example.com/p/a.jpg"}))));
    google.finish_processing();
    const gbp::LocalPostName name{"111", "9001", "7001"};

    const auto updated = await(google.update_local_post(name, {"summary"}, {{"summary", "Open late on Saturdays."}, {"languageCode", "es"}}));
    ASSERT_TRUE(updated);
    EXPECT_EQ(updated->summary, "Open late on Saturdays.");
    EXPECT_EQ(updated->languageCode, "en") << "a member the mask does not name is ignored";
    EXPECT_EQ(updated->callToAction->actionType, "BOOK");
    EXPECT_EQ(updated->state, "LIVE") << "the state it had, unless the test says what Google does";
    EXPECT_EQ(updated->raw.at("summary").as_string(), "Open late on Saturdays.");

    // A field the mask names and the body leaves out is cleared.
    google.state_after_update = "PROCESSING";
    const auto cleared = await(google.update_local_post(name, {"callToAction", "media"}, {{"media", boost::json::array{}}}));
    ASSERT_TRUE(cleared);
    EXPECT_FALSE(cleared->callToAction);
    EXPECT_FALSE(cleared->media);
    EXPECT_EQ(cleared->state, "PROCESSING");
    EXPECT_EQ(await(google.get_local_post(name))->state, "PROCESSING");

    // Refused, lost, and lost after arriving.
    google.fail_update = ApiError{ApiError::Kind::other, 400, "HTTP 400 INVALID_ARGUMENT"};
    EXPECT_FALSE(await(google.update_local_post(name, {"summary"}, {{"summary", "Refused."}})));
    google.fail_update.reset();
    google.update_times_out = true;
    EXPECT_THROW(await(google.update_local_post(name, {"summary"}, {{"summary", "Lost."}})), boost::system::system_error);
    EXPECT_EQ(await(google.get_local_post(name))->summary, "Open late on Saturdays.");
    google.timeout_after_updating = true;
    EXPECT_THROW(await(google.update_local_post(name, {"summary"}, {{"summary", "Arrived."}})), boost::system::system_error);
    EXPECT_EQ(await(google.get_local_post(name))->summary, "Arrived.");
    google.update_times_out = false;

    EXPECT_EQ(await(google.update_local_post({"111", "9001", "9999"}, {"summary"}, {})).error().kind, ApiError::Kind::not_found);
    EXPECT_EQ(await(google.update_local_post(name, {"topicType"}, {{"topicType", "EVENT"}})).error().http_status, 400u);
    EXPECT_EQ(google.updates.size(), 7u) << "every update that reached Google is counted, whatever became of it";
    EXPECT_THAT(google.updates[0].mask, testing::ElementsAre("summary"));
}
