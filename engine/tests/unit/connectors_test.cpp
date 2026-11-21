// The Google connectors against a scripted HTTP client: which requests go
// out, how responses and errors are mapped, that the reader only reads, and
// what every connector relies on in the one authorised call (google::send).

#include "connectors/gbp/reader.hpp"
#include "connectors/google/auth.hpp"
#include "connectors/pubsub/subscriber.hpp"
#include "core/crypto.hpp"
#include "unit/connector_fixture.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/system/system_error.hpp>
#include <gtest/gtest.h>

#include <algorithm>

using namespace poni;
using namespace std::chrono_literals;
using google::ApiError;
using test::fixture;
using test::kTokenOk;

namespace {

// A reader wired to the fake HTTP client, with a token already answered for.
class Connectors : public test::ConnectorTest {
protected:
    Connectors()
        : ConnectorTest({gbp::kAccountsApi, gbp::kLocationsApi, gbp::kReviewsApi, google::kOAuthApi, pubsub::kPubsubApi}),
          tokens_(clock_, google::refresh_fetch(http_, limiter_, loader())),
          reader_(api_) {}

    google::CredentialsLoader loader() {
        return [this]() -> boost::asio::awaitable<google::Result<google::OAuthCredentials>> {
            ++loads_;
            if (!credentials_) co_return std::unexpected(google::not_configured("no token stored"));
            co_return *credentials_;
        };
    }

    // Requests sent to the APIs, without the token requests.
    std::vector<http::ClientRequest> api_requests() const {
        std::vector<http::ClientRequest> out;
        for (const auto& request : http_.requests) {
            if (request.url.host() != "oauth2.googleapis.com") out.push_back(request);
        }
        return out;
    }

    std::optional<google::OAuthCredentials> credentials_{{"client-id", "client-secret", "refresh-token"}};
    int loads_ = 0;
    google::CachedTokenSource tokens_;
    google::Api api_{http_, tokens_, limiter_};
    gbp::HttpReader reader_;
};

}  // namespace

TEST_F(Connectors, AccessTokenIsRefreshedOnceAndCachedUntilItExpires) {
    http_.reply(200, std::string(kTokenOk));
    EXPECT_EQ(await(tokens_.access_token()), "token-1");
    EXPECT_EQ(await(tokens_.access_token()), "token-1");
    ASSERT_EQ(http_.requests.size(), 1u);
    const auto& request = http_.requests[0];
    EXPECT_EQ(request.method, http::ClientMethod::post);
    EXPECT_EQ(request.url.buffer(), "https://oauth2.googleapis.com/token");
    EXPECT_EQ(request.body, "grant_type=refresh_token&client_id=client-id&client_secret=client-secret&refresh_token=refresh-token");

    clock_.advance(3600s - 30s);  // inside the safety margin before expiry
    http_.reply(200, R"({"access_token": "token-2", "expires_in": 3600})");
    EXPECT_EQ(await(tokens_.access_token()), "token-2");
    EXPECT_EQ(loads_, 2) << "the stored credentials are read again on every refresh";
}

TEST_F(Connectors, ARevokedRefreshTokenIsReportedAsUnauthenticated) {
    std::vector<std::optional<std::string>> seen;
    google::CachedTokenSource tokens(clock_, google::refresh_fetch(http_, limiter_, loader(),
                                                                   [&](std::optional<std::string> error) -> boost::asio::awaitable<void> {
                                                                       seen.push_back(std::move(error));
                                                                       co_return;
                                                                   }));
    http_.reply(400, R"({"error": "invalid_grant", "error_description": "Token has been expired or revoked."})");
    const auto failed = await(tokens.access_token());
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().kind, ApiError::Kind::unauthenticated);
    EXPECT_EQ(failed.error().message, "HTTP 400 invalid_grant: Token has been expired or revoked.");

    http_.reply(200, std::string(kTokenOk));
    EXPECT_TRUE(await(tokens.access_token()));
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_TRUE(seen[0]);
    EXPECT_FALSE(seen[1]) << "a success clears the reported error";
}

TEST_F(Connectors, WithoutStoredCredentialsNothingIsSent) {
    credentials_.reset();
    const auto accounts = await(reader_.list_accounts());
    ASSERT_FALSE(accounts);
    EXPECT_EQ(accounts.error().kind, ApiError::Kind::not_configured);
    EXPECT_TRUE(http_.requests.empty());
}

