// The pieces of dashboard sign-in that need no database: the checks of an
// ID token's claims, the domain rule, the attempt limit, and settings checks.

#include "connectors/identity/provider.hpp"
#include "http/browser_flow.hpp"
#include "core/crypto.hpp"
#include "http/auth.hpp"
#include "unit/connector_fixture.hpp"

#include <boost/json/serialize.hpp>
#include <gtest/gtest.h>

using namespace poni;
using namespace std::chrono_literals;

namespace {

std::int64_t epoch(core::TimePoint t) {
    return std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
}

boost::json::object claims() {
    return {{"iss", "https://accounts.google.com"},
            {"aud", "client-id"},
            {"sub", "1234567890"},
            {"email", "staff@xvisionmediaus.com"},
            {"email_verified", true},
            {"hd", "xvisionmediaus.com"},
            {"name", "Staff Member"},
            {"nonce", "the-nonce"},
            {"exp", epoch(test::t0()) + 3600}};
}

// claims() with one of them changed.
boost::json::object with(std::string_view key, boost::json::value value) {
    auto changed = claims();
    changed[key] = std::move(value);
    return changed;
}

// An ID token as Google's token endpoint returns one. Nothing checks its
// signature (D-091 point 12), so the third part is not one.
std::string id_token(const boost::json::object& token_claims) {
    return core::base64url_encode(R"({"alg":"RS256","kid":"k1","typ":"JWT"})") + "." +
           core::base64url_encode(boost::json::serialize(token_claims)) + "." + core::base64url_encode("not a signature");
}

// The token endpoint's answer to a code exchange, with that ID token.
std::string token_answer(std::string_view token) {
    return boost::json::serialize(boost::json::object{{"access_token", "at"}, {"expires_in", 3600}, {"id_token", token}});
}

identity::Identity workspace_user() {
    return {.subject = "1",
            .email = "staff@xvisionmediaus.com",
            .email_verified = true,
            .hosted_domain = "xvisionmediaus.com",
            .name = "Staff",
            .picture_url = std::nullopt};
}

// The provider against a scripted Google.
class GoogleProvider : public test::ConnectorTest {
protected:
    GoogleProvider() : ConnectorTest({identity::kSignInApi}) {}
};

}  // namespace

TEST(IdTokenClaims, MapToAnIdentity) {
    const auto identity = identity::identity_from_claims(claims(), "client-id", "the-nonce", test::t0());
    ASSERT_TRUE(identity) << identity.error();
    EXPECT_EQ(identity->subject, "1234567890");
    EXPECT_EQ(identity->email, "staff@xvisionmediaus.com");
    EXPECT_TRUE(identity->email_verified);
    EXPECT_EQ(identity->hosted_domain, "xvisionmediaus.com");
}

TEST(IdTokenClaims, AreCheckedForIssuerAudienceNonceAndExpiry) {
    const auto check = [](std::string_view key, boost::json::value value) {
        return identity::identity_from_claims(with(key, std::move(value)), "client-id", "the-nonce", test::t0()).has_value();
    };
    EXPECT_FALSE(check("iss", "https://evil.example"));
    EXPECT_FALSE(check("aud", "another-client")) << "a token issued to another application";
    EXPECT_FALSE(check("nonce", "replayed"));
    EXPECT_FALSE(check("exp", epoch(test::t0()) - 3600));
    EXPECT_FALSE(check("sub", ""));
    EXPECT_TRUE(check("iss", "accounts.google.com"));
    EXPECT_TRUE(check("exp", epoch(test::t0()) - 30)) << "a minute of clock difference is allowed";
    EXPECT_FALSE(identity::identity_from_claims(claims(), "client-id", "", test::t0())) << "an empty nonce never matches";
}

