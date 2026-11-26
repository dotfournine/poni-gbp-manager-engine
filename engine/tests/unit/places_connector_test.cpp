// The Places time-zone connector (D-109) against a scripted HTTP client: the
// one request it sends, what each answer becomes, and that no error message
// carries the key or the address.

#include "connectors/google/places.hpp"
#include "unit/connector_fixture.hpp"

#include <gmock/gmock.h>

#include <stdexcept>
#include <string>

using namespace poni;
using google::ApiError;
using testing::HasSubstr;
using testing::Not;

namespace {

constexpr std::string_view kKey = "test-places-key";
constexpr std::string_view kPlaceId = "ChIJ-made_up-0123";

class Places : public test::ConnectorTest {
protected:
    Places() : test::ConnectorTest({google::kPlacesApi}) {}

    google::Result<std::string> ask(google::PlaceTimeZones& places, std::string place_id = std::string(kPlaceId)) {
        return await(places.time_zone(std::move(place_id)));
    }

    // Neither the key nor the address is in what the caller records.
    static void expect_says_nothing_secret(const ApiError& error) {
        EXPECT_THAT(error.message, Not(HasSubstr(std::string(kKey))));
        EXPECT_THAT(error.message, Not(HasSubstr("places.googleapis.com")));
        EXPECT_THAT(error.message, Not(HasSubstr(std::string(kPlaceId))));
    }

    google::HttpPlaceTimeZones places_{http_, limiter_, std::string(kKey)};
};

TEST_F(Places, TheZoneComesFromOneGetWithTheKeyAndTheMaskInHeaders) {
    http_.reply(200, test::fixture("places-time-zone.json"));
    const auto zone = ask(places_);
    ASSERT_TRUE(zone) << zone.error().message;
    EXPECT_EQ(*zone, "America/Chicago");

    ASSERT_EQ(http_.requests.size(), 1u);
    const auto& request = http_.requests[0];
    EXPECT_EQ(request.method, http::ClientMethod::get);
    EXPECT_EQ(request.url.buffer(), "https://places.googleapis.com/v1/places/ChIJ-made_up-0123");
    EXPECT_EQ(http_.header(0, "X-Goog-Api-Key"), kKey);
    EXPECT_EQ(http_.header(0, "X-Goog-FieldMask"), "timeZone");
    EXPECT_EQ(http_.header(0, "Authorization"), "") << "a key, and no OAuth token";
    EXPECT_EQ(request.headers.size(), 2u);
}

TEST_F(Places, EachAnswerBecomesItsKindOfError) {
    const struct {
        unsigned status;
        ApiError::Kind kind;
    } cases[] = {
        {404, ApiError::Kind::not_found},     {403, ApiError::Kind::unauthenticated}, {401, ApiError::Kind::unauthenticated},
        {429, ApiError::Kind::rate_limited},  {503, ApiError::Kind::unavailable},     {400, ApiError::Kind::other},
    };
    for (const auto& c : cases) {
        // Google's message may quote the request; only its status word is kept.
        http_.reply(c.status, std::format(R"({{"error": {{"code": {}, "message": "key {} for places.googleapis.com/v1/places/{}", "status": "PERMISSION_DENIED"}}}})",
                                          c.status, kKey, kPlaceId));
        const auto zone = ask(places_);
        ASSERT_FALSE(zone) << c.status;
        EXPECT_EQ(zone.error().kind, c.kind) << c.status;
        EXPECT_EQ(zone.error().http_status, c.status);
        EXPECT_EQ(zone.error().message, std::format("Google Places answered {} PERMISSION_DENIED", c.status));
        expect_says_nothing_secret(zone.error());
    }
    // A body that is not Google's error shape, or a status word that is no word, leaves the status alone.
    http_.reply(403, "<html>denied</html>");
    EXPECT_EQ(ask(places_).error().message, "Google Places answered 403");
    http_.reply(403, std::format(R"({{"error": {{"status": "{}"}}}})", kKey));
    EXPECT_EQ(ask(places_).error().message, "Google Places answered 403");
}

TEST_F(Places, ATimeoutIsUnavailable) {
    http_.fail();
    const auto zone = ask(places_);
    ASSERT_FALSE(zone);
    EXPECT_EQ(zone.error().kind, ApiError::Kind::unavailable);
    expect_says_nothing_secret(zone.error());
}

TEST_F(Places, AnAnswerWithoutAZoneIsAnInvalidResponse) {
    for (const auto* body : {R"({})", R"({"timeZone": {}})", R"({"timeZone": {"id": ""}})", R"({"timeZone": "America/New_York"})", "not json"}) {
        http_.reply(200, body);
        const auto zone = ask(places_);
        ASSERT_FALSE(zone) << body;
        EXPECT_EQ(zone.error().kind, ApiError::Kind::invalid_response) << body;
        expect_says_nothing_secret(zone.error());
    }
}

TEST_F(Places, APlaceIdThatIsNotPlainIsAnsweredWithoutARequest) {
    for (const auto* id : {"a/b", "..", "x%2F", "", "a b", "a?b"}) {
        const auto zone = ask(places_, id);
        ASSERT_FALSE(zone) << id;
        EXPECT_EQ(zone.error().kind, ApiError::Kind::invalid_response) << id;
        EXPECT_EQ(zone.error().message, "Google gave this location a place id the engine does not send on.");
    }
    EXPECT_TRUE(http_.requests.empty());
    EXPECT_TRUE(http_.refused.empty()) << "nothing reached the outbound rule";
}

TEST_F(Places, WithoutAKeyNothingIsSentUnlessTheMockIsOn) {
    google::HttpPlaceTimeZones without_key(http_, limiter_, "");
    const auto zone = ask(without_key);
    ASSERT_FALSE(zone);
    EXPECT_EQ(zone.error().kind, ApiError::Kind::not_configured);
    EXPECT_EQ(zone.error().message, "No Google Places key is configured.");
    EXPECT_TRUE(http_.requests.empty());

    // The mock takes any key: a fixed stand-in goes there.
    google::HttpPlaceTimeZones against_mock(http_, limiter_, "", true);
    http_.reply(200, R"({"timeZone": {"id": "America/New_York", "version": "2025b"}})");
    ASSERT_TRUE(ask(against_mock));
    ASSERT_EQ(http_.requests.size(), 1u);
    EXPECT_EQ(http_.header(0, "X-Goog-Api-Key"), "mock-places-key");

    // A configured key is sent as it is, mock or not.
    google::HttpPlaceTimeZones mock_with_key(http_, limiter_, std::string(kKey), true);
    http_.reply(200, R"({"timeZone": {"id": "America/New_York"}})");
    ASSERT_TRUE(ask(mock_with_key));
    EXPECT_EQ(http_.header(1, "X-Goog-Api-Key"), kKey);
}

TEST_F(Places, TheLimiterSlotIsTakenBeforeTheRequest) {
    // A limiter that does not know the Places API refuses the slot: so the connector asks for that one, first.
    core::RateLimiter unconfigured{io_.get_executor(), clock_};
    google::HttpPlaceTimeZones places(http_, unconfigured, std::string(kKey));
    EXPECT_THROW(ask(places), std::logic_error);
    EXPECT_TRUE(http_.requests.empty());
}

}  // namespace
