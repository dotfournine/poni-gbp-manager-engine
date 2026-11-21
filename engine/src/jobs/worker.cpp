#include "jobs/worker.hpp"

#include "core/log.hpp"

#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <exception>
#include <format>
#include <optional>
#include <utility>

namespace poni::jobs {

namespace asio = boost::asio;

namespace {

// Pause after a database error before trying again.
constexpr std::chrono::milliseconds kErrorPause{5000};

}  // namespace

Worker::Worker(asio::any_io_executor ex, JobQueue& queue, const Registry& registry, Options options)
    : queue_(queue), registry_(registry), options_(std::move(options)), timer_(std::move(ex)) {}

asio::awaitable<bool> Worker::run_once() {
    auto job = co_await queue_.claim(options_.id);
    if (!job) co_return false;

    const boost::json::object ctx{{"job_id", job->id}, {"kind", job->kind}, {"attempt", job->attempts}};

    // co_await isn't allowed inside a catch block, so the error is carried out of it.
    std::optional<std::string> error;
    if (const Handler* handler = registry_.find(job->kind)) {
        try {
            co_await (*handler)(*job);
        } catch (const std::exception& e) {
            error = e.what();
        } catch (...) {
            error = "unknown error";
        }
    } else {
        error = std::format("no handler registered for job kind '{}'", job->kind);
    }

    if (!error) {
        if (co_await queue_.complete(*job)) {
            // debug: polling jobs run every few seconds; their handlers log what matters.
            log::debug("job succeeded", ctx);
        } else {
            log::warn("job finished after its lock expired; result not recorded", ctx);
        }
        co_return true;
    }

    auto fields = ctx;
    fields["error"] = *error;
    const auto outcome = co_await queue_.fail(*job, *error);
    switch (outcome) {
        case FailOutcome::retry_scheduled: log::warn("job failed; will retry", fields); break;
        case FailOutcome::failed_permanently: log::error("job failed permanently", fields); break;
        case FailOutcome::lock_lost: log::warn("job failed after its lock expired; result not recorded", fields); break;
    }
    if (outcome == FailOutcome::failed_permanently) co_await report_permanent_failure(options_.on_permanent_failure, *job, *error);
    co_return true;
}

asio::awaitable<void> Worker::run() {
    while (!stopping_) {
        auto pause = options_.poll_interval;
        std::optional<std::string> error;
        try {
            if (co_await run_once()) continue;
        } catch (const std::exception& e) {
            error = e.what();
        }
        if (error) {
            log::error("job worker: queue error", {{"worker", options_.id}, {"error", *error}});
            pause = kErrorPause;
        }
        if (stopping_) break;
        timer_.expires_after(pause);
        boost::system::error_code cancelled;
        co_await timer_.async_wait(asio::redirect_error(asio::use_awaitable, cancelled));
    }
}

void Worker::stop() {
    stopping_ = true;
    timer_.cancel();
}

}  // namespace poni::jobs
