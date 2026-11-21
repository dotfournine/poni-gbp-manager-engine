// Dashboard sign-in end to end with a fake identity provider: the Google
// flow, the domain rule, sessions and their cookies, CSRF, expiry, the
// attempt limit and the authorization hook on every endpoint.

#include "api_fixture.hpp"
#include "core/crypto.hpp"
#include "db/repo_auth.hpp"

#include <gmock/gmock.h>

using namespace poni;
using namespace std::chrono_literals;
using testing::HasSubstr;
using testing::Not;

namespace {

class Auth : public test::ApiTest {};

}  // namespace

TEST_F(Auth, NobodyIsSignedInAtFirst) {
    const auto session = json(get("/api/session"));
    EXPECT_FALSE(session.at("authenticated").as_bool());
    EXPECT_TRUE(session.at("user").is_null());
    EXPECT_TRUE(session.at("csrf_token").is_null());
    EXPECT_TRUE(session.at("sign_in").at("google").as_bool());
    EXPECT_FALSE(session.at("sign_in").at("dev").as_bool());
    EXPECT_EQ(session.at("sign_in").at("domains").as_array(), (boost::json::array{"xvisionmediaus.com", "xvisionus.com"}))
        << "the sign-in page shows which accounts are accepted";
    EXPECT_FALSE(session.at("google_mock").as_bool());
}

TEST_F(Auth, TheSessionSaysWhenTheEngineRunsAgainstTheGoogleMock) {
    build({.dev_login_email = "dev@localhost", .google_mock_url = "http://127.0.0.1:8090"});
    EXPECT_TRUE(json(get("/api/session")).at("google_mock").as_bool()) << "the dashboard shows it, signed in or not";
}

TEST_F(Auth, SigningInWithGoogleCreatesTheUserASessionAndAnAuditEntry) {
    provider_.identities["the-code"] = staff();

    const auto start = get("/api/auth/google/start");
    EXPECT_EQ(start.status, 302u);
    EXPECT_THAT(header(start, "Location"), testing::StartsWith("https://idp.example/authorize"));
    const auto flow = set_cookie(start, "poni_signin");
    EXPECT_THAT(flow, testing::AllOf(HasSubstr("HttpOnly"), HasSubstr("SameSite=Lax"), HasSubstr("Path=/api/auth"), HasSubstr("Max-Age=600")));
    ASSERT_TRUE(provider_.last_request);
    EXPECT_EQ(provider_.last_request->redirect_uri, "http://localhost:5173/api/auth/google/callback");

    const auto done = get("/api/auth/google/callback?code=the-code&state=" + provider_.last_request->state, {.cookie = pair_of(flow)});
    EXPECT_EQ(done.status, 302u);
    EXPECT_EQ(header(done, "Location"), "http://localhost:5173/");
    const auto cookie = set_cookie(done, "poni_session");
    EXPECT_THAT(cookie, testing::AllOf(HasSubstr("HttpOnly"), HasSubstr("SameSite=Strict"), HasSubstr("Path=/"), HasSubstr("Max-Age=604800")));
    EXPECT_THAT(set_cookie(done, "poni_signin"), HasSubstr("Max-Age=0")) << "the one-time flow cookie is cleared";

    // PKCE: the verifier sent with the code hashes to the challenge sent to the provider.
    EXPECT_EQ(core::base64url_encode(core::sha256_raw(provider_.redeemed_verifier)), provider_.last_request->code_challenge);
    EXPECT_NE(provider_.last_request->state, provider_.last_request->nonce);

    const test::Browser browser{.cookie = pair_of(cookie)};
    const auto session = json(get("/api/session", browser));
    EXPECT_TRUE(session.at("authenticated").as_bool());
    EXPECT_EQ(session.at("user").at("email").as_string(), "staff@xvisionmediaus.com");
    EXPECT_GE(session.at("csrf_token").as_string().size(), 40u);

    EXPECT_EQ(count("users WHERE email = 'staff@xvisionmediaus.com' AND status = 'active'"), 1);
    EXPECT_EQ(count("sessions"), 1);
    const auto token = cookie.substr(std::string("poni_session=").size(), cookie.find(';') - std::string("poni_session=").size());
    EXPECT_EQ(count("sessions WHERE token_hash = '" + core::sha256_hex(token) + "'"), 1);
    EXPECT_EQ(count("sessions WHERE token_hash = '" + token + "'"), 0) << "only the token's hash is stored";
    EXPECT_EQ(count("audit_log WHERE action = 'session.login' AND actor = 'staff@xvisionmediaus.com' AND actor_type = 'user' "
                    "AND ip = '203.0.113.10' AND (after->>'first_sign_in')::boolean"),
              1);

    // A second sign-in reuses the user.
    sign_in(staff());
    EXPECT_EQ(count("users"), 1);
    EXPECT_EQ(count("sessions"), 2);
}

