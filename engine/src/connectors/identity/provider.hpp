// Who is signing in to the dashboard (D-033). The engine only needs two
// things from an identity provider: where to send the browser, and who came
// back. GoogleProvider speaks OpenID Connect to Google; tests use a fake.
#pragma once

#include "core/clock.hpp"
#include "core/rate_limiter.hpp"
#include "http/client.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/json/object.hpp>

#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace poni::identity {

// What the provider says about the person, from the ID token its token
// endpoint returned: checked claims (identity_from_claims), no more.
struct Identity {
    std::string subject;  // the provider's stable id for the account
    std::string email;
    bool email_verified = false;
    // The Google Workspace domain the account belongs to ("hd"); empty for
    // consumer accounts such as gmail.com.
    std::optional<std::string> hosted_domain;
    std::string name;
    std::optional<std::string> picture_url;
};

struct AuthorizationRequest {
    std::string redirect_uri;
    std::string state;
    std::string nonce;
    std::string code_challenge;  // PKCE, S256
};

class Provider {
public:
    virtual ~Provider() = default;

    // The provider page the browser is sent to.
    virtual std::string authorization_url(const AuthorizationRequest& request) const = 0;

    // Exchanges the code the browser came back with and returns the verified
    // identity, or why it can't be trusted. `nonce` must be the one sent in
    // the authorization request.
    virtual boost::asio::awaitable<std::expected<Identity, std::string>> redeem(std::string code,
                                                                                std::string code_verifier,
                                                                                std::string redirect_uri,
                                                                                std::string nonce) = 0;
};

// Rate limiter key for sign-in's code exchange at Google's token endpoint: its
// own, so that sign-ins and the engine's token refreshes don't wait for each other.
inline constexpr std::string_view kSignInApi = "google.signin";

class GoogleProvider final : public Provider {
public:
    // `hosted_domains` only shapes the hint Google gets so its account chooser
    // offers the right accounts: the one domain, or "any Workspace account"
    // when there are several. It is not the check: the caller verifies the
    // returned identity's domain itself.
    GoogleProvider(http::Client& http, const core::Clock& clock, core::RateLimiter& limiter, std::string client_id,
                   std::string client_secret, std::vector<std::string> hosted_domains);

    std::string authorization_url(const AuthorizationRequest& request) const override;
    boost::asio::awaitable<std::expected<Identity, std::string>> redeem(std::string code, std::string code_verifier,
                                                                        std::string redirect_uri,
                                                                        std::string nonce) override;

private:
    http::Client& http_;
    const core::Clock& clock_;
    core::RateLimiter& limiter_;
    std::string client_id_;
    std::string client_secret_;
    std::vector<std::string> hosted_domains_;
};

// Checks an ID token's claims (issuer, audience, expiry, nonce) and maps
// them. No signature is checked, here or before (D-091 point 12): the claims
// must be those google::exchange_code returned, which are the token
// endpoint's own answer to the engine's code exchange.
std::expected<Identity, std::string> identity_from_claims(const boost::json::object& claims, std::string_view client_id,
                                                          std::string_view nonce, core::TimePoint now);

}  // namespace poni::identity
