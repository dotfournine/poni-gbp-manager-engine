// Sign-in for the phone app (D-095) end to end with a fake identity provider:
// the start with a PKCE challenge, the callback's redirect to the app, the
// one-time code and its exchange, the bearer token in the authorization hook,
// the two kinds of session and their lifetimes, sign-out and development
// sign-in. The dashboard's own sign-in is in auth_test.cpp.

#include "api_fixture.hpp"
#include "core/crypto.hpp"
#include "core/log.hpp"
#include "db/repo_auth.hpp"
#include "http/browser_flow.hpp"
#include "jobs/builtin.hpp"
#include "jobs/scheduler.hpp"

#include <boost/asio/co_spawn.hpp>
#include <gmock/gmock.h>

using namespace poni;
using namespace std::chrono_literals;
using testing::HasSubstr;
using testing::Not;

namespace {

const std::string kStaff = "staff@xvisionmediaus.com";
const std::string kCallback = "/api/auth/google/callback";

class AppSignIn : public test::ApiTest {
protected:
    // The value of the attempt's cookie in a start's answer.
    static std::string flow_secret(const http::Response& start) {
        return http::cookie_value(pair_of(set_cookie(start, "poni_signin")), "poni_signin").value_or("");
    }

    // How many audit entries hold `text` anywhere.
    long audited(const std::string& text) {
        return count("audit_log WHERE concat(actor, action, object_id, before::text, after::text) LIKE '%" + text + "%'");
    }

    // What the hourly housekeeping deletes, with the decided lifetimes.
    std::size_t purge_sessions() {
        return tx([&](pqxx::work& tx) { return repo::purge_sessions(tx, clock_.now(), domain::SessionLifetimes{}); });
    }
    std::size_t purge_codes() {
        return tx([&](pqxx::work& tx) { return repo::purge_app_codes(tx, clock_.now()); });
    }
};

}  // namespace

TEST_F(AppSignIn, TheAppSignsInWithGoogleAndGetsAToken) {
    provider_.identities["the-code"] = staff();
    const auto challenge = http::s256_challenge(kAppVerifier);

    const auto start = get(app_start());
    EXPECT_EQ(start.status, 302u);
    EXPECT_THAT(header(start, "Location"), testing::StartsWith("https://idp.example/authorize"));
    const auto flow = set_cookie(start, "poni_signin");
    EXPECT_THAT(flow, testing::AllOf(HasSubstr("HttpOnly"), HasSubstr("SameSite=Lax"), HasSubstr("Path=/api/auth"), HasSubstr("Max-Age=600")));
    ASSERT_TRUE(provider_.last_request);
    EXPECT_EQ(provider_.last_request->redirect_uri, "http://localhost:5173/api/auth/google/callback") << "Google is asked as for the dashboard";
    EXPECT_NE(provider_.last_request->code_challenge, challenge) << "the challenge Google gets is the engine's own, not the app's";

    const auto done = get(kCallback + "?code=the-code&state=" + provider_.last_request->state, {.cookie = pair_of(flow)});
    EXPECT_EQ(done.status, 302u);
    const auto code = app_code(done);
    ASSERT_GE(code.size(), 40u) << header(done, "Location");
    EXPECT_TRUE(set_cookie(done, "poni_session").empty()) << "the app's sign-in sets no session cookie";
    EXPECT_THAT(set_cookie(done, "poni_signin"), HasSubstr("Max-Age=0")) << "the one-time flow cookie is cleared";
    EXPECT_EQ(http::s256_challenge(provider_.redeemed_verifier), provider_.last_request->code_challenge);
    EXPECT_EQ(count("sessions"), 0) << "there is no session before the code is exchanged";
    EXPECT_EQ(count("app_sign_in_codes WHERE code_hash = '" + core::sha256_hex(code) + "' AND code_challenge = '" + challenge + "'"), 1);
    EXPECT_EQ(count("app_sign_in_codes WHERE code_hash = '" + code + "'"), 0) << "only the code's hash is stored";
    // The callback wrote the user, so it wrote an entry: in the person's name, and without the code or the challenge.
    EXPECT_EQ(count("audit_log WHERE action = 'session.app_code_issued' AND actor = '" + kStaff + "' AND actor_type = 'user' "
                    "AND ip = '203.0.113.10' AND object_type = 'user' AND object_id = (SELECT id::text FROM users) "
                    "AND after = jsonb_build_object('email', actor, 'first_sign_in', true)"),
              1);
    EXPECT_EQ(count("audit_log WHERE action = 'session.login'"), 0) << "that one is the exchange's";

    const auto answer = exchange(code);
    ASSERT_EQ(answer.status, 200u) << answer.body;
    EXPECT_TRUE(set_cookie(answer, "poni_session").empty());
    const auto token = std::string(json(answer).at("token").as_string());
    EXPECT_GE(token.size(), 40u);
    EXPECT_EQ(json(answer).at("expires_at").as_string(), "2027-02-08T12:00:00.000000Z") << "ninety days after signing in";
    EXPECT_TRUE(json(answer).at("session").at("authenticated").as_bool());
    EXPECT_EQ(json(answer).at("session").at("user").at("email").as_string(), kStaff);
    EXPECT_TRUE(json(answer).at("session").at("csrf_token").is_null());

    EXPECT_EQ(count("app_sign_in_codes"), 0);
    EXPECT_EQ(count("sessions"), 1);
    EXPECT_EQ(count("sessions WHERE kind = 'app' AND csrf_token IS NULL AND token_hash = '" + core::sha256_hex(token) + "'"), 1);
    EXPECT_EQ(count("sessions WHERE token_hash = '" + token + "'"), 0) << "only the token's hash is stored";
    EXPECT_EQ(count("audit_log WHERE action = 'session.login' AND actor = '" + kStaff + "' AND actor_type = 'user' "
                    "AND ip = '203.0.113.10' AND after->>'kind' = 'app' AND (after->>'first_sign_in')::boolean"),
              1);

    // The token is the session: reading, the session itself, and a change without a CSRF token.
    EXPECT_EQ(as_app(verb::get, "/api/clients", token).status, 200u);
    const auto session = json(as_app(verb::get, "/api/session", token));
    EXPECT_TRUE(session.at("authenticated").as_bool());
    EXPECT_EQ(session.at("user").at("email").as_string(), kStaff);
    EXPECT_TRUE(session.at("csrf_token").is_null()) << "a token needs none";
    EXPECT_EQ(as_app(verb::post, "/api/clients", token, R"({"name": "Made in the app"})").status, 201u);
    EXPECT_EQ(count("clients"), 1);

    const auto out = as_app(verb::post, "/api/auth/logout", token);
    EXPECT_EQ(out.status, 204u);
    EXPECT_TRUE(set_cookie(out, "poni_session").empty()) << "there is no cookie to clear";
    EXPECT_EQ(as_app(verb::get, "/api/clients", token).status, 401u);
    EXPECT_FALSE(json(as_app(verb::get, "/api/session", token)).at("authenticated").as_bool());
    EXPECT_EQ(count("sessions"), 0);
    EXPECT_EQ(count("audit_log WHERE action = 'session.logout' AND actor = '" + kStaff + "' AND after->>'kind' = 'app'"), 1);

    // A second sign-in reuses the user.
    app_sign_in();
    EXPECT_EQ(count("users"), 1);
    EXPECT_EQ(count("audit_log WHERE action = 'session.login' AND after->>'kind' = 'app' AND NOT (after->>'first_sign_in')::boolean"), 1);
    EXPECT_EQ(count("audit_log WHERE action = 'session.app_code_issued'"), 2);
    EXPECT_EQ(count("audit_log WHERE action = 'session.app_code_issued' AND NOT (after->>'first_sign_in')::boolean"), 1);
}

