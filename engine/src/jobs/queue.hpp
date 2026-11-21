// Job queue in PostgreSQL (D-005). A job is a row in `jobs`:
//
//   queued --claim--> running --complete--> succeeded
//                        |
//                        +--fail--> queued (run_at moved out by the backoff)
//                        +--fail, attempts used up--> failed
//
// Workers claim with FOR UPDATE SKIP LOCKED, so several can poll one table
// without blocking each other. Delivery is at-least-once: a job can run again
// after a crash or an expired lock, so handlers must be safe to repeat.
#pragma once

#include "core/clock.hpp"
#include "db/database.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/json/value.hpp>
#include <pqxx/transaction_base>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace poni::jobs {

struct NewJob {
    std::string kind;
    boost::json::value payload = boost::json::object{};
    // Earliest start; empty means now.
    std::optional<core::TimePoint> run_at;
    // At most one job exists per (kind, idempotency_key); enqueueing a second is a no-op.
    std::optional<std::string> idempotency_key;
    int max_attempts = 5;
};

struct Job {
    std::int64_t id = 0;
    std::string kind;
    boost::json::value payload;
    int attempts = 0;  // including the current one
    int max_attempts = 0;

    // The id the payload holds under `key` ("draft_id"). Throws if it holds none.
    std::int64_t payload_id(std::string_view key) const { return payload.at(key).to_number<std::int64_t>(); }
    // This run is the job's last: a failure now is recorded, not retried.
    bool last_attempt() const { return attempts >= max_attempts; }
};

enum class FailOutcome { retry_scheduled, failed_permanently, lock_lost };

// Told about a job that has used up its attempts (FR-7: failing jobs are a
// system problem), with its last error. What it throws is logged and otherwise ignored.
using FailureHook = std::function<boost::asio::awaitable<void>(Job job, std::string error)>;

// Tells `hook` (if any) that `job` failed for good; logs what the hook throws.
boost::asio::awaitable<void> report_permanent_failure(const FailureHook& hook, Job job, std::string error);

// What requeue_stale did: how many running jobs whose lock expired it took
// back, and those of them that had no attempt left and failed.
struct Requeued {
    std::size_t count = 0;
    std::vector<Job> failed;
};

// The error a job whose lock expired is left with.
inline constexpr std::string_view kLockExpired = "lock expired: the worker stopped or the job ran too long";

// Blocking queue operations on a transaction the caller owns. Use these to
// enqueue a job in the same transaction as the change that needs it.
namespace sql {

// Returns the new job's id, or nothing if a job with the same kind and
// idempotency key already exists.
std::optional<std::int64_t> enqueue(pqxx::transaction_base& tx, const NewJob& job, core::TimePoint now);

// Claims the oldest job that is due, or nothing.
std::optional<Job> claim(pqxx::transaction_base& tx, const std::string& worker_id, core::TimePoint now);

// False if the job is no longer this worker's (its lock expired and it was claimed again).
bool complete(pqxx::transaction_base& tx, const Job& job, core::TimePoint now);

FailOutcome fail(pqxx::transaction_base& tx, const Job& job, const std::string& error, core::TimePoint now);

// Puts running jobs whose lock is older than `lock_timeout` back in the queue
// (or fails them if their attempts are used up).
Requeued requeue_stale(pqxx::transaction_base& tx, std::chrono::seconds lock_timeout, core::TimePoint now);

// Deletes succeeded jobs that finished before `cutoff`. Returns how many.
std::size_t purge_succeeded(pqxx::transaction_base& tx, core::TimePoint cutoff);

}  // namespace sql

// Coroutine interface: each call is one transaction on the database pool.
class JobQueue {
public:
    JobQueue(db::Database& db, const core::Clock& clock) : db_(db), clock_(clock) {}

    boost::asio::awaitable<std::optional<std::int64_t>> enqueue(NewJob job);
    boost::asio::awaitable<std::optional<Job>> claim(std::string worker_id);
    boost::asio::awaitable<bool> complete(Job job);
    boost::asio::awaitable<FailOutcome> fail(Job job, std::string error);
    boost::asio::awaitable<Requeued> requeue_stale(std::chrono::seconds lock_timeout);
    boost::asio::awaitable<std::size_t> purge_succeeded(std::chrono::seconds older_than);

    const core::Clock& clock() const { return clock_; }

private:
    db::Database& db_;
    const core::Clock& clock_;
};

}  // namespace poni::jobs
