#include "connectors/google/auth.hpp"

#include "core/crypto.hpp"
#include "core/json.hpp"

#include <boost/json/parse.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

namespace poni::google {

namespace asio = boost::asio;
using namespace std::chrono_literals;

namespace {

// Refresh this long before the token really expires.
constexpr auto kExpiryMargin = 60s;
// How long an API is left alone after it answered 429.
constexpr auto kRateLimitPause = 60s;

ApiError::Kind kind_for(unsigned http_status, std::string_view status) {
    using Kind = ApiError::Kind;
    if (status == "NOT_FOUND" || http_status == 404) return Kind::not_found;
    if (status == "UNAUTHENTICATED" || http_status == 401) return Kind::unauthenticated;
    if (status == "PERMISSION_DENIED" || http_status == 403) return Kind::permission_denied;
    if (status == "RESOURCE_EXHAUSTED" || http_status == 429) return Kind::rate_limited;
    if (http_status >= 500) return Kind::unavailable;
    return Kind::other;
}

// What a validation error says, field by field (error.details[].errorDetails[]
// of a v4 answer): "summary: Must be at most 1500 characters; ...".
std::string validation_details(const boost::json::object& error) {
    std::string out;
    const auto* details = error.if_contains("details");
    if (!details || !details->is_array()) return out;
    for (const auto& detail : details->get_array()) {
        const auto* errors = detail.is_object() ? detail.get_object().if_contains("errorDetails") : nullptr;
        if (!errors || !errors->is_array()) continue;
        for (const auto& item : errors->get_array()) {
            if (!item.is_object()) continue;
            const auto* field = item.get_object().if_contains("field");
            const auto* message = item.get_object().if_contains("message");
            if (!message || !message->is_string()) continue;
            if (!out.empty()) out += "; ";
            if (field && field->is_string()) out += std::format("{}: ", std::string_view(field->get_string()));
            out += std::string_view(message->get_string());
        }
    }
    return out;
}

struct AdcFile {
    std::string type;
    std::optional<std::string> client_id;
    std::optional<std::string> client_secret;
    std::optional<std::string> refresh_token;
};
BOOST_DESCRIBE_STRUCT(AdcFile, (), (type, client_id, client_secret, refresh_token))

// Turns a non-2xx Google response into an ApiError. Handles both shapes:
// OAuth ({"error": "...", "error_description": "..."}) and the APIs
// ({"error": {"status": "...", "message": "..."}}), with what a validation
// error says field by field added in brackets. The value Google echoes back
// is left out: it is the text that was sent.
ApiError describe_failure(const http::ClientResponse& response) {
    boost::system::error_code ec;
    const auto parsed = boost::json::parse(response.body, ec);
    if (!ec && parsed.is_object()) {
        const auto& object = parsed.get_object();
        if (const auto* error = object.if_contains("error")) {
            if (error->is_string()) {
                std::string message(error->get_string());
                if (const auto* d = object.if_contains("error_description"); d && d->is_string()) {
                    message += std::format(": {}", std::string_view(d->get_string()));
                }
                // invalid_grant: the refresh token was revoked or has expired.
                const auto kind = error->get_string() == "invalid_grant" || error->get_string() == "invalid_client"
                                      ? ApiError::Kind::unauthenticated
                                      : kind_for(response.status, {});
                return {kind, response.status, std::format("HTTP {} {}", response.status, message)};
            }
            if (error->is_object()) {
                const auto text = [&](std::string_view key) -> std::string {
                    const auto* v = error->get_object().if_contains(key);
                    return v && v->is_string() ? std::string(v->get_string()) : std::string{};
                };
                const auto status = text("status");
                auto message = std::format("HTTP {} {} {}", response.status, status, text("message"));
                if (const auto details = validation_details(error->get_object()); !details.empty()) message += std::format(" ({})", details);
                return {kind_for(response.status, status), response.status, std::move(message)};
            }
        }
    }
    return {kind_for(response.status, {}), response.status,
            std::format("HTTP {} ({} bytes)", response.status, response.body.size())};
}

// A token endpoint's answer. `what` names it in the error for one that can't be read.
Result<TokenResponse> read_token(const http::ClientResponse& response, std::string_view what) {
    if (!response.ok()) return std::unexpected(describe_failure(response));
    auto token = core::parse_json<TokenResponse>(response.body);
    if (!token) return std::unexpected(ApiError{ApiError::Kind::invalid_response, response.status, std::format("{}: {}", what, token.error())});
    return std::move(*token);
}

// POST https://oauth2.googleapis.com/token with the given form fields, after a slot from the limiter under `limiter_key`.
asio::awaitable<Result<TokenResponse>> token_request(http::Client& http, core::RateLimiter& limiter, std::string_view limiter_key,
                                                     std::string form_body) {
    http::ClientRequest request;
    request.method = http::ClientMethod::post;
    request.url = boost::urls::url("https://oauth2.googleapis.com/token");
    request.content_type = "application/x-www-form-urlencoded";
    request.body = std::move(form_body);
    co_await limiter.acquire(limiter_key);
    const auto response = co_await http.send(std::move(request));
    co_return read_token(response, "token response");
}

// The claims of an ID token, WITHOUT a check of its signature (D-091 point 12).
//
// That is sound for one kind of token only: the one in the token endpoint's
// own answer to a code exchange the engine made. The engine asked Google
// directly, over TLS whose certificate it verified, so the answer can only
// be Google's; OpenID Connect Core 3.1.3.7 allows the TLS server validation
// in place of the signature check in exactly this case. (Against the local
// mock of Google, D-087, the answer is the mock's, over loopback: made-up
// people in a database that holds nothing real.) A token that came
// any other way (from a browser, a header, a request body, a file, a
// redirect) proves nothing without its signature and must never be given to
// this function. So it is no utility: it has no declaration in a header,
// and its one caller is exchange_code below. If the engine ever needs to
// read a token it did not fetch itself, that needs a signature check, not
// this.
std::optional<boost::json::object> decode_claims(std::string_view id_token) {
    const auto first = id_token.find('.');
    const auto second = first == std::string_view::npos ? first : id_token.find('.', first + 1);
    if (second == std::string_view::npos || id_token.find('.', second + 1) != std::string_view::npos) return std::nullopt;
    const auto payload = core::base64_decode(id_token.substr(first + 1, second - first - 1));
    if (!payload) return std::nullopt;
    boost::system::error_code ec;
    auto claims = boost::json::parse(*payload, ec);
    if (ec || !claims.is_object()) return std::nullopt;
    return std::move(claims.get_object());
}

// $GOOGLE_APPLICATION_CREDENTIALS, else gcloud's default location.
std::filesystem::path default_adc_path() {
    if (const char* p = std::getenv("GOOGLE_APPLICATION_CREDENTIALS"); p && *p) return p;
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home ? home : ".") / ".config/gcloud/application_default_credentials.json";
}

// gcloud application-default credentials of type "authorized_user". Service
// account key files aren't supported: production uses the metadata server.
Result<OAuthCredentials> load_adc(const std::filesystem::path& path) {
    const auto fail = [&](std::string why) { return std::unexpected(not_configured(std::format("{}: {}", path.string(), why))); };
    std::ifstream in(path, std::ios::binary);
    if (!in) return fail("cannot read the application-default credentials (run `gcloud auth application-default login`)");
    std::ostringstream text;
    text << in.rdbuf();
    const auto adc = core::parse_json<AdcFile>(text.str());
    if (!adc) return fail(adc.error());
    if (adc->type != "authorized_user" || !adc->client_id || !adc->client_secret || !adc->refresh_token) {
        return fail(std::format("credentials of type '{}' are not supported; use user credentials locally and the "
                                "metadata server in production",
                                adc->type));
    }
    return OAuthCredentials{*adc->client_id, *adc->client_secret, *adc->refresh_token};
}

// The token of the VM's service account, from the metadata server.
TokenFetch metadata_fetch(http::Client& http, core::RateLimiter& limiter) {
    return [&http, &limiter]() -> asio::awaitable<Result<TokenResponse>> {
        http::ClientRequest request;
        request.url = boost::urls::url("http://metadata.google.internal/computeMetadata/v1/instance/service-accounts/default/token");
        request.headers.emplace_back("Metadata-Flavor", "Google");
        request.timeout = 5s;
        co_await limiter.acquire(kOAuthApi);
        const auto response = co_await http.send(std::move(request));
        co_return read_token(response, "metadata token response");
    };
}

}  // namespace