TEST_F(Connectors, ListsAccountsAndFollowsPages) {
    http_.reply(200, std::string(kTokenOk));
    http_.reply(200, R"({"accounts": [{"name": "accounts/1", "accountName": "One", "type": "PERSONAL", "vettedState": "x"}],
                         "nextPageToken": "p2"})");
    http_.reply(200, R"({"accounts": [{"name": "accounts/2"}]})");
    const auto accounts = await(reader_.list_accounts());
    ASSERT_TRUE(accounts) << accounts.error().message;
    ASSERT_EQ(accounts->size(), 2u);
    EXPECT_EQ(accounts->at(1).name, "accounts/2");

    const auto requests = api_requests();
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_EQ(requests[0].url.buffer(), "https://mybusinessaccountmanagement.googleapis.com/v1/accounts");
    EXPECT_EQ(requests[1].url.buffer(), "https://mybusinessaccountmanagement.googleapis.com/v1/accounts?pageToken=p2");
    EXPECT_EQ(http_.header(1, "Authorization"), "Bearer token-1");
}

TEST_F(Connectors, AnAccountWithNoLocationsIsAnEmptyListNotAnError) {
    http_.reply(200, std::string(kTokenOk));
    http_.reply(200, "{}");  // Google omits empty arrays
    const auto locations = await(reader_.list_locations("111"));
    ASSERT_TRUE(locations);
    EXPECT_TRUE(locations->empty());
}

TEST_F(Connectors, MapsALocationAndKeepsTheRawPayload) {
    http_.reply(200, std::string(kTokenOk));
    http_.reply(200, R"({"locations": [)" + fixture("location-gangnam.json") + "]}");
    const auto locations = await(reader_.list_locations("111"));
    ASSERT_TRUE(locations) << locations.error().message;
    ASSERT_EQ(locations->size(), 1u);
    const auto& location = locations->at(0);
    EXPECT_EQ(location.title, "Gangnam AYCE Buffet");
    EXPECT_EQ(location.storefrontAddress->locality, "Chantilly");
    EXPECT_EQ(location.categories->primaryCategory->displayName, "Buffet restaurant");
    EXPECT_TRUE(location.raw.as_object().contains("regularHours")) << "fields the engine doesn't map are kept";

    const auto url = api_requests().at(0).url;
    EXPECT_EQ(url.encoded_path(), "/v1/accounts/111/locations");
    EXPECT_TRUE(url.params().contains("readMask"));
}

TEST_F(Connectors, AnIdCannotChangeTheRequestPath) {
    http_.reply(200, std::string(kTokenOk));
    http_.reply(200, "{}");
    await(reader_.list_reviews("111", "5/../../admin?x=1", std::nullopt));
    const auto url = api_requests().at(0).url;
    EXPECT_EQ(url.host(), "mybusiness.googleapis.com");
    EXPECT_EQ(url.encoded_path(), "/v4/accounts/111/locations/5%2F..%2F..%2Fadmin%3Fx=1/reviews");
}

TEST_F(Connectors, ListsReviewsNewestFirstFromTheSample) {
    http_.reply(200, std::string(kTokenOk));
    auto body = boost::json::parse(fixture("reviews-gangnam.json")).as_object();
    body["totalReviewCount"] = 158;
    body["nextPageToken"] = "next";
    http_.reply(200, boost::json::serialize(body));

    const auto page = await(reader_.list_reviews("111283263702173569518", "6336855403902537758", std::nullopt));
    ASSERT_TRUE(page) << page.error().message;
    EXPECT_EQ(page->reviews.size(), body.at("reviews").as_array().size());
    EXPECT_EQ(page->skipped, 0u);
    EXPECT_EQ(page->next_page_token, "next");
    EXPECT_EQ(page->total_review_count, 158);
    const auto& first = page->reviews.at(0);
    EXPECT_EQ(first.starRating, "FIVE");
    EXPECT_TRUE(first.reviewReply);
    EXPECT_EQ(first.raw.at("reviewId").as_string(), first.reviewId);

    const auto url = api_requests().at(0).url;
    EXPECT_EQ(url.encoded_path(), "/v4/accounts/111283263702173569518/locations/6336855403902537758/reviews");
    EXPECT_EQ((*url.params().find("orderBy")).value, "updateTime desc");
    EXPECT_EQ((*url.params().find("pageSize")).value, "50");
}

