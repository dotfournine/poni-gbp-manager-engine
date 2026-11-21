// Push to the phone app (D-097), without a network: the provider token,
// what each of Apple's answers means and what a delivery's answers come to,
// the requests the APNs sender makes, and what a push says.

#include "connectors/notify/apns.hpp"
#include "core/crypto.hpp"
#include "domain/push.hpp"
#include "unit/connector_fixture.hpp"
#include "unit/test_key.hpp"

#include <boost/json/parse.hpp>
#include <gmock/gmock.h>

using namespace poni;
using namespace std::chrono_literals;
using test::test_key;
using testing::HasSubstr;

namespace {

// The 32-byte device token APNs hands an app, in hex.
const std::string kToken(64, 'a');

}  // namespace

// ---- the provider token -------------------------------------------------------------------

TEST(ProviderToken, IsAJwtWithTheKeyIdTheTeamAndTheTimeSignedWithTheKey) {
    const auto key = *core::Es256Key::from_p8(test_key().p8);
    const auto jwt = notify::provider_token(key, "ABC123DEFG", "TEAM123456", test::t0());
    const auto first = jwt.find('.');
    const auto second = jwt.find('.', first + 1);
    ASSERT_NE(second, std::string::npos);
    const auto header = boost::json::parse(*core::base64_decode(jwt.substr(0, first))).as_object();
    const auto claims = boost::json::parse(*core::base64_decode(jwt.substr(first + 1, second - first - 1))).as_object();
    EXPECT_EQ(header.at("alg").as_string(), "ES256");
    EXPECT_EQ(header.at("kid").as_string(), "ABC123DEFG");
    EXPECT_EQ(claims.at("iss").as_string(), "TEAM123456");
    EXPECT_EQ(claims.at("iat").as_int64(), std::chrono::duration_cast<std::chrono::seconds>(test::t0().time_since_epoch()).count());
    EXPECT_TRUE(test_key().verifies(jwt.substr(0, second), *core::base64_decode(jwt.substr(second + 1))));
}

// ---- Apple's answers --------------------------------------------------------------------

TEST(ApnsAnswer, EachAnswerMeansWhatAppleSaysItMeans) {
    using Kind = notify::PushAnswer::Kind;
    const auto kind = [](unsigned status, std::string body) { return notify::read_answer({status, std::move(body)}).kind; };
    EXPECT_EQ(kind(200, ""), Kind::accepted);
    EXPECT_EQ(kind(410, R"({"reason":"Unregistered","timestamp":1700000000000})"), Kind::device_gone);
    EXPECT_EQ(kind(400, R"({"reason":"BadDeviceToken"})"), Kind::device_gone);
    EXPECT_EQ(kind(400, R"({"reason":"DeviceTokenNotForTopic"})"), Kind::device_gone);
    EXPECT_EQ(kind(403, R"({"reason":"ExpiredProviderToken"})"), Kind::retry);
    EXPECT_TRUE(notify::read_answer({403, R"({"reason":"ExpiredProviderToken"})"}).token_expired);
    EXPECT_FALSE(notify::read_answer({503, R"({"reason":"ServiceUnavailable"})"}).token_expired);
    EXPECT_EQ(kind(429, R"({"reason":"TooManyRequests"})"), Kind::retry);
    EXPECT_EQ(kind(500, R"({"reason":"InternalServerError"})"), Kind::retry);
    EXPECT_EQ(kind(503, R"({"reason":"ServiceUnavailable"})"), Kind::retry);
    EXPECT_EQ(kind(403, R"({"reason":"InvalidProviderToken"})"), Kind::sender_refused);
    EXPECT_EQ(kind(403, R"({"reason":"BadEnvironmentKeyInToken"})"), Kind::sender_refused);
    EXPECT_EQ(kind(400, R"({"reason":"TopicDisallowed"})"), Kind::sender_refused);
    EXPECT_EQ(kind(400, R"({"reason":"BadCollapseId"})"), Kind::refused);
    EXPECT_EQ(kind(413, R"({"reason":"PayloadTooLarge"})"), Kind::refused);
    EXPECT_EQ(kind(400, "not json"), Kind::refused);
    // What goes into the log is Apple's word, nothing more.
    EXPECT_EQ(notify::read_answer({400, R"({"reason":"Bad\r\nCollapse<b>Id"})"}).reason, "400 BadCollapsebId");
}

// ---- the sender -------------------------------------------------------------------------

namespace {

class Apns : public test::ConnectorTest {
protected:
    Apns() : ConnectorTest({notify::kApnsApi}) {}

    notify::ApnsSender sender(core::ApnsEnvironment environment = core::ApnsEnvironment::sandbox) {
        return notify::ApnsSender(http_, limiter_, clock_,
                                  {.environment = environment,
                                   .key = *core::Es256Key::from_p8(test_key().p8),
                                   .key_id = "ABC123DEFG",
                                   .team_id = "TEAM123456",
                                   .bundle_id = "com.example.gbpmanager"});
    }