TEST_F(AppSignIn, ACodeNobodyExchangesIsStillInTheAuditLog) {
    // The app was closed in the sheet: the callback created the user, and nothing follows.
    ASSERT_FALSE(app_code(app_sign_in_response(staff())).empty());
    EXPECT_EQ(count("users"), 1);
    EXPECT_EQ(count("sessions"), 0);
    EXPECT_EQ(count("audit_log WHERE action = 'session.app_code_issued' AND actor = '" + kStaff + "' AND (after->>'first_sign_in')::boolean"), 1)
        << "no user row is written without an entry";
    EXPECT_EQ(count("audit_log WHERE action = 'session.login'"), 0);

    // A second attempt of the same person refreshes the user, and says so.
    clock_.advance(5min);
    ASSERT_FALSE(app_code(app_sign_in_response(staff())).empty());
    EXPECT_EQ(count("audit_log WHERE action = 'session.app_code_issued' AND NOT (after->>'first_sign_in')::boolean"), 1);
    EXPECT_EQ(count("audit_log"), 2);
}

TEST_F(AppSignIn, ACodeWorksOnceAndForAMinute) {
    const auto code = app_code(app_sign_in_response(staff()));
    EXPECT_EQ(exchange(code).status, 200u);
    const auto again = exchange(code);
    EXPECT_EQ(again.status, 400u);
    EXPECT_EQ(error_code(again), "invalid_request");
    EXPECT_EQ(count("sessions"), 1);

    const auto in_time = app_code(app_sign_in_response(staff()));
    clock_.advance(59s);
    EXPECT_EQ(exchange(in_time).status, 200u);

    const auto late = app_code(app_sign_in_response(staff()));
    clock_.advance(61s);
    const auto too_late = exchange(late);
    EXPECT_EQ(too_late.status, 400u);
    EXPECT_EQ(count("app_sign_in_codes"), 0) << "it was presented, so it is gone";
    EXPECT_EQ(count("sessions"), 2);

    // Whatever was wrong with a code, the answer is the same.
    const auto unknown = exchange("a-code-nobody-was-given");
    EXPECT_EQ(unknown.status, 400u);
    EXPECT_EQ(again.body, unknown.body);
    EXPECT_EQ(too_late.body, unknown.body);
}

TEST_F(AppSignIn, ACodeWithAWrongVerifierIsRefusedAndSpent) {
    const auto code = app_code(app_sign_in_response(staff()));
    const auto wrong = exchange(code, "somebody-elses-verifier-0123456789-0123456789");
    EXPECT_EQ(wrong.status, 400u);
    EXPECT_EQ(wrong.body, exchange("a-code-nobody-was-given").body) << "the answer does not say that the code was a real one";
    EXPECT_EQ(exchange(code).status, 400u) << "spent by the wrong attempt: the right verifier comes too late";
    EXPECT_EQ(count("app_sign_in_codes"), 0);
    EXPECT_EQ(count("sessions"), 0);
    EXPECT_EQ(count("audit_log WHERE action = 'session.login'"), 0);
    // Somebody held a code who did not start the sign-in: the audit log says whose.
    EXPECT_EQ(count("audit_log WHERE action = 'session.login_refused' AND actor_type = 'anonymous' AND actor = '" + kStaff +
                    "' AND after->>'reason' = 'the app''s code came with a wrong verifier'"),
              1);

    // The challenge travelled through the browser; it is not the verifier.
    const auto next = app_code(app_sign_in_response(staff()));
    EXPECT_EQ(exchange(next, http::s256_challenge(kAppVerifier)).status, 400u);

    // A verifier short enough to be found by trying is none (RFC 7636: 43 to 128 unreserved characters), even
    // with its own challenge at the start; nor is one with another character in it.
    for (const std::string& weak : {std::string("1234"), std::string(42, 'a'), std::string(129, 'a'), std::string(42, 'a') + "+"}) {
        const auto code_for_it = app_code(app_sign_in_response(staff(), http::s256_challenge(weak)));
        ASSERT_FALSE(code_for_it.empty());
        EXPECT_EQ(exchange(code_for_it, weak).status, 400u) << weak.size() << " characters";
        clock_.advance(11min);  // the next sign-in is in a new window of the attempt limit
    }
    const std::string longest(128, 'a');
    EXPECT_EQ(exchange(app_code(app_sign_in_response(staff(), http::s256_challenge(longest))), longest).status, 200u);
    EXPECT_EQ(count("sessions"), 1);
}