TEST_F(Connectors, AReviewThatCannotBeMappedIsSkippedNotFatal) {
    http_.reply(200, std::string(kTokenOk));
    http_.reply(200, R"({"reviews": [
        {"reviewId": "ok", "name": "accounts/1/locations/2/reviews/ok", "createTime": "2026-01-01T00:00:00Z", "updateTime": "2026-01-01T00:00:00Z"},
        {"reviewId": "no-times", "name": "accounts/1/locations/2/reviews/no-times"},
        {"reviewId": "bad-name", "name": "somewhere/else", "createTime": "2026-01-01T00:00:00Z", "updateTime": "2026-01-01T00:00:00Z"}]})");
    const auto page = await(reader_.list_reviews("1", "2", std::nullopt));
    ASSERT_TRUE(page);
    EXPECT_EQ(page->reviews.size(), 1u);
    EXPECT_EQ(page->skipped, 2u);
}

TEST_F(Connectors, BatchGetReviewsIsAPostThatOnlyReads) {
    http_.reply(200, std::string(kTokenOk));
    http_.reply(200, fixture("batch-get-reviews.json"));
    const auto page = await(reader_.batch_get_reviews("111", {"222", "333"}, std::string("tok")));
    ASSERT_TRUE(page) << page.error().message;
    EXPECT_EQ(page->reviews.size(), 3u);

    const auto request = api_requests().at(0);
    EXPECT_EQ(request.method, http::ClientMethod::post);
    EXPECT_EQ(request.url.buffer(), "https://mybusiness.googleapis.com/v4/accounts/111/locations:batchGetReviews");
    const auto body = boost::json::parse(request.body).as_object();
    EXPECT_EQ(boost::json::serialize(body.at("locationNames")),
              R"(["accounts/111/locations/222","accounts/111/locations/333"])");
    EXPECT_EQ(body.at("orderBy").as_string(), "updateTime desc");
    EXPECT_EQ(body.at("pageToken").as_string(), "tok");
    EXPECT_FALSE(body.at("ignoreRatingOnlyReviews").as_bool());
}

TEST_F(Connectors, AReviewThatIsGoneIsNotFound) {
    http_.reply(200, std::string(kTokenOk));
    http_.reply(404, R"({"error": {"code": 404, "message": "Requested entity was not found.", "status": "NOT_FOUND"}})");
    const auto review = await(reader_.get_review({"1", "2", "3"}));
    ASSERT_FALSE(review);
    EXPECT_EQ(review.error().kind, ApiError::Kind::not_found);
    EXPECT_EQ(api_requests().at(0).url.buffer(), "https://mybusiness.googleapis.com/v4/accounts/1/locations/2/reviews/3");
}

TEST_F(Connectors, A401GetsOneRetryWithAFreshToken) {
    http_.reply(200, std::string(kTokenOk));
    http_.reply(401, R"({"error": {"status": "UNAUTHENTICATED", "message": "expired"}})");
    http_.reply(200, R"({"access_token": "token-2", "expires_in": 3600})");
    http_.reply(200, "{}");
    EXPECT_TRUE(await(reader_.list_accounts()));
    EXPECT_EQ(http_.header(3, "Authorization"), "Bearer token-2");

    // A second 401 in a row is given up on.
    http_.reply(401, "{}");
    http_.reply(200, R"({"access_token": "token-3", "expires_in": 3600})");
    http_.reply(401, "{}");
    const auto failed = await(reader_.list_accounts());
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().kind, ApiError::Kind::unauthenticated);
}

