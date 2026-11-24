// Connecting the GBP managing account, the sending mailbox and the
// Workspace administrator from the dashboard (D-079, D-106 point 2), over a
// real PostgreSQL and a scripted Google: the engine's own HTTP layer as it
// runs, with FakeHttp standing in for accounts.google.com, the Business
// Profile APIs and the Workspace directory. No test reaches
// Google; the owner does the real sign-in.

#include "api_fixture.hpp"
#include "core/crypto.hpp"
#include "core/rate_limiter.hpp"
#include "connectors/gbp/reader.hpp"
#include "connectors/google/auth.hpp"
#include "connectors/workspace/directory.hpp"
#include "db/repo_auth.hpp"
#include "db/repo_notifications.hpp"
#include "http/connect.hpp"

#include <boost/url/parse.hpp>
#include <gmock/gmock.h>

using namespace poni;
using namespace std::chrono_literals;
using test::Browser;
using testing::HasSubstr;
using testing::Not;

namespace {

constexpr std::string_view kMailbox = "host@xvisionmediaus.com";
constexpr std::string_view kManager = "gbp-manager@xvisionmediaus.com";
constexpr std::string_view kRefreshToken = "1//refresh-token-of-the-account";
constexpr std::string_view kClientSecret = "the-oauth-client-secret";
// The Workspace administrator, made up: its address is of the sign-in domains the fixture configures, and its id is
// the `sub` of its sign-in and its Directory id (D-106 point 2).
constexpr std::string_view kAdministrator = "directory-admin@example.com";
constexpr std::string_view kAdministratorId = "104000000000000000042";
constexpr std::string_view kDirectoryScopes = "https://www.googleapis.com/auth/admin.directory.user.readonly openid email";
constexpr std::string_view kDirectoryList = "https://admin.googleapis.com/admin/directory/v1/users?customer=my_customer&maxResults=500";

class Connect : public test::ApiTest {
protected:
    void SetUp() override {
        ApiTest::SetUp();
        if (HasFatalFailure()) return;
        for (const auto api : {google::kOAuthApi, gbp::kAccountsApi, gbp::kLocationsApi, workspace::kDirectoryApi}) {
            limiter_.configure(std::string(api), {.per_minute = 6000, .burst = 100});
        }
        rebuild(true, true);
        browser_ = sign_in();
    }

    // The HTTP layer with the connection routes, as main() assembles it.
    void rebuild(bool with_client, bool with_key, std::string google_mock_url = {}) {
        build({});
        connect_.reset();
        connect_config_ = {};
        if (with_key) connect_config_.secret_box = box_;
        connect_config_.public_url = "http://localhost:5173";
        connect_config_.gbp_oauth_client_id = with_client ? "client-id.apps.googleusercontent.com" : "";
        connect_config_.gbp_oauth_client_secret = with_client ? std::string(kClientSecret) : "";
        connect_config_.google_mock_url = std::move(google_mock_url);
        connect_config_.signin_domains = {"example.com"};
        connect_ = std::make_unique<http::ConnectService>(db(), clock_, google_, limiter_, connect_config_,
                                                          [this](std::string_view account) { connected_.emplace_back(account); });
        connect_->add_routes(*router_);
    }

    struct Started {
        std::string url;     // where the browser is sent
        std::string state;   // as Google would send it back
        std::string cookie;  // the flow cookie, "name=value"
        boost::urls::url parsed;
        std::string param(std::string_view name) const {
            for (const auto& p : parsed.params()) {
                if (p.key == name) return p.value;
            }
            return {};
        }
    };
    // Started by browser_, or with `token` by the phone app's session.
    Started start(const std::string& account, const std::string& token = {}) {
        const auto path = "/api/connections/" + account + "/start";
        const auto response = token.empty() ? post(path, "") : as_app(verb::post, path, token);
        EXPECT_EQ(response.status, 200u) << response.body;
        Started started;
        started.url = std::string(json(response).at("authorization_url").as_string());
        started.parsed = boost::urls::parse_uri(started.url).value();
        started.state = started.param("state");
        started.cookie = pair_of(set_cookie(response, "poni_connect"));
        return started;
    }
    // Google's redirect: a top-level navigation from another site, so it carries the Lax flow cookie and not the session.
    http::Response redirect_back(const std::string& account, const Started& started, const std::string& query) {
        return get("/api/connections/" + account + "/callback?" + query, {.cookie = started.cookie});
    }
    http::Response finish(const std::string& account, const Started& started) {
        return redirect_back(account, started, "code=the-code&state=" + started.state);
    }
    static std::string location(const http::Response& response) { return header(response, "Location"); }

    // What Google answers when the code is exchanged (by default: with the ID token of the managing account), and when
    // the new refresh token is tried.
    void google_accepts_the_code(std::optional<std::string> extra = {}) {
        google_.reply(200, std::format(R"({{"access_token": "at-1", "expires_in": 3600, "refresh_token": "{}"{}}})", kRefreshToken,
                                       extra.value_or(with_id_token(kManager))));
    }
    void google_refreshes() { google_.reply(200, R"({"access_token": "at-2", "expires_in": 3600})"); }
    void google_lists_one_account_with_two_locations() {
        google_.reply(200, R"({"accounts": [{"name": "accounts/111", "accountName": "PONI", "type": "PERSONAL"}]})");
        google_.reply(200, R"({"locations": [{"name": "locations/9001", "title": "One"}, {"name": "locations/9002", "title": "Two"}]})");
    }
    // An ID token as Google's token endpoint returns it; only its payload is read.
    static std::string id_token(std::string_view email, std::string_view verified = "true", std::string_view sub = {}) {
        const auto subject = sub.empty() ? std::string() : std::format(R"(, "sub": "{}")", sub);
        return "header." + core::base64url_encode(std::format(R"({{"email": "{}", "email_verified": {}{}}})", email, verified, subject)) +
               ".signature";
    }
    // The members of the token endpoint's answer that carry it.
    static std::string with_id_token(std::string_view email) { return std::format(R"(, "id_token": "{}")", id_token(email)); }
    void google_accepts_the_mailbox(std::string_view email, std::string_view scope = "https://www.googleapis.com/auth/gmail.send openid email") {
        google_accepts_the_code(std::format(R"(, "scope": "{}", "id_token": "{}")", scope, id_token(email)));
    }