TEST_F(AppSignIn, OfTwoExchangesAtOnceOneGetsTheToken) {
    for (int round = 0; round < 3; ++round) {
        const auto code = app_code(app_sign_in_response(staff()));
        std::vector<unsigned> statuses;
        for (int i = 0; i < 2; ++i) {
            http::Request request;
            request.method = verb::post;
            request.path = "/api/auth/app-token";
            request.remote_ip = "203.0.113.10";
            request.body = boost::json::serialize(boost::json::object{{"code", code}, {"code_verifier", kAppVerifier}});
            // Both are on their way before either is answered; the pool runs their transactions on two threads.
            boost::asio::co_spawn(io_, router_->dispatch(std::move(request)), [&statuses](std::exception_ptr error, http::Response response) {
                EXPECT_FALSE(error);
                statuses.push_back(response.status);
            });
        }
        io_.run();
        io_.restart();
        EXPECT_THAT(statuses, testing::UnorderedElementsAre(200u, 400u)) << "round " << round;
    }
    EXPECT_EQ(count("sessions"), 3);
    EXPECT_EQ(count("audit_log WHERE action = 'session.login'"), 3);
}

TEST_F(AppSignIn, ADashboardSignInLeavesNoCodeToExchange) {
    const auto done = sign_in_response(staff());
    EXPECT_EQ(header(done, "Location"), "http://localhost:5173/");
    const auto cookie = pair_of(set_cookie(done, "poni_session"));
    ASSERT_FALSE(cookie.empty());
    EXPECT_EQ(count("app_sign_in_codes"), 0);
    EXPECT_EQ(count("sessions WHERE kind = 'browser' AND csrf_token <> ''"), 1);
    EXPECT_EQ(count("audit_log WHERE action = 'session.login' AND after->>'kind' = 'browser'"), 1);
    EXPECT_EQ(count("audit_log WHERE action = 'session.app_code_issued'"), 0);

    EXPECT_EQ(exchange("code-sub-" + kStaff).status, 400u) << "the code Google sent is not a code of the engine";
    EXPECT_EQ(exchange(http::cookie_value(cookie, "poni_session").value()).status, 400u) << "nor is the session's token";
    EXPECT_EQ(count("sessions"), 1);
}

TEST_F(AppSignIn, TheStartTakesOnlyTheAppWithAWellFormedChallenge) {
    const auto challenge = http::s256_challenge(kAppVerifier);
    ASSERT_EQ(challenge.size(), 43u);
    const auto refused = [&](const std::string& query) {
        const auto response = get("/api/auth/google/start" + query);
        EXPECT_EQ(response.status, 400u) << query;
        EXPECT_EQ(error_code(response), "invalid_request") << query;
        EXPECT_TRUE(set_cookie(response, "poni_signin").empty()) << query;
    };
    refused("?client=web");
    refused("?client=android&code_challenge=" + challenge);
    refused("?client=ios");
    refused("?client=ios&code_challenge=" + challenge.substr(1));
    refused("?client=ios&code_challenge=" + challenge + "A");
    refused("?client=ios&code_challenge=" + challenge.substr(1) + ".");
    refused("?client=ios&code_challenge=" + challenge.substr(1) + "%3D");
    refused("?client=ios&code_challenge=" + challenge.substr(1) + "%2B");
    EXPECT_FALSE(provider_.last_request) << "nobody was sent to Google";

    // A dashboard start is what it was: the secret alone in the cookie, and state, nonce and challenge derived from it.
    const http::BrowserFlow flow("poni-signin", "poni_signin", "/api/auth", false);
    const auto plain = get("/api/auth/google/start");
    EXPECT_EQ(plain.status, 302u);
    const auto secret = flow_secret(plain);
    EXPECT_THAT(secret, testing::MatchesRegex("[A-Za-z0-9_-]{43}"));
    EXPECT_EQ(provider_.last_request->state, flow.derive("state", secret));
    EXPECT_EQ(provider_.last_request->nonce, flow.derive("nonce", secret));
    EXPECT_EQ(provider_.last_request->code_challenge, flow.code_challenge(secret));
    // Without `client` a challenge means nothing.
    EXPECT_THAT(flow_secret(get("/api/auth/google/start?code_challenge=" + challenge)), testing::MatchesRegex("[A-Za-z0-9_-]{43}"));

    // The app's: the cookie also holds the challenge, and the state belongs to both.
    const auto app = get(app_start());
    EXPECT_EQ(app.status, 302u);
    const auto app_secret = flow_secret(app);
    EXPECT_THAT(app_secret, testing::MatchesRegex("[A-Za-z0-9_-]{43}\\.[A-Za-z0-9_-]{43}"));
    EXPECT_THAT(app_secret, testing::EndsWith("." + challenge));
    EXPECT_EQ(provider_.last_request->state, flow.derive("state", app_secret));
    EXPECT_NE(provider_.last_request->state, flow.derive("state", app_secret.substr(0, 43)));
}