TEST_F(Connectors, RateLimitAndServerErrorsAreTold) {
    http_.reply(200, std::string(kTokenOk));
    http_.reply(429, R"({"error": {"status": "RESOURCE_EXHAUSTED", "message": "Quota exceeded"}})");
    const auto limited = await(reader_.list_accounts());
    ASSERT_FALSE(limited);
    EXPECT_EQ(limited.error().kind, ApiError::Kind::rate_limited);
    EXPECT_GE(limiter_.reserve(gbp::kAccountsApi), 60s) << "after a 429 the API is left alone for a minute";

    http_.reply(503, "upstream down");
    const auto down = await(reader_.list_locations("1"));
    ASSERT_FALSE(down);
    EXPECT_EQ(down.error().kind, ApiError::Kind::unavailable);
    EXPECT_EQ(down.error().message, "HTTP 503 (13 bytes)") << "a body that isn't Google's JSON is not echoed";
}

// The reader has no call that writes to Google.
TEST_F(Connectors, EveryReaderCallIsAGetExceptTheReadOnlyBatch) {
    http_.reply(200, std::string(kTokenOk));
    for (int i = 0; i < 5; ++i) http_.reply(200, "{}");
    await(reader_.list_accounts());
    await(reader_.list_locations("1"));
    await(reader_.list_reviews("1", "2", std::nullopt));
    await(reader_.batch_get_reviews("1", {"2"}, std::nullopt));
    await(reader_.get_review({"1", "2", "3"}));

    const auto requests = api_requests();
    ASSERT_EQ(requests.size(), 5u);
    for (const auto& request : requests) {
        const bool batch = request.url.encoded_path().ends_with(":batchGetReviews");
        EXPECT_EQ(request.method, batch ? http::ClientMethod::post : http::ClientMethod::get) << request.url.buffer();
        EXPECT_FALSE(request.url.encoded_path().contains("/reply")) << "the reply endpoint is never called";
    }
}

// ---- one authorised call (google::send): what every connector relies on ------------------

namespace {

// The create of a local post: the write that must go out once.
http::ClientRequest a_create() {
    http::ClientRequest request;
    request.method = http::ClientMethod::post;
    request.url = boost::urls::url("https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts");
    request.content_type = "application/json";
    request.body = R"({"summary":"Fall specials start Monday."})";
    return request;
}

}  // namespace

TEST_F(Connectors, OnlyAFirst401IsSentAgainAndOnlyOnce) {
    const auto creates = [&] { return std::ranges::count_if(http_.requests, [](const auto& r) { return http::is_gbp_host(r.url); }); };
    http_.reply(200, std::string(kTokenOk));

    // Refused with the new token too: that is the answer, and there is no third request.
    http_.reply(401, "{}");
    http_.reply(200, R"({"access_token": "token-2", "expires_in": 3600})");
    http_.reply(401, "{}");
    const auto refused = await(google::send(api_, gbp::kReviewsApi, a_create()));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().kind, ApiError::Kind::unauthenticated);
    EXPECT_EQ(refused.error().http_status, 401u);
    EXPECT_EQ(creates(), 2);

    // Whatever follows the first 401 is the answer: a 5xx then says the outcome is open, and nothing is sent after it.
    http_.reply(401, "{}");
    http_.reply(200, R"({"access_token": "token-3", "expires_in": 3600})");
    http_.reply(503, "{}");
    EXPECT_EQ(await(google::send(api_, gbp::kReviewsApi, a_create())).error().http_status, 503u);
    EXPECT_EQ(creates(), 4);

    // No other answer is a reason to send again.
    for (const unsigned status : {400u, 403u, 404u, 408u, 409u, 499u, 500u, 502u, 503u, 504u}) {
        const auto before = creates();
        http_.reply(status, "{}");
        EXPECT_FALSE(await(google::send(api_, gbp::kReviewsApi, a_create())));
        EXPECT_EQ(creates(), before + 1) << status;
    }
    // A transport failure on the second try is not caught and not tried a third time.
    http_.reply(401, "{}");
    http_.reply(200, R"({"access_token": "token-4", "expires_in": 3600})");
    http_.fail();
    const auto before = creates();
    EXPECT_THROW(await(google::send(api_, gbp::kReviewsApi, a_create())), boost::system::system_error);
    EXPECT_EQ(creates(), before + 2);
}