ApiError not_configured(std::string message) { return {ApiError::Kind::not_configured, 0, std::move(message)}; }
ApiError invalid_response(std::string message) { return {ApiError::Kind::invalid_response, 200, std::move(message)}; }

asio::awaitable<Result<CodeExchange>> exchange_code(http::Client& http, core::RateLimiter& limiter, std::string_view limiter_key,
                                                    std::string client_id, std::string client_secret, std::string code,
                                                    std::string redirect_uri, std::string code_verifier) {
    auto tokens = co_await token_request(
        http, limiter, limiter_key,
        std::format("grant_type=authorization_code&code={}&client_id={}&client_secret={}&redirect_uri={}&code_verifier={}", http::form_encode(code),
                    http::form_encode(client_id), http::form_encode(client_secret), http::form_encode(redirect_uri), http::form_encode(code_verifier)));
    if (!tokens) co_return std::unexpected(std::move(tokens.error()));
    CodeExchange exchanged{.tokens = std::move(*tokens), .id_token_claims = std::nullopt};
    if (exchanged.tokens.id_token) exchanged.id_token_claims = decode_claims(*exchanged.tokens.id_token);
    co_return exchanged;
}

asio::awaitable<Result<std::string>> CachedTokenSource::access_token() {
    if (!token_.empty() && clock_.now() < expires_at_) co_return token_;
    auto fetched = co_await fetch_();
    if (!fetched) co_return std::unexpected(std::move(fetched.error()));
    token_ = std::move(fetched->access_token);
    expires_at_ = clock_.now() + std::chrono::seconds(fetched->expires_in) - kExpiryMargin;
    co_return token_;
}

