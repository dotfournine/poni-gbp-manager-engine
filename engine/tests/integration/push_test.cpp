// Push to the phone app (D-097) over a real PostgreSQL, with Apple's push
// service a fake that keeps what it is given: who has a device, what a push
// rule may be, what a delivery sends and to which devices, and what the log
// says when a device is gone, Apple is busy or refuses the settings.

#include "db/repo_auth.hpp"
#include "db/repo_devices.hpp"
#include "db/repo_notifications.hpp"
#include "domain/push.hpp"
#include "reply_fixture.hpp"

#include <boost/json/parse.hpp>
#include <gmock/gmock.h>

using namespace poni;
using namespace std::chrono_literals;
using test::FakeLlm;
using test::kBuffet;
using test::make_review;
using testing::HasSubstr;
using testing::Not;
using Kind = notify::PushAnswer::Kind;

namespace {

std::string token_of(char c) { return std::string(64, c); }

constexpr auto kSandbox = core::ApnsEnvironment::sandbox;

class Push : public test::ReplyTest {
protected:
    // A rule as a person makes one, on an engine with the APNs settings.
    repo::Result<model::NotificationRule> make_rule(std::string event_type, std::string channel, std::vector<std::string> recipients,
                                                   bool push_configured = true) {
        return as_test([&](pqxx::work& tx, audit::Writer& audit) {
            auto result = repo::create_rule(
                tx, audit, {.event_type = event_type, .channel = channel, .location_id = std::nullopt, .recipients = recipients}, push_configured);
            if (!result) audit.nothing_changed();
            return result;
        });
    }

    // A person signed in to the phone app on a device that registered its
    // token. Returns the session's hash.
    std::string phone(const std::string& email, char token) {
        const std::string hash = "session-" + email + "-" + token;
        add_app_session(email, hash);
        as_test([&](pqxx::work& tx, audit::Writer& audit) {
            return repo::register_device(tx, audit, hash, {.apns_token = token_of(token), .environment = "sandbox", .app_version = "1.0 (1)"},
                                         kSandbox)
                .value();
        });
        return hash;
    }

    repo::NotificationEvent escalation(std::string key = "case:7:opened:1") {
        return {.type = "escalation",
                .key = std::move(key),
                .location_id = buffet_,
                .review_id = std::nullopt,
                .subject = "Escalation: Gangnam AYCE Buffet - Illness or food poisoning",
                .body = "The review said: By 2am we were both throwing up.",
                .link_path = "/#/escalations/7/show",
                .escalation_id = 7};
    }

    void emit(const repo::NotificationEvent& event) {
        tx([&](pqxx::work& tx) { repo::emit(tx, event, clock_.now()); });
    }

    std::string push_delivery(const std::string& column) {
        return value<std::string>("SELECT coalesce(" + column + "::text, '') FROM notification_deliveries WHERE channel = 'push' ORDER BY id DESC LIMIT 1");
    }
};

}  // namespace

// ---- rules -----------------------------------------------------------------------------

TEST_F(Push, APushRuleIsForEscalationsAndSystemProblemsOnAnEngineThatCanPush) {
    const auto made = make_rule("escalation", "push", {"Owner@XVisionMediaUS.com"});
    ASSERT_TRUE(made);
    EXPECT_EQ(made->channel, "push");
    EXPECT_THAT(made->recipients, testing::ElementsAre("owner@xvisionmediaus.com"));
    EXPECT_TRUE(make_rule("system_problem", "push", {"owner@xvisionmediaus.com"}));
    // The same event by email is a rule of its own.
    EXPECT_TRUE(make_rule("escalation", "email", {"owner@xvisionmediaus.com"}));
    EXPECT_EQ(make_rule("escalation", "push", {"x@xvisionmediaus.com"}).error().code, "rule_exists");

    EXPECT_EQ(make_rule("approval_needed", "push", {"x@xvisionmediaus.com"}).error().kind, repo::Failure::Kind::invalid);
    EXPECT_EQ(make_rule("location_report", "push", {"x@xvisionmediaus.com"}).error().kind, repo::Failure::Kind::invalid);
    EXPECT_EQ(make_rule("escalation", "sms", {"x@xvisionmediaus.com"}).error().kind, repo::Failure::Kind::invalid);
    EXPECT_EQ(make_rule("system_problem", "push", {"x@xvisionmediaus.com"}, false).error().code, "push_not_configured");
    // The database holds the same rule.
    EXPECT_THROW(exec("INSERT INTO notification_rules (event_type, channel, recipients, created_at, updated_at) "
                      "VALUES ('approval_needed', 'push', '[]', now(), now())"),
                 pqxx::check_violation);
}