TEST_F(AppSignIn, AnAppAttemptsErrorsGoBackToTheApp) {
    provider_.identities["the-code"] = staff();
    const auto flow = pair_of(set_cookie(get(app_start()), "poni_signin"));
    const auto state = provider_.last_request->state;
    const auto location = [&](const std::string& query, const std::string& cookie) {
        const auto response = get(kCallback + query, {.cookie = cookie});
        EXPECT_EQ(response.status, 302u) << query;
        EXPECT_TRUE(set_cookie(response, "poni_session").empty()) << query;
        return header(response, "Location");
    };
    const std::string good = "?code=the-code&state=" + state;
    EXPECT_EQ(location("?error=access_denied&state=" + state, flow), "gbpmanager://auth?error=cancelled");
    EXPECT_EQ(location("?code=the-code&state=wrong", flow), "gbpmanager://auth?error=failed");
    EXPECT_EQ(location("?state=" + state, flow), "gbpmanager://auth?error=failed");
    EXPECT_EQ(location("?code=unknown&state=" + state, flow), "gbpmanager://auth?error=failed") << "the provider refused the code";

    // The challenge in the cookie is the one the start set, or the callback answers nobody.
    const auto secret_and_dot = flow.substr(0, flow.find('.') + 1);
    EXPECT_EQ(location(good, secret_and_dot + http::s256_challenge("somebody-elses-verifier-0123456789-0123456789")), "gbpmanager://auth?error=failed")
        << "another challenge under the same secret";
    EXPECT_EQ(location(good, secret_and_dot + "not-a-challenge"), "gbpmanager://auth?error=failed");
    EXPECT_EQ(location(good, flow.substr(0, flow.find('.'))), "http://localhost:5173/#/login?error=failed")
        << "the challenge cut off: a dashboard attempt, and the state is not its own";
    EXPECT_EQ(location(good, ""), "http://localhost:5173/#/login?error=expired") << "without the cookie nobody knows the attempt was the app's";
    EXPECT_EQ(count("app_sign_in_codes"), 0);
    EXPECT_EQ(count("sessions"), 0);
    EXPECT_EQ(count("users"), 0);
    EXPECT_EQ(count("audit_log"), 0);

    auto outsider = staff("someone@gmail.com");
    outsider.hosted_domain.reset();
    EXPECT_EQ(header(app_sign_in_response(outsider), "Location"), "gbpmanager://auth?error=domain");
    EXPECT_EQ(count("audit_log WHERE action = 'session.login_refused' AND actor = 'someone@gmail.com'"), 1);
    EXPECT_EQ(count("users"), 0);

    app_sign_in();
    exec("UPDATE users SET status = 'disabled'");
    const auto disabled = app_sign_in_response(staff());
    EXPECT_EQ(header(disabled, "Location"), "gbpmanager://auth?error=disabled");
    EXPECT_THAT(set_cookie(disabled, "poni_signin"), HasSubstr("Max-Age=0"));
    EXPECT_EQ(count("app_sign_in_codes"), 0);
    EXPECT_EQ(count("audit_log WHERE action = 'session.login_refused' AND after->>'reason' = 'the user is disabled'"), 1);
    EXPECT_EQ(count("audit_log WHERE action = 'session.app_code_issued'"), 1) << "the one sign-in that got a code, and no refused one";

    // An engine without Google sign-in says so to the one who asked.
    build({}, false);
    EXPECT_EQ(header(get(app_start()), "Location"), "gbpmanager://auth?error=not_configured");
    EXPECT_EQ(location(good, flow), "gbpmanager://auth?error=not_configured");
    EXPECT_EQ(header(get("/api/auth/google/start"), "Location"), "http://localhost:5173/#/login?error=not_configured");
    EXPECT_EQ(get("/api/auth/google/start?client=web").status, 400u);
}

TEST_F(AppSignIn, AnAppSessionEndsThirtyDaysAfterItsLastUseAndNinetyDaysAfterSignIn) {
    const auto idle = app_sign_in();
    clock_.advance(24h * 29);
    EXPECT_EQ(as_app(verb::get, "/api/clients", idle).status, 200u);
    clock_.advance(24h * 30 - 1min);
    EXPECT_EQ(as_app(verb::get, "/api/clients", idle).status, 200u) << "thirty days from its last use, not from signing in";
    clock_.advance(24h * 30 + 1min);
    EXPECT_EQ(as_app(verb::get, "/api/clients", idle).status, 401u) << "unused for more than thirty days";
    EXPECT_FALSE(json(as_app(verb::get, "/api/session", idle)).at("authenticated").as_bool());

    // Used every day, a token still ends ninety days after signing in.
    const auto daily = app_sign_in();
    for (int day = 1; day < 90; ++day) {
        clock_.advance(24h);
        ASSERT_EQ(as_app(verb::get, "/api/clients", daily).status, 200u) << "day " << day;
    }
    clock_.advance(24h + 1min);
    EXPECT_EQ(as_app(verb::get, "/api/clients", daily).status, 401u);

    // A dashboard session beside it keeps its own day and its own week.
    const auto browser = sign_in();
    const auto token = app_sign_in();
    clock_.advance(25h);
    EXPECT_EQ(get("/api/clients", browser).status, 401u);
    EXPECT_EQ(as_app(verb::get, "/api/clients", token).status, 200u);
}

TEST_F(AppSignIn, EndedSessionsAndCodesArePurgedEachByItsOwnLimit) {
    const auto browser = sign_in();
    const auto token = app_sign_in();
    clock_.advance(25h);
    EXPECT_EQ(purge_sessions(), 1u) << "the browser's, idle for more than a day";
    EXPECT_EQ(count("sessions WHERE kind = 'app'"), 1);
    EXPECT_EQ(as_app(verb::get, "/api/clients", token).status, 200u);

    clock_.advance(24h * 30 - 1min);
    EXPECT_EQ(purge_sessions(), 0u);
    clock_.advance(2min);
    EXPECT_EQ(purge_sessions(), 1u) << "the app's, idle for more than thirty days";
    EXPECT_EQ(count("sessions"), 0);

    // An app session at its ninety days goes however recently it was used.
    const auto used = app_sign_in();
    for (int i = 0; i < 3; ++i) {
        clock_.advance(24h * 29);
        ASSERT_EQ(as_app(verb::get, "/api/clients", used).status, 200u) << "after " << (i + 1) * 29 << " days";
    }
    EXPECT_EQ(purge_sessions(), 0u);
    clock_.advance(24h * 3);
    EXPECT_EQ(purge_sessions(), 1u) << "day ninety, three days after its last use";

    // A code nobody exchanged.
    const auto code = app_code(app_sign_in_response(staff()));
    ASSERT_FALSE(code.empty());
    clock_.advance(59s);
    EXPECT_EQ(purge_codes(), 0u);
    clock_.advance(1s);
    EXPECT_EQ(purge_codes(), 1u);
    EXPECT_EQ(count("app_sign_in_codes"), 0);
}