TEST_F(GoogleProvider, SendsTheBrowserToGoogleWithPkceAndTheDomainHint) {
    identity::GoogleProvider provider(http_, clock_, limiter_, "client-id", "client-secret", {"xvisionmediaus.com"});
    const boost::urls::url url(provider.authorization_url(
        {.redirect_uri = "https://gbp.example/api/auth/google/callback", .state = "s", .nonce = "n", .code_challenge = "c"}));
    EXPECT_EQ(url.host(), "accounts.google.com");
    const auto param = [&](std::string_view name) { return (*url.params().find(name)).value; };
    EXPECT_EQ(param("client_id"), "client-id");
    EXPECT_EQ(param("redirect_uri"), "https://gbp.example/api/auth/google/callback");
    EXPECT_EQ(param("scope"), "openid email profile");
    EXPECT_EQ(param("code_challenge_method"), "S256");
    EXPECT_EQ(param("hd"), "xvisionmediaus.com");
    EXPECT_FALSE(url.params().contains("client_secret"));
}

TEST_F(GoogleProvider, WithSeveralDomainsTheHintAsksForAnyWorkspaceAccount) {
    const identity::AuthorizationRequest request{
        .redirect_uri = "https://gbp.example/api/auth/google/callback", .state = "s", .nonce = "n", .code_challenge = "c"};

    const identity::GoogleProvider two(http_, clock_, limiter_, "client-id", "client-secret", {"xvisionmediaus.com", "xvisionus.com"});
    const boost::urls::url url(two.authorization_url(request));
    EXPECT_EQ((*url.params().find("hd")).value, "*");

    const identity::GoogleProvider none(http_, clock_, limiter_, "client-id", "client-secret", {});
    EXPECT_FALSE(boost::urls::url(none.authorization_url(request)).params().contains("hd"));
}

TEST_F(GoogleProvider, RedeemsACodeAndVerifiesTheIdToken) {
    identity::GoogleProvider provider(http_, clock_, limiter_, "client-id", "client-secret", {"xvisionmediaus.com"});
    http_.reply(200, token_answer(id_token(claims())));
    const auto identity = await(provider.redeem("the-code", "the-verifier", "https://gbp.example/cb", "the-nonce"));
    ASSERT_TRUE(identity) << identity.error();
    EXPECT_EQ(identity->email, "staff@xvisionmediaus.com");
    ASSERT_EQ(http_.requests.size(), 1u) << "one request: the code exchange. No keys are fetched";
    const auto& request = http_.requests.at(0);
    EXPECT_EQ(request.method, http::ClientMethod::post);
    EXPECT_EQ(request.url.buffer(), "https://oauth2.googleapis.com/token");
    EXPECT_EQ(request.body, "grant_type=authorization_code&code=the-code&client_id=client-id&client_secret=client-secret"
                            "&redirect_uri=https%3A%2F%2Fgbp.example%2Fcb&code_verifier=the-verifier");

    // A refused code, an answer without an ID token, and one whose token is no token.
    http_.reply(400, R"({"error": "invalid_grant", "error_description": "Bad Request"})");
    EXPECT_FALSE(await(provider.redeem("used-code", "v", "https://gbp.example/cb", "the-nonce")));
    http_.reply(200, R"({"access_token": "at", "expires_in": 3600})");
    EXPECT_FALSE(await(provider.redeem("code-2", "v", "https://gbp.example/cb", "the-nonce")));
    const auto payload = core::base64url_encode(boost::json::serialize(claims()));
    for (const std::string& bad : {std::string(), std::string("abc"), "a." + payload, "a." + payload + ".c.d", std::string("!!!.###.$$$"),
                                  "a." + core::base64url_encode("not JSON") + ".c", "a." + core::base64url_encode(R"(["a list"])") + ".c"}) {
        http_.reply(200, token_answer(bad));
        EXPECT_FALSE(await(provider.redeem("code-3", "v", "https://gbp.example/cb", "the-nonce"))) << bad;
    }
}

