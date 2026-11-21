// A worker is one loop: claim a job, run its handler, record the result.
// The engine starts several on the same io_context.
#pragma once

#include "jobs/queue.hpp"
#include "jobs/registry.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <functional>
#include <string>

namespace poni::jobs {

class Worker {
public:
    struct Options {
        std::string id;  // stored in jobs.locked_by
        std::chrono::milliseconds poll_interval{1000};
        // Told when a job has used up its attempts.
        FailureHook on_permanent_failure;
    };

    Worker(boost::asio::any_io_executor ex, JobQueue& queue, const Registry& registry, Options options);

    // Claims and runs at most one job. Returns false if nothing was due.
    // Handler failures are recorded on the job; only queue (database) errors throw.
    boost::asio::awaitable<bool> run_once();

    // Loops until stop(): runs jobs back to back and sleeps `poll_interval` when idle.
    boost::asio::awaitable<void> run();

    // The loop ends after the job in progress, if any.
    void stop();

private:
    JobQueue& queue_;
    const Registry& registry_;
    Options options_;
    boost::asio::steady_timer timer_;
    bool stopping_ = false;
};

}  // namespace poni::jobs
