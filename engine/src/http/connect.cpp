#include "http/connect.hpp"

#include "connectors/gbp/reader.hpp"
#include "connectors/google/auth.hpp"
#include "connectors/notify/gmail.hpp"
#include "connectors/workspace/directory.hpp"
#include "core/log.hpp"
#include "core/text.hpp"
#include "db/audit.hpp"
#include "db/repo_auth.hpp"
#include "db/repo_notifications.hpp"
#include "http/auth.hpp"

#include <boost/json/object.hpp>
#include <boost/url/url.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <ranges>
#include <stdexcept>

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
// Read-only: the engine never changes anything in a Workspace (D-106 point 3).
constexpr std::string_view kDirectoryScope = "https://www.googleapis.com/auth/admin.directory.user.readonly";

std::string attempt_key(std::string_view secret) { return core::sha256_hex(std::format("poni-connect:attempt:{}", secret)); }

constexpr std::string_view kNoMailbox = "the sending mailbox is not connected; connect it on the dashboard's status page";

// One row per account that can be connected: what its sign-in asks Google for, the name its token is
// stored under, the integration status its connection marks, and what its token source says while
// nothing is connected. Every decision by account reads its row, so a name that has none is refused
// and never falls through to another account's token.
struct Account {
    std::string_view name, scope, credential, status, missing;
};
constexpr std::array<Account, 3> kAccounts{{
    {kConnectGbp, kGbpScope, repo::kGbpCredential, repo::kGbpTokenStatus,
     "no GBP account is connected; connect it on the dashboard's status page"},
    {kConnectGmail, notify::kGmailSendScope, repo::kGmailCredential, repo::kEmailStatus, kNoMailbox},
    {kConnectWorkspace, kDirectoryScope, repo::kWorkspaceCredential, repo::kWorkspaceStatus,
     "no Workspace administrator is connected; connect one on the dashboard's status page"},
}};

// The row of `name`, or null.
const Account* account_named(std::string_view name) {
    const auto found = std::ranges::find(kAccounts, name, &Account::name);
    return found == kAccounts.end() ? nullptr : &*found;
}

bool known_account(std::string_view account) { return account_named(account) != nullptr; }

// Whether `scope` is one of the space-separated entries of `scopes`, compared whole: a scope whose
// name merely contains it (".../gmail.send.other") is another permission.
bool granted(std::string_view scopes, std::string_view scope) {
    return std::ranges::any_of(scopes | std::views::split(' '),
                               [&](const auto entry) { return std::string_view(entry.begin(), entry.end()) == scope; });
}

// The address of the account that signed in: the "email" claim of the ID token that came with the
// engine's own code exchange (D-091 point 12), if Google says it is verified; empty otherwise.
std::string verified_email(const std::optional<boost::json::object>& claims) {
    if (!claims) return {};
    const auto* email = claims->if_contains("email");
    const auto* verified = claims->if_contains("email_verified");  // a boolean, as sign-in reads it
    if (!email || !email->is_string() || !verified || !verified->is_bool() || !verified->get_bool()) return {};
    return std::string(email->get_string());
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

// Reads the directory's list with the new token and finds the account in it under `sub`, the id its own
// sign-in carries (D-106 point 2): that shows the API is enabled, that a Directory id is a sign-in's `sub`
// here, that the list the staff check reads holds the account, and that it is an administrator. The rest
// of the list is neither looked at nor kept.
asio::awaitable<std::expected<void, std::string>> check_workspace(Client& http, const core::Clock& clock, core::RateLimiter& limiter,
                                                                  google::OAuthCredentials credentials, std::string sub,
                                                                  std::string account_email) {
    google::CachedTokenSource tokens(clock, google::refresh_fetch(http, limiter, fixed(std::move(credentials))));
    workspace::HttpDirectory directory({http, tokens, limiter});
    const auto users = co_await google::guarded(directory.list_users());
    if (!users) co_return std::unexpected(users.error().message);
    const auto entry = std::ranges::find(*users, sub, &workspace::DirectoryUser::id);
    if (entry == users->end()) co_return std::unexpected(std::string("the directory's list does not hold the account"));
    if (text::ascii_lower(entry->primary_email) != text::ascii_lower(account_email)) {
        co_return std::unexpected(std::string("the directory's list names another address for the account"));
    }
    if (entry->suspended) co_return std::unexpected(std::string("the account is suspended in the directory"));
    if (entry->archived) co_return std::unexpected(std::string("the account is archived in the directory"));
    if (!entry->is_admin && !entry->is_delegated_admin) co_return std::unexpected(std::string("the account is no administrator"));
    co_return std::expected<void, std::string>{};
}

}  // namespace