    static std::vector<notify::Push> two() {
        return {{.device_token = kToken, .payload = R"({"aps":{}})", .collapse_id = "c1"},
                {.device_token = std::string(64, 'b'), .payload = R"({"aps":{}})", .collapse_id = "c1"}};
    }
};

}  // namespace

TEST_F(Apns, ADeliverysPushesGoOnOneConnectionToTheEnginesEnvironment) {
    auto apns = sender();
    http_.reply(200, "");
    http_.reply(410, R"({"reason":"Unregistered"})");
    const auto answers = await(apns.send(two()));
    ASSERT_TRUE(answers);
    ASSERT_EQ(answers->size(), 2u);
    EXPECT_EQ((*answers)[0].kind, notify::PushAnswer::Kind::accepted);
    EXPECT_EQ((*answers)[1].kind, notify::PushAnswer::Kind::device_gone);
    EXPECT_THAT(http_.h2_batches, testing::ElementsAre(2u));
    ASSERT_EQ(http_.requests.size(), 2u);
    EXPECT_EQ(http_.requests[0].url.buffer(), "https://api.sandbox.push.apple.com/3/device/" + kToken);
    EXPECT_EQ(http_.requests[0].body, R"({"aps":{}})");
    EXPECT_EQ(http_.header(0, "apns-topic"), "com.example.gbpmanager");
    EXPECT_EQ(http_.header(0, "apns-push-type"), "alert");
    EXPECT_EQ(http_.header(0, "apns-priority"), "10");
    EXPECT_EQ(http_.header(0, "apns-collapse-id"), "c1");
    const auto in_a_day = std::chrono::duration_cast<std::chrono::seconds>((test::t0() + 24h).time_since_epoch()).count();
    EXPECT_EQ(http_.header(0, "apns-expiration"), std::to_string(in_a_day));
    EXPECT_THAT(http_.header(0, "authorization"), testing::StartsWith("bearer "));
    EXPECT_EQ(http_.header(0, "authorization"), http_.header(1, "authorization"));

    auto production = sender(core::ApnsEnvironment::production);
    http_.reply(200, "");
    ASSERT_TRUE(await(production.send({two()[0]})));
    EXPECT_EQ(http_.requests.back().url.host(), "api.push.apple.com");
}

TEST_F(Apns, TheProviderTokenIsKeptFiftyMinutesAndDroppedWhenAppleCallsItExpired) {
    auto apns = sender();
    const auto bearer = [&] { return http_.header(http_.requests.size() - 1, "authorization"); };
    http_.reply(200, "");
    ASSERT_TRUE(await(apns.send({two()[0]})));
    const auto first = bearer();
    clock_.advance(49min);
    http_.reply(200, "");
    ASSERT_TRUE(await(apns.send({two()[0]})));
    EXPECT_EQ(bearer(), first);
    clock_.advance(2min);
    http_.reply(200, "");
    ASSERT_TRUE(await(apns.send({two()[0]})));
    const auto second = bearer();
    EXPECT_NE(second, first);

    http_.reply(403, R"({"reason":"ExpiredProviderToken"})");
    const auto expired = await(apns.send({two()[0]}));
    ASSERT_TRUE(expired);
    EXPECT_EQ((*expired)[0].kind, notify::PushAnswer::Kind::retry);
    clock_.advance(1s);
    http_.reply(200, "");
    ASSERT_TRUE(await(apns.send({two()[0]})));
    EXPECT_NE(bearer(), second);
}

TEST_F(Apns, ANetworkFailureMayPassAndARefusedRequestDoesNot) {
    auto apns = sender();
    http_.fail();
    const auto failed = await(apns.send({two()[0]}));
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().kind, notify::SendError::Kind::transient);

    // A token that is not hex makes an address the client does not send to.
    const auto refused = await(apns.send({{.device_token = "ABC", .payload = "{}", .collapse_id = "c"}}));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().kind, notify::SendError::Kind::permanent);
    EXPECT_EQ(http_.refused.size(), 1u);
}

// ---- what a push says -------------------------------------------------------------------

