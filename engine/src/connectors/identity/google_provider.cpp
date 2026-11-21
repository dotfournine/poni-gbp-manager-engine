#include "connectors/identity/provider.hpp"

#include "connectors/google/auth.hpp"

#include <chrono>
#include <format>

namespace poni::identity {

namespace asio = boost::asio;
using namespace std::chrono_literals;

namespace {

// Allowance for clocks that disagree a little.
constexpr auto kClockSkew = 60s;

std::string_view text(const boost::json::object& object, std::string_view key) {
    const auto* v = object.if_contains(key);
    return v && v->is_string() ? std::string_view(v->get_string()) : std::string_view{};
}

}  // namespace

std::expected<Identity, std::string> identity_from_claims(const boost::json::object& claims, std::string_view client_id,
                                                          std::string_view nonce, core::TimePoint now) {
    const auto issuer = text(claims, "iss");
    if (issuer != "https://accounts.google.com" && issuer != "accounts.google.com") {
        return std::unexpected("the ID token has the wrong issuer");
    }
    if (text(claims, "aud") != client_id) return std::unexpected("the ID token was issued to another application");
    if (nonce.empty() || text(claims, "nonce") != nonce) return std::unexpected("the ID token's nonce doesn't match");

    const auto* exp = claims.if_contains("exp");
    if (!exp || !exp->is_number()) return std::unexpected("the ID token has no expiry");
    const auto expires = core::TimePoint(std::chrono::seconds(exp->to_number<std::int64_t>()));
    if (now > expires + kClockSkew) return std::unexpected("the ID token has expired");

    Identity identity;
    identity.subject = text(claims, "sub");
    identity.email = text(claims, "email");
    if (identity.subject.empty() || identity.email.empty()) return std::unexpected("the ID token names no account");
    if (const auto* verified = claims.if_contains("email_verified"); verified && verified->is_bool()) {
        identity.email_verified = verified->get_bool();
    }
    if (const auto hd = text(claims, "hd"); !hd.empty()) identity.hosted_domain = std::string(hd);
    identity.name = text(claims, "name");
    if (const auto picture = text(claims, "picture"); !picture.empty()) identity.picture_url = std::string(picture);
    return identity;
}

GoogleProvider::GoogleProvider(http::Client& http, const core::Clock& clock, core::RateLimiter& limiter, std::string client_id,
                               std::string client_secret, std::vector<std::string> hosted_domains)
    : http_(http),
      clock_(clock),
      limiter_(limiter),
      client_id_(std::move(client_id)),
      client_secret_(std::move(client_secret)),
      hosted_domains_(std::move(hosted_domains)) {}

std::string GoogleProvider::authorization_url(const AuthorizationRequest& request) const {
    boost::urls::url url("https://accounts.google.com/o/oauth2/v2/auth");
    auto params = url.params();
    params.append({"client_id", client_id_});
    params.append({"redirect_uri", request.redirect_uri});
    params.append({"response_type", "code"});
    params.append({"scope", "openid email profile"});
    params.append({"state", request.state});
    params.append({"nonce", request.nonce});
    params.append({"code_challenge", request.code_challenge});
    params.append({"code_challenge_method", "S256"});
    params.append({"prompt", "select_account"});
    // "hd" takes one domain; "*" asks Google for any Workspace account.
    if (hosted_domains_.size() == 1) {
        params.append({"hd", hosted_domains_.front()});
    } else if (!hosted_domains_.empty()) {
        params.append({"hd", "*"});
    }
    return std::string(url.buffer());
}

asio::awaitable<std::expected<Identity, std::string>> GoogleProvider::redeem(std::string code, std::string code_verifier,
                                                                             std::string redirect_uri,
                                                                             std::string nonce) {
    const auto exchanged = co_await google::exchange_code(http_, limiter_, kSignInApi, client_id_, client_secret_, std::move(code), std::move(redirect_uri),
                                                          std::move(code_verifier));
    if (!exchanged) co_return std::unexpected(std::format("the code was not accepted: {}", exchanged.error().message));
    if (!exchanged->tokens.id_token) co_return std::unexpected(std::string("Google returned no ID token"));
    if (!exchanged->id_token_claims) co_return std::unexpected(std::string("the token is malformed"));
    // The token is the token endpoint's own answer to this exchange, so its
    // signature is not checked (D-091 point 12). Everything it claims is.
    co_return identity_from_claims(*exchanged->id_token_claims, client_id_, nonce, clock_.now());
}

}  // namespace poni::identity