    // The code exchange of a Workspace administrator: its ID token names `sub` (none if empty) and `email`.
    void google_accepts_the_administrator(std::string_view sub = kAdministratorId, std::string_view scope = kDirectoryScopes,
                                          std::string_view email = kAdministrator) {
        google_accepts_the_code(std::format(R"(, "scope": "{}", "id_token": "{}")", scope, id_token(email, "true", sub)));
    }
    // One entry of the directory's list, a delegated administrator unless `flags` says otherwise.
    static std::string directory_user(std::string_view id, std::string_view email, std::string_view flags = R"(, "isDelegatedAdmin": true)") {
        return std::format(R"({{"id": "{}", "primaryEmail": "{}"{}}})", id, email, flags);
    }
    // The directory's list with `entry` second, between a super administrator and a colleague, and the page token if any.
    static std::string directory_with(std::string_view entry, std::string_view next_page = {}) {
        const auto next = next_page.empty() ? std::string() : std::format(R"(, "nextPageToken": "{}")", next_page);
        const auto first = directory_user("104000000000000000041", "first-admin@example.com", R"(, "isAdmin": true)");
        const auto last = directory_user("104000000000000000043", "colleague@example.com", "");
        return std::format(R"({{"users": [{}, {}, {}]{}}})", first, entry, last, next);
    }
    // The requests that went to the directory's host.
    std::vector<http::ClientRequest> directory_requests() const {
        std::vector<http::ClientRequest> out;
        for (const auto& request : google_.requests) {
            if (request.url.host() == "admin.googleapis.com") out.push_back(request);
        }
        return out;
    }

    std::optional<google::OAuthCredentials> stored(std::string_view name) {
        pqxx::work tx{conn()};
        return repo::load_oauth_credentials(tx, box_, name).value();
    }
    boost::json::value status() { return json(get("/api/system/status", browser_)); }
    std::string sent(std::size_t request) { return google_.requests.at(request).body; }

    test::FakeHttp google_;
    core::RateLimiter limiter_{io_.get_executor(), clock_};
    core::SecretBox box_ = core::SecretBox::from_base64_key(core::base64_encode(std::string(32, 'k'))).value();
    std::unique_ptr<http::ConnectService> connect_;
    // The accounts the engine was told to stop using its cached access token for.
    std::vector<std::string> connected_;
    core::Config connect_config_;
};

}  // namespace

// ---- starting ---------------------------------------------------------------------------

TEST_F(Connect, AgainstTheGoogleMockTheBrowserGoesToTheMocksConsentPage) {
    rebuild(true, true, "http://127.0.0.1:8090");
    const auto started = start("gbp");
    EXPECT_THAT(started.url, testing::StartsWith("http://127.0.0.1:8090/accounts.google.com/o/oauth2/v2/auth?"));
    // Everything else is what Google would get: the mock sends the browser back to the same callback.
    EXPECT_EQ(started.param("redirect_uri"), "http://localhost:5173/api/connections/gbp/callback");
    EXPECT_EQ(started.param("scope"), "https://www.googleapis.com/auth/business.manage openid email");
    EXPECT_EQ(started.state.size(), 43u);
}

TEST_F(Connect, OnlyASignedInPersonWithTheCsrfTokenCanStart) {
    EXPECT_EQ(call(verb::post, "/api/connections/gbp/start").status, 401u);
    const Browser no_csrf{.cookie = browser_.cookie, .csrf = {}};
    EXPECT_EQ(call(verb::post, "/api/connections/gbp/start", no_csrf).status, 403u);
    EXPECT_EQ(call(verb::post, "/api/connections/gmail/start", no_csrf).status, 403u);
    EXPECT_EQ(post("/api/connections/drive/start", "").status, 404u);
    EXPECT_EQ(get("/api/connections/gbp/start", browser_).status, 405u) << "starting changes state: it is a POST";
    EXPECT_TRUE(google_.requests.empty());
}

TEST_F(Connect, StartSendsTheBrowserToGoogleForTheManagingAccount) {
    const auto started = start("gbp");
    EXPECT_THAT(started.url, testing::StartsWith("https://accounts.google.com/o/oauth2/v2/auth?"));
    EXPECT_EQ(started.param("client_id"), "client-id.apps.googleusercontent.com");
    EXPECT_EQ(started.param("redirect_uri"), "http://localhost:5173/api/connections/gbp/callback");
    EXPECT_EQ(started.param("response_type"), "code");
    // openid and email to learn which account signed in (D-100).
    EXPECT_EQ(started.param("scope"), "https://www.googleapis.com/auth/business.manage openid email");
    // Google only returns a refresh token on a fresh consent; the account is the one picked in the chooser.
    EXPECT_EQ(started.param("access_type"), "offline");
    EXPECT_EQ(started.param("prompt"), "select_account consent");
    EXPECT_EQ(started.param("login_hint"), "") << "no setting names the account";
    EXPECT_EQ(started.param("code_challenge_method"), "S256");
    EXPECT_EQ(started.param("code_challenge").size(), 43u);
    EXPECT_EQ(started.state.size(), 43u);
    // Neither the secret of the client nor the one of this attempt is in the address.
    EXPECT_THAT(started.url, Not(HasSubstr(std::string(kClientSecret))));
    EXPECT_THAT(started.url, Not(HasSubstr(started.cookie.substr(started.cookie.find('=') + 1))));

    const auto response = post("/api/connections/gbp/start", "");
    const auto cookie = set_cookie(response, "poni_connect");
    EXPECT_THAT(cookie, HasSubstr("HttpOnly"));
    EXPECT_THAT(cookie, HasSubstr("SameSite=Lax")) << "Google's redirect is a cross-site navigation";
    EXPECT_THAT(cookie, HasSubstr("Path=/api/connections"));
    EXPECT_THAT(cookie, HasSubstr("Max-Age=600"));
    EXPECT_NE(json(response).at("authorization_url").as_string(), started.url) << "every attempt has its own state";
}