// D-091 point 12: the signature of the ID token is not checked, because the
// token is the token endpoint's own answer to the engine's code exchange.
// What the token claims is checked as before, and so is who may sign in.
TEST_F(GoogleProvider, ATokenIsStillRefusedForWhatItClaims) {
    identity::GoogleProvider provider(http_, clock_, limiter_, "client-id", "client-secret", {"xvisionmediaus.com"});
    const std::vector<std::string> allowed{"xvisionmediaus.com"};
    const auto redeem = [&](const boost::json::object& token_claims) {
        http_.reply(200, token_answer(id_token(token_claims)));
        return await(provider.redeem("the-code", "the-verifier", "https://gbp.example/cb", "the-nonce"));
    };
    const auto signed_in = redeem(claims());
    ASSERT_TRUE(signed_in) << signed_in.error();
    EXPECT_TRUE(http::domain_allowed(*signed_in, allowed));

    EXPECT_FALSE(redeem(with("aud", "another-client"))) << "a token Google issued to another application";
    EXPECT_FALSE(redeem(with("iss", "https://evil.example"))) << "a token of another issuer";
    EXPECT_FALSE(redeem(with("nonce", "replayed"))) << "a token of another sign-in";
    EXPECT_FALSE(redeem(with("exp", epoch(test::t0()) - 3600))) << "an expired token";
    auto no_nonce = claims();
    no_nonce.erase("nonce");
    EXPECT_FALSE(redeem(no_nonce));

    // A domain that may not sign in: the token is in order and names the person; the sign-in rule refuses them.
    auto outsider = claims();
    outsider["email"] = "someone@elsewhere.example";
    outsider["hd"] = "elsewhere.example";
    const auto other_domain = redeem(outsider);
    ASSERT_TRUE(other_domain) << other_domain.error();
    EXPECT_FALSE(http::domain_allowed(*other_domain, allowed));
    auto consumer = claims();
    consumer["email"] = "someone@gmail.com";
    consumer.erase("hd");
    const auto personal = redeem(consumer);
    ASSERT_TRUE(personal) << personal.error();
    EXPECT_FALSE(http::domain_allowed(*personal, allowed));
    const auto unverified = redeem(with("email_verified", false));
    ASSERT_TRUE(unverified) << unverified.error();
    EXPECT_FALSE(http::domain_allowed(*unverified, allowed));

    for (const auto& request : http_.requests) EXPECT_EQ(request.url.buffer(), "https://oauth2.googleapis.com/token");
}

TEST(SignInDomain, OnlyVerifiedWorkspaceAccountsOfTheDomainPass) {
    const std::vector<std::string> one{"xvisionmediaus.com"};
    EXPECT_TRUE(http::domain_allowed(workspace_user(), one));

    auto upper = workspace_user();
    upper.email = "Staff@XVisionMediaUS.com";
    upper.hosted_domain = "XVISIONMEDIAUS.COM";
    EXPECT_TRUE(http::domain_allowed(upper, one));

    auto unverified = workspace_user();
    unverified.email_verified = false;
    EXPECT_FALSE(http::domain_allowed(unverified, one));

    // A consumer account has no hosted domain, whatever its address says.
    auto consumer = workspace_user();
    consumer.hosted_domain.reset();
    EXPECT_FALSE(http::domain_allowed(consumer, one));

    auto other = workspace_user();
    other.email = "someone@gmail.com";
    other.hosted_domain = "gmail.com";
    EXPECT_FALSE(http::domain_allowed(other, one));

    auto lookalike = workspace_user();
    lookalike.email = "staff@xvisionmediaus.com.evil.example";
    lookalike.hosted_domain = "xvisionmediaus.com.evil.example";
    EXPECT_FALSE(http::domain_allowed(lookalike, one));

    auto mismatch = workspace_user();
    mismatch.email = "staff@elsewhere.example";
    EXPECT_FALSE(http::domain_allowed(mismatch, one));

    EXPECT_FALSE(http::domain_allowed(workspace_user(), std::vector<std::string>{})) << "no domains, nobody gets in";
}