// ---- a delivery -------------------------------------------------------------------------

TEST_F(Push, AnEscalationPushesItsSubjectAndIdsToEveryDeviceOfThePerson) {
    ASSERT_TRUE(make_rule("escalation", "push", {"owner@xvisionmediaus.com"}));
    phone("owner@xvisionmediaus.com", 'a');
    phone("owner@xvisionmediaus.com", 'b');
    phone("someone.else@xvisionmediaus.com", 'c');
    emit(escalation());
    run_jobs();

    ASSERT_EQ(push_.sent.size(), 2u);
    EXPECT_EQ(push_.sent[0].device_token, token_of('a'));
    EXPECT_EQ(push_.sent[1].device_token, token_of('b'));
    EXPECT_EQ(push_.sent[0].collapse_id, domain::push_collapse_id("case:7:opened:1"));
    const auto payload = boost::json::parse(push_.sent[0].payload).as_object();
    EXPECT_EQ(payload.at("aps").at("alert").at("body").as_string(), "Escalation: Gangnam AYCE Buffet - Illness or food poisoning");
    EXPECT_EQ(payload.at("escalation_id").as_int64(), 7);
    EXPECT_EQ(payload.at("location_id").as_int64(), buffet_);
    EXPECT_THAT(push_.sent[0].payload, Not(HasSubstr("throwing up"))) << "a lock screen never shows what a review says";
    // One row for the person, as for email.
    EXPECT_EQ(count("notification_deliveries WHERE channel = 'push'"), 1);
    EXPECT_EQ(push_delivery("recipient || ' ' || status || ' ' || status_reason"), "owner@xvisionmediaus.com sent reached 2 of 2 device(s)");
    EXPECT_EQ(push_delivery("escalation_id"), "7");
    EXPECT_TRUE(channel_.sent.empty());
}

TEST_F(Push, AFlaggedReviewPushesTheEscalationItOpened) {
    ASSERT_TRUE(make_rule("escalation", "push", {"owner@xvisionmediaus.com"}));
    phone("owner@xvisionmediaus.com", 'a');
    await(sync_->full_sync(buffet_));
    run_jobs();
    llm_.analyzer = FakeLlm::always(test::kSevereAnswer);
    llm_.generator = FakeLlm::always(test::kHoldingAnswer);
    gbp_.put(make_review(gbp_.account_id, kBuffet, "bad", "TWO", "By 2am we were both throwing up. Pretty sure it was the crab legs.",
                         "2026-11-10T11:58:00Z", "2026-11-10T11:58:00Z"));
    await(sync_->poll_reviews());
    run_jobs();

    ASSERT_EQ(push_.sent.size(), 1u);
    const auto payload = boost::json::parse(push_.sent[0].payload).as_object();
    EXPECT_EQ(payload.at("escalation_id").as_int64(), value<std::int64_t>("SELECT id FROM cases"));
    EXPECT_EQ(payload.at("review_id").as_int64(), value<std::int64_t>("SELECT review_id FROM cases"));
    EXPECT_EQ(payload.at("aps").at("category").as_string(), "ESCALATION");
    EXPECT_THAT(push_.sent[0].payload, Not(HasSubstr("crab")));
}

TEST_F(Push, ADeviceAppleCallsGoneIsDroppedAndTheOthersStillGetIt) {
    ASSERT_TRUE(make_rule("escalation", "push", {"owner@xvisionmediaus.com"}));
    phone("owner@xvisionmediaus.com", 'a');
    phone("owner@xvisionmediaus.com", 'b');
    push_.answers = {{Kind::device_gone, "410 Unregistered"}, {Kind::accepted, ""}};
    emit(escalation());
    run_jobs();
    EXPECT_EQ(push_delivery("status"), "sent");
    EXPECT_THAT(push_delivery("status_reason"), HasSubstr("reached 1 of 2 device(s); 1 no longer registered and dropped"));
    EXPECT_EQ(value<std::string>("SELECT string_agg(apns_token, ',') FROM devices"), token_of('b'));
}

