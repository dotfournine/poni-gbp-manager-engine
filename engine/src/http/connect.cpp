#include "http/connect.hpp"

#include "connectors/gbp/reader.hpp"
#include "connectors/google/auth.hpp"
#include "connectors/notify/gmail.hpp"
#include "core/log.hpp"
#include "db/audit.hpp"
#include "db/repo_auth.hpp"
#include "db/repo_notifications.hpp"
#include "http/auth.hpp"

#include <boost/json/object.hpp>
#include <boost/url/url.hpp>

#include "core/text.hpp"

#include <format>

namespace poni::http {

namespace asio = boost::asio;
using boost::beast::http::verb;
using namespace std::chrono_literals;

namespace {

// The state and the PKCE verifier come from the secret in this cookie (http/browser_flow.hpp).
constexpr std::string_view kFlowCookie = "poni_connect";
constexpr auto kAttemptLifetime = 600s;
constexpr std::size_t kMaxAttempts = 1000;
constexpr std::string_view kGbpScope = "https://www.googleapis.com/auth/business.manage";

std::string attempt_key(std::string_view secret) { return core::sha256_hex(std::format("poni-connect:attempt:{}", secret)); }

bool known_account(std::string_view account) { return account == kConnectGbp || account == kConnectGmail; }

// The "email" claim of the ID token that came with the code exchange; empty if there was none.
std::string email_claim(const std::optional<boost::json::object>& claims) {
    const auto* email = claims ? claims->if_contains("email") : nullptr;
    return email && email->is_string() ? std::string(email->get_string()) : std::string{};
}

// A token source that always refreshes with the given credentials: used to
// prove they work before anything is stored.
google::CredentialsLoader fixed(google::OAuthCredentials credentials) {
    return [credentials = std::move(credentials)]() -> asio::awaitable<google::Result<google::OAuthCredentials>> {
        co_return credentials;
    };
}

struct Seen {
    std::size_t accounts = 0;
    std::size_t locations = 0;
};

// Refreshes the token and lists what the account can see.
asio::awaitable<std::expected<Seen, std::string>> check_gbp(Client& http, const core::Clock& clock, core::RateLimiter& limiter,
                                                             google::OAuthCredentials credentials) {
    google::CachedTokenSource tokens(clock, google::refresh_fetch(http, limiter, fixed(std::move(credentials))));
    gbp::HttpReader reader({http, tokens, limiter});
    const auto accounts = co_await reader.list_accounts();
    if (!accounts) co_return std::unexpected(accounts.error().message);
    Seen seen{.accounts = accounts->size(), .locations = 0};
    for (const auto& account : *accounts) {
        const auto id = gbp::strip_prefix(account.name, "accounts/");
        if (!id) continue;
        const auto locations = co_await reader.list_locations(*id);
        if (!locations) co_return std::unexpected(locations.error().message);
        seen.locations += locations->size();
    }
    // An account that manages nothing would replace a working token with a useless one, and the status page
    // would still say "connected": a personal account picked by mistake looks exactly like this.
    if (seen.locations == 0) co_return std::unexpected(std::string("the account can see no Business Profile location"));
    co_return seen;
}

// Proves a refresh token for the sending mailbox works: Google hands out an access token for it.
asio::awaitable<std::expected<void, std::string>> check_gmail(Client& http, core::RateLimiter& limiter, google::OAuthCredentials credentials) {
    const auto fetch = google::refresh_fetch(http, limiter, fixed(std::move(credentials)));
    const auto token = co_await fetch();
    if (!token) co_return std::unexpected(token.error().message);
    co_return std::expected<void, std::string>{};
}

}  // namespace

google::CredentialsLoader stored_credentials(db::Database& db, const core::SecretBox* box, std::string_view account) {
    const bool gmail = account == kConnectGmail;
    return [&db, box, name = std::string(gmail ? repo::kGmailCredential : repo::kGbpCredential),
            missing = std::string(gmail ? "the sending mailbox is not connected; connect it on the dashboard's status page"
                                        : "no GBP account is connected; connect it on the dashboard's status page")]()
               -> asio::awaitable<google::Result<google::OAuthCredentials>> {
        if (!box) co_return std::unexpected(google::not_configured("PONI_SECRET_KEY is not set"));
        auto loaded = co_await db.tx([box, &name](pqxx::work& tx) { return repo::load_oauth_credentials(tx, *box, name); });
        if (!loaded) co_return std::unexpected(google::not_configured(loaded.error()));
        if (!*loaded) co_return std::unexpected(google::not_configured(missing));
        co_return std::move(**loaded);
    };
}

ConnectService::ConnectService(db::Database& db, const core::Clock& clock, Client& http, core::RateLimiter& limiter,
                               const core::Config& config, OnConnected on_connected)
    : db_(db),
      clock_(clock),
      http_(http),
      limiter_(limiter),
      config_(config),
      on_connected_(std::move(on_connected)),
      flow_("poni-connect", kFlowCookie, "/api/connections", served_over_https(config)) {}

void ConnectService::add_routes(Router& router) {
    router.add(verb::post, "/api/connections/{account}/start", {Access::user, "connections.start"},
               [this](const Request& request) { return start(request); });
    // Anonymous as far as the router goes: the session cookie is not sent with Google's redirect. Who it is
    // for is decided by the attempt the flow cookie belongs to.
    router.add(verb::get, "/api/connections/{account}/callback", {Access::anonymous, "connections.callback"},
               [this](const Request& request) { return callback(request); });
}

std::string ConnectService::redirect_uri(std::string_view account) const {
    return std::format("{}/api/connections/{}/callback", config_.public_url, account);
}

Response ConnectService::back_to_status(std::string_view account, std::string_view error) const {
    // The dashboard's status page shows a message for the code.
    auto response = Response::redirect(error.empty() ? std::format("{}/#/status?connected={}", config_.public_url, account)
                                                     : std::format("{}/#/status?account={}&connect_error={}", config_.public_url,
                                                                   account, error));
    response.headers.emplace_back("Set-Cookie", flow_.cookie("", 0s));
    return response;
}

asio::awaitable<Response> ConnectService::start(const Request& request) {
    const auto found = request.params.find("account");
    if (found == request.params.end() || !known_account(found->second)) co_return Response::error(404, "not_found", "No such account to connect.");
    const auto& account = found->second;
    if (config_.gbp_oauth_client_id.empty() || config_.gbp_oauth_client_secret.empty()) {
        co_return Response::error(409, "not_configured",
                                  "The engine has no Google OAuth client to connect an account with. Set PONI_GBP_OAUTH_CLIENT_ID and "
                                  "PONI_GBP_OAUTH_CLIENT_SECRET in its configuration.");
    }
    if (!config_.secret_box) {
        co_return Response::error(409, "not_configured",
                                  "The engine has no key to encrypt the token with. Set PONI_SECRET_KEY in its configuration.");
    }
    if (account == kConnectGmail && config_.email_from.empty()) {
        co_return Response::error(409, "not_configured",
                                  "The engine has no mailbox to send notifications from. Set PONI_EMAIL_FROM in its configuration.");
    }

    const auto now = clock_.now();
    std::erase_if(attempts_, [&](const auto& item) { return item.second.expires <= now; });
    if (attempts_.size() >= kMaxAttempts) attempts_.clear();  // only signed-in staff can start one; this is a bound, not a defence

    const auto secret = core::random_token();
    attempts_.insert_or_assign(attempt_key(secret),
                               Attempt{.account = account, .started_by = *request.principal, .expires = now + kAttemptLifetime});

    boost::urls::url url("https://accounts.google.com/o/oauth2/v2/auth");
    auto params = url.params();
    params.append({"client_id", config_.gbp_oauth_client_id});
    params.append({"redirect_uri", redirect_uri(account)});
    params.append({"response_type", "code"});
    if (account == kConnectGmail) {
        // openid and email only to learn which mailbox signed in; the token is used for gmail.send alone.
        params.append({"scope", std::format("{} openid email", notify::kGmailSendScope)});
        params.append({"login_hint", config_.email_from});
    } else {
        params.append({"scope", kGbpScope});
    }
    // offline + consent: Google only returns a refresh token on a fresh consent.
    params.append({"access_type", "offline"});
    params.append({"prompt", "consent"});
    params.append({"state", flow_.derive("state", secret)});
    params.append({"code_challenge", flow_.code_challenge(secret)});
    params.append({"code_challenge_method", "S256"});

    if (config_.google_mock()) {
        // The mock's consent page approves at once and sends the browser back to the same callback.
        if (auto mocked = google_mock_address(boost::urls::url(config_.google_mock_url), url)) url = std::move(*mocked);
    }
    auto response = Response::json(200, boost::json::object{{"authorization_url", std::string_view(url.buffer())}});
    response.headers.emplace_back("Set-Cookie", flow_.cookie(secret, kAttemptLifetime));
    co_return response;
}

asio::awaitable<Response> ConnectService::callback(const Request& request) {
    const auto found = request.params.find("account");
    if (found == request.params.end() || !known_account(found->second)) co_return Response::error(404, "not_found", "No such account to connect.");
    const std::string account = found->second;

    const auto secret = flow_.secret(request);
    if (!secret) co_return back_to_status(account, "expired");
    // Used once, whatever comes of it: the code Google sent is too.
    const auto entry = attempts_.find(attempt_key(*secret));
    if (entry == attempts_.end()) co_return back_to_status(account, "expired");
    const Attempt attempt = std::move(entry->second);
    attempts_.erase(entry);
    if (attempt.expires <= clock_.now() || attempt.account != account) co_return back_to_status(account, "expired");

    const auto code = flow_.returned_code(request, *secret);
    if (!code) co_return back_to_status(account, code.error());

    // The person who started must still be signed in: signing out, or being disabled, ends what they began.
    const bool still_signed_in = co_await db_.tx(
        [hash = attempt.started_by.session_token_hash, kind = attempt.started_by.kind, now = clock_.now(),
         lifetimes = session_lifetimes(config_)](pqxx::work& tx) { return repo::find_session(tx, hash, kind, now, lifetimes).has_value(); });
    if (!still_signed_in) co_return back_to_status(account, "expired");

    std::optional<std::string> error;
    try {
        error = co_await connect(attempt, *code, flow_.verifier(*secret), request.remote_ip);
    } catch (const std::exception& e) {
        log::warn("connecting a Google account failed", {{"account", account}, {"reason", e.what()}});
        error = "failed";
    }
    co_return back_to_status(account, error.value_or(""));
}

asio::awaitable<std::optional<std::string>> ConnectService::connect(const Attempt& attempt, std::string code, std::string verifier,
                                                                    std::string ip) {
    const bool gmail = attempt.account == kConnectGmail;
    const auto refuse = [&](std::string_view error, std::string_view reason) {
        log::warn("a Google account was not connected", {{"account", attempt.account}, {"error", error}, {"reason", reason}});
        return std::optional<std::string>(std::string(error));
    };
    const auto& client_id = config_.gbp_oauth_client_id;
    const auto& client_secret = config_.gbp_oauth_client_secret;
    const auto exchanged = co_await google::exchange_code(http_, limiter_, google::kOAuthApi, client_id, client_secret, std::move(code),
                                                          redirect_uri(attempt.account), std::move(verifier));
    if (!exchanged) co_return refuse("refused", exchanged.error().message);
    const auto& tokens = exchanged->tokens;
    if (!tokens.refresh_token || tokens.refresh_token->empty()) co_return refuse("no_refresh_token", "Google returned no refresh token");

    const google::OAuthCredentials credentials{client_id, client_secret, *tokens.refresh_token};
    boost::json::object stored{{"source", "dashboard"}};
    if (gmail) {
        // The wrong mailbox would send every alert from somebody's own address.
        const auto signed_in = email_claim(exchanged->id_token_claims);
        if (signed_in.empty() || !text::ascii_iequals(signed_in, config_.email_from)) {
            co_return refuse("wrong_mailbox", "the account that signed in is not the configured sender");
        }
        if (!tokens.scope || !tokens.scope->contains(notify::kGmailSendScope)) {
            co_return refuse("no_send_permission", "the gmail.send scope was not granted");
        }
        const auto works = co_await check_gmail(http_, limiter_, credentials);
        if (!works) co_return refuse("refused", works.error());
        stored["mailbox"] = config_.email_from;
    } else {
        const auto seen = co_await check_gbp(http_, clock_, limiter_, credentials);
        if (!seen) co_return refuse("no_access", seen.error());
        stored["accounts"] = seen->accounts;
        stored["locations"] = seen->locations;
    }

    const std::string name(gmail ? repo::kGmailCredential : repo::kGbpCredential);
    const std::string status(gmail ? repo::kEmailStatus : repo::kGbpTokenStatus);
    const auto who = audit::Actor::user(attempt.started_by.user_id, attempt.started_by.email, std::move(ip));
    co_await db_.audited(who, clock_.now(), [this, name, status, credentials, stored = std::move(stored)](pqxx::work& tx, audit::Writer& audit) {
        repo::save_oauth_credentials(tx, *config_.secret_box, name, credentials, audit.now());
        repo::mark_integration_ok(tx, status, audit.now());
        // What was connected and what it can see; never the token.
        audit.record({.action = "credential.store", .object_type = "credential", .object_id = name, .after = stored});
    });
    if (on_connected_) on_connected_(attempt.account);
    log::info("a Google account was connected", {{"account", attempt.account}, {"by", attempt.started_by.email}});
    co_return std::nullopt;
}

}  // namespace poni::http