TEST(SignInDomain, SeveralDomainsMayBeAllowed) {
    const std::vector<std::string> one{"xvisionmediaus.com"};
    const std::vector<std::string> both{"xvisionmediaus.com", "xvisionus.com"};

    auto second = workspace_user();
    second.email = "staff@xvisionus.com";
    second.hosted_domain = "xvisionus.com";
    EXPECT_TRUE(http::domain_allowed(second, both));
    EXPECT_FALSE(http::domain_allowed(second, one));
    EXPECT_TRUE(http::domain_allowed(workspace_user(), both)) << "the first domain still works";

    // An address on one allowed domain in the organization of the other: Google
    // may name the organization's main domain for its other domains.
    auto secondary = workspace_user();
    secondary.email = "staff@xvisionus.com";
    EXPECT_TRUE(http::domain_allowed(secondary, both));
    EXPECT_FALSE(http::domain_allowed(secondary, one)) << "the address's own domain must be allowed too";

    // Either half outside the list is refused.
    auto foreign_org = workspace_user();
    foreign_org.email = "staff@xvisionus.com";
    foreign_org.hosted_domain = "elsewhere.example";
    EXPECT_FALSE(http::domain_allowed(foreign_org, both));
    auto consumer = second;
    consumer.hosted_domain.reset();
    EXPECT_FALSE(http::domain_allowed(consumer, both));
    auto no_domain = workspace_user();
    no_domain.email = "staff@";
    EXPECT_FALSE(http::domain_allowed(no_domain, both));
}

TEST(LoginThrottle, LocksAnAddressOutAfterTooManyAttempts) {
    core::ManualClock clock(test::t0());
    http::LoginThrottle throttle(clock, {.max_attempts = 3, .window = 600s, .lockout = 900s});

    for (int i = 0; i < 3; ++i) EXPECT_FALSE(throttle.attempt("203.0.113.9"));
    EXPECT_EQ(throttle.attempt("203.0.113.9"), 900s);
    EXPECT_FALSE(throttle.attempt("198.51.100.1")) << "other addresses are not affected";

    clock.advance(600s);
    EXPECT_EQ(throttle.attempt("203.0.113.9"), 300s) << "still locked, and waiting doesn't reset the count early";
    clock.advance(301s);
    EXPECT_FALSE(throttle.attempt("203.0.113.9"));
}

TEST(LoginThrottle, ForgetsAttemptsWhenTheWindowPasses) {
    core::ManualClock clock(test::t0());
    http::LoginThrottle throttle(clock, {.max_attempts = 2, .window = 60s, .lockout = 900s});
    EXPECT_FALSE(throttle.attempt("a"));
    EXPECT_FALSE(throttle.attempt("a"));
    clock.advance(61s);
    EXPECT_FALSE(throttle.attempt("a"));
    EXPECT_FALSE(throttle.attempt("a"));
    EXPECT_TRUE(throttle.attempt("a"));
}

// The settings sign-in reads are checked where every setting is (core::load_config).
TEST(AuthOptions, RefuseInsecureSettings) {
    const auto refused = [](core::Environment env) { return !core::load_config(env, std::nullopt).has_value(); };
    EXPECT_FALSE(refused({})) << "the default is local development on http://localhost:5173";

    EXPECT_FALSE(refused({{"PONI_PUBLIC_URL", "https://gbp.xvisionmediaus.com"}}));
    EXPECT_TRUE(http::served_over_https({.public_url = "https://gbp.xvisionmediaus.com"}));

    EXPECT_TRUE(refused({{"PONI_PUBLIC_URL", "https://gbp.xvisionmediaus.com"}, {"PONI_DEV_LOGIN_EMAIL", "dev@localhost"}}))
        << "development sign-in can't be on behind an https address";

    // (An empty value is no value: the default address is used.)
    for (const auto* bad : {"http://gbp.xvisionmediaus.com", "https://gbp.example/", "https://gbp.example/app", "gbp.example",
                            "ftp://gbp.example"}) {
        EXPECT_TRUE(refused({{"PONI_PUBLIC_URL", bad}})) << bad;
    }
    EXPECT_FALSE(refused({{"PONI_PUBLIC_URL", "http://127.0.0.1:5173"}}));

    EXPECT_TRUE(refused({{"PONI_SIGNIN_DOMAINS", " , "}})) << "sign-in needs at least one allowed domain";
    EXPECT_TRUE(refused({{"PONI_SIGNIN_DOMAINS", "xvisionmediaus.com, bad domain"}}));
}

