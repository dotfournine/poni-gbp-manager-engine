// Sign-in for the dashboard and the phone app, sessions and the authorization
// hook (architecture 4.5, D-033, D-095).
//
//   GET  /api/session                the signed-in user, or how to sign in
//   GET  /api/auth/google/start      sends the browser to Google
//   GET  /api/auth/google/callback   Google sends it back here; creates the session
//   POST /api/auth/app-token         the app exchanges its one-time code for a token
//   POST /api/auth/dev-login         local development only (PONI_DEV_LOGIN_EMAIL)
//   POST /api/auth/logout
//
// Sessions live in PostgreSQL, known by the hash of their token only, and
// are of two kinds (domain/session.hpp).
//
// The dashboard's: the browser holds the token in an HttpOnly,
// SameSite=Strict cookie (Secure and "__Host-" prefixed when the dashboard is
// served over HTTPS). Every changing request must also carry the session's
// CSRF token in X-CSRF-Token.
//
// The phone app's: one Google sign-in for both clients. The app starts it
// with `client=ios` and the S256 challenge of a PKCE verifier; the callback,
// after the same checks, sends the app's browser sheet to gbpmanager://auth
// with a one-time code, and the app exchanges the code and the verifier for a
// token it sends as "Authorization: Bearer". The token is read from that
// header and from nowhere else; a request with it gets no CSRF and no Origin
// check, which guard the cookie. No token, code or verifier is written to a
// log or an audit entry.
#pragma once

#include "connectors/identity/provider.hpp"
#include "core/clock.hpp"
#include "core/config.hpp"
#include "db/database.hpp"
#include "domain/models.hpp"
#include "http/browser_flow.hpp"
#include "http/router.hpp"

#include <chrono>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace poni::http {

// Limits sign-in attempts per client address: at most `max_attempts` in
// `window`, then every attempt is refused until `lockout` has passed.
class LoginThrottle {
public:
    struct Options {
        int max_attempts = 20;
        std::chrono::seconds window{600};
        std::chrono::seconds lockout{900};
    };

    LoginThrottle(const core::Clock& clock, Options options) : clock_(clock), options_(options) {}
    explicit LoginThrottle(const core::Clock& clock) : LoginThrottle(clock, Options{}) {}

    // Counts one attempt. Returns how long the client must wait if it is over
    // the limit, or nothing if the attempt may proceed.
    std::optional<std::chrono::seconds> attempt(std::string_view client);

private:
    struct Entry {
        core::TimePoint window_start;
        int attempts = 0;
        core::TimePoint locked_until;
    };
    const core::Clock& clock_;
    Options options_;
    std::map<std::string, Entry, std::less<>> entries_;
};

// Each kind of session's lifetime, as configured.
inline domain::SessionLifetimes session_lifetimes(const core::Config& config) {
    return {.browser = {config.session_ttl, config.session_idle}, .app = {config.app_session_ttl, config.app_session_idle}};
}

// True if the identity is a verified Google Workspace account whose
// organization and email address both belong to `domains`.
bool domain_allowed(const identity::Identity& identity, std::span<const std::string> domains);

class AuthService {
public:
    // `provider` may be null: Google sign-in is then not configured. Reads the
    // public address, the sign-in domains, the session lifetimes, development
    // sign-in and the Google mock (D-087, shown on every page) from `config`,
    // which must outlive the service.
    AuthService(db::Database& db, const core::Clock& clock, identity::Provider* provider, const core::Config& config);

    // The router's authorization hook. Must not outlive this object.
    AuthHook hook();

    void add_routes(Router& router);

    // Name of the session cookie ("__Host-poni_session" over HTTPS).
    std::string session_cookie_name() const;

private:
    boost::asio::awaitable<std::optional<Response>> authorize(Request& request, const RouteInfo& route);
    boost::asio::awaitable<Response> session(const Request& request);
    boost::asio::awaitable<Response> google_start(const Request& request);
    boost::asio::awaitable<Response> google_callback(const Request& request);
    boost::asio::awaitable<Response> app_token(const Request& request);
    boost::asio::awaitable<Response> dev_login(const Request& request);
    boost::asio::awaitable<Response> logout(const Request& request);

    struct SignedIn {
        std::string token;  // goes into the cookie, or to the app
        Principal principal;
        core::TimePoint expires_at;  // when the session ends at the latest
    };
    // Creates the user if needed and a session of this kind, with the audit
    // entry, in one transaction. Nothing if the user is disabled.
    boost::asio::awaitable<std::optional<SignedIn>> sign_in(const identity::Identity& identity, const Request& request,
                                                            domain::SessionKind kind);
    // For a sign-in the app started: creates the user if needed and the
    // one-time code the app exchanges for its token, with an audit entry of
    // its own, in one transaction. Nothing if the user is disabled.
    boost::asio::awaitable<std::optional<std::string>> issue_app_code(const identity::Identity& identity, std::string code_challenge,
                                                                      std::string ip);
    boost::asio::awaitable<void> audit_refusal(std::string email, std::string reason, std::string ip);

    std::string session_cookie(std::string_view token, std::chrono::seconds max_age) const;
    // Where a sign-in that failed ends: the dashboard's sign-in page, or the app's own address.
    Response login_error(std::string_view code, domain::SessionKind kind) const;
    model::Session describe(const std::optional<Principal>& principal) const;
    Response app_token_answer(SignedIn signed_in) const;

    db::Database& db_;
    const core::Clock& clock_;
    identity::Provider* provider_;
    const core::Config& config_;
    domain::SessionLifetimes lifetimes_;
    BrowserFlow flow_;
    // The start, the callback and development sign-in share one limit per
    // address. The app's exchange has its own, so that an app sign-in costs
    // two of the shared one, as a dashboard's does, and trying codes is
    // limited without locking anybody out of signing in.
    LoginThrottle throttle_;
    LoginThrottle exchange_throttle_;
};

}  // namespace poni::http