TEST_F(Connect, StartForTheMailboxAsksForThePermissionToSendAndNamesNoMailbox) {
    const auto started = start("gmail");
    EXPECT_EQ(started.param("scope"), "https://www.googleapis.com/auth/gmail.send openid email");
    EXPECT_EQ(started.param("prompt"), "select_account consent");
    EXPECT_EQ(started.param("login_hint"), "") << "the mailbox is whichever account signs in (D-100)";
    EXPECT_EQ(started.param("redirect_uri"), "http://localhost:5173/api/connections/gmail/callback");
}

TEST_F(Connect, AnEngineWithoutTheOAuthClientOrTheKeySaysSo) {
    EXPECT_TRUE(connect_->configured());
    rebuild(false, true);
    browser_ = sign_in();
    EXPECT_FALSE(connect_->configured());
    auto refused = post("/api/connections/gbp/start", "");
    EXPECT_EQ(refused.status, 409u);
    EXPECT_EQ(error_code(refused), "not_configured");
    EXPECT_THAT(std::string(json(refused).at("error").at("message").as_string()), HasSubstr("PONI_GBP_OAUTH_CLIENT_ID"));

    rebuild(true, false);
    browser_ = sign_in();
    refused = post("/api/connections/gmail/start", "");
    EXPECT_EQ(error_code(refused), "not_configured");
    EXPECT_THAT(std::string(json(refused).at("error").at("message").as_string()), HasSubstr("PONI_SECRET_KEY"));
}

// ---- the GBP managing account -----------------------------------------------------------

TEST_F(Connect, TheManagingAccountIsCheckedEncryptedStoredAndAuditedInThePersonsName) {
    EXPECT_FALSE(status().at("gbp").at("connected").as_bool());
    const auto started = start("gbp");
    google_accepts_the_code();
    google_refreshes();
    google_lists_one_account_with_two_locations();

    const auto response = finish("gbp", started);
    EXPECT_EQ(response.status, 302u);
    EXPECT_EQ(location(response), "http://localhost:5173/#/status?connected=gbp");
    EXPECT_THAT(set_cookie(response, "poni_connect"), HasSubstr("Max-Age=0")) << "the flow cookie is used up";

    // The code was exchanged with the verifier that belongs to the challenge, at the address Google was told.
    ASSERT_EQ(google_.requests.size(), 4u);
    EXPECT_EQ(google_.requests[0].url.buffer(), "https://oauth2.googleapis.com/token");
    EXPECT_THAT(sent(0), HasSubstr("grant_type=authorization_code"));
    EXPECT_THAT(sent(0), HasSubstr("code=the-code"));
    EXPECT_THAT(sent(0), HasSubstr("redirect_uri=http%3A%2F%2Flocalhost%3A5173%2Fapi%2Fconnections%2Fgbp%2Fcallback"));
    EXPECT_THAT(sent(0), HasSubstr("code_verifier="));
    // Then the new token was tried: a refresh, the accounts, their locations.
    EXPECT_THAT(sent(1), HasSubstr("grant_type=refresh_token"));
    EXPECT_THAT(std::string(google_.requests[2].url.buffer()), HasSubstr("/accounts"));
    EXPECT_THAT(std::string(google_.requests[3].url.buffer()), HasSubstr("accounts/111/locations"));

    const auto credentials = stored(repo::kGbpCredential);
    ASSERT_TRUE(credentials);
    EXPECT_EQ(credentials->refresh_token, kRefreshToken);
    EXPECT_EQ(credentials->client_id, "client-id.apps.googleusercontent.com");
    // Encrypted at rest (D-043): neither the token nor the client secret is readable in the row.
    const auto row = value<std::string>("SELECT ciphertext FROM credentials WHERE name = 'gbp'");
    EXPECT_THAT(row, Not(HasSubstr(std::string(kRefreshToken))));
    EXPECT_THAT(row, Not(HasSubstr(std::string(kClientSecret))));

    EXPECT_TRUE(status().at("gbp").at("connected").as_bool());
    EXPECT_EQ(status().at("gbp").at("account_email").as_string(), kManager) << "the account a client invites (D-100)";
    EXPECT_EQ(value<std::string>("SELECT account_email FROM credentials WHERE name = 'gbp'"), kManager);
    EXPECT_FALSE(status().at("gbp").at("last_ok_at").is_null());
    EXPECT_THAT(connected_, testing::ElementsAre("gbp")) << "the engine drops the access token it cached for the account before";
    EXPECT_EQ(count("audit_log WHERE action = 'credential.store' AND object_id = 'gbp' AND actor_type = 'user' "
                    "AND actor = 'staff@xvisionmediaus.com' AND ip = '203.0.113.10'"),
              1);
    const auto after = value<std::string>("SELECT after::text FROM audit_log WHERE action = 'credential.store'");
    EXPECT_THAT(after, HasSubstr("\"locations\": 2"));
    EXPECT_THAT(after, HasSubstr(std::string(kManager)));
    EXPECT_THAT(after, Not(HasSubstr(std::string(kRefreshToken)))) << "the audit log says what was connected, never the token";
    EXPECT_THAT(after, Not(HasSubstr(std::string(kClientSecret))));
    // Nothing of it reached the browser either.
    EXPECT_THAT(response.body + location(response), Not(HasSubstr(std::string(kRefreshToken))));
}

