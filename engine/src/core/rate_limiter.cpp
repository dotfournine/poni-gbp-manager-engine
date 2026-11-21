#include "core/rate_limiter.hpp"

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <format>
#include <stdexcept>

namespace poni::core {

using std::chrono::microseconds;

RateLimiter::RateLimiter(boost::asio::any_io_executor ex, const Clock& clock) : ex_(std::move(ex)), clock_(clock) {}

void RateLimiter::configure(std::string api, Limit limit) {
    if (limit.per_minute <= 0 || limit.burst < 1) throw std::logic_error("rate limit must be positive");
    const auto interval = microseconds(static_cast<microseconds::rep>(60'000'000.0 / limit.per_minute));
    const auto tolerance = microseconds(static_cast<microseconds::rep>(static_cast<double>(interval.count()) * (limit.burst - 1)));
    buckets_.insert_or_assign(std::move(api), Bucket{interval, tolerance, TimePoint{}});
}

RateLimiter::Bucket& RateLimiter::bucket(std::string_view api) {
    const auto it = buckets_.find(api);
    if (it == buckets_.end()) throw std::logic_error(std::format("no rate limit configured for '{}'", api));
    return it->second;
}

// Generic cell rate algorithm: `next` is when the following request would be
// due if all went out at the steady rate; a request may run up to `tolerance`
// ahead of that.
microseconds RateLimiter::reserve(std::string_view api) {
    auto& b = bucket(api);
    const auto now = clock_.now();
    const auto allowed_at = b.next - b.tolerance;
    const auto wait = std::max(microseconds::zero(), std::chrono::duration_cast<microseconds>(allowed_at - now));
    b.next = std::max(b.next, now) + b.interval;
    return wait;
}

boost::asio::awaitable<void> RateLimiter::acquire(std::string_view api) {
    const auto wait = reserve(api);
    if (wait <= microseconds::zero()) co_return;
    boost::asio::steady_timer timer(ex_);
    timer.expires_after(wait);
    co_await timer.async_wait(boost::asio::use_awaitable);
}

void RateLimiter::pause(std::string_view api, std::chrono::seconds duration) {
    auto& b = bucket(api);
    b.next = std::max(b.next, clock_.now() + duration + b.tolerance);
}

}  // namespace poni::core