TEST_F(Connectors, GooglesStatusIsPassedOnAsItIs) {
    http_.reply(200, std::string(kTokenOk));
    // jobs/local_posts.cpp decides from the status whether a create may be sent again.
    for (const unsigned status : {300u, 400u, 403u, 404u, 408u, 409u, 429u, 499u, 500u, 503u}) {
        http_.reply(status, R"({"error": {"status": "FAILED_PRECONDITION", "message": "no"}})");
        const auto answer = await(google::send(api_, gbp::kReviewsApi, a_create()));
        ASSERT_FALSE(answer) << status;
        EXPECT_EQ(answer.error().http_status, status);
        // A 429 leaves the API alone for a minute; the test has no minute.
        limiter_.configure(std::string(gbp::kReviewsApi), {.per_minute = 6000, .burst = 100});
        clock_.advance(61s);
    }
    // A 2xx that can't be read is 200, whatever Google's 2xx was: "answered, and the outcome is not known".
    for (const unsigned status : {200u, 201u, 204u}) {
        http_.reply(status, "<html>proxy</html>");
        const auto unreadable = await(google::fetch_json(api_, gbp::kReviewsApi, a_create()));
        ASSERT_FALSE(unreadable);
        EXPECT_EQ(unreadable.error().kind, ApiError::Kind::invalid_response);
        EXPECT_EQ(unreadable.error().http_status, 200u);
    }

    // No token: nothing is sent, and the token source's own error comes back, with 0 where nothing was asked ...
    const auto sent = http_.requests.size();
    tokens_.invalidate();
    credentials_.reset();
    const auto no_token = await(google::send(api_, gbp::kReviewsApi, a_create()));
    ASSERT_FALSE(no_token);
    EXPECT_EQ(no_token.error().kind, ApiError::Kind::not_configured);
    EXPECT_EQ(no_token.error().http_status, 0u);
    EXPECT_EQ(http_.requests.size(), sent);
    // ... and with the token endpoint's status where Google refused the refresh token.
    credentials_ = google::OAuthCredentials{"client-id", "client-secret", "revoked"};
    http_.reply(400, R"({"error": "invalid_grant", "error_description": "Token has been expired or revoked."})");
    const auto revoked = await(google::send(api_, gbp::kReviewsApi, a_create()));
    ASSERT_FALSE(revoked);
    EXPECT_EQ(revoked.error().kind, ApiError::Kind::unauthenticated);
    EXPECT_EQ(revoked.error().http_status, 400u);
    EXPECT_EQ(http_.requests.size(), sent + 1) << "only the token request went out";
}

TEST_F(Connectors, ATransportFailureThrowsAndIsNeverAnOrdinaryError) {
    http_.reply(200, std::string(kTokenOk));
    http_.fail();
    EXPECT_THROW(await(google::send(api_, gbp::kReviewsApi, a_create())), boost::system::system_error);
    http_.fail();
    EXPECT_THROW(await(google::fetch_json(api_, gbp::kReviewsApi, a_create())), boost::system::system_error);
    EXPECT_EQ(http_.requests.size(), 3u) << "each went out once and was not repeated";
    // The same for a token that can't be fetched: no answer is not "no token".
    tokens_.invalidate();
    http_.fail();
    EXPECT_THROW(await(google::send(api_, gbp::kReviewsApi, a_create())), boost::system::system_error);
}

TEST_F(Connectors, AValidationErrorCarriesGooglesFieldDetailsForEveryApi) {
    http_.reply(200, std::string(kTokenOk));
    http_.reply(400, R"({"error": {"code": 400, "status": "INVALID_ARGUMENT", "message": "Request contains an invalid argument.",
                         "details": [{"@type": "type.googleapis.com/google.mybusiness.v4.ValidationError",
                                      "errorDetails": [{"code": 3, "field": "comment", "message": "Too long.", "value": "the text that was sent"},
                                                       {"code": 3, "message": "No field named."}]}]}})");
    const auto refused = await(google::send(api_, gbp::kReviewsApi, a_create()));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().message, "HTTP 400 INVALID_ARGUMENT Request contains an invalid argument. (comment: Too long.; No field named.)");
}

TEST(ReviewName, ParsesOnlyTheFullForm) {
    const auto name = gbp::parse_review_name("accounts/1/locations/22/reviews/abc-DEF_3");
    ASSERT_TRUE(name);
    EXPECT_EQ(name->account_id, "1");
    EXPECT_EQ(name->location_id, "22");
    EXPECT_EQ(name->review_id, "abc-DEF_3");
    for (const auto* bad : {"", "accounts/1/locations/2", "accounts/1/locations/2/reviews/", "accounts//locations/2/reviews/3",
                            "x/1/locations/2/reviews/3", "accounts/1/locations/2/reviews/3/reply"}) {
        EXPECT_FALSE(gbp::parse_review_name(bad)) << bad;
    }
}