TEST(PushPayload, AnEscalationCarriesItsSubjectAndTheIdsTheAppOpens) {
    const auto payload = boost::json::parse(domain::push_payload({.event_type = "escalation",
                                                                  .subject = "Escalation: Gangnam Buffet - Food poisoning",
                                                                  .escalation_id = 412,
                                                                  .review_id = 88213,
                                                                  .location_id = 17,
                                                                  .local_post_id = std::nullopt}))
                             .as_object();
    const auto& aps = payload.at("aps").as_object();
    EXPECT_EQ(aps.at("alert").at("title").as_string(), "New escalation");
    EXPECT_EQ(aps.at("alert").at("body").as_string(), "Escalation: Gangnam Buffet - Food poisoning");
    EXPECT_EQ(aps.at("category").as_string(), "ESCALATION");
    EXPECT_EQ(aps.at("thread-id").as_string(), "escalation");
    EXPECT_EQ(aps.at("sound").as_string(), "default");
    EXPECT_EQ(payload.at("event_type").as_string(), "escalation");
    EXPECT_EQ(payload.at("escalation_id").as_int64(), 412);
    EXPECT_EQ(payload.at("review_id").as_int64(), 88213);
    EXPECT_EQ(payload.at("location_id").as_int64(), 17);
    EXPECT_FALSE(payload.contains("local_post_id"));
}

TEST(PushPayload, ASystemProblemIsOfItsOwnKindAndALongSubjectIsCut) {
    const std::string subject(500, 'x');
    const auto payload = boost::json::parse(domain::push_payload({.event_type = "system_problem",
                                                                  .subject = subject,
                                                                  .escalation_id = std::nullopt,
                                                                  .review_id = std::nullopt,
                                                                  .location_id = 3,
                                                                  .local_post_id = 9}))
                             .as_object();
    const auto& aps = payload.at("aps").as_object();
    EXPECT_EQ(aps.at("alert").at("title").as_string(), "System problem");
    EXPECT_EQ(aps.at("category").as_string(), "SYSTEM");
    EXPECT_LE(aps.at("alert").at("body").as_string().size(), domain::kPushLineLength + 3);
    EXPECT_EQ(payload.at("local_post_id").as_int64(), 9);
    EXPECT_FALSE(payload.contains("review_id"));
}

TEST(PushPayload, TheCollapseIdIsTheSameForTheSameEventAndFitsApple) {
    const auto id = domain::push_collapse_id("case:12:opened:40");
    EXPECT_EQ(id.size(), 64u);
    EXPECT_EQ(id, domain::push_collapse_id("case:12:opened:40"));
    EXPECT_NE(id, domain::push_collapse_id("case:12:opened:41"));
}

// ---- what a delivery's answers come to ------------------------------------------------------

TEST(PushOutcome, ADeviceThatTookItMakesItSentAndGoneDevicesAreCounted) {
    using Kind = domain::PushAnswer::Kind;
    const auto sent = domain::push_outcome({{Kind::device_gone, "410 Unregistered"}, {Kind::accepted, ""}}, false);
    EXPECT_EQ(sent.status, "sent");
    EXPECT_EQ(sent.summary, "reached 1 of 2 device(s); 1 no longer registered and dropped");
    EXPECT_EQ(domain::push_outcome({{Kind::device_gone, "400 BadDeviceToken"}}, false).status, "not_sent");
    const auto refused = domain::push_outcome({{Kind::refused, "400 BadCollapseId"}}, false);
    EXPECT_EQ(refused.status, "failed");
    EXPECT_EQ(refused.summary, "reached 0 of 1 device(s); Apple answered 400 BadCollapseId");
}

TEST(PushOutcome, AnAnswerThatMayPassSendsAgainUntilTheLastAttempt) {
    using Kind = domain::PushAnswer::Kind;
    const std::vector<domain::PushAnswer> answers{{Kind::accepted, ""}, {Kind::retry, "503 ServiceUnavailable"}};
    EXPECT_EQ(domain::push_outcome(answers, false).status, "pending");
    EXPECT_EQ(domain::push_outcome(answers, true).status, "sent");
    EXPECT_EQ(domain::push_outcome({{Kind::retry, "429 TooManyRequests"}}, true).status, "failed");
}

TEST(PushOutcome, SettingsAppleRefusesAreNamed) {
    using Kind = domain::PushAnswer::Kind;
    const auto out = domain::push_outcome({{Kind::sender_refused, "403 InvalidProviderToken"}}, false);
    EXPECT_EQ(out.status, "failed");
    EXPECT_EQ(out.sender_problem, "403 InvalidProviderToken");
    EXPECT_TRUE(domain::push_outcome({{Kind::accepted, ""}}, false).sender_problem.empty());
}

TEST(PushOutcome, ADeviceTokenIsLowerCaseHexOf32To100Bytes) {
    EXPECT_TRUE(domain::is_device_token(std::string(64, 'a')));
    EXPECT_TRUE(domain::is_device_token(std::string(200, '0')));
    EXPECT_FALSE(domain::is_device_token(std::string(62, 'a')));
    EXPECT_FALSE(domain::is_device_token(std::string(202, 'a')));
    EXPECT_FALSE(domain::is_device_token(std::string(65, 'a')));
    EXPECT_FALSE(domain::is_device_token(std::string(64, 'A')));
    EXPECT_FALSE(domain::is_device_token(std::string(63, 'a') + "g"));
}
