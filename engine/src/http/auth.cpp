#include "http/auth.hpp"

#include "core/crypto.hpp"
#include "core/log.hpp"
#include "db/audit.hpp"
#include "db/repo_auth.hpp"
#include "http/api_support.hpp"

#include <boost/describe/class.hpp>
#include <boost/json/value_from.hpp>

#include "core/text.hpp"

#include <algorithm>
#include <exception>
#include <format>

namespace poni::http {

namespace asio = boost::asio;
using boost::beast::http::verb;
using domain::SessionKind;
using namespace std::chrono_literals;

namespace {

// Where a sign-in the phone app started ends (D-095): the app's own address.
// A constant: nothing of it is taken from a request, so no request can have
// a code sent anywhere else.
constexpr std::string_view kAppAddress = "gbpmanager://auth";
// The one value of `client` there is.
constexpr std::string_view kAppClient = "ios";
// How long the app has to exchange its one-time code.
constexpr auto kAppCodeLifetime = 60s;

// The body of POST /api/auth/app-token.
struct AppTokenRequest {
    std::string code;
    std::string code_verifier;
};
BOOST_DESCRIBE_STRUCT(AppTokenRequest, (), (code, code_verifier))

// State, nonce and PKCE verifier all come from the secret in this cookie, so
// the sign-in flow needs no server-side storage (http/browser_flow.hpp).
constexpr std::string_view kFlowCookie = "poni_signin";
constexpr auto kFlowLifetime = 600s;
constexpr std::size_t kMaxThrottleEntries = 10'000;

Response too_many_attempts(std::chrono::seconds wait) {
    auto response = Response::error(429, "too_many_attempts", "Too many sign-in attempts. Try again later.");
    response.headers.emplace_back("Retry-After", std::to_string(wait.count()));
    return response;
}

Principal to_principal(const model::User& user, std::string token_hash, SessionKind kind, std::optional<std::string> csrf_token) {
    return {.user_id = user.id,
            .email = user.email,
            .name = user.name,
            .picture_url = user.picture_url,
            .session_token_hash = std::move(token_hash),
            .kind = kind,
            .csrf_token = std::move(csrf_token)};
}

// The principal of a session that was just written.
Principal to_principal(const model::User& user, const repo::NewSession& row) {
    return to_principal(user, row.token_hash, row.kind, row.csrf_token);
}

bool base64url_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

// An S256 challenge as the app must send one: 43 characters of base64url.
bool is_code_challenge(std::string_view text) { return text.size() == 43 && std::ranges::all_of(text, base64url_char); }

// A PKCE verifier as RFC 7636 wants one: 43 to 128 unreserved characters. A
// shorter one could be found by trying, from the challenge that travelled in
// the start's address.
bool is_code_verifier(std::string_view text) {
    return text.size() >= 43 && text.size() <= 128 &&
           std::ranges::all_of(text, [](char c) { return base64url_char(c) || c == '.' || c == '~'; });
}

// Who is signing in, from the `client` parameter: the dashboard (none) or the
// phone app ("ios"). Nothing for any other value.
std::optional<SessionKind> client_kind(const QueryParams& query) {
    const auto client = query.get("client");
    if (!client) return SessionKind::browser;
    if (*client == kAppClient) return SessionKind::app;
    return std::nullopt;
}

Response unknown_client() { return api_support::invalid("client must be ios, or be left out."); }

// The token of an "Authorization: Bearer <token>" header, which may be empty.
// Nothing without the header, and nothing for another scheme: that is not the
// app's (a proxy's Basic, say), and the request is judged by its cookie. The
// scheme ends at a space or a tab, so that nothing that starts with the word
// Bearer and a blank is taken for another scheme.
std::optional<std::string_view> bearer_token(std::string_view header) {
    constexpr std::string_view scheme = "bearer";
    const auto blank = [](char c) { return c == ' ' || c == '\t'; };
    if (header.size() < scheme.size() || !text::ascii_iequals(header.substr(0, scheme.size()), scheme)) return std::nullopt;
    header.remove_prefix(scheme.size());
    if (!header.empty() && !blank(header.front())) return std::nullopt;
    while (!header.empty() && blank(header.front())) header.remove_prefix(1);
    return header;
}

// A session about to be written: its token, which only the one who signs in
// is given, and its row.
struct PreparedSession {
    std::string token;
    repo::NewSession row;
};

PreparedSession prepare_session(const domain::SessionLifetime& lifetime, core::TimePoint now, SessionKind kind, const Request& request) {
    auto token = core::random_token();
    auto hash = core::sha256_hex(token);
    return {.token = std::move(token),
            .row = {.token_hash = std::move(hash),
                    .kind = kind,
                    .csrf_token = kind == SessionKind::app ? std::nullopt : std::optional(core::random_token()),
                    .expires_at = now + lifetime.total,
                    .user_agent = std::string(request.header("user-agent").substr(0, 300)),
                    .ip = request.remote_ip}};
}

// Writes the session and the audit entry of the sign-in.
void open_session(pqxx::transaction_base& tx, const model::User& user, bool first_sign_in, const repo::NewSession& row,
                  core::TimePoint now) {
    repo::create_session(tx, user.id, row, now);
    audit::record(tx, audit::Actor::user(user.id, user.email, row.ip),
                  {.action = "session.login",
                   .object_type = "user",
                   .object_id = std::to_string(user.id),
                   .after = {{"email", user.email}, {"first_sign_in", first_sign_in}, {"kind", domain::name(row.kind)}}},
                  now);
}

audit::Entry refusal(std::string_view email, std::string_view reason) {
    return {.action = "session.login_refused", .object_type = "user", .after = {{"email", email}, {"reason", reason}}};
}

repo::NewUser new_user_of(const identity::Identity& identity) {
    return {identity.subject, identity.email, identity.name, identity.picture_url};
}

}  // namespace

// Only a verified address of a Workspace account, with both the organization
// (the "hd" claim) and the address's own domain among the allowed ones. "hd"
// is what says the account belongs to an organization; the email suffix alone
// would also match a consumer account with a look-alike address. The two may
// be different allowed domains: an organization can only hold addresses of
// domains it has proven to own, and Google may name the organization's main
// domain for an address on one of its other domains.
bool domain_allowed(const identity::Identity& identity, std::span<const std::string> domains) {
    const auto allowed = [&](std::string_view domain) {
        return !domain.empty() && std::ranges::any_of(domains, [&](const std::string& d) { return text::ascii_iequals(d, domain); });
    };
    if (!identity.email_verified || !identity.hosted_domain || !allowed(*identity.hosted_domain)) return false;
    const auto at = identity.email.rfind('@');
    return at != std::string::npos && allowed(std::string_view(identity.email).substr(at + 1));
}

std::optional<std::chrono::seconds> LoginThrottle::attempt(std::string_view client) {
    const auto now = clock_.now();
    if (entries_.size() >= kMaxThrottleEntries) {
        std::erase_if(entries_, [&](const auto& item) {
            return now >= item.second.locked_until && now - item.second.window_start >= options_.window;
        });
    }
    auto it = entries_.find(client);
    if (it == entries_.end()) it = entries_.emplace(std::string(client), Entry{now, 0, {}}).first;
    auto& entry = it->second;

    if (now < entry.locked_until) {
        return std::chrono::ceil<std::chrono::seconds>(entry.locked_until - now);
    }
    if (now - entry.window_start >= options_.window) {
        entry.window_start = now;
        entry.attempts = 0;
    }
    if (++entry.attempts > options_.max_attempts) {
        entry.locked_until = now + options_.lockout;
        return options_.lockout;
    }
    return std::nullopt;
}

AuthService::AuthService(db::Database& db, const core::Clock& clock, identity::Provider* provider, const core::Config& config)
    : db_(db),
      clock_(clock),
      provider_(provider),
      config_(config),
      lifetimes_(session_lifetimes(config)),
      flow_("poni-signin", kFlowCookie, "/api/auth", served_over_https(config)),
      throttle_(clock),
      exchange_throttle_(clock) {}

AuthHook AuthService::hook() {
    return [this](Request& request, const RouteInfo& route) { return authorize(request, route); };
}

std::string AuthService::session_cookie_name() const { return served_over_https(config_) ? "__Host-poni_session" : "poni_session"; }

std::string AuthService::session_cookie(std::string_view token, std::chrono::seconds max_age) const {
    return std::format("{}={}; Path=/; Max-Age={}; HttpOnly; SameSite=Strict{}", session_cookie_name(), token,
                       max_age.count(), served_over_https(config_) ? "; Secure" : "");
}

Response AuthService::login_error(std::string_view code, SessionKind kind) const {
    // The dashboard's sign-in page shows a message for the code; the app gets the same codes, and
    // too_many_attempts, which the dashboard gets as a 429.
    auto response = Response::redirect(kind == SessionKind::app ? std::format("{}?error={}", kAppAddress, code)
                                                                : std::format("{}/#/login?error={}", config_.public_url, code));
    response.headers.emplace_back("Set-Cookie", flow_.cookie("", 0s));
    return response;
}

Response AuthService::app_token_answer(SignedIn signed_in) const {
    // A time as the API writes one: the form the database's times leave it in (db::iso).
    return api_support::ok(model::AppToken{.token = std::move(signed_in.token),
                                           .expires_at = std::format("{:%FT%T}Z", signed_in.expires_at),
                                           .session = describe(signed_in.principal)});
}

model::Session AuthService::describe(const std::optional<Principal>& principal) const {
    model::Session session;
    session.sign_in = {.google = provider_ != nullptr,
                       .dev = !config_.dev_login_email.empty(),
                       .domains = config_.signin_domains};
    session.google_mock = config_.google_mock();
    if (principal) {
        session.authenticated = true;
        session.user = model::User{principal->user_id, principal->email, principal->name, principal->picture_url};
        session.csrf_token = principal->csrf_token;
    }
    return session;
}

asio::awaitable<std::optional<Response>> AuthService::authorize(Request& request, const RouteInfo& route) {
    // A request with a bearer token is the phone app's (D-095) and stands on
    // that token alone: a cookie it also carries is not looked at, so a token
    // that is wrong or has ended is never made good by a cookie. The token is
    // read from this header and from nowhere else. Each kind of session is
    // looked up as its own kind: a token sent as the cookie, or a cookie's
    // value sent as a token, finds nothing.
    auto kind = SessionKind::browser;
    std::optional<std::string> token;
    if (const auto bearer = bearer_token(request.header("authorization"))) {
        kind = SessionKind::app;
        token = std::string(*bearer);
    } else {
        token = cookie_value(request.header("cookie"), session_cookie_name());
    }
    if (token && !token->empty()) {
        auto hash = core::sha256_hex(*token);
        auto found = co_await db_.tx([hash, kind, now = clock_.now(), lifetimes = lifetimes_](pqxx::work& tx) {
            return repo::find_session(tx, hash, kind, now, lifetimes);
        });
        if (found) request.principal = to_principal(found->user, std::move(hash), kind, std::move(found->csrf_token));
    }
    if (route.access == Access::anonymous) co_return std::nullopt;
    if (!request.principal) co_return Response::error(401, "unauthenticated", "Sign in to continue.");

    if (kind == SessionKind::browser && request.method != verb::get && request.method != verb::head) {
        // Changing requests with the cookie must come from the dashboard
        // itself: the session cookie is SameSite=Strict, the Origin must be
        // ours when the browser sends one, and the request must carry the
        // session's CSRF token, which another site can neither read nor send.
        // Both guard against a browser that attaches the cookie to a request
        // another site made. Nothing attaches a bearer token by itself, so a
        // request with one gets neither check.
        if (const auto origin = request.header("origin"); !origin.empty() && origin != config_.public_url) {
            co_return Response::error(403, "forbidden_origin", "This request came from another site.");
        }
        const auto& csrf_token = request.principal->csrf_token;
        if (!csrf_token || !core::constant_time_equal(request.header("x-csrf-token"), *csrf_token)) {
            co_return Response::error(403, "csrf_token_invalid", "The request is missing a valid X-CSRF-Token header.");
        }
    }
    // One role today (D-007): a signed-in user may do everything. Roles will
    // be decided here, from request.principal and route.action.
    co_return std::nullopt;
}

asio::awaitable<Response> AuthService::session(const Request& request) {
    co_return Response::json(200, boost::json::value_from(describe(request.principal)));
}

asio::awaitable<Response> AuthService::google_start(const Request& request) {
    // Every request is counted, whatever it says, before anything else is
    // decided; whose it is only decides how a refusal is answered.
    const auto wait = throttle_.attempt(request.remote_ip);
    const QueryParams query(request.query);
    const auto kind = client_kind(query);
    // The app's sheet shows a page as it is: the app is sent its own code instead of the 429's JSON.
    if (wait) co_return kind == SessionKind::app ? login_error("too_many_attempts", SessionKind::app) : too_many_attempts(*wait);
    if (!kind) co_return unknown_client();
    // The app's own PKCE challenge, for the exchange of its one-time code. It is no secret.
    std::string app_code_challenge;
    if (*kind == SessionKind::app) {
        app_code_challenge = query.get("code_challenge").value_or("");
        if (!is_code_challenge(app_code_challenge)) {
            co_return api_support::invalid("code_challenge must be the S256 challenge of a PKCE verifier: 43 characters of base64url.");
        }
    }
    if (!provider_) co_return login_error("not_configured", *kind);

    // The app's challenge is bound to the attempt: the callback gets it back as it is here, or fails.
    auto secret = core::random_token();
    if (*kind == SessionKind::app) secret = BrowserFlow::with_note(secret, app_code_challenge);
    auto response = Response::redirect(provider_->authorization_url({
        .redirect_uri = config_.public_url + "/api/auth/google/callback",
        .state = flow_.derive("state", secret),
        .nonce = flow_.derive("nonce", secret),
        .code_challenge = flow_.code_challenge(secret),
    }));
    response.headers.emplace_back("Set-Cookie", flow_.cookie(secret, kFlowLifetime));
    co_return response;
}

asio::awaitable<Response> AuthService::google_callback(const Request& request) {
    const auto& ip = request.remote_ip;
    // Counted first, as at the start, whatever the cookie says.
    const auto wait = throttle_.attempt(ip);
    // Whose attempt it is, the dashboard's or the app's, is in the attempt's
    // cookie and nowhere else. Without the cookie it is nobody's, and the
    // answer goes to the dashboard. Every check below is the same for both.
    const auto secret = flow_.secret(request);
    const auto challenge = secret ? BrowserFlow::note(*secret) : std::nullopt;
    const auto kind = challenge ? SessionKind::app : SessionKind::browser;
    if (wait) co_return kind == SessionKind::app ? login_error("too_many_attempts", kind) : too_many_attempts(*wait);
    if (!provider_) co_return login_error("not_configured", kind);

    if (!secret) co_return login_error("expired", kind);
    if (challenge && !is_code_challenge(*challenge)) co_return login_error("failed", kind);  // not a cookie the start set
    const auto code = flow_.returned_code(request, *secret);
    if (!code) co_return login_error(code.error(), kind);

    std::expected<identity::Identity, std::string> identity = std::unexpected(std::string("not attempted"));
    try {
        identity = co_await provider_->redeem(*code, flow_.verifier(*secret), config_.public_url + "/api/auth/google/callback",
                                              flow_.derive("nonce", *secret));
    } catch (const std::exception& e) {
        identity = std::unexpected(std::string(e.what()));
    }
    if (!identity) {
        log::warn("sign-in failed", {{"reason", identity.error()}});
        co_return login_error("failed", kind);
    }
    if (!domain_allowed(*identity, config_.signin_domains)) {
        co_await audit_refusal(identity->email, "not an account of an allowed domain", ip);
        co_return login_error("domain", kind);
    }
    if (challenge) {
        // The app's attempt ends in no session and no cookie here: the sheet
        // goes back to the app with a code, and the app proves with its
        // verifier that it is the one that started (app_token).
        std::string code_challenge(*challenge);
        const auto app_code = co_await issue_app_code(*identity, std::move(code_challenge), ip);
        if (!app_code) {
            co_await audit_refusal(identity->email, "the user is disabled", ip);
            co_return login_error("disabled", kind);
        }
        auto to_app = Response::redirect(std::format("{}?code={}", kAppAddress, *app_code));
        to_app.headers.emplace_back("Set-Cookie", flow_.cookie("", 0s));
        co_return to_app;
    }
    const auto signed_in = co_await sign_in(*identity, request, SessionKind::browser);
    if (!signed_in) {
        co_await audit_refusal(identity->email, "the user is disabled", ip);
        co_return login_error("disabled", kind);
    }

    auto response = Response::redirect(config_.public_url + "/");
    response.headers.emplace_back("Set-Cookie", session_cookie(signed_in->token, config_.session_ttl));
    response.headers.emplace_back("Set-Cookie", flow_.cookie("", 0s));
    co_return response;
}

asio::awaitable<Response> AuthService::app_token(const Request& request) {
    if (const auto wait = exchange_throttle_.attempt(request.remote_ip)) co_return too_many_attempts(*wait);
    auto body = api_support::body_as<AppTokenRequest>(request);
    if (!body) co_return std::move(body.error());

    const auto now = clock_.now();
    auto prepared = prepare_session(lifetimes_.app, now, SessionKind::app, request);
    // One transaction, committed whatever it finds: a code that was presented
    // is gone, with the right verifier or a wrong one, and of two requests
    // with the same code the second finds none.
    // A verifier that is not one has no challenge, and so equals none: it is a wrong verifier, and spends the code.
    auto challenge = is_code_verifier(body->code_verifier) ? s256_challenge(body->code_verifier) : std::string{};
    auto user = co_await db_.tx([code_hash = core::sha256_hex(body->code), challenge = std::move(challenge), row = prepared.row,
                                 now](pqxx::work& tx) -> std::expected<model::User, std::string_view> {
        const auto taken = repo::take_app_code(tx, code_hash, now);
        if (!taken) return std::unexpected("no such code, or it was used");
        if (taken->expired) return std::unexpected("the code had expired");
        // These two are about a person, so the audit log gets them. A wrong
        // verifier means that somebody holds a code who did not start the sign-in.
        const auto refuse = [&](std::string_view reason) {
            audit::record(tx, audit::Actor::anonymous(taken->user.email, row.ip), refusal(taken->user.email, reason), now);
            return std::unexpected(reason);
        };
        if (!core::constant_time_equal(challenge, taken->code_challenge)) return refuse("the app's code came with a wrong verifier");
        if (!taken->user_active) return refuse("the user is disabled");
        open_session(tx, taken->user, taken->first_sign_in, row, now);
        return taken->user;
    });
    if (!user) {
        log::warn("the app's sign-in code was refused", {{"reason", user.error()}});
        // One answer for every reason: it says nothing about a code to somebody who is guessing.
        co_return api_support::invalid("The code is not valid, was used or has expired. Start the sign-in again.");
    }
    SignedIn signed_in{std::move(prepared.token), to_principal(*user, prepared.row), prepared.row.expires_at};
    co_return app_token_answer(std::move(signed_in));
}

asio::awaitable<Response> AuthService::dev_login(const Request& request) {
    // Looks like any unknown path unless development sign-in is switched on.
    const auto& email = config_.dev_login_email;
    if (email.empty()) co_return Response::error(404, "not_found", "No such endpoint.");
    if (const auto wait = throttle_.attempt(request.remote_ip)) co_return too_many_attempts(*wait);
    const auto kind = client_kind(QueryParams(request.query));
    if (!kind) co_return unknown_client();

    const identity::Identity identity{.subject = "dev:" + email,
                                      .email = email,
                                      .email_verified = true,
                                      .hosted_domain = std::nullopt,
                                      .name = "Developer",
                                      .picture_url = std::nullopt};
    auto signed_in = co_await sign_in(identity, request, *kind);
    if (!signed_in) co_return Response::error(403, "user_disabled", "This user is disabled.");
    if (*kind == SessionKind::app) co_return app_token_answer(std::move(*signed_in));  // the token, and no cookie
    auto response = Response::json(200, boost::json::value_from(describe(signed_in->principal)));
    response.headers.emplace_back("Set-Cookie", session_cookie(signed_in->token, config_.session_ttl));
    co_return response;
}

asio::awaitable<Response> AuthService::logout(const Request& request) {
    const auto& principal = *request.principal;  // the hook guarantees a user
    co_await db_.audited(audit::Actor::user(principal.user_id, principal.email, request.remote_ip), clock_.now(),
                         [hash = principal.session_token_hash, kind = principal.kind](pqxx::work& tx, audit::Writer& audit) {
                             repo::delete_session(tx, hash);
                             audit.record("session.logout", "user", audit.user_id(), {.after = {{"kind", domain::name(kind)}}});
                         });
    auto response = Response::no_content();
    // A token was in no cookie. A cookie the request carried beside it was not looked at, and stays.
    if (principal.kind == SessionKind::browser) response.headers.emplace_back("Set-Cookie", session_cookie("", 0s));
    co_return response;
}

asio::awaitable<std::optional<AuthService::SignedIn>> AuthService::sign_in(const identity::Identity& identity, const Request& request,
                                                                           SessionKind kind) {
    const auto now = clock_.now();
    auto prepared = prepare_session(lifetimes_.of(kind), now, kind, request);
    const auto user = co_await db_.run([new_user = new_user_of(identity), row = prepared.row, now](pqxx::connection& conn) -> std::optional<model::User> {
        pqxx::work tx{conn};
        const auto upserted = repo::upsert_user(tx, new_user, now);
        if (upserted.disabled) return std::nullopt;  // rolled back: a disabled user's row stays as it was
        open_session(tx, upserted.user, upserted.created, row, now);
        tx.commit();
        return upserted.user;
    });
    if (!user) co_return std::nullopt;
    co_return SignedIn{std::move(prepared.token), to_principal(*user, prepared.row), prepared.row.expires_at};
}

asio::awaitable<std::optional<std::string>> AuthService::issue_app_code(const identity::Identity& identity, std::string code_challenge,
                                                                        std::string ip) {
    const auto now = clock_.now();
    auto code = core::random_token();
    const bool issued = co_await db_.run([new_user = new_user_of(identity), code_hash = core::sha256_hex(code),
                                          code_challenge = std::move(code_challenge), ip = std::move(ip), now](pqxx::connection& conn) {
        pqxx::work tx{conn};
        const auto upserted = repo::upsert_user(tx, new_user, now);
        if (upserted.disabled) return false;  // rolled back, as in sign_in
        const auto& user = upserted.user;
        repo::create_app_code(tx, {.code_hash = code_hash,
                                   .user_id = user.id,
                                   .code_challenge = code_challenge,
                                   .first_sign_in = upserted.created,
                                   .expires_at = now + kAppCodeLifetime});
        // The user row is never written without an entry, and a code that was issued and never exchanged
        // can be seen. session.login follows at the exchange. Neither the code nor the challenge is in it.
        audit::record(tx, audit::Actor::user(user.id, user.email, ip),
                      {.action = "session.app_code_issued",
                       .object_type = "user",
                       .object_id = std::to_string(user.id),
                       .after = {{"email", user.email}, {"first_sign_in", upserted.created}}},
                      now);
        tx.commit();
        return true;
    });
    if (!issued) co_return std::nullopt;
    co_return code;
}

asio::awaitable<void> AuthService::audit_refusal(std::string email, std::string reason, std::string ip) {
    log::warn("sign-in refused", {{"reason", reason}});
    co_await db_.audited(audit::Actor::anonymous(email, std::move(ip)), clock_.now(),
                         [email, reason = std::move(reason)](pqxx::work&, audit::Writer& audit) { audit.record(refusal(email, reason)); });
}

void AuthService::add_routes(Router& router) {
    router.add(verb::get, "/api/session", {Access::anonymous, "session.read"},
               [this](const Request& request) { return session(request); });
    router.add(verb::get, "/api/auth/google/start", {Access::anonymous, "session.sign_in"},
               [this](const Request& request) { return google_start(request); });
    router.add(verb::get, "/api/auth/google/callback", {Access::anonymous, "session.sign_in"},
               [this](const Request& request) { return google_callback(request); });
    router.add(verb::post, "/api/auth/app-token", {Access::anonymous, "session.sign_in"},
               [this](const Request& request) { return app_token(request); });
    router.add(verb::post, "/api/auth/dev-login", {Access::anonymous, "session.sign_in"},
               [this](const Request& request) { return dev_login(request); });
    router.add(verb::post, "/api/auth/logout", {Access::user, "session.sign_out"},
               [this](const Request& request) { return logout(request); });
}

}  // namespace poni::http