// Switching to another managing account is connecting it: one connection at a time (D-100).
TEST_F(Connect, ConnectingAgainReplacesTheStoredTokenAndItsAccount) {
    for (const auto& [token, email] : {std::pair{"1//the-first", "personal@gmail.com"}, std::pair{"1//the-second", "gbp@xvisionmediaus.com"}}) {
        const auto started = start("gbp");
        google_.reply(200, std::format(R"({{"access_token": "at", "expires_in": 3600, "refresh_token": "{}"{}}})", token, with_id_token(email)));
        google_refreshes();
        google_lists_one_account_with_two_locations();
        ASSERT_EQ(location(finish("gbp", started)), "http://localhost:5173/#/status?connected=gbp");
        EXPECT_EQ(status().at("gbp").at("account_email").as_string(), email);
    }
    EXPECT_EQ(stored(repo::kGbpCredential)->refresh_token, "1//the-second");
    EXPECT_EQ(count("credentials WHERE name = 'gbp'"), 1);
    EXPECT_EQ(count("audit_log WHERE action = 'credential.store'"), 2);
}

TEST_F(Connect, AnAccountGoogleNamesNoVerifiedAddressForIsNotStored) {
    // No ID token: the scope was not granted, or Google sent none.
    auto started = start("gbp");
    google_accepts_the_code("");
    EXPECT_EQ(location(finish("gbp", started)), "http://localhost:5173/#/status?account=gbp&connect_error=no_email");
    // An address Google does not call verified.
    started = start("gbp");
    google_accepts_the_code(std::format(R"(, "id_token": "{}")", id_token(kManager, "false")));
    EXPECT_EQ(location(finish("gbp", started)), "http://localhost:5173/#/status?account=gbp&connect_error=no_email");
    EXPECT_EQ(google_.requests.size(), 2u) << "only the exchanges: the account's token was not tried";
    EXPECT_FALSE(stored(repo::kGbpCredential));
    EXPECT_TRUE(status().at("gbp").at("account_email").is_null());
    EXPECT_EQ(count("audit_log WHERE action = 'credential.store'"), 0);
}

TEST_F(Connect, AnAccountThatCannotListLocationsIsNotStored) {
    const auto started = start("gbp");
    google_accepts_the_code();
    google_refreshes();
    google_.reply(403, R"({"error": {"status": "PERMISSION_DENIED", "message": "The caller does not have permission"}})");
    EXPECT_EQ(location(finish("gbp", started)), "http://localhost:5173/#/status?account=gbp&connect_error=no_access");
    EXPECT_FALSE(stored(repo::kGbpCredential));
    EXPECT_FALSE(status().at("gbp").at("connected").as_bool());
    EXPECT_EQ(count("audit_log WHERE action = 'credential.store'"), 0);
}

// A personal account picked by mistake: Google answers every call, and it manages nothing.
TEST_F(Connect, AnAccountThatSeesNoLocationDoesNotReplaceTheStoredToken) {
    auto started = start("gbp");
    google_accepts_the_code();
    google_refreshes();
    google_lists_one_account_with_two_locations();
    ASSERT_EQ(location(finish("gbp", started)), "http://localhost:5173/#/status?connected=gbp");

    started = start("gbp");
    google_.reply(200, std::format(R"({{"access_token": "at", "expires_in": 3600, "refresh_token": "1//of-a-personal-account"{}}})",
                                   with_id_token("someone@gmail.com")));
    google_refreshes();
    google_.reply(200, R"({"accounts": [{"name": "accounts/222", "accountName": "Someone", "type": "PERSONAL"}]})");
    google_.reply(200, "{}");  // Google omits an empty list
    EXPECT_EQ(location(finish("gbp", started)), "http://localhost:5173/#/status?account=gbp&connect_error=no_access");
    EXPECT_EQ(stored(repo::kGbpCredential)->refresh_token, kRefreshToken) << "the working token is still the one stored";
    EXPECT_EQ(status().at("gbp").at("account_email").as_string(), kManager) << "and so is its account";
    EXPECT_EQ(count("audit_log WHERE action = 'credential.store'"), 1);
    EXPECT_THAT(connected_, testing::ElementsAre("gbp"));

    // No account at all.
    started = start("gbp");
    google_accepts_the_code();
    google_refreshes();
    google_.reply(200, "{}");
    EXPECT_EQ(location(finish("gbp", started)), "http://localhost:5173/#/status?account=gbp&connect_error=no_access");
    EXPECT_EQ(count("audit_log WHERE action = 'credential.store'"), 1);
}

TEST_F(Connect, WithoutARefreshTokenNothingIsStored) {
    const auto started = start("gbp");
    google_.reply(200, R"({"access_token": "at-1", "expires_in": 3600})");
    EXPECT_EQ(location(finish("gbp", started)), "http://localhost:5173/#/status?account=gbp&connect_error=no_refresh_token");
    EXPECT_FALSE(stored(repo::kGbpCredential));
    EXPECT_EQ(google_.requests.size(), 1u);
}

TEST_F(Connect, ACodeGoogleDoesNotAcceptAndAnOutageAreNotStored) {
    auto started = start("gbp");
    google_.reply(400, R"({"error": "invalid_grant", "error_description": "Bad Request"})");
    EXPECT_EQ(location(finish("gbp", started)), "http://localhost:5173/#/status?account=gbp&connect_error=refused");

    started = start("gbp");
    google_accepts_the_code();
    google_.reply(503, "upstream unavailable");
    EXPECT_THAT(location(finish("gbp", started)), HasSubstr("connect_error="));
    EXPECT_FALSE(stored(repo::kGbpCredential));
    EXPECT_EQ(count("audit_log WHERE action = 'credential.store'"), 0);
}

// ---- the redirect belongs to one attempt of one browser ----------------------------------

TEST_F(Connect, TheRedirectMustBelongToTheAttemptOfTheBrowserThatStarted) {
    const auto started = start("gbp");

    // Another browser, without the flow cookie.
    EXPECT_EQ(location(get("/api/connections/gbp/callback?code=the-code&state=" + started.state)),
              "http://localhost:5173/#/status?account=gbp&connect_error=expired");
    // A cookie this engine never issued.
    EXPECT_EQ(location(get("/api/connections/gbp/callback?code=the-code&state=" + started.state, {.cookie = "poni_connect=made-up"})),
              "http://localhost:5173/#/status?account=gbp&connect_error=expired");
    // The right cookie with a state that is not its own: refused, and the attempt is used up.
    EXPECT_EQ(location(redirect_back("gbp", started, "code=the-code&state=someone-elses")),
              "http://localhost:5173/#/status?account=gbp&connect_error=failed");
    google_accepts_the_code();
    EXPECT_EQ(location(finish("gbp", started)), "http://localhost:5173/#/status?account=gbp&connect_error=expired");

    EXPECT_TRUE(google_.requests.empty()) << "no code was sent to Google";
    EXPECT_FALSE(stored(repo::kGbpCredential));
}

