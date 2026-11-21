// Push to the phone app through Apple's push service, APNs (D-097). One
// environment per engine, one token key, and HTTP/2 through the outbound
// client: a delivery's pushes go out on one connection, one stream each.
//
// The provider token is a JWT signed with the key (ES256). Apple takes one
// for up to an hour and refuses one renewed more often than every 20
// minutes, so it is kept for 50 and dropped when Apple calls it expired.
//
// A push carries ids and a line of text that may be shown on a lock screen
// (domain/push.hpp); nothing here reads it. A device token is in no log.
#pragma once

#include "connectors/notify/channel.hpp"
#include "core/clock.hpp"
#include "core/config.hpp"
#include "core/rate_limiter.hpp"
#include "domain/push.hpp"
#include "http/client.hpp"

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace poni::notify {

// Rate limiter key: one slot per connection.
inline constexpr std::string_view kApnsApi = "apns";

struct Push {
    std::string device_token;  // lower-case hex, as the app registered it
    std::string payload;       // the JSON Apple is given
    // An alert with the same id replaces the one already shown, so a push
    // that is sent again does not show twice.
    std::string collapse_id;
};

using domain::PushAnswer;

class PushSender {
public:
    virtual ~PushSender() = default;
    // One answer per push, in order. An error when nothing could be sent:
    // `transient` for a network failure, as with email.
    virtual boost::asio::awaitable<std::expected<std::vector<PushAnswer>, SendError>> send(std::vector<Push> pushes) = 0;
};

// The provider token: a JWT with the key's id, the team and the time it was made.
std::string provider_token(const core::Es256Key& key, std::string_view key_id, std::string_view team_id, core::TimePoint issued_at);

// What one of Apple's answers means (Apple's "Handling notification responses from APNs").
PushAnswer read_answer(const http::ClientResponse& response);

class ApnsSender final : public PushSender {
public:
    // `http`, `limiter` and `clock` must outlive the sender.
    ApnsSender(http::Client& http, core::RateLimiter& limiter, const core::Clock& clock, core::ApnsSettings settings)
        : http_(http), limiter_(limiter), clock_(clock), settings_(std::move(settings)) {}

    boost::asio::awaitable<std::expected<std::vector<PushAnswer>, SendError>> send(std::vector<Push> pushes) override;

    // How long a provider token is used.
    static constexpr std::chrono::minutes kTokenLifetime{50};
    // How long Apple keeps a push for a phone that is off.
    static constexpr std::chrono::hours kExpiresAfter{24};

private:
    std::string token();

    http::Client& http_;
    core::RateLimiter& limiter_;
    const core::Clock& clock_;
    core::ApnsSettings settings_;
    std::optional<std::string> token_;
    core::TimePoint token_made_{};
};

}  // namespace poni::notify