TEST_F(Push, WhenEveryDeviceIsGoneNothingWasSent) {
    ASSERT_TRUE(make_rule("escalation", "push", {"owner@xvisionmediaus.com"}));
    phone("owner@xvisionmediaus.com", 'a');
    push_.answers = {{Kind::device_gone, "400 BadDeviceToken"}};
    emit(escalation());
    run_jobs();
    EXPECT_EQ(push_delivery("status"), "not_sent");
    EXPECT_EQ(count("devices"), 0);
}

TEST_F(Push, AnAnswerThatMayPassSendsAgainToEveryDevice) {
    ASSERT_TRUE(make_rule("escalation", "push", {"owner@xvisionmediaus.com"}));
    phone("owner@xvisionmediaus.com", 'a');
    phone("owner@xvisionmediaus.com", 'b');
    push_.answers = {{Kind::accepted, ""}, {Kind::retry, "503 ServiceUnavailable"}};
    emit(escalation());
    run_jobs();
    EXPECT_EQ(push_delivery("status || ' ' || attempts"), "pending 1");
    EXPECT_THAT(push_delivery("status_reason"), HasSubstr("503 ServiceUnavailable"));
    clock_.advance(31s);
    run_jobs();
    EXPECT_EQ(push_delivery("status || ' ' || attempts"), "sent 2");
    // Sent again to both: the collapse id makes it one alert on the phone that had it.
    ASSERT_EQ(push_.sent.size(), 4u);
    EXPECT_EQ(push_.sent[2].collapse_id, push_.sent[0].collapse_id);
}

TEST_F(Push, ANetworkFailureIsRetriedAndThenSent) {
    ASSERT_TRUE(make_rule("system_problem", "push", {"owner@xvisionmediaus.com"}));
    phone("owner@xvisionmediaus.com", 'a');
    push_.failures.push_back({notify::SendError::Kind::transient, "network error: timed out"});
    emit({.type = "system_problem", .key = "p1", .location_id = std::nullopt, .review_id = std::nullopt,
          .subject = "Lost access to a location", .body = "Details.", .link_path = "/#/status"});
    run_jobs();
    EXPECT_EQ(push_delivery("status || ' ' || status_reason"), "pending network error: timed out");
    EXPECT_EQ(value<std::string>("SELECT last_error FROM integration_status WHERE name = 'push'"), "network error: timed out");
    clock_.advance(31s);
    run_jobs();
    EXPECT_EQ(push_delivery("status"), "sent");
    const auto payload = boost::json::parse(push_.sent.at(0).payload).as_object();
    EXPECT_EQ(payload.at("aps").at("category").as_string(), "SYSTEM");
    EXPECT_EQ(payload.at("event_type").as_string(), "system_problem");
}

TEST_F(Push, SettingsAppleRefusesFailTheDeliveryAndAreASystemProblemOnceADay) {
    ASSERT_TRUE(make_rule("escalation", "push", {"owner@xvisionmediaus.com"}));
    ASSERT_TRUE(make_rule("system_problem", "email", {"ops@xvisionmediaus.com"}));
    phone("owner@xvisionmediaus.com", 'a');
    push_.answers = {{Kind::sender_refused, "403 InvalidProviderToken"}, {Kind::sender_refused, "403 InvalidProviderToken"}};
    emit(escalation("case:7:opened:1"));
    emit(escalation("case:8:opened:2"));
    run_jobs();
    EXPECT_EQ(count("notification_deliveries WHERE channel = 'push' AND status = 'failed'"), 2);
    EXPECT_THAT(push_delivery("status_reason"), HasSubstr("403 InvalidProviderToken"));
    ASSERT_EQ(channel_.sent.size(), 1u) << "the ops address hears of it once";
    EXPECT_EQ(channel_.sent[0].subject, "Apple refused the engine's push settings");
    EXPECT_THAT(value<std::string>("SELECT last_error FROM integration_status WHERE name = 'push'"), HasSubstr("InvalidProviderToken"));
}

