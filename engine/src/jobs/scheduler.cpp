#include "jobs/scheduler.hpp"

#include "core/log.hpp"

#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <exception>
#include <format>
#include <utility>

namespace poni::jobs {

namespace asio = boost::asio;

core::TimePoint slot_start(core::TimePoint now, std::chrono::seconds interval) {
    const auto since_epoch = std::chrono::floor<std::chrono::seconds>(now.time_since_epoch());
    return core::TimePoint(since_epoch - since_epoch % interval);
}

std::string slot_key(const Recurring& recurring, core::TimePoint slot) {
    return std::format("{}@{}", recurring.kind,
                       std::chrono::duration_cast<std::chrono::seconds>(slot.time_since_epoch()).count());
}

Scheduler::Scheduler(asio::any_io_executor ex, JobQueue& queue, std::vector<Recurring> recurring, Options options)
    : queue_(queue), options_(options), timer_(std::move(ex)) {
    for (auto& r : recurring) entries_.push_back({std::move(r), std::nullopt});
}

asio::awaitable<std::size_t> Scheduler::tick() {
    std::size_t enqueued = 0;
    for (auto& entry : entries_) {
        const auto slot = slot_start(queue_.clock().now(), entry.recurring.interval);
        if (entry.last_slot == slot) continue;
        const auto id = co_await queue_.enqueue({
            .kind = entry.recurring.kind,
            .payload = boost::json::object{},
            .run_at = std::nullopt,
            .idempotency_key = slot_key(entry.recurring, slot),
            .max_attempts = entry.recurring.max_attempts,
        });
        entry.last_slot = slot;
        if (id) {
            ++enqueued;
            log::debug("scheduled recurring job", {{"kind", entry.recurring.kind}, {"job_id", *id}});
        }
    }
    auto stale = co_await queue_.requeue_stale(options_.job_lock_timeout);
    if (stale.count > 0) log::warn("requeued jobs whose lock expired", {{"count", stale.count}, {"failed", stale.failed.size()}});
    for (auto& job : stale.failed) {
        log::error("job failed permanently", {{"job_id", job.id}, {"kind", job.kind}, {"error", kLockExpired}});
        co_await report_permanent_failure(options_.on_permanent_failure, std::move(job), std::string(kLockExpired));
    }
    co_return enqueued;
}

asio::awaitable<void> Scheduler::run() {
    while (!stopping_) {
        std::optional<std::string> error;
        try {
            co_await tick();
        } catch (const std::exception& e) {
            error = e.what();
        }
        if (error) log::error("scheduler: tick failed", {{"error", *error}});
        if (stopping_) break;
        timer_.expires_after(options_.tick_interval);
        boost::system::error_code cancelled;
        co_await timer_.async_wait(asio::redirect_error(asio::use_awaitable, cancelled));
    }
}

void Scheduler::stop() {
    stopping_ = true;
    timer_.cancel();
}

}  // namespace poni::jobs