TEST_F(Connect, AnAttemptIsUsedOnceLastsTenMinutesAndIsForOneAccount) {
    // Used once: a second redirect with the same code does nothing.
    auto started = start("gbp");
    google_accepts_the_code();
    google_refreshes();
    google_lists_one_account_with_two_locations();
    ASSERT_EQ(location(finish("gbp", started)), "http://localhost:5173/#/status?connected=gbp");
    EXPECT_EQ(location(finish("gbp", started)), "http://localhost:5173/#/status?account=gbp&connect_error=expired");
    EXPECT_EQ(google_.requests.size(), 4u);

    // Ten minutes.
    started = start("gbp");
    clock_.advance(11min);
    EXPECT_EQ(location(finish("gbp", started)), "http://localhost:5173/#/status?account=gbp&connect_error=expired");

    // An attempt for the managing account cannot store a mailbox token, or the other way round.
    started = start("gbp");
    EXPECT_EQ(location(finish("gmail", started)), "http://localhost:5173/#/status?account=gmail&connect_error=expired");
    EXPECT_EQ(google_.requests.size(), 4u);
    EXPECT_FALSE(stored(repo::kGmailCredential));
    EXPECT_EQ(get("/api/connections/drive/callback?code=x&state=y").status, 404u);
}

TEST_F(Connect, APersonWhoCancelsAtGoogleOrSignsOutMeanwhileConnectsNothing) {
    auto started = start("gbp");
    EXPECT_EQ(location(redirect_back("gbp", started, "error=access_denied&state=" + started.state)),
              "http://localhost:5173/#/status?account=gbp&connect_error=cancelled");

    started = start("gbp");
    ASSERT_EQ(post("/api/auth/logout", "").status, 204u);
    google_accepts_the_code();
    EXPECT_EQ(location(finish("gbp", started)), "http://localhost:5173/#/status?account=gbp&connect_error=expired");
    EXPECT_TRUE(google_.requests.empty());
    EXPECT_FALSE(stored(repo::kGbpCredential));
}

TEST_F(Connect, AnAttemptStartedWithATokenStandsOnThatTokensSession) {
    // Every route takes the app's token (D-095). Whether the person who started is still signed in is asked of
    // the kind of session that started: a token's session here, with its own lifetime.
    const auto token = app_sign_in(staff("phone@xvisionmediaus.com"));
    auto started = start("gbp", token);
    clock_.advance(9min);
    google_accepts_the_code();
    google_refreshes();
    google_lists_one_account_with_two_locations();
    EXPECT_EQ(location(finish("gbp", started)), "http://localhost:5173/#/status?connected=gbp");
    EXPECT_TRUE(stored(repo::kGbpCredential));
    EXPECT_EQ(count("audit_log WHERE actor = 'phone@xvisionmediaus.com' AND action NOT LIKE 'session.%'"), 1) << "in that person's name";

    started = start("gbp", token);
    ASSERT_EQ(as_app(verb::post, "/api/auth/logout", token).status, 204u);
    const auto asked = google_.requests.size();
    EXPECT_EQ(location(finish("gbp", started)), "http://localhost:5173/#/status?account=gbp&connect_error=expired");
    EXPECT_EQ(google_.requests.size(), asked) << "the token was signed out meanwhile: Google is not asked";
}

// ---- the sending mailbox -------------------------------------------------------------------