TEST_F(AppSignIn, TheHourlyHousekeepingPurgesThemWithTheLifetimesItWasGiven) {
    const auto browser = sign_in();
    const auto token = app_sign_in();
    ASSERT_FALSE(app_code(app_sign_in_response(staff())).empty());
    // The job as main() installs it, here with an app session that may go unused for two days.
    jobs::Registry registry;
    jobs::install_builtin(registry, db(), queue(), {.sessions = {.browser = {24h * 7, 24h}, .app = {24h * 90, 48h}}});
    const auto housekeeping = [&] {
        clock_.advance(1h);  // the job's next slot
        jobs::Scheduler scheduler(io_.get_executor(), queue(), registry.recurring(), {});
        await(scheduler.tick());
        run_jobs(registry);
    };

    housekeeping();
    EXPECT_EQ(count("sessions"), 2);
    EXPECT_EQ(count("app_sign_in_codes"), 0) << "the code nobody exchanged";
    clock_.advance(24h);
    housekeeping();
    EXPECT_EQ(count("sessions WHERE kind = 'browser'"), 0);
    EXPECT_EQ(count("sessions WHERE kind = 'app'"), 1);
    clock_.advance(24h);
    housekeeping();
    EXPECT_EQ(count("sessions"), 0);
    EXPECT_EQ(count("jobs WHERE kind = 'system.housekeeping' AND status <> 'succeeded'"), 0);
}

TEST_F(AppSignIn, ATokenIsNoCookieAndACookieIsNoToken) {
    const auto token = app_sign_in();
    const auto browser = sign_in();
    const auto cookie_value = http::cookie_value(browser.cookie, "poni_session").value();

    EXPECT_EQ(get("/api/clients", {.cookie = "poni_session=" + token}).status, 401u) << "a token sent as the cookie";
    EXPECT_EQ(call(verb::post, "/api/clients", {.cookie = "poni_session=" + token}, R"({"name": "x"})").status, 401u)
        << "and no changing request gets through on a session that has no CSRF token";
    EXPECT_EQ(as_app(verb::get, "/api/clients", cookie_value).status, 401u) << "a cookie's value sent as a token";
    EXPECT_EQ(as_app(verb::post, "/api/clients", cookie_value, R"({"name": "x"})").status, 401u);

    // A request with a bearer token stands on that token alone, whatever cookie it also carries.
    const auto bad_beside_good = call(verb::get, "/api/clients", browser, {}, bearer("not-a-token"));
    EXPECT_EQ(bad_beside_good.status, 401u);
    EXPECT_EQ(error_code(bad_beside_good), "unauthenticated");
    EXPECT_FALSE(json(call(verb::get, "/api/session", browser, {}, bearer("not-a-token"))).at("authenticated").as_bool());
    EXPECT_EQ(call(verb::get, "/api/clients", browser, {}, {{"authorization", "Bearer"}}).status, 401u) << "the scheme without a token";
    EXPECT_EQ(call(verb::get, "/api/clients", browser, {}, {{"authorization", "Bearer "}}).status, 401u);
    EXPECT_EQ(call(verb::get, "/api/clients", browser, {}, {{"authorization", "Bearer\tnot-a-token"}}).status, 401u) << "a tab after the scheme";
    EXPECT_EQ(call(verb::post, "/api/clients", browser, R"({"name": "x"})", bearer("not-a-token")).status, 401u);
    EXPECT_TRUE(json(call(verb::get, "/api/session", browser, {}, bearer(token))).at("csrf_token").is_null())
        << "a good token beside a good cookie: the session is the token's";

    // Another scheme is not the app's; the request is judged by its cookie.
    EXPECT_EQ(call(verb::get, "/api/clients", browser, {}, {{"authorization", "Basic c3RhZmY6c2VjcmV0"}}).status, 200u);
    EXPECT_EQ(call(verb::get, "/api/clients", {}, {}, {{"authorization", "Basic " + token}}).status, 401u);
    EXPECT_EQ(call(verb::get, "/api/clients", {}, {}, {{"authorization", "Bearer" + token}}).status, 401u);
    EXPECT_EQ(call(verb::get, "/api/clients", {}, {}, {{"authorization", "bearer " + token}}).status, 200u) << "the scheme's case does not matter";

    // The token is read from that header and from nowhere else.
    EXPECT_EQ(get("/api/clients?token=" + token + "&access_token=" + token).status, 401u);
    EXPECT_EQ(call(verb::get, "/api/clients", {}, {}, {{"x-auth-token", token}, {"x-api-key", token}, {"x-csrf-token", token}}).status, 401u);
    EXPECT_EQ(call(verb::post, "/api/clients", {}, boost::json::serialize(boost::json::object{{"name", "x"}, {"token", token}})).status, 401u);
    EXPECT_EQ(count("clients"), 0);
}

TEST_F(AppSignIn, EveryRouteThatTakesTheCookieTakesTheToken) {
    const auto token = app_sign_in();
    std::size_t checked = 0;
    for (const auto& route : user_routes("999999999")) {
        const auto unknown = call(route.method, route.path, {}, "{}", bearer("not-a-token"));
        EXPECT_EQ(unknown.status, 401u) << route.name;
        EXPECT_EQ(error_code(unknown), "unauthenticated") << route.name;
        // Reading is enough to see the hook let the token through; a route that changes something is not run here.
        if (route.method != verb::get) continue;
        const auto status = as_app(verb::get, route.path, token).status;
        EXPECT_TRUE(status != 401 && status != 403) << route.name << " answered " << status;
        ++checked;
    }
    EXPECT_GE(checked, 14u);
}

TEST_F(AppSignIn, SigningOutWithATokenEndsThatTokenOnly) {
    const auto token = app_sign_in();
    const auto second_phone = app_sign_in();
    const auto browser = sign_in();

    // The browser's cookie beside the token, and no CSRF token.
    const auto out = call(verb::post, "/api/auth/logout", {.cookie = browser.cookie}, {}, bearer(token));
    EXPECT_EQ(out.status, 204u);
    EXPECT_TRUE(set_cookie(out, "poni_session").empty()) << "the cookie beside the token is another session's and stays";
    EXPECT_EQ(as_app(verb::get, "/api/clients", token).status, 401u);
    EXPECT_EQ(as_app(verb::get, "/api/clients", second_phone).status, 200u);
    EXPECT_EQ(get("/api/clients", browser).status, 200u);
    EXPECT_EQ(count("sessions"), 2);

    // The dashboard's sign-out is what it was.
    EXPECT_EQ(call(verb::post, "/api/auth/logout", {.cookie = browser.cookie}).status, 403u);
    EXPECT_THAT(set_cookie(call(verb::post, "/api/auth/logout", browser), "poni_session"), HasSubstr("Max-Age=0"));
    EXPECT_EQ(count("audit_log WHERE action = 'session.logout' AND after->>'kind' = 'browser'"), 1);
}