google::CredentialsLoader stored_credentials(db::Database& db, const core::SecretBox* box, std::string_view account) {
    const Account* row = account_named(account);
    if (!row) {
        // A programming error: never another account's token in its place.
        return []() -> asio::awaitable<google::Result<google::OAuthCredentials>> {
            co_return std::unexpected(google::not_configured("no such connection"));
        };
    }
    // The row is a constant: it outlives the loader.
    return [&db, box, row]() -> asio::awaitable<google::Result<google::OAuthCredentials>> {
        if (!box) co_return std::unexpected(google::not_configured("PONI_SECRET_KEY is not set"));
        auto loaded = co_await db.tx([box, name = row->credential](pqxx::work& tx) { return repo::load_oauth_credentials(tx, *box, name); });
        if (!loaded) co_return std::unexpected(google::not_configured(loaded.error()));
        if (!*loaded) co_return std::unexpected(google::not_configured(std::string(row->missing)));
        co_return std::move(**loaded);
    };
}

notify::MailboxAddress stored_mailbox_address(db::Database& db) {
    return [&db]() -> asio::awaitable<std::expected<std::string, std::string>> {
        auto stored = co_await db.tx([](pqxx::work& tx) { return repo::load_credential(tx, repo::kGmailCredential); });
        if (!stored) co_return std::unexpected(std::string(kNoMailbox));
        if (!stored->account_email) {
            co_return std::unexpected(std::string(
                "the sending mailbox was connected before the engine kept its address; connect it again on the dashboard's status page"));
        }
        co_return std::move(*stored->account_email);
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
    // openid and email only to learn which account signed in (D-100); the token is used for its own scope alone.
    params.append({"scope", std::format("{} openid email", account_named(account)->scope)});
    // offline + consent: Google only returns a refresh token on a fresh consent. The chooser, always: the
    // account connected is the one picked there, also when the browser is signed in to another.
    params.append({"access_type", "offline"});
    params.append({"prompt", "select_account consent"});
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
    // start and callback refuse a name without a row: an attempt is only ever for one of them.
    const Account* account = account_named(attempt.account);
    if (!account) throw std::logic_error("an attempt to connect an account the engine does not know");
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

    // Which account this is, as Google says: the status page shows it, and email is sent from it (D-100).
    auto account_email = verified_email(exchanged->id_token_claims);
    if (account_email.empty()) co_return refuse("no_email", "Google named no verified address for the account that signed in");

    const google::OAuthCredentials credentials{client_id, client_secret, *tokens.refresh_token};
    boost::json::object stored{{"source", "dashboard"}, {"account_email", account_email}};
    // Each account's own check. No branch is a default: a row without a check stores nothing.
    if (account->name == kConnectGmail) {
        if (!tokens.scope || !granted(*tokens.scope, notify::kGmailSendScope)) {
            co_return refuse("no_send_permission", "the gmail.send scope was not granted");
        }
        const auto works = co_await check_gmail(http_, limiter_, credentials);
        if (!works) co_return refuse("refused", works.error());
    } else if (account->name == kConnectGbp) {
        const auto seen = co_await check_gbp(http_, clock_, limiter_, credentials);
        if (!seen) co_return refuse("no_access", seen.error());
        stored["accounts"] = seen->accounts;
        stored["locations"] = seen->locations;
    } else if (account->name == kConnectWorkspace) {
        if (!tokens.scope || !granted(*tokens.scope, kDirectoryScope)) {
            co_return refuse("no_directory_permission", "the admin.directory.user.readonly scope was not granted");
        }
        // An administrator of another Workspace (a test tenant picked in the chooser) would pass every other
        // check, and the engine would then find none of its users.
        if (!address_in_domains(account_email, config_.signin_domains)) {
            co_return refuse("no_access", "the account is not of the sign-in domains");
        }
        // The id the directory knows the account by, from the ID token of the engine's own exchange (D-091 point 12). Logged nowhere.
        std::string sub;
        const auto* claim = exchanged->id_token_claims ? exchanged->id_token_claims->if_contains("sub") : nullptr;
        if (claim && claim->is_string()) sub = std::string(claim->get_string());
        if (sub.empty() || !is_plain_id(sub)) co_return refuse("no_access", "Google named no id for the account");
        const auto admin = co_await check_workspace(http_, clock_, limiter_, credentials, std::move(sub), account_email);
        if (!admin) co_return refuse("no_access", admin.error());
    } else {
        throw std::logic_error("no check for the account being connected");
    }

    const std::string name(account->credential);
    const std::string status(account->status);
    const auto who = audit::Actor::user(attempt.started_by.user_id, attempt.started_by.email, std::move(ip));
    co_await db_.audited(who, clock_.now(), [this, name, status, credentials, account_email = std::move(account_email),
                                             stored = std::move(stored)](pqxx::work& tx, audit::Writer& audit) {
        repo::save_oauth_credentials(tx, *config_.secret_box, name, credentials, account_email, audit.now());
        repo::mark_integration_ok(tx, status, audit.now());
        // What was connected and what it can see; never the token.
        audit.record({.action = "credential.store", .object_type = "credential", .object_id = name, .after = stored});
    });
    if (on_connected_) on_connected_(attempt.account);
    log::info("a Google account was connected", {{"account", attempt.account}, {"by", attempt.started_by.email}});
    co_return std::nullopt;
}

}  // namespace poni::http