TokenFetch refresh_fetch(http::Client& http, core::RateLimiter& limiter, CredentialsLoader loader, RefreshObserver observer) {
    return [&http, &limiter, loader = std::move(loader), observer = std::move(observer)]() -> asio::awaitable<Result<TokenResponse>> {
        const auto credentials = co_await loader();
        Result<TokenResponse> refreshed = std::unexpected(ApiError{});
        if (credentials) {
            refreshed = co_await token_request(
                http, limiter, kOAuthApi,
                std::format("grant_type=refresh_token&client_id={}&client_secret={}&refresh_token={}", http::form_encode(credentials->client_id),
                            http::form_encode(credentials->client_secret), http::form_encode(credentials->refresh_token)));
        } else {
            refreshed = std::unexpected(credentials.error());
        }
        if (observer) {
            std::optional<std::string> error;
            if (!refreshed) error = refreshed.error().message;
            co_await observer(std::move(error));
        }
        co_return refreshed;
    };
}

TokenFetch service_account_fetch(http::Client& http, core::RateLimiter& limiter, bool from_metadata) {
    if (from_metadata) return metadata_fetch(http, limiter);
    return refresh_fetch(http, limiter, []() -> asio::awaitable<Result<OAuthCredentials>> { co_return load_adc(default_adc_path()); });
}

asio::awaitable<Result<http::ClientResponse>> send(Api api, std::string_view limiter_key, http::ClientRequest request) {
    for (int attempt = 0;; ++attempt) {
        auto token = co_await api.tokens.access_token();
        if (!token) {
            // A token endpoint's 404 is no answer about what the request names, and callers read not_found as
            // "it is gone" (a review, a local post, a report's page): without a token it is not_configured.
            auto error = std::move(token.error());
            if (error.kind == ApiError::Kind::not_found) error.kind = ApiError::Kind::not_configured;
            co_return std::unexpected(std::move(error));
        }
        co_await api.limiter.acquire(limiter_key);
        // A copy the first time: the request is needed again if this token turns out to be refused.
        http::ClientRequest attempt_request;
        if (attempt == 0) {
            attempt_request = request;
        } else {
            attempt_request = std::move(request);
        }
        attempt_request.headers.emplace(attempt_request.headers.begin(), "Authorization", "Bearer " + *token);
        auto response = co_await api.http.send(std::move(attempt_request));

        if (response.status == 401 && attempt == 0) {
            // The cached token may have been revoked early. Google did nothing with the request.
            api.tokens.invalidate();
            continue;
        }
        if (!response.ok()) {
            auto error = describe_failure(response);
            if (error.kind == ApiError::Kind::rate_limited) api.limiter.pause(limiter_key, kRateLimitPause);
            co_return std::unexpected(std::move(error));
        }
        co_return response;
    }
}

const boost::json::array* array_member(const boost::json::value& value, std::string_view key) {
    if (!value.is_object()) return nullptr;
    const auto* member = value.get_object().if_contains(key);
    return member && member->is_array() ? &member->get_array() : nullptr;
}

std::optional<std::string> next_page_token(const boost::json::value& page) {
    if (!page.is_object()) return std::nullopt;
    const auto* token = page.get_object().if_contains("nextPageToken");
    if (!token || !token->is_string() || token->get_string().empty()) return std::nullopt;
    return std::string(token->get_string());
}

asio::awaitable<Result<boost::json::value>> fetch_json(Api api, std::string_view limiter_key, http::ClientRequest request) {
    const auto response = co_await send(api, limiter_key, std::move(request));
    if (!response) co_return std::unexpected(response.error());
    boost::system::error_code ec;
    auto body = boost::json::parse(response->body, ec);
    if (ec) co_return std::unexpected(invalid_response(std::format("response is not JSON: {}", ec.message())));
    co_return body;
}

boost::urls::url url(std::string_view origin, std::initializer_list<std::string_view> segments) {
    boost::urls::url out(origin);
    for (const auto segment : segments) out.segments().push_back(segment);
    return out;
}

}  // namespace poni::google