TEST_F(AppSignIn, ADisabledUserIsLockedOutOfTheAppAtOnce) {
    const auto token = app_sign_in();
    const auto code = app_code(app_sign_in_response(staff()));
    ASSERT_FALSE(code.empty());
    exec("UPDATE users SET status = 'disabled'");

    EXPECT_EQ(as_app(verb::get, "/api/clients", token).status, 401u);
    EXPECT_FALSE(json(as_app(verb::get, "/api/session", token)).at("authenticated").as_bool());
    const auto refused = exchange(code);
    EXPECT_EQ(refused.status, 400u) << "a code from before is no way in";
    EXPECT_EQ(refused.body, exchange("a-code-nobody-was-given").body);
    EXPECT_EQ(count("sessions"), 1) << "no new session";
    EXPECT_EQ(count("audit_log WHERE action = 'session.login_refused' AND actor = '" + kStaff + "' AND after->>'reason' = 'the user is disabled'"), 1);
    EXPECT_EQ(header(app_sign_in_response(staff()), "Location"), "gbpmanager://auth?error=disabled");
}

TEST_F(AppSignIn, ABearerRequestNeedsNoCsrfTokenAndGetsNoOriginCheck) {
    const auto token = app_sign_in();
    EXPECT_EQ(as_app(verb::post, "/api/clients", token, R"({"name": "Client A"})").status, 201u);
    EXPECT_EQ(as_app(verb::post, "/api/clients", token, R"({"name": "Client B"})", {{"origin", "https://elsewhere.example"}}).status, 201u)
        << "the Origin check guards the cookie; no browser sends this token by itself";
    EXPECT_EQ(as_app(verb::post, "/api/clients", token, R"({"name": "Client C"})", {{"x-csrf-token", "anything"}}).status, 201u);

    // The cookie keeps both checks.
    const auto browser = sign_in();
    const std::string body = R"({"name": "Client D"})";
    EXPECT_EQ(error_code(call(verb::post, "/api/clients", {.cookie = browser.cookie}, body)), "csrf_token_invalid");
    EXPECT_EQ(error_code(call(verb::post, "/api/clients", browser, body, {{"origin", "https://elsewhere.example"}})), "forbidden_origin");
    EXPECT_EQ(count("clients"), 3);
}

TEST_F(AppSignIn, DevelopmentSignInGivesTheAppATokenInsteadOfTheCookie) {
    const auto off = call(verb::post, "/api/auth/dev-login?client=ios");
    EXPECT_EQ(off.status, 404u) << "it exists only with PONI_DEV_LOGIN_EMAIL, which the engine takes on plain http://localhost alone";
    EXPECT_EQ(error_code(off), "not_found");

    build({.dev_login_email = "dev@localhost"});
    const auto on = call(verb::post, "/api/auth/dev-login?client=ios");
    ASSERT_EQ(on.status, 200u) << on.body;
    EXPECT_TRUE(set_cookie(on, "poni_session").empty());
    const auto token = std::string(json(on).at("token").as_string());
    EXPECT_EQ(json(on).at("expires_at").as_string(), "2027-02-08T12:00:00.000000Z");
    EXPECT_EQ(json(on).at("session").at("user").at("email").as_string(), "dev@localhost");
    EXPECT_TRUE(json(on).at("session").at("csrf_token").is_null());
    EXPECT_EQ(as_app(verb::post, "/api/clients", token, R"({"name": "Made in development"})").status, 201u);
    EXPECT_EQ(count("sessions WHERE kind = 'app'"), 1);
    EXPECT_EQ(count("audit_log WHERE action = 'session.login' AND actor = 'dev@localhost' AND after->>'kind' = 'app'"), 1);

    const auto other = call(verb::post, "/api/auth/dev-login?client=android");
    EXPECT_EQ(other.status, 400u);
    EXPECT_EQ(error_code(other), "invalid_request");
    EXPECT_TRUE(set_cookie(other, "poni_session").empty());
    EXPECT_EQ(count("sessions"), 1);

    // Without `client` it is the dashboard's: the session and its cookie.
    const auto web = call(verb::post, "/api/auth/dev-login");
    ASSERT_EQ(web.status, 200u);
    EXPECT_FALSE(set_cookie(web, "poni_session").empty());
    EXPECT_TRUE(json(web).at("csrf_token").is_string());
    EXPECT_FALSE(json(web).as_object().contains("token"));
}

TEST_F(AppSignIn, TheExchangeHasAnAttemptLimitOfItsOwn) {
    // Trying codes is limited, per address.
    for (int i = 0; i < 20; ++i) EXPECT_EQ(exchange("guess-" + std::to_string(i)).status, 400u);
    const auto refused = exchange("one-more-guess");
    EXPECT_EQ(refused.status, 429u);
    EXPECT_EQ(error_code(refused), "too_many_attempts");
    EXPECT_EQ(header(refused, "Retry-After"), "900");
    // It locks nobody out of signing in: the start and the callback count on their own.
    const auto done = app_sign_in_response(staff());
    const auto code = app_code(done);
    EXPECT_FALSE(code.empty()) << header(done, "Location");
    EXPECT_EQ(get("/api/auth/google/start").status, 302u);
    EXPECT_EQ(exchange(code).status, 429u) << "the exchange itself stays locked";

    clock_.advance(15min + 1s);
    EXPECT_EQ(exchange("a-guess-later").status, 400u);

    // And the other way round: an address locked out of the start can still exchange.
    const auto late = app_code(app_sign_in_response(staff()));
    for (int i = 0; i < 18; ++i) EXPECT_EQ(get("/api/auth/google/start").status, 302u);
    EXPECT_EQ(header(get(app_start()), "Location"), "gbpmanager://auth?error=too_many_attempts");
    EXPECT_EQ(get(kCallback + "?code=x&state=y").status, 429u);
    EXPECT_EQ(exchange(late).status, 200u);
}

