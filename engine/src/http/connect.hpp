// Connecting the two Google accounts the engine works with, from the
// dashboard (D-079): the GBP managing account, whose token reads reviews
// and posts replies (D-043), and the mailbox notifications are sent from
// (D-066).
//
//   POST /api/connections/{account}/start      where at Google to sign in
//   GET  /api/connections/{account}/callback   Google sends the browser back here
//
// `account` is "gbp" or "gmail". A signed-in person starts; the engine
// builds Google's address, receives the redirect, exchanges the code, checks
// the account, encrypts the token and stores it, with an audit entry in that
// person's name. The client secret, the refresh token and the encryption key
// never reach the browser, and nothing is stored unless every check passed.
//
// The session cookie is SameSite=Strict, so it is not sent with Google's
// redirect. The redirect is therefore tied to its start in another way: a
// random secret in a short-lived Lax cookie, and on this side the attempt it
// belongs to (who started it, for which account, until when). An attempt is
// used once.
#pragma once

#include "connectors/google/auth.hpp"
#include "core/clock.hpp"
#include "core/config.hpp"
#include "core/rate_limiter.hpp"
#include "db/database.hpp"
#include "http/browser_flow.hpp"
#include "http/client.hpp"
#include "http/router.hpp"

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace poni::http {

inline constexpr std::string_view kConnectGbp = "gbp";
inline constexpr std::string_view kConnectGmail = "gmail";

// Told which account ("gbp" or "gmail") has a new token stored, so the
// engine drops the access token it cached for the old one and uses the
// new account from its next call on.
using OnConnected = std::function<void(std::string_view account)>;

// The refresh token of `account` ("gbp" or "gmail") as it was connected
// here, for the account's token source. Read from the database on every
// refresh, so a new connection is used without a restart. `box` may be
// null (no PONI_SECRET_KEY): nothing can then be read. `db` must outlive the loader.
google::CredentialsLoader stored_credentials(db::Database& db, const core::SecretBox* box, std::string_view account);

class ConnectService {
public:
    // Reads the public address (the OAuth client must list <public_url>/api/connections/gbp/callback and
    // .../gmail/callback among its redirect addresses), the OAuth client both accounts sign in through
    // (PONI_GBP_OAUTH_CLIENT_ID and _SECRET), the key (without PONI_SECRET_KEY a token can't be
    // encrypted, so nothing can be connected), the sender (the only account "gmail" accepts), the
    // session lifetime and the Google mock (its consent page instead of Google's, D-087) from `config`,
    // which must outlive the service.
    ConnectService(db::Database& db, const core::Clock& clock, Client& http, core::RateLimiter& limiter, const core::Config& config,
                   OnConnected on_connected);

    // Must not outlive this object.
    void add_routes(Router& router);

    // The engine has the OAuth client and the key: the dashboard's "Connect" buttons can work.
    bool configured() const {
        return config_.secret_box.has_value() && !config_.gbp_oauth_client_id.empty() && !config_.gbp_oauth_client_secret.empty();
    }

private:
    struct Attempt {
        std::string account;
        Principal started_by;
        core::TimePoint expires;
    };

    boost::asio::awaitable<Response> start(const Request& request);
    boost::asio::awaitable<Response> callback(const Request& request);
    // Exchanges the code, checks the account and stores its token. The error's code is one of the callback's.
    boost::asio::awaitable<std::optional<std::string>> connect(const Attempt& attempt, std::string code, std::string verifier,
                                                               std::string ip);

    std::string redirect_uri(std::string_view account) const;
    Response back_to_status(std::string_view account, std::string_view error) const;

    db::Database& db_;
    const core::Clock& clock_;
    Client& http_;
    core::RateLimiter& limiter_;
    const core::Config& config_;
    OnConnected on_connected_;
    BrowserFlow flow_;
    // By the hash of the flow cookie's secret. In memory: a restart ends the attempts under way, and the person starts again.
    std::map<std::string, Attempt, std::less<>> attempts_;
};

}  // namespace poni::http
