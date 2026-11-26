// A place's time zone from Google Places (D-109): one GET of
// places.googleapis.com/v1/places/{id} with the field mask `timeZone`.
//
// The one Google call that does not go through google::send: it carries an
// API key (X-Goog-Api-Key) and no OAuth token. The key is never in an
// address, a log line or an error message.
#pragma once

#include "connectors/google/auth.hpp"
#include "core/rate_limiter.hpp"
#include "http/client.hpp"

#include <boost/asio/awaitable.hpp>

#include <string>
#include <string_view>

namespace poni::google {

// Rate limiter key. The engine asks once per newly discovered location.
inline constexpr std::string_view kPlacesApi = "google.places";

// The IANA time zone Google Places gives for a place id, and nothing else.
class PlaceTimeZones {
public:
    virtual ~PlaceTimeZones() = default;
    // The zone's IANA name (for example "America/New_York"), or an ApiError whose
    // message names neither the key nor the address.
    virtual boost::asio::awaitable<Result<std::string>> time_zone(std::string place_id) = 0;
};

// Places API (New), Place Details. A transport failure is returned as
// `unavailable`, not thrown: the read changes nothing at Google.
class HttpPlaceTimeZones final : public PlaceTimeZones {
public:
    // `api_key`: PONI_GOOGLE_PLACES_API_KEY. `google_mock`: whether
    // PONI_GOOGLE_MOCK_URL is set; the mock takes any key, so an empty one is
    // sent as a fixed stand-in there, and nothing is sent without one otherwise.
    HttpPlaceTimeZones(http::Client& http, core::RateLimiter& limiter, std::string api_key, bool google_mock = false);

    boost::asio::awaitable<Result<std::string>> time_zone(std::string place_id) override;

private:
    http::Client& http_;
    core::RateLimiter& limiter_;
    std::string api_key_;
};

}  // namespace poni::google
