// What every Google connector shares: the error type they return, access
// tokens, and the one way a request with a token is sent (`send`).
//
// A token comes from a stored refresh token (the GBP managing account, the
// sending mailbox, or gcloud's application-default credentials locally) or
// from the metadata server (the VM's service account, in production). A
// CachedTokenSource keeps it until shortly before it expires.
#pragma once

#include "core/clock.hpp"
#include "core/rate_limiter.hpp"
#include "http/client.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/describe/class.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>
#include <boost/system/system_error.hpp>

#include <cstdint>
#include <expected>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace poni::google {

// An API that answered with an error, or couldn't be asked. Transport
// failures (DNS, TLS, timeouts) are exceptions, not ApiErrors.
struct ApiError {
    enum class Kind {
        not_configured,    // no credentials stored, or they can't be read
        unauthenticated,   // the token was refused (revoked, expired grant)
        permission_denied,
        not_found,
        rate_limited,
        unavailable,       // 5xx
        invalid_response,  // a 2xx whose body isn't what the API documents
        other,
    };
    Kind kind = Kind::other;
    unsigned http_status = 0;
    std::string message;  // never contains a secret
};

template <class T>
using Result = std::expected<T, ApiError>;

// Google refused before doing anything, and trying again may work: a rate
// limit, a token it refused, or no token to ask with.
inline bool worth_retrying(ApiError::Kind kind) {
    return kind == ApiError::Kind::rate_limited || kind == ApiError::Kind::unauthenticated || kind == ApiError::Kind::not_configured;
}

// A read with a transport failure (DNS, TLS, a timeout: an exception)
// turned into an ApiError (unavailable, "network error: ..."), so that the
// caller handles "Google can't be reached" and "Google said no" in one
// place. Only for a call that changes nothing at Google: for a write, a
// transport failure means its outcome is unknown, and must not read like a
// refusal.
template <class T>
boost::asio::awaitable<Result<T>> guarded(boost::asio::awaitable<Result<T>> call) {
    std::string failure;
    try {
        co_return co_await std::move(call);
    } catch (const boost::system::system_error& e) {
        failure = e.code().message();
    }
    co_return std::unexpected(ApiError{ApiError::Kind::unavailable, 0, "network error: " + failure});
}

// What asking Google for one thing (a review, a post) came to.
template <class T>
struct Read {
    std::optional<T> value;
    bool gone = false;  // Google answered NOT_FOUND for it
    std::string error;  // anything else: nothing is known
};

template <class T>
boost::asio::awaitable<Read<T>> read(boost::asio::awaitable<Result<T>> call) {
    Read<T> out;
    auto result = co_await guarded(std::move(call));
    if (result) {
        out.value = std::move(*result);
    } else if (result.error().kind == ApiError::Kind::not_found) {
        out.gone = true;
    } else {
        out.error = std::move(result.error().message);
    }
    co_return out;
}

// Nothing was asked: there are no credentials to ask with.
ApiError not_configured(std::string message);

// Google answered 2xx with a body that is not what the API documents.
ApiError invalid_response(std::string message);

struct OAuthCredentials {
    std::string client_id;
    std::string client_secret;
    std::string refresh_token;
};
BOOST_DESCRIBE_STRUCT(OAuthCredentials, (), (client_id, client_secret, refresh_token))

struct TokenResponse {
    std::string access_token;
    std::int64_t expires_in = 0;
    std::optional<std::string> refresh_token;
    std::optional<std::string> scope;
    std::optional<std::string> id_token;
};
BOOST_DESCRIBE_STRUCT(TokenResponse, (), (access_token, expires_in, refresh_token, scope, id_token))

class TokenSource {
public:
    virtual ~TokenSource() = default;
    virtual boost::asio::awaitable<Result<std::string>> access_token() = 0;
    // Forget the cached token; the next call fetches a new one. Call after a 401.
    virtual void invalidate() = 0;
};

// Rate limiter key for Google's token endpoint (the refreshes of the engine's
// own tokens, and connecting an account) and the metadata server.
inline constexpr std::string_view kOAuthApi = "google.oauth";

// How a CachedTokenSource gets a new token.
using TokenFetch = std::function<boost::asio::awaitable<Result<TokenResponse>>()>;

// Keeps the token `fetch` returned until a minute before it expires.
class CachedTokenSource final : public TokenSource {
public:
    CachedTokenSource(const core::Clock& clock, TokenFetch fetch) : clock_(clock), fetch_(std::move(fetch)) {}

    boost::asio::awaitable<Result<std::string>> access_token() override;
    void invalidate() override { token_.clear(); }

private:
    const core::Clock& clock_;
    TokenFetch fetch_;
    std::string token_;
    core::TimePoint expires_at_{};
};