TEST_F(Connectors, PubsubPullDecodesMessagesAndNamesTheQuotaProject) {
    pubsub::HttpSubscriber subscriber(api_, {"my-project", "my-sub"}, "my-project");
    const auto data = core::base64_encode(R"({"type": "NEW_REVIEW", "review": "accounts/1/locations/2/reviews/3"})");
    http_.reply(200, std::string(kTokenOk));
    http_.reply(200, std::format(R"({{"receivedMessages": [{{"ackId": "a1", "message": {{"data": "{}", "messageId": "m1",
                                      "publishTime": "2026-11-09T17:51:43Z"}}}}]}})", data));
    const auto messages = await(subscriber.pull(10));
    ASSERT_TRUE(messages) << messages.error().message;
    ASSERT_EQ(messages->size(), 1u);
    EXPECT_EQ(messages->at(0).message_id, "m1");
    EXPECT_TRUE(messages->at(0).data.contains("NEW_REVIEW"));

    const auto request = api_requests().at(0);
    EXPECT_EQ(request.method, http::ClientMethod::post);
    EXPECT_EQ(request.url.buffer(), "https://pubsub.googleapis.com/v1/projects/my-project/subscriptions/my-sub:pull");
    EXPECT_EQ(http_.header(1, "x-goog-user-project"), "my-project");

    http_.reply(200, "{}");
    EXPECT_TRUE(await(subscriber.acknowledge({"a1"})));
    EXPECT_EQ(api_requests().at(1).url.encoded_path(), "/v1/projects/my-project/subscriptions/my-sub:acknowledge");
    EXPECT_EQ(api_requests().at(1).body, R"({"ackIds":["a1"]})");

    http_.reply(200, "{}");
    const auto none = await(subscriber.pull(10));
    ASSERT_TRUE(none);
    EXPECT_TRUE(none->empty());
}

TEST_F(Connectors, PubsubErrorsAreReturnedNotSwallowed) {
    pubsub::HttpSubscriber subscriber(api_, {"p", "s"}, std::nullopt);
    http_.reply(200, std::string(kTokenOk));
    http_.reply(403, R"({"error": {"status": "PERMISSION_DENIED", "message": "User not authorized"}})");
    const auto failed = await(subscriber.pull(10));
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().kind, ApiError::Kind::permission_denied);

    // A subscription that is not named that way never gets as far as a subscriber: the configuration refuses it.
    EXPECT_FALSE(pubsub::parse_subscription("my-sub"));
    EXPECT_EQ(pubsub::parse_subscription("projects/p/subscriptions/s"), (std::pair<std::string, std::string>{"p", "s"}));
}

TEST_F(Connectors, PubsubIsLeftAloneAfterARateLimit) {
    pubsub::HttpSubscriber subscriber(api_, {"p", "s"}, std::nullopt);
    http_.reply(200, std::string(kTokenOk));
    http_.reply(429, R"({"error": {"status": "RESOURCE_EXHAUSTED", "message": "Quota exceeded"}})");
    EXPECT_EQ(await(subscriber.acknowledge({"a1"})).error().kind, ApiError::Kind::rate_limited);
    EXPECT_GE(limiter_.reserve(pubsub::kPubsubApi), 60s) << "for a minute, like every other API";
}

TEST_F(Connectors, TheMetadataServerGivesTheVmsToken) {
    google::CachedTokenSource metadata(clock_, google::service_account_fetch(http_, limiter_, true));
    http_.reply(200, R"({"access_token": "vm-token", "expires_in": 1800, "token_type": "Bearer"})");
    EXPECT_EQ(await(metadata.access_token()), "vm-token");
    EXPECT_EQ(await(metadata.access_token()), "vm-token");
    ASSERT_EQ(http_.requests.size(), 1u);
    EXPECT_EQ(http_.requests[0].url.host(), "metadata.google.internal");
    EXPECT_EQ(http_.header(0, "Metadata-Flavor"), "Google");
}