TEST_F(Connect, TheMailboxIsTheAccountThatSignedInWithPermissionToSend) {
    EXPECT_FALSE(status().at("email").at("connected").as_bool());
    EXPECT_TRUE(status().at("email").at("account_email").is_null());

    // Without the box for gmail.send ticked.
    auto started = start("gmail");
    google_accepts_the_mailbox(kMailbox, "openid email");
    EXPECT_EQ(location(finish("gmail", started)), "http://localhost:5173/#/status?account=gmail&connect_error=no_send_permission");
    EXPECT_FALSE(stored(repo::kGmailCredential));

    // No ID token at all: who signed in is not known, so nothing is stored.
    started = start("gmail");
    google_accepts_the_code(R"(, "scope": "https://www.googleapis.com/auth/gmail.send")");
    EXPECT_EQ(location(finish("gmail", started)), "http://localhost:5173/#/status?account=gmail&connect_error=no_email");
    EXPECT_FALSE(stored(repo::kGmailCredential));

    // Any account with the permission: no setting names the mailbox (D-100). Its address is kept as Google wrote it.
    started = start("gmail");
    google_accepts_the_mailbox("Host@XVisionMediaUS.com");
    google_refreshes();
    EXPECT_EQ(location(finish("gmail", started)), "http://localhost:5173/#/status?connected=gmail");
    EXPECT_EQ(stored(repo::kGmailCredential)->refresh_token, kRefreshToken);
    EXPECT_FALSE(stored(repo::kGbpCredential)) << "the two accounts have a token each";
    EXPECT_TRUE(status().at("email").at("connected").as_bool());
    EXPECT_EQ(status().at("email").at("account_email").as_string(), "Host@XVisionMediaUS.com");
    EXPECT_EQ(count("audit_log WHERE action = 'credential.store' AND object_id = 'gmail' AND actor = 'staff@xvisionmediaus.com'"), 1);
    EXPECT_THAT(connected_, testing::ElementsAre("gmail"));
    EXPECT_THAT(value<std::string>("SELECT after::text FROM audit_log WHERE action = 'credential.store'"),
                HasSubstr("Host@XVisionMediaUS.com"));
    EXPECT_EQ(await(http::stored_mailbox_address(db())()).value(), "Host@XVisionMediaUS.com") << "email is sent from it";

    // Another mailbox replaces it.
    started = start("gmail");
    google_accepts_the_mailbox("alerts@xvisionus.com");
    google_refreshes();
    EXPECT_EQ(location(finish("gmail", started)), "http://localhost:5173/#/status?connected=gmail");
    EXPECT_EQ(status().at("email").at("account_email").as_string(), "alerts@xvisionus.com");
    EXPECT_EQ(await(http::stored_mailbox_address(db())()).value(), "alerts@xvisionus.com");

    // Connecting the mailbox switches no sending on (D-066): that stays a person's choice.
    EXPECT_FALSE(status().at("email").at("sending_enabled").as_bool());
    EXPECT_EQ(value<bool>("SELECT email_sending_enabled FROM system_settings"), false);
}

// A scope is one entry of Google's space-separated list, compared whole: one whose name holds the wanted one is another permission.
TEST_F(Connect, TheMailboxsPermissionIsComparedWhole) {
    const auto started = start("gmail");
    google_accepts_the_mailbox(kMailbox, "https://www.googleapis.com/auth/gmail.send.other openid email");
    EXPECT_EQ(location(finish("gmail", started)), "http://localhost:5173/#/status?account=gmail&connect_error=no_send_permission");
    EXPECT_FALSE(stored(repo::kGmailCredential));
    EXPECT_EQ(google_.requests.size(), 1u) << "only the exchange: the token was not tried";
    EXPECT_EQ(count("audit_log WHERE action = 'credential.store'"), 0);
}

TEST_F(Connect, NoMailIsSentFromAMailboxWhoseAddressIsNotKnown) {
    const auto address = http::stored_mailbox_address(db());
    EXPECT_THAT(await(address()).error(), HasSubstr("the sending mailbox is not connected"));
    // Connected before the engine kept the address: there is no setting to fall back on.
    tx([&](pqxx::work& tx) {
        repo::save_oauth_credentials(tx, box_, repo::kGmailCredential, {"client", "secret", "1//old"}, std::nullopt, clock_.now());
    });
    EXPECT_THAT(await(address()).error(), HasSubstr("connect it again"));
    EXPECT_TRUE(status().at("email").at("connected").as_bool());
    EXPECT_TRUE(status().at("email").at("account_email").is_null());
}

// ---- the Workspace administrator ----------------------------------------------------------

TEST_F(Connect, TheWorkspaceAccountIsAnAdministratorThatCanReadTheDirectory) {
    EXPECT_FALSE(status().at("workspace").at("connected").as_bool());
    const auto started = start("workspace");
    // Read-only access to the users, and nothing that can change them.
    EXPECT_EQ(started.param("scope"), kDirectoryScopes);
    EXPECT_EQ(started.param("prompt"), "select_account consent");
    EXPECT_EQ(started.param("include_granted_scopes"), "") << "a token carries what was asked for and nothing more";
    EXPECT_EQ(started.param("redirect_uri"), "http://localhost:5173/api/connections/workspace/callback");

    google_accepts_the_administrator();
    google_refreshes();
    google_.reply(200, directory_with(directory_user(kAdministratorId, "Directory-Admin@Example.com")));
    const auto response = finish("workspace", started);
    EXPECT_EQ(location(response), "http://localhost:5173/#/status?connected=workspace");

    // The exchange, the refresh, and one read: the GET of the list. The account is found by its id, not taken first.
    ASSERT_EQ(google_.requests.size(), 3u);
    EXPECT_THAT(sent(1), HasSubstr("grant_type=refresh_token"));
    const auto read = directory_requests();
    ASSERT_EQ(read.size(), 1u);
    EXPECT_EQ(read[0].method, http::ClientMethod::get);
    EXPECT_EQ(read[0].url.buffer(), kDirectoryList);

    const auto credentials = stored(repo::kWorkspaceCredential);
    ASSERT_TRUE(credentials);
    EXPECT_EQ(credentials->refresh_token, kRefreshToken);
    EXPECT_EQ(value<std::string>("SELECT account_email FROM credentials WHERE name = 'workspace'"), kAdministrator);
    const auto shown = status().at("workspace");
    EXPECT_TRUE(shown.at("connected").as_bool());
    EXPECT_EQ(shown.at("account_email").as_string(), kAdministrator);
    EXPECT_FALSE(shown.at("last_ok_at").is_null());
    EXPECT_TRUE(shown.at("last_error").is_null());
    EXPECT_THAT(connected_, testing::ElementsAre("workspace"));
    EXPECT_EQ(count("audit_log WHERE action = 'credential.store' AND object_type = 'credential' AND object_id = 'workspace' "
                    "AND actor = 'staff@xvisionmediaus.com'"),
              1);
    const auto after = value<std::string>("SELECT after::text FROM audit_log WHERE action = 'credential.store'");
    EXPECT_THAT(after, HasSubstr("\"source\": \"dashboard\""));
    EXPECT_THAT(after, HasSubstr(std::string(kAdministrator)));
    EXPECT_THAT(after, Not(HasSubstr(std::string(kRefreshToken))));
    EXPECT_THAT(after, Not(HasSubstr(std::string(kAdministratorId)))) << "the sub is stored nowhere";
    EXPECT_FALSE(stored(repo::kGbpCredential));
    EXPECT_FALSE(stored(repo::kGmailCredential));
}

TEST_F(Connect, ConnectingTheWorkspaceLeavesTheOtherTwoAlone) {
    auto started = start("gbp");
    google_accepts_the_code();
    google_refreshes();
    google_lists_one_account_with_two_locations();
    ASSERT_EQ(location(finish("gbp", started)), "http://localhost:5173/#/status?connected=gbp");
    started = start("gmail");
    google_accepts_the_mailbox(kMailbox);
    google_refreshes();
    ASSERT_EQ(location(finish("gmail", started)), "http://localhost:5173/#/status?connected=gmail");

    const auto row = [&](std::string_view table, std::string_view name) {
        return value<std::string>(std::format("SELECT row_to_json(t)::text FROM {} t WHERE name = '{}'", table, name));
    };
    const auto before = std::vector{row("credentials", "gbp"), row("credentials", "gmail"), row("integration_status", "gbp_token"),
                                    row("integration_status", "email")};
    clock_.advance(1h);

    started = start("workspace");
    google_accepts_the_administrator();
    google_refreshes();
    google_.reply(200, directory_with(directory_user(kAdministratorId, kAdministrator)));
    ASSERT_EQ(location(finish("workspace", started)), "http://localhost:5173/#/status?connected=workspace");
    EXPECT_EQ((std::vector{row("credentials", "gbp"), row("credentials", "gmail"), row("integration_status", "gbp_token"),
                           row("integration_status", "email")}),
              before);
    EXPECT_EQ(stored(repo::kGbpCredential)->refresh_token, kRefreshToken);
    EXPECT_THAT(connected_, testing::ElementsAre("gbp", "gmail", "workspace")) << "only the Workspace's cached token is let go";

    // And the other way round: connecting the managing account again leaves the Workspace's token as it is.
    const auto workspace = std::vector{row("credentials", "workspace"), row("integration_status", "workspace")};
    clock_.advance(1h);
    started = start("gbp");
    google_accepts_the_code();
    google_refreshes();
    google_lists_one_account_with_two_locations();
    ASSERT_EQ(location(finish("gbp", started)), "http://localhost:5173/#/status?connected=gbp");
    EXPECT_EQ((std::vector{row("credentials", "workspace"), row("integration_status", "workspace")}), workspace);
}

TEST_F(Connect, TheWorkspaceNeedsThePermissionToReadUsers) {
    // The person unticked it at Google, or a scope list holds only one whose name contains it.
    for (const auto scope : {"openid email", "https://www.googleapis.com/auth/admin.directory.user.readonly.more openid email"}) {
        SCOPED_TRACE(scope);
        const auto started = start("workspace");
        google_accepts_the_administrator(kAdministratorId, scope);
        EXPECT_EQ(location(finish("workspace", started)),
                  "http://localhost:5173/#/status?account=workspace&connect_error=no_directory_permission");
    }
    EXPECT_EQ(google_.requests.size(), 2u) << "only the exchanges: the token was not tried";
    EXPECT_FALSE(stored(repo::kWorkspaceCredential));
    EXPECT_FALSE(status().at("workspace").at("connected").as_bool());
    EXPECT_EQ(count("audit_log WHERE action = 'credential.store'"), 0);
}

TEST_F(Connect, TheWorkspaceAccountMustBeInTheList) {
    using Answer = std::optional<std::pair<unsigned, std::string>>;  // none: the request goes out and nothing comes back
    const auto refused = [&](std::string_view why, std::string_view sub, std::vector<Answer> answers) {
        SCOPED_TRACE(why);
        const auto started = start("workspace");
        google_accepts_the_administrator(sub);
        if (!answers.empty()) google_refreshes();
        for (const auto& answer : answers) {
            if (answer) google_.reply(answer->first, answer->second);
            else google_.fail();
        }
        EXPECT_EQ(location(finish("workspace", started)), "http://localhost:5173/#/status?account=workspace&connect_error=no_access");
    };
    const std::string denied = R"({"error": {"code": 403, "status": "PERMISSION_DENIED", "message": "Not Authorized to access this resource/api"}})";
    refused("no sub", "", {});
    refused("a sub that is not a plain id", "104.000/42", {});
    refused("the list answers 403", kAdministratorId, {Answer{{403, denied}}});
    refused("the list answers 404", kAdministratorId, {Answer{{404, R"({"error": {"code": 404, "message": "Not Found"}})"}}});
    refused("no entry with the id, though another entry has the address", kAdministratorId,
            {Answer{{200, directory_with(directory_user("104000000000000000099", kAdministrator))}}});
    refused("the entry has another address", kAdministratorId,
            {Answer{{200, directory_with(directory_user(kAdministratorId, "someone-else@example.com"))}}});
    refused("suspended", kAdministratorId,
            {Answer{{200, directory_with(directory_user(kAdministratorId, kAdministrator, R"(, "isDelegatedAdmin": true, "suspended": true)"))}}});
    refused("archived", kAdministratorId,
            {Answer{{200, directory_with(directory_user(kAdministratorId, kAdministrator, R"(, "isDelegatedAdmin": true, "archived": true)"))}}});
    refused("the second page fails after the account was on the first", kAdministratorId,
            {Answer{{200, directory_with(directory_user(kAdministratorId, kAdministrator), "made-up-page-2")}},
             Answer{{503, R"({"error": {"code": 503, "message": "Backend Error"}})"}}});
    refused("the read fails on the network", kAdministratorId, {Answer{}});

    EXPECT_FALSE(stored(repo::kWorkspaceCredential));
    EXPECT_FALSE(status().at("workspace").at("connected").as_bool());
    EXPECT_EQ(count("audit_log WHERE action = 'credential.store'"), 0);
    EXPECT_TRUE(connected_.empty());
    const auto read = directory_requests();
    EXPECT_EQ(read.size(), 9u) << "one read for each case that got that far, and the second page";
    for (const auto& request : read) {
        EXPECT_EQ(request.method, http::ClientMethod::get);
        EXPECT_THAT(std::string(request.url.buffer()), testing::StartsWith(std::string(kDirectoryList)));
    }
    EXPECT_TRUE(google_.refused.empty());
}

TEST_F(Connect, TheWorkspaceAccountMustBeAnAdministrator) {
    auto started = start("workspace");
    google_accepts_the_administrator();
    google_refreshes();
    google_.reply(200, directory_with(directory_user(kAdministratorId, kAdministrator, "")));
    EXPECT_EQ(location(finish("workspace", started)), "http://localhost:5173/#/status?account=workspace&connect_error=no_access");
    EXPECT_FALSE(stored(repo::kWorkspaceCredential));

    // An administrator with a role, and a super administrator: the token can only read either way.
    for (const auto flags : {R"(, "isDelegatedAdmin": true)", R"(, "isAdmin": true)"}) {
        SCOPED_TRACE(flags);
        started = start("workspace");
        google_accepts_the_administrator();
        google_refreshes();
        google_.reply(200, directory_with(directory_user(kAdministratorId, kAdministrator, flags)));
        EXPECT_EQ(location(finish("workspace", started)), "http://localhost:5173/#/status?connected=workspace");
    }
    EXPECT_TRUE(stored(repo::kWorkspaceCredential));
    EXPECT_EQ(count("audit_log WHERE action = 'credential.store' AND object_id = 'workspace'"), 2);
}

// A test tenant picked by mistake in Google's account chooser would pass every other check, and the engine would then
// find none of the staff in its directory.
TEST_F(Connect, AnAdministratorOfAnotherWorkspaceIsRefused) {
    const auto started = start("workspace");
    google_accepts_the_administrator(kAdministratorId, kDirectoryScopes, "admin@test-tenant.example.org");
    EXPECT_EQ(location(finish("workspace", started)), "http://localhost:5173/#/status?account=workspace&connect_error=no_access");
    EXPECT_EQ(google_.requests.size(), 1u) << "only the exchange";
    EXPECT_TRUE(directory_requests().empty()) << "the directory was not asked";
    EXPECT_FALSE(stored(repo::kWorkspaceCredential));
    EXPECT_EQ(count("audit_log WHERE action = 'credential.store'"), 0);
}

TEST_F(Connect, TheWorkspaceTokenIsLoadedUnderItsOwnName) {
    const auto none = await(http::stored_credentials(db(), &box_, http::kConnectWorkspace)());
    ASSERT_FALSE(none);
    EXPECT_EQ(none.error().kind, google::ApiError::Kind::not_configured);
    EXPECT_THAT(none.error().message, HasSubstr("no Workspace administrator is connected"));

    const auto started = start("workspace");
    google_accepts_the_administrator();
    google_refreshes();
    google_.reply(200, directory_with(directory_user(kAdministratorId, kAdministrator)));
    ASSERT_EQ(location(finish("workspace", started)), "http://localhost:5173/#/status?connected=workspace");

    const auto workspace = await(http::stored_credentials(db(), &box_, http::kConnectWorkspace)());
    ASSERT_TRUE(workspace) << workspace.error().message;
    EXPECT_EQ(workspace->refresh_token, kRefreshToken);
    const auto gbp = await(http::stored_credentials(db(), &box_, http::kConnectGbp)());
    ASSERT_FALSE(gbp) << "the Workspace's token is not the managing account's";
    EXPECT_THAT(gbp.error().message, HasSubstr("no GBP account is connected"));
    // Sealed under its own name: the same ciphertext opens under no other.
    tx([&](pqxx::work& tx) { tx.exec("UPDATE credentials SET name = 'gbp' WHERE name = 'workspace'"); });
    EXPECT_FALSE(await(http::stored_credentials(db(), &box_, http::kConnectGbp)()));
}

TEST_F(Connect, AgainstTheGoogleMockTheWorkspaceGoesToTheMocksConsentPage) {
    rebuild(true, true, "http://127.0.0.1:8090");
    const auto started = start("workspace");
    EXPECT_THAT(started.url, testing::StartsWith("http://127.0.0.1:8090/accounts.google.com/o/oauth2/v2/auth?"));
    EXPECT_EQ(started.param("scope"), kDirectoryScopes);
    EXPECT_EQ(started.param("redirect_uri"), "http://localhost:5173/api/connections/workspace/callback");
}

// CORE-3: what the engine's token sources load.
TEST_F(Connect, TheTokenSourcesLoadWhatWasConnected) {
    const auto load = [&](const core::SecretBox* box, std::string_view account) {
        return await(http::stored_credentials(db(), box, account)());
    };
    const auto none = load(&box_, http::kConnectGbp);
    ASSERT_FALSE(none);
    EXPECT_EQ(none.error().kind, google::ApiError::Kind::not_configured);
    EXPECT_THAT(none.error().message, HasSubstr("no GBP account is connected"));
    EXPECT_THAT(load(&box_, http::kConnectGmail).error().message, HasSubstr("the sending mailbox is not connected"));
    EXPECT_THAT(load(nullptr, http::kConnectGbp).error().message, HasSubstr("PONI_SECRET_KEY is not set"));

    tx([&](pqxx::work& tx) {
        repo::save_oauth_credentials(tx, box_, repo::kGbpCredential, {"client", "secret", std::string(kRefreshToken)}, std::nullopt,
                                     clock_.now());
    });
    const auto stored = load(&box_, http::kConnectGbp);
    ASSERT_TRUE(stored) << stored.error().message;
    EXPECT_EQ(stored->refresh_token, kRefreshToken);
    EXPECT_FALSE(load(&box_, http::kConnectGmail)) << "each account has its own token";
}

// A name the engine has no connection for never loads another account's token in its place.
TEST_F(Connect, AnUnknownConnectionLoadsNothing) {
    tx([&](pqxx::work& tx) {
        repo::save_oauth_credentials(tx, box_, repo::kGbpCredential, {"client", "secret", std::string(kRefreshToken)}, std::nullopt,
                                     clock_.now());
    });
    const auto loaded = await(http::stored_credentials(db(), &box_, "drive")());
    ASSERT_FALSE(loaded);
    EXPECT_EQ(loaded.error().kind, google::ApiError::Kind::not_configured);
    EXPECT_EQ(loaded.error().message, "no such connection");
    EXPECT_TRUE(await(http::stored_credentials(db(), &box_, http::kConnectGbp)())) << "the GBP token is there, and was not given";
}

TEST_F(Connect, TheStatusPageKnowsWhetherConnectingIsPossible) {
    api_options_.connect_configured = connect_->configured();
    rebuild(true, true);
    browser_ = sign_in();
    EXPECT_TRUE(status().at("connect_configured").as_bool());
    api_options_.connect_configured = false;
    rebuild(false, true);
    browser_ = sign_in();
    EXPECT_FALSE(status().at("connect_configured").as_bool());
}
