// The phone app's devices through the API (D-097): the app registers its
// token with its bearer token, once per session; a browser cannot; an
// engine without the APNs settings or of the other environment refuses;
// a person removes only their own; signing out takes the device along.
// And a push rule through the API.

#include "api_fixture.hpp"

#include <gmock/gmock.h>

using namespace poni;
using boost::beast::http::verb;
using testing::HasSubstr;

namespace {

class DevicesApi : public test::ApiTest {
protected:
    void SetUp() override {
        test::ApiTest::SetUp();
        api_options_.push_environment = core::ApnsEnvironment::sandbox;
        build({});
        browser_ = sign_in();
    }

    static std::string registration(char token = 'a', std::string environment = "sandbox", std::string version = "1.0 (12)") {
        return boost::json::serialize(
            boost::json::object{{"apns_token", std::string(64, token)}, {"environment", environment}, {"app_version", version}});
    }
};

}  // namespace

TEST_F(DevicesApi, TheAppRegistersItsDeviceOncePerSession) {
    const auto token = app_sign_in();
    const auto made = as_app(verb::post, "/api/devices", token, registration());
    ASSERT_EQ(made.status, 201u) << made.body;
    const auto device = json(made).as_object();
    EXPECT_EQ(device.at("environment").as_string(), "sandbox");
    EXPECT_EQ(device.at("app_version").as_string(), "1.0 (12)");
    EXPECT_TRUE(device.contains("last_seen_at"));
    EXPECT_FALSE(device.contains("apns_token"));

    // Every launch registers again; only a change is a change.
    const auto again = as_app(verb::post, "/api/devices", token, registration());
    ASSERT_EQ(again.status, 201u);
    EXPECT_EQ(json(again).at("id"), device.at("id"));
    EXPECT_EQ(count("audit_log WHERE action = 'device.register'"), 1);
    const auto updated = as_app(verb::post, "/api/devices", token, registration('a', "sandbox", "1.1 (13)"));
    EXPECT_EQ(json(updated).at("id"), device.at("id"));
    EXPECT_EQ(count("audit_log WHERE action = 'device.register'"), 2);
    EXPECT_EQ(count("devices"), 1);
}

TEST_F(DevicesApi, OnlyTheAppOnAnEngineThatPushesToItsEnvironmentRegisters) {
    // The dashboard's session has no phone.
    const auto from_browser = post("/api/devices", registration());
    EXPECT_EQ(from_browser.status, 400u);
    EXPECT_THAT(from_browser.body, HasSubstr("Only the phone app"));

    const auto token = app_sign_in();
    const auto other = as_app(verb::post, "/api/devices", token, registration('a', "production"));
    EXPECT_EQ(other.status, 400u);
    EXPECT_THAT(other.body, HasSubstr("APNs sandbox"));
    EXPECT_EQ(as_app(verb::post, "/api/devices", token, registration('z')).status, 400u) << "a token is hex";
    // A member the spec does not have is refused (D-092).
    EXPECT_EQ(as_app(verb::post, "/api/devices", token,
                     R"({"apns_token":")" + std::string(64, 'a') + R"(","environment":"sandbox","app_version":"1","name":"x"})")
                  .status,
              400u);

    api_options_.push_environment.reset();
    build({});
    const auto unconfigured = as_app(verb::post, "/api/devices", token, registration());
    EXPECT_EQ(unconfigured.status, 400u);
    EXPECT_THAT(unconfigured.body, HasSubstr("no push settings"));
    EXPECT_EQ(count("devices"), 0);
}

TEST_F(DevicesApi, APersonRemovesTheirOwnDeviceAndSigningOutTakesItAlong) {
    const auto mine = app_sign_in();
    const auto id = json(as_app(verb::post, "/api/devices", mine, registration('a'))).at("id").as_int64();
    const auto theirs = app_sign_in(staff("manager@xvisionmediaus.com"));
    const auto other_id = json(as_app(verb::post, "/api/devices", theirs, registration('b'))).at("id").as_int64();

    EXPECT_EQ(as_app(verb::delete_, "/api/devices/" + std::to_string(other_id), mine).status, 404u);
    EXPECT_EQ(as_app(verb::delete_, "/api/devices/" + std::to_string(id), mine).status, 204u);
    EXPECT_EQ(as_app(verb::delete_, "/api/devices/" + std::to_string(id), mine).status, 404u);
    EXPECT_EQ(count("audit_log WHERE action = 'device.unregister'"), 1);

    EXPECT_EQ(as_app(verb::post, "/api/auth/logout", theirs).status, 204u);
    EXPECT_EQ(count("devices"), 0);
}

// What a push rule may be is tested on the repository (Push.APushRule...); here, only that the API carries the channel and the setting.
TEST_F(DevicesApi, APushRuleIsMadeOnTheWebAndRefusedWithoutTheApnsSettings) {
    const auto made = post("/api/notification-rules", R"({"event_type":"escalation","channel":"push","recipients":["owner@xvisionmediaus.com"]})");
    ASSERT_EQ(made.status, 201u) << made.body;
    EXPECT_EQ(json(made).at("channel").as_string(), "push");

    api_options_.push_environment.reset();
    build({});
    browser_ = sign_in();
    const auto refused = post("/api/notification-rules", R"({"event_type":"system_problem","channel":"push","recipients":["a@xvisionmediaus.com"]})");
    EXPECT_EQ(refused.status, 409u);
    EXPECT_THAT(refused.body, HasSubstr("push_not_configured"));
}