TEST_F(Auth, BehindHttpsTheCookieIsSecureAndHostBound) {
    build({.public_url = "https://gbp.xvisionmediaus.com"});
    const auto done = sign_in_response(staff());
    const auto cookie = set_cookie(done, "__Host-poni_session");
    EXPECT_THAT(cookie, testing::AllOf(HasSubstr("; Secure"), HasSubstr("HttpOnly"), HasSubstr("SameSite=Strict"), HasSubstr("Path=/;")));
    EXPECT_THAT(cookie, Not(HasSubstr("Domain="))) << "__Host- cookies must not name a domain";
    EXPECT_EQ(header(done, "Location"), "https://gbp.xvisionmediaus.com/");
    EXPECT_TRUE(json(get("/api/session", {.cookie = pair_of(cookie)})).at("authenticated").as_bool());
}

TEST_F(Auth, OnlyWorkspaceAccountsOfTheDomainGetIn) {
    auto outsider = staff("someone@gmail.com");
    outsider.hosted_domain.reset();
    auto response = sign_in_response(outsider);
    EXPECT_EQ(header(response, "Location"), "http://localhost:5173/#/login?error=domain");
    EXPECT_TRUE(set_cookie(response, "poni_session").empty());

    // The right address but a consumer account (no Workspace domain), and an unverified address.
    auto consumer = staff();
    consumer.hosted_domain.reset();
    EXPECT_THAT(header(sign_in_response(consumer), "Location"), HasSubstr("error=domain"));
    auto unverified = staff();
    unverified.email_verified = false;
    EXPECT_THAT(header(sign_in_response(unverified), "Location"), HasSubstr("error=domain"));

    EXPECT_EQ(count("users"), 0);
    EXPECT_EQ(count("sessions"), 0);
    EXPECT_EQ(count("audit_log WHERE action = 'session.login_refused' AND actor_type = 'anonymous'"), 3);
    EXPECT_EQ(count("audit_log WHERE action = 'session.login_refused' AND actor = 'someone@gmail.com'"), 1);
}

TEST_F(Auth, AccountsOfTheSecondDomainGetInAndOtherDomainsStayOut) {
    auto second = staff("staff@xvisionus.com");
    second.hosted_domain = "xvisionus.com";
    const auto browser = sign_in(second);
    EXPECT_EQ(json(get("/api/session", browser)).at("user").at("email").as_string(), "staff@xvisionus.com");
    EXPECT_EQ(count("audit_log WHERE action = 'session.login' AND actor = 'staff@xvisionus.com'"), 1);

    // Another Workspace organization is still refused.
    auto stranger = staff("staff@elsewhere.example");
    stranger.hosted_domain = "elsewhere.example";
    EXPECT_THAT(header(sign_in_response(stranger), "Location"), HasSubstr("error=domain"));

    // With one domain configured, the second one is refused like any other.
    build({.signin_domains = {"xvisionmediaus.com"}});
    EXPECT_THAT(header(sign_in_response(second), "Location"), HasSubstr("error=domain"));
    EXPECT_EQ(json(get("/api/session")).at("sign_in").at("domains").as_array(), (boost::json::array{"xvisionmediaus.com"}));
    EXPECT_EQ(count("audit_log WHERE action = 'session.login_refused'"), 2);
}

