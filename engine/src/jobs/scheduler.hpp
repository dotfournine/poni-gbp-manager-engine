// Recurring jobs (Registry::every). Time is cut into slots of `interval`
// (aligned to the Unix epoch); once per slot the scheduler enqueues the job
// with the idempotency key "<kind>@<slot start>". The key makes this safe across restarts and
// across several engine processes: the slot's job exists at most once.
//
// The same loop puts jobs with an expired lock back in the queue, and tells
// `on_permanent_failure` about those that had no attempt left.
#pragma once

#include "jobs/queue.hpp"
#include "jobs/registry.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace poni::jobs {

// Start of the slot that contains `now`.
core::TimePoint slot_start(core::TimePoint now, std::chrono::seconds interval);

std::string slot_key(const Recurring& recurring, core::TimePoint slot);

class Scheduler {
public:
    struct Options {
        std::chrono::milliseconds tick_interval{5000};
        // Longer than the longest job (core::Config::job_lock_timeout says why).
        std::chrono::seconds job_lock_timeout{2400};
        FailureHook on_permanent_failure;
    };

    Scheduler(boost::asio::any_io_executor ex, JobQueue& queue, std::vector<Recurring> recurring, Options options);

    // One pass: enqueue every recurring job whose current slot has none yet,
    // and requeue stale jobs. Returns the number of jobs enqueued.
    boost::asio::awaitable<std::size_t> tick();

    boost::asio::awaitable<void> run();
    void stop();

private:
    struct Entry {
        Recurring recurring;
        std::optional<core::TimePoint> last_slot;  // already enqueued by this process
    };

    JobQueue& queue_;
    std::vector<Entry> entries_;
    Options options_;
    boost::asio::steady_timer timer_;
    bool stopping_ = false;
};

}  // namespace poni::jobs