// ---- who has a device -------------------------------------------------------------------

TEST_F(Push, APersonWithoutADeviceIsNotSentTo) {
    ASSERT_TRUE(make_rule("escalation", "push", {"owner@xvisionmediaus.com"}));
    emit(escalation());
    run_jobs();
    EXPECT_EQ(push_delivery("status || ' ' || status_reason"), "not_sent the person has no device signed in to the phone app");
    EXPECT_TRUE(push_.sent.empty());
}

TEST_F(Push, AnEndedSessionOrADisabledUserStopsThePushes) {
    ASSERT_TRUE(make_rule("escalation", "push", {"owner@xvisionmediaus.com"}));
    const auto signed_out = phone("owner@xvisionmediaus.com", 'a');
    const auto idle = phone("owner@xvisionmediaus.com", 'b');
    phone("owner@xvisionmediaus.com", 'c');
    // Signing out deletes the session, and the device with it.
    tx([&](pqxx::work& tx) { repo::delete_session(tx, signed_out); });
    EXPECT_EQ(count("devices"), 2);
    // A session that was not used for 30 days has ended, though housekeeping has not yet deleted it.
    exec("UPDATE sessions SET last_seen_at = last_seen_at - interval '31 days' WHERE token_hash = '" + idle + "'");
    emit(escalation("case:7:opened:1"));
    run_jobs();
    ASSERT_EQ(push_.sent.size(), 1u);
    EXPECT_EQ(push_.sent[0].device_token, token_of('c'));

    exec("UPDATE users SET status = 'disabled' WHERE email = 'owner@xvisionmediaus.com'");
    emit(escalation("case:8:opened:2"));
    run_jobs();
    EXPECT_EQ(push_.sent.size(), 1u);
    EXPECT_EQ(push_delivery("status"), "not_sent");
}

TEST_F(Push, ATokenBelongsToOneSessionAndARegistrationThatChangesNothingWritesNothing) {
    const auto first = phone("owner@xvisionmediaus.com", 'a');
    EXPECT_EQ(count("audit_log WHERE action = 'device.register'"), 1);
    // The app registers on every launch.
    as_test([&](pqxx::work& tx, audit::Writer& audit) {
        return repo::register_device(tx, audit, first, {.apns_token = token_of('a'), .environment = "sandbox", .app_version = "1.0 (1)"},
                                     kSandbox)
            .value();
    });
    EXPECT_EQ(count("audit_log WHERE action = 'device.register'"), 1);
    EXPECT_EQ(count("audit_log WHERE after::text LIKE '%" + token_of('a') + "%'"), 0) << "a token is in no audit entry";

    // The phone changes hands: the same token, another person's session.
    phone("manager@xvisionmediaus.com", 'a');
    EXPECT_EQ(count("devices"), 1);
    EXPECT_EQ(value<std::string>("SELECT u.email FROM devices d JOIN sessions s ON s.token_hash = d.session_token_hash "
                                 "JOIN users u ON u.id = s.user_id"),
              "manager@xvisionmediaus.com");

    const auto refused = [&](repo::NewDevice device, std::optional<core::ApnsEnvironment> environment) {
        return as_test([&](pqxx::work& tx, audit::Writer& audit) {
            auto result = repo::register_device(tx, audit, first, device, environment);
            if (!result) audit.nothing_changed();
            return result;
        });
    };
    EXPECT_FALSE(refused({.apns_token = token_of('d'), .environment = "production", .app_version = "1"}, kSandbox));
    EXPECT_FALSE(refused({.apns_token = token_of('d'), .environment = "sandbox", .app_version = "1"}, std::nullopt));
    EXPECT_FALSE(refused({.apns_token = "not-hex", .environment = "sandbox", .app_version = "1"}, kSandbox));
    EXPECT_FALSE(refused({.apns_token = token_of('d'), .environment = "sandbox", .app_version = ""}, kSandbox));
    // A token in capitals is the same token.
    EXPECT_TRUE(refused({.apns_token = std::string(64, 'D'), .environment = "sandbox", .app_version = "1"}, kSandbox));
    EXPECT_EQ(value<std::string>("SELECT apns_token FROM devices WHERE session_token_hash = '" + first + "'"), token_of('d'));
}