TEST_F(Auth, TheCallbackOnlyWorksForTheBrowserThatStarted) {
    provider_.identities["the-code"] = staff();
    const auto start = get("/api/auth/google/start");
    const auto flow = pair_of(set_cookie(start, "poni_signin"));
    const auto state = provider_.last_request->state;

    const auto location = [&](const std::string& target, const std::string& cookie) {
        return header(get(target, {.cookie = cookie}), "Location");
    };
    EXPECT_THAT(location("/api/auth/google/callback?code=the-code&state=" + state, ""), HasSubstr("error=expired"))
        << "no flow cookie: another browser, or too late";
    EXPECT_THAT(location("/api/auth/google/callback?code=the-code&state=" + state, "poni_signin=someone-elses"),
                HasSubstr("error=failed"));
    EXPECT_THAT(location("/api/auth/google/callback?code=the-code&state=wrong", flow), HasSubstr("error=failed"));
    EXPECT_THAT(location("/api/auth/google/callback?state=" + state, flow), HasSubstr("error=failed"));
    EXPECT_THAT(location("/api/auth/google/callback?error=access_denied&state=" + state, flow), HasSubstr("error=cancelled"));
    EXPECT_THAT(location("/api/auth/google/callback?code=unknown&state=" + state, flow), HasSubstr("error=failed"))
        << "the provider refused the code";
    EXPECT_EQ(count("sessions"), 0);
}

TEST_F(Auth, EveryApiEndpointNeedsASession) {
    // Every route in the table, not a list kept by hand: the 14 this test once named and every one added since.
    EXPECT_GE(expect_session_and_csrf({""}, "", false), 14u);
    // The walk checks the routes that say they need a session. Which routes say they don't is pinned here, so a
    // route marked open by mistake fails this test instead of leaving the walk.
    std::vector<std::string> open;
    for (const auto& route : router_->routes()) {
        if (route.info.access != http::Access::user) {
            std::string path;
            for (const auto& segment : route.segments) path += "/" + segment;
            open.push_back(std::format("{} {}", boost::beast::http::to_string(route.method), path));
        }
    }
    EXPECT_THAT(open, testing::UnorderedElementsAre("GET /api/health", "GET /api/session", "GET /api/auth/google/start",
                                                    "GET /api/auth/google/callback", "POST /api/auth/app-token",
                                                    "POST /api/auth/dev-login"));
    EXPECT_EQ(get("/api/health").status, 200u) << "health stays open for monitoring";
}

TEST_F(Auth, ChangingRequestsNeedTheCsrfTokenAndOurOrigin) {
    const auto browser = sign_in();
    const std::string body = R"({"name": "Client A"})";

    const auto no_token = call(verb::post, "/api/clients", {.cookie = browser.cookie}, body);
    EXPECT_EQ(no_token.status, 403u);
    EXPECT_EQ(error_code(no_token), "csrf_token_invalid");
    EXPECT_EQ(call(verb::post, "/api/clients", {.cookie = browser.cookie, .csrf = "guess"}, body).status, 403u);

    const auto elsewhere = call(verb::post, "/api/clients", browser, body, {{"origin", "https://evil.example"}});
    EXPECT_EQ(elsewhere.status, 403u);
    EXPECT_EQ(error_code(elsewhere), "forbidden_origin");
    EXPECT_EQ(count("clients"), 0);

    EXPECT_EQ(call(verb::post, "/api/clients", browser, body, {{"origin", "http://localhost:5173"}}).status, 201u);
    EXPECT_EQ(get("/api/clients", {.cookie = browser.cookie}).status, 200u) << "reading needs no token";

    // One session's token doesn't work for another session.
    const auto second = sign_in(staff("other@xvisionmediaus.com"));
    EXPECT_EQ(call(verb::post, "/api/clients", {.cookie = second.cookie, .csrf = browser.csrf}, R"({"name": "B"})").status, 403u);
}