TEST_F(AppSignIn, AnAppSignInCostsTwoOfTheSharedLimitAsTheDashboardsDoes) {
    // Ten sign-ins from one address, each a start, a callback and an exchange: 20 of the shared limit.
    for (int i = 0; i < 10; ++i) EXPECT_FALSE(app_sign_in().empty()) << "sign-in " << i + 1;
    EXPECT_EQ(header(get(app_start()), "Location"), "gbpmanager://auth?error=too_many_attempts") << "the eleventh";
    EXPECT_EQ(count("sessions"), 10);
}

TEST_F(AppSignIn, OverTheAttemptLimitTheAppIsSentBackWithItsOwnCode) {
    provider_.identities["the-code"] = staff();
    // An attempt of the dashboard's and one of the app's, both as far as Google, and the limit reached with them.
    const auto web_flow = pair_of(set_cookie(get("/api/auth/google/start"), "poni_signin"));
    const auto web_state = provider_.last_request->state;
    for (int i = 0; i < 18; ++i) EXPECT_EQ(get("/api/auth/google/start").status, 302u);
    const auto app_flow = pair_of(set_cookie(get(app_start()), "poni_signin"));
    const auto app_state = provider_.last_request->state;  // the fake provider takes the code of its last request

    // The app's start: back to the app, and no attempt begins.
    const auto start = get(app_start());
    EXPECT_EQ(start.status, 302u);
    EXPECT_EQ(header(start, "Location"), "gbpmanager://auth?error=too_many_attempts");
    EXPECT_EQ(set_cookie(start, "poni_signin"), "poni_signin=; Path=/api/auth; Max-Age=0; HttpOnly; SameSite=Lax")
        << "the attempt's cookie is only cleared, as by every other redirect to the app";
    EXPECT_TRUE(set_cookie(start, "poni_session").empty());
    EXPECT_EQ(provider_.last_request->state, app_state) << "nobody was sent to Google";
    // The limit is decided before the challenge is looked at; the start's 400 is for a start under the limit.
    EXPECT_EQ(header(get("/api/auth/google/start?client=ios"), "Location"), "gbpmanager://auth?error=too_many_attempts");

    // The dashboard's start, and one of a client there is not, keep the 429.
    const auto dashboard = get("/api/auth/google/start");
    EXPECT_EQ(dashboard.status, 429u);
    EXPECT_EQ(error_code(dashboard), "too_many_attempts");
    EXPECT_EQ(header(dashboard, "Retry-After"), "900");
    EXPECT_EQ(get("/api/auth/google/start?client=web").status, 429u);
    EXPECT_EQ(provider_.last_request->state, app_state);

    // The app's callback, with its cookie, a good state and a code Google would accept: back to the app, and nothing is written.
    const auto callback = get(kCallback + "?code=the-code&state=" + app_state, {.cookie = app_flow});
    EXPECT_EQ(callback.status, 302u);
    EXPECT_EQ(header(callback, "Location"), "gbpmanager://auth?error=too_many_attempts");
    EXPECT_THAT(set_cookie(callback, "poni_signin"), HasSubstr("Max-Age=0"));
    EXPECT_TRUE(set_cookie(callback, "poni_session").empty());
    EXPECT_TRUE(provider_.redeemed_verifier.empty()) << "the code was not redeemed";
    // Whose attempt it is is decided as before: a cookie with a note is the app's, the note well-formed or not.
    const auto forged = app_flow.substr(0, app_flow.find('.') + 1) + "not-a-challenge";
    EXPECT_EQ(header(get(kCallback + "?code=the-code&state=" + app_state, {.cookie = forged}), "Location"),
              "gbpmanager://auth?error=too_many_attempts");

    // The dashboard's callback, and one without a cookie (nobody's attempt), keep the 429.
    const auto web = get(kCallback + "?code=the-code&state=" + web_state, {.cookie = web_flow});
    EXPECT_EQ(web.status, 429u);
    EXPECT_EQ(error_code(web), "too_many_attempts");
    EXPECT_EQ(header(web, "Retry-After"), "900");
    EXPECT_EQ(get(kCallback + "?code=the-code&state=" + app_state).status, 429u);
    EXPECT_TRUE(provider_.redeemed_verifier.empty());

    EXPECT_EQ(count("sessions"), 0);
    EXPECT_EQ(count("users"), 0);
    EXPECT_EQ(count("app_sign_in_codes"), 0);
    EXPECT_EQ(count("audit_log"), 0);

    // The refused callback spent nothing on the server: presented again after the lockout, the same cookie,
    // state and code still sign in. (A browser would have lost that cookie by then: the refusals cleared it.)
    clock_.advance(15min + 1s);
    EXPECT_FALSE(app_code(get(kCallback + "?code=the-code&state=" + app_state, {.cookie = app_flow})).empty())
        << "the refused callback redeemed, stored and spent nothing";
}

