// Central rate limiter for outbound APIs. Google's quotas are per project,
// so every caller of one API shares one budget here (architecture 2.1).
//
// Each API has a steady rate and a burst. A caller reserves a slot and waits
// until its turn; nothing is ever dropped. Callers:
//
//     co_await limiter.acquire("gbp.reviews");
//
// The engine runs on one io_context thread, so this needs no locks.
#pragma once

#include "core/clock.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <map>
#include <string>
#include <string_view>

namespace poni::core {

class RateLimiter {
public:
    struct Limit {
        double per_minute = 60;
        // How many requests may go out back to back after a quiet spell.
        double burst = 1;
    };

    RateLimiter(boost::asio::any_io_executor ex, const Clock& clock);

    void configure(std::string api, Limit limit);

    // Takes the next slot for `api` and returns how long the caller must wait
    // before sending. Throws std::logic_error for an API that wasn't
    // configured, so nothing goes out unthrottled by mistake.
    std::chrono::microseconds reserve(std::string_view api);

    // reserve(), then sleep for the returned time.
    boost::asio::awaitable<void> acquire(std::string_view api);

    // No request to `api` before now + `duration` (after HTTP 429).
    void pause(std::string_view api, std::chrono::seconds duration);

private:
    struct Bucket {
        std::chrono::microseconds interval;   // time between requests at the steady rate
        std::chrono::microseconds tolerance;  // how far ahead of schedule a burst may run
        TimePoint next;                       // when the next request is due at the steady rate
    };
    Bucket& bucket(std::string_view api);

    boost::asio::any_io_executor ex_;
    const Clock& clock_;
    std::map<std::string, Bucket, std::less<>> buckets_;
};

}  // namespace poni::core