// Where the refresh token comes from. Asked on every refresh (about once an
// hour), so a new sign-in is picked up without a restart.
using CredentialsLoader = std::function<boost::asio::awaitable<Result<OAuthCredentials>>()>;
// Told the outcome of every refresh: nothing on success, the reason otherwise.
using RefreshObserver = std::function<boost::asio::awaitable<void>(std::optional<std::string> error)>;

// A token for the refresh token `loader` returns. `http` and `limiter` must
// outlive the token source that is given the fetch.
TokenFetch refresh_fetch(http::Client& http, core::RateLimiter& limiter, CredentialsLoader loader, RefreshObserver observer = {});

// The engine's own identity at Google, for Pub/Sub and Cloud Storage: the
// VM's service account from the metadata server when `from_metadata`
// (PONI_GOOGLE_CREDENTIALS=metadata, production), gcloud's application-default
// credentials of a user otherwise ($GOOGLE_APPLICATION_CREDENTIALS, else
// gcloud's own file).
TokenFetch service_account_fetch(http::Client& http, core::RateLimiter& limiter, bool from_metadata);

// What the token endpoint answered when the engine redeemed an authorization code.
struct CodeExchange {
    TokenResponse tokens;
    // The claims of the ID token in that answer; nothing if it held none
    // that can be read. Their signature was not checked and need not be
    // (auth.cpp says why, at decode_claims). They are still only claims:
    // whoever signs a person in on them checks issuer, audience, nonce and
    // expiry (identity::identity_from_claims).
    std::optional<boost::json::object> id_token_claims;
};

// Redeems the code Google's consent page sent the browser back with (the
// authorization code grant with PKCE), after a slot from the limiter under
// `limiter_key`: sign-in has a key of its own, so that a run of sign-ins
// cannot hold up a refresh of the engine's own tokens (kOAuthApi). The
// two places that sign someone in at Google go through here: dashboard
// sign-in (identity::GoogleProvider) and connecting an account
// (http/connect). It is the only way to the claims of an ID token: there is
// no function that takes a token from anywhere else.
boost::asio::awaitable<Result<CodeExchange>> exchange_code(http::Client& http, core::RateLimiter& limiter, std::string_view limiter_key,
                                                           std::string client_id, std::string client_secret, std::string code,
                                                           std::string redirect_uri, std::string code_verifier);

// ---- one authorised call ---------------------------------------------------------------

// What a connector calls a Google API with: the outbound client, the tokens
// of the account it acts as, and the central rate limiter. All three must
// outlive the connector.
struct Api {
    http::Client& http;
    TokenSource& tokens;
    core::RateLimiter& limiter;
};

// Sends `request` with the account's bearer token, after a slot from the
// limiter under `limiter_key` (one of the constants the connectors define).
// Three things every caller relies on:
//
//  - Only a first 401 is tried again, once, with a new token: Google did
//    nothing with a request whose token it refused. Nothing else is ever
//    sent twice from here, so a write goes out once.
//  - Any other answer outside 2xx comes back as an ApiError with Google's
//    status in `http_status` and Google's words in the message (for a
//    validation error of the v4 API also what it says field by field:
//    "summary: Must be at most 1500 characters"). A rate limit also keeps
//    every caller off that API for a minute. Without a token nothing is
//    sent and the token source's own error is returned, with one change:
//    its `not_found` (a token endpoint's 404) becomes `not_configured`,
//    because nothing was asked about what the request names and callers
//    read `not_found` as "it is gone" (a review, a local post, a report's page).
//  - A transport failure throws (boost::system::system_error), as the
//    client does. For a write that means "the outcome is unknown", and
//    nothing here turns it into an error that reads like a refusal.
boost::asio::awaitable<Result<http::ClientResponse>> send(Api api, std::string_view limiter_key, http::ClientRequest request);

// The member `key` of a JSON object if it is a list; nothing otherwise. Google
// leaves an empty list out, so "nothing" is read as "no entries".
const boost::json::array* array_member(const boost::json::value& value, std::string_view key);

// The "nextPageToken" of one page of a list; nothing on the last page.
std::optional<std::string> next_page_token(const boost::json::value& page);

// send(), and the body of the 2xx as JSON (`invalid_response` if it is none): a call whose answer is read.
boost::asio::awaitable<Result<boost::json::value>> fetch_json(Api api, std::string_view limiter_key, http::ClientRequest request);

// `origin` ("https://mybusiness.googleapis.com") and the segments of a path.
// Boost.URL percent-encodes each segment, so an id can never change the path.
boost::urls::url url(std::string_view origin, std::initializer_list<std::string_view> segments);

}  // namespace poni::google