TEST_F(AppSignIn, EveryRequestToTheStartAndTheCallbackCountsWhateverItSays) {
    // Twenty requests of every kind the two routes answer (a 400, an error back to the app or the
    // dashboard, a redirect to Google), and none of them signs in.
    const auto flow = pair_of(set_cookie(get(app_start()), "poni_signin"));
    for (int i = 0; i < 3; ++i) EXPECT_EQ(get("/api/auth/google/start?client=web").status, 400u);
    for (int i = 0; i < 3; ++i) EXPECT_EQ(get("/api/auth/google/start?client=ios").status, 400u);
    for (int i = 0; i < 3; ++i) EXPECT_EQ(header(get(kCallback + "?code=x&state=wrong", {.cookie = flow}), "Location"), "gbpmanager://auth?error=failed");
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(header(get(kCallback + "?code=x&state=y", {.cookie = flow.substr(0, flow.find('.') + 1) + "not-a-challenge"}), "Location"),
                  "gbpmanager://auth?error=failed");
    }
    for (int i = 0; i < 3; ++i) EXPECT_EQ(header(get(kCallback + "?code=x&state=y"), "Location"), "http://localhost:5173/#/login?error=expired");
    for (int i = 0; i < 3; ++i) EXPECT_THAT(header(get(app_start()), "Location"), testing::StartsWith("https://idp.example/authorize"));
    EXPECT_EQ(header(get(kCallback + "?error=access_denied", {.cookie = flow}), "Location"), "gbpmanager://auth?error=cancelled");

    // The twenty-first is refused, and it counted too: the lockout began with it. Had it not
    // counted, ten minutes later the window would have begun again and the start gone through.
    EXPECT_EQ(header(get(app_start()), "Location"), "gbpmanager://auth?error=too_many_attempts");
    clock_.advance(10min);
    const auto later = get("/api/auth/google/start");
    EXPECT_EQ(later.status, 429u);
    EXPECT_EQ(header(later, "Retry-After"), "300") << "locked since the app's refused start";

    // The same for the callback: the app's refused callback is the one the lockout begins with.
    clock_.advance(5min + 1s);
    const auto again = pair_of(set_cookie(get(app_start()), "poni_signin"));
    for (int i = 0; i < 19; ++i) EXPECT_EQ(get("/api/auth/google/start").status, 302u);
    EXPECT_EQ(header(get(kCallback + "?code=x&state=y", {.cookie = again}), "Location"), "gbpmanager://auth?error=too_many_attempts");
    clock_.advance(10min);
    EXPECT_EQ(header(get(kCallback + "?code=x&state=y", {.cookie = again}), "Location"), "gbpmanager://auth?error=too_many_attempts")
        << "locked since the app's refused callback";
    EXPECT_EQ(header(get("/api/auth/google/start"), "Retry-After"), "300");
}

TEST_F(AppSignIn, TheExchangeReadsItsBodyAsTheSpecSays) {
    const auto code = app_code(app_sign_in_response(staff()));
    const auto refused = [&](std::string body, std::string_view names) {
        const auto response = call(verb::post, "/api/auth/app-token", {}, std::move(body));
        EXPECT_EQ(response.status, 400u) << response.body;
        EXPECT_EQ(error_code(response), "invalid_request");
        EXPECT_THAT(error_message(response), HasSubstr(std::string(names)));
    };
    const auto with = [&](boost::json::object members) {
        boost::json::object body{{"code", code}, {"code_verifier", kAppVerifier}};
        for (const auto& member : members) body[member.key()] = member.value();
        return boost::json::serialize(body);
    };
    refused(boost::json::serialize(boost::json::object{{"code", code}}), "code_verifier");
    refused(boost::json::serialize(boost::json::object{{"code_verifier", kAppVerifier}}), "code");
    refused(with({{"not_in_the_spec", true}}), "not_in_the_spec");
    refused(with({{"code", nullptr}}), "code");
    refused(with({{"code_verifier", 5}}), "code_verifier");
    refused("[]", "The body must be a JSON object.");
    refused("", "The body must be a JSON object.");
    EXPECT_EQ(count("app_sign_in_codes"), 1) << "a body that was not read spends no code";
    EXPECT_EQ(exchange(code).status, 200u);
    EXPECT_EQ(get("/api/auth/app-token?code=" + code).status, 405u) << "a code is taken in a body, never in an address";
}

TEST_F(AppSignIn, NoTokenCodeOrVerifierIsLoggedOrAudited) {
    const std::string wrong_verifier = "somebody-elses-verifier-0123456789-0123456789";
    log::init(log::Level::debug, log::Format::json);
    testing::internal::CaptureStdout();

    const auto spent = app_code(app_sign_in_response(staff()));
    exchange(spent, wrong_verifier);
    exchange(spent);
    const auto code = app_code(app_sign_in_response(staff()));
    const auto token = std::string(json(exchange(code)).at("token").as_string());
    as_app(verb::post, "/api/clients", token, R"({"name": "Made in the app"})");
    as_app(verb::get, "/api/clients", "a-token-nobody-was-given");
    as_app(verb::post, "/api/auth/logout", token);

    const auto logged = testing::internal::GetCapturedStdout();
    log::init(log::Level::info, log::Format::json);
    EXPECT_THAT(logged, HasSubstr("the app's sign-in code was refused")) << "the lines were captured";
    const std::string challenge = http::s256_challenge(kAppVerifier);
    for (const auto& secret : {spent, code, token, kAppVerifier, wrong_verifier, core::sha256_hex(token), core::sha256_hex(code)}) {
        ASSERT_GE(secret.size(), 40u);
        EXPECT_THAT(logged, Not(HasSubstr(secret)));
        EXPECT_EQ(audited(secret), 0);
    }
    EXPECT_EQ(audited(challenge), 0);
    EXPECT_GE(audited(kStaff), 3) << "the entries are there: two sign-ins, a refusal, a sign-out";
}

TEST_F(AppSignIn, TheTableTakesNoSessionOfTheWrongShape) {
    app_sign_in();
    const auto insert = [&](std::string_view columns, std::string_view values) {
        exec(std::format("INSERT INTO sessions (token_hash, user_id, created_at, last_seen_at, expires_at{}) "
                         "SELECT 'hash-of-a-made-up-token', id, now(), now(), now(){} FROM users",
                         columns, values));
    };
    EXPECT_THROW(insert(", kind, csrf_token", ", 'app', 'a-csrf-token'"), pqxx::check_violation) << "an app session has no CSRF token";
    EXPECT_THROW(insert(", kind", ", 'browser'"), pqxx::check_violation) << "a browser session has one";
    EXPECT_THROW(insert(", kind, csrf_token", ", 'browser', ''"), pqxx::check_violation) << "and not an empty one";
    EXPECT_THROW(insert(", kind, csrf_token", ", 'tablet', 'a-csrf-token'"), pqxx::check_violation);
    EXPECT_THROW(insert(", csrf_token", ", 'a-csrf-token'"), pqxx::not_null_violation) << "whoever writes a session says its kind";
    EXPECT_EQ(count("sessions"), 1);
}