TEST(ClientIp, TrustsForwardedForOnlyWhenTold) {
    http::Request request;
    request.remote_ip = "10.0.0.2";
    request.headers["x-forwarded-for"] = "1.2.3.4, 203.0.113.7";
    EXPECT_EQ(http::client_ip(request, false), "10.0.0.2");
    // The last entry is the one the reverse proxy added; earlier ones are the client's claim.
    EXPECT_EQ(http::client_ip(request, true), "203.0.113.7");
    request.headers.clear();
    EXPECT_EQ(http::client_ip(request, true), "10.0.0.2");

    // The router resolves it once: a handler (and the sign-in hook) reads request.remote_ip.
    http::Router router;
    router.add(boost::beast::http::verb::get, "/ip", {http::Access::anonymous, "test.ip"},
               [](const http::Request& seen) -> boost::asio::awaitable<http::Response> {
                   co_return http::Response::json(200, boost::json::value(seen.remote_ip));
               });
    request.path = "/ip";
    request.headers["x-forwarded-for"] = "1.2.3.4, 203.0.113.7";
    boost::asio::io_context io;
    EXPECT_EQ(test::run(io, router.dispatch(request)).body, "\"10.0.0.2\"");
    router.trust_forwarded_for(true);
    EXPECT_EQ(test::run(io, router.dispatch(request)).body, "\"203.0.113.7\"");

    // So does the check before the body of a route that takes a file.
    std::string seen_early;
    router.set_auth_hook([&seen_early](http::Request& early, const http::RouteInfo&) -> boost::asio::awaitable<std::optional<http::Response>> {
        seen_early = early.remote_ip;
        co_return std::nullopt;
    });
    router.add(boost::beast::http::verb::post, "/file", {http::Access::anonymous, "test.file", 2 * http::Router::kDefaultBodyLimit},
               [](const http::Request&) -> boost::asio::awaitable<http::Response> { co_return http::Response::no_content(); });
    request.method = boost::beast::http::verb::post;
    request.path = "/file";
    EXPECT_FALSE(test::run(io, router.refuse_before_body(request)));
    EXPECT_EQ(seen_early, "203.0.113.7");
}

// What a start binds to its attempt travels in the cookie after the secret, and is under the state (D-095).
TEST(SignIn, ANoteBoundToAnAttemptIsUnderItsState) {
    const http::BrowserFlow flow("poni-signin", "poni_signin", "/api/auth", false);
    const std::string secret = "a-secret-as-random-token-makes-one-0123456789";
    EXPECT_FALSE(http::BrowserFlow::note(secret)) << "an attempt started without a note";

    const auto value = http::BrowserFlow::with_note(secret, "the-note");
    EXPECT_EQ(value, secret + ".the-note");
    EXPECT_EQ(http::BrowserFlow::note(value), "the-note");
    EXPECT_EQ(http::BrowserFlow::note(secret + "."), "") << "a note that was cut down to nothing is still a note";

    // Another note, or none, is another state, nonce and verifier.
    for (const auto& other : {http::BrowserFlow::with_note(secret, "another-note"), secret}) {
        EXPECT_NE(flow.derive("state", value), flow.derive("state", other));
        EXPECT_NE(flow.derive("nonce", value), flow.derive("nonce", other));
        EXPECT_NE(flow.verifier(value), flow.verifier(other));
    }
    EXPECT_EQ(http::s256_challenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"), "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM")
        << "the example of RFC 7636, appendix B";
}