TEST_F(Auth, SigningOutEndsTheSession) {
    const auto browser = sign_in();
    EXPECT_EQ(call(verb::post, "/api/auth/logout", {.cookie = browser.cookie}).status, 403u) << "sign-out is a changing request";

    const auto out = call(verb::post, "/api/auth/logout", browser);
    EXPECT_EQ(out.status, 204u);
    EXPECT_THAT(set_cookie(out, "poni_session"), HasSubstr("Max-Age=0"));
    EXPECT_EQ(get("/api/clients", browser).status, 401u);
    EXPECT_FALSE(json(get("/api/session", browser)).at("authenticated").as_bool());
    EXPECT_EQ(count("sessions"), 0);
    EXPECT_EQ(count("audit_log WHERE action = 'session.logout' AND actor = 'staff@xvisionmediaus.com'"), 1);
}

TEST_F(Auth, SessionsEndWhenIdleAndAtTheirAbsoluteLimit) {
    const auto idle = sign_in();
    clock_.advance(23h);
    EXPECT_EQ(get("/api/clients", idle).status, 200u);
    clock_.advance(25h);
    EXPECT_EQ(get("/api/clients", idle).status, 401u) << "idle for more than a day";

    // Used every twelve hours, a session still ends after seven days.
    const auto busy = sign_in();
    for (int i = 0; i < 13; ++i) {
        clock_.advance(12h);
        EXPECT_EQ(get("/api/clients", busy).status, 200u) << "after " << (i + 1) * 12 << " hours";
    }
    clock_.advance(12h + 1min);
    EXPECT_EQ(get("/api/clients", busy).status, 401u);

    const auto purged = [&] {
        pqxx::connection conn{url()};
        pqxx::work tx{conn};
        const auto n = repo::purge_sessions(tx, clock_.now(), domain::SessionLifetimes{});
        tx.commit();
        return n;
    }();
    EXPECT_EQ(purged, 2u);
    EXPECT_EQ(count("sessions"), 0);
}

TEST_F(Auth, ADisabledUserIsLockedOutAtOnce) {
    const auto browser = sign_in();
    exec("UPDATE users SET status = 'disabled'");
    EXPECT_EQ(get("/api/clients", browser).status, 401u);

    const auto again = sign_in_response(staff());
    EXPECT_EQ(header(again, "Location"), "http://localhost:5173/#/login?error=disabled");
    EXPECT_EQ(count("sessions"), 1) << "no new session";
    EXPECT_EQ(count("audit_log WHERE action = 'session.login_refused' AND after->>'reason' = 'the user is disabled'"), 1);
}

TEST_F(Auth, TooManySignInAttemptsFromOneAddressAreRefused) {
    for (int i = 0; i < 20; ++i) EXPECT_EQ(get("/api/auth/google/start").status, 302u);
    const auto refused = get("/api/auth/google/start");
    EXPECT_EQ(refused.status, 429u);
    EXPECT_EQ(error_code(refused), "too_many_attempts");
    EXPECT_EQ(header(refused, "Retry-After"), "900");
    EXPECT_EQ(get("/api/auth/google/callback?code=x&state=y").status, 429u);

    clock_.advance(15min + 1s);
    EXPECT_EQ(get("/api/auth/google/start").status, 302u);
}

TEST_F(Auth, DevelopmentSignInExistsOnlyWhenSwitchedOn) {
    const auto off = call(verb::post, "/api/auth/dev-login");
    EXPECT_EQ(off.status, 404u);
    EXPECT_EQ(error_code(off), "not_found");

    build({.dev_login_email = "dev@localhost"});
    EXPECT_TRUE(json(get("/api/session")).at("sign_in").at("dev").as_bool());
    const auto on = call(verb::post, "/api/auth/dev-login");
    ASSERT_EQ(on.status, 200u);
    const test::Browser browser{.cookie = pair_of(set_cookie(on, "poni_session")),
                                .csrf = std::string(json(on).at("csrf_token").as_string())};
    EXPECT_EQ(json(on).at("user").at("email").as_string(), "dev@localhost");
    EXPECT_EQ(call(verb::post, "/api/clients", browser, R"({"name": "Made in development"})").status, 201u);
}

TEST_F(Auth, WithoutAnOAuthClientGoogleSignInSaysSo) {
    build({}, false);
    EXPECT_FALSE(json(get("/api/session")).at("sign_in").at("google").as_bool());
    EXPECT_EQ(header(get("/api/auth/google/start"), "Location"), "http://localhost:5173/#/login?error=not_configured");
}
