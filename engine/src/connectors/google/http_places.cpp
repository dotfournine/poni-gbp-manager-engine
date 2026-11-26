#include "connectors/google/places.hpp"

#include "core/json.hpp"

#include <boost/describe/class.hpp>
#include <boost/json/parse.hpp>
#include <boost/system/system_error.hpp>

#include <algorithm>
#include <format>
#include <optional>
#include <utility>

namespace poni::google {

namespace asio = boost::asio;

namespace {

// What the mock is sent when no key is configured: it checks only that there is one.
constexpr std::string_view kMockKey = "mock-places-key";

struct GoogleTimeZone {
    std::string id;
};
BOOST_DESCRIBE_STRUCT(GoogleTimeZone, (), (id))

struct GooglePlace {
    std::optional<GoogleTimeZone> timeZone;
};
BOOST_DESCRIBE_STRUCT(GooglePlace, (), (timeZone))

// Google's error.status word ("PERMISSION_DENIED") when it is one, never
// error.message, which may quote the request.
std::string status_word(std::string_view body) {
    boost::system::error_code ec;
    const auto parsed = boost::json::parse(body, ec);
    if (ec || !parsed.is_object()) return {};
    const auto* error = parsed.get_object().if_contains("error");
    if (!error || !error->is_object()) return {};
    const auto* status = error->get_object().if_contains("status");
    if (!status || !status->is_string()) return {};
    const std::string_view word = status->get_string();
    const bool plain = !word.empty() && word.size() <= 64 &&
                       std::ranges::all_of(word, [](char c) { return (c >= 'A' && c <= 'Z') || c == '_'; });
    return plain ? std::string(word) : std::string{};
}

ApiError failure(const http::ClientResponse& response) {
    using Kind = ApiError::Kind;
    const auto status = response.status;
    const Kind kind = status == 404                    ? Kind::not_found
                      : status == 401 || status == 403 ? Kind::unauthenticated
                      : status == 429                  ? Kind::rate_limited
                      : status >= 500                  ? Kind::unavailable
                                                       : Kind::other;
    const auto word = status_word(response.body);
    std::string message = word.empty() ? std::format("Google Places answered {}", status) : std::format("Google Places answered {} {}", status, word);
    return ApiError{kind, status, std::move(message)};
}

}  // namespace

HttpPlaceTimeZones::HttpPlaceTimeZones(http::Client& http, core::RateLimiter& limiter, std::string api_key, bool google_mock)
    : http_(http), limiter_(limiter), api_key_(api_key.empty() && google_mock ? std::string(kMockKey) : std::move(api_key)) {}

asio::awaitable<Result<std::string>> HttpPlaceTimeZones::time_zone(std::string place_id) {
    // The outbound rule would refuse it by throwing, and the caller records only what is returned.
    if (!http::is_plain_id(place_id)) {
        co_return std::unexpected(ApiError{ApiError::Kind::invalid_response, 0, "Google gave this location a place id the engine does not send on."});
    }
    if (api_key_.empty()) co_return std::unexpected(ApiError{ApiError::Kind::not_configured, 0, "No Google Places key is configured."});

    http::ClientRequest request;
    request.method = http::ClientMethod::get;
    request.url = url("https://places.googleapis.com", {"v1", "places", place_id});
    request.headers.emplace_back("X-Goog-Api-Key", api_key_);
    request.headers.emplace_back("X-Goog-FieldMask", "timeZone");

    co_await limiter_.acquire(kPlacesApi);
    std::optional<http::ClientResponse> response;
    std::string transport_failure;
    try {
        response = co_await http_.send(std::move(request));
    } catch (const boost::system::system_error& e) {
        transport_failure = e.code().message();
    }
    if (!response) co_return std::unexpected(ApiError{ApiError::Kind::unavailable, 0, "network error: " + transport_failure});
    if (!response->ok()) co_return std::unexpected(failure(*response));

    auto place = core::parse_json<GooglePlace>(response->body);
    if (!place || !place->timeZone || place->timeZone->id.empty()) {
        co_return std::unexpected(invalid_response("Google Places answered without a time zone for this location."));
    }
    co_return std::move(place->timeZone->id);
}

}  // namespace poni::google
