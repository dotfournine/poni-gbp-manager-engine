#include "jobs/queue.hpp"

#include "core/log.hpp"
#include "jobs/backoff.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <pqxx/transaction>

#include <utility>

namespace poni::jobs {

namespace {

// A row of `RETURNING id, kind, payload::text, attempts, max_attempts`.
Job job_of(const pqxx::row_ref& row) {
    return Job{
        .id = row[0].as<std::int64_t>(),
        .kind = row[1].as<std::string>(),
        .payload = boost::json::parse(row[2].as<std::string>()),
        .attempts = row[3].as<int>(),
        .max_attempts = row[4].as<int>(),
    };
}

}  // namespace

namespace sql {

std::optional<std::int64_t> enqueue(pqxx::transaction_base& tx, const NewJob& job, core::TimePoint now) {
    const auto rows = tx.exec(
        R"sql(
        INSERT INTO jobs (kind, payload, run_at, max_attempts, idempotency_key, created_at, updated_at)
        VALUES ($1, $2::jsonb, $3::timestamptz, $4, $5, $6::timestamptz, $6::timestamptz)
        ON CONFLICT (kind, idempotency_key) WHERE idempotency_key IS NOT NULL DO NOTHING
        RETURNING id)sql",
        pqxx::params{job.kind, boost::json::serialize(job.payload), db::to_timestamptz(job.run_at.value_or(now)),
                     job.max_attempts, job.idempotency_key, db::to_timestamptz(now)});
    if (rows.empty()) return std::nullopt;
    return rows[0][0].as<std::int64_t>();
}

std::optional<Job> claim(pqxx::transaction_base& tx, const std::string& worker_id, core::TimePoint now) {
    const auto rows = tx.exec(
        R"sql(
        UPDATE jobs
           SET status = 'running', attempts = attempts + 1,
               locked_at = $2::timestamptz, locked_by = $1, updated_at = $2::timestamptz
         WHERE id = (SELECT id FROM jobs
                      WHERE status = 'queued' AND run_at <= $2::timestamptz
                      ORDER BY run_at, id
                      FOR UPDATE SKIP LOCKED
                      LIMIT 1)
        RETURNING id, kind, payload::text, attempts, max_attempts)sql",
        pqxx::params{worker_id, db::to_timestamptz(now)});
    if (rows.empty()) return std::nullopt;
    return job_of(rows[0]);
}

bool complete(pqxx::transaction_base& tx, const Job& job, core::TimePoint now) {
    const auto result = tx.exec(
        R"sql(
        UPDATE jobs
           SET status = 'succeeded', finished_at = $3::timestamptz, updated_at = $3::timestamptz,
               locked_at = NULL, locked_by = NULL, last_error = NULL
         WHERE id = $1 AND status = 'running' AND attempts = $2)sql",
        pqxx::params{job.id, job.attempts, db::to_timestamptz(now)});
    return result.affected_rows() == 1;
}

FailOutcome fail(pqxx::transaction_base& tx, const Job& job, const std::string& error, core::TimePoint now) {
    const bool last = job.last_attempt();
    const auto retry_at = now + retry_delay(job.attempts);
    const auto result = tx.exec(
        R"sql(
        UPDATE jobs
           SET status = CASE WHEN $3 THEN 'failed' ELSE 'queued' END,
               run_at = CASE WHEN $3 THEN run_at ELSE $4::timestamptz END,
               finished_at = CASE WHEN $3 THEN $5::timestamptz END,
               last_error = $6, locked_at = NULL, locked_by = NULL, updated_at = $5::timestamptz
         WHERE id = $1 AND status = 'running' AND attempts = $2)sql",
        pqxx::params{job.id, job.attempts, last, db::to_timestamptz(retry_at), db::to_timestamptz(now), error});
    if (result.affected_rows() != 1) return FailOutcome::lock_lost;
    return last ? FailOutcome::failed_permanently : FailOutcome::retry_scheduled;
}

Requeued requeue_stale(pqxx::transaction_base& tx, std::chrono::seconds lock_timeout, core::TimePoint now) {
    const auto rows = tx.exec(
        R"sql(
        UPDATE jobs
           SET status = CASE WHEN attempts >= max_attempts THEN 'failed' ELSE 'queued' END,
               finished_at = CASE WHEN attempts >= max_attempts THEN $2::timestamptz END,
               last_error = $3,
               locked_at = NULL, locked_by = NULL, updated_at = $2::timestamptz
         WHERE status = 'running' AND locked_at < $1::timestamptz
        RETURNING id, kind, payload::text, attempts, max_attempts, status = 'failed')sql",
        pqxx::params{db::to_timestamptz(now - lock_timeout), db::to_timestamptz(now), kLockExpired});
    Requeued out{.count = static_cast<std::size_t>(rows.size()), .failed = {}};
    for (const auto& row : rows) {
        if (row[5].as<bool>()) out.failed.push_back(job_of(row));
    }
    return out;
}

std::size_t purge_succeeded(pqxx::transaction_base& tx, core::TimePoint cutoff) {
    const auto result = tx.exec("DELETE FROM jobs WHERE status = 'succeeded' AND finished_at < $1::timestamptz",
                                pqxx::params{db::to_timestamptz(cutoff)});
    return static_cast<std::size_t>(result.affected_rows());
}

}  // namespace sql

boost::asio::awaitable<void> report_permanent_failure(const FailureHook& hook, Job job, std::string error) {
    if (!hook) co_return;
    const auto id = job.id;
    std::optional<std::string> hook_error;
    try {
        co_await hook(std::move(job), std::move(error));
    } catch (const std::exception& e) {
        hook_error = e.what();
    }
    if (hook_error) log::warn("the job-failure hook failed", {{"job_id", id}, {"error", *hook_error}});
}

boost::asio::awaitable<std::optional<std::int64_t>> JobQueue::enqueue(NewJob job) {
    co_return co_await db_.tx([job = std::move(job), now = clock_.now()](pqxx::work& tx) { return sql::enqueue(tx, job, now); });
}

boost::asio::awaitable<std::optional<Job>> JobQueue::claim(std::string worker_id) {
    co_return co_await db_.tx([worker_id = std::move(worker_id), now = clock_.now()](pqxx::work& tx) { return sql::claim(tx, worker_id, now); });
}

boost::asio::awaitable<bool> JobQueue::complete(Job job) {
    co_return co_await db_.tx([job = std::move(job), now = clock_.now()](pqxx::work& tx) { return sql::complete(tx, job, now); });
}

boost::asio::awaitable<FailOutcome> JobQueue::fail(Job job, std::string error) {
    co_return co_await db_.tx([job = std::move(job), error = std::move(error), now = clock_.now()](pqxx::work& tx) {
        return sql::fail(tx, job, error, now);
    });
}

boost::asio::awaitable<Requeued> JobQueue::requeue_stale(std::chrono::seconds lock_timeout) {
    co_return co_await db_.tx([lock_timeout, now = clock_.now()](pqxx::work& tx) { return sql::requeue_stale(tx, lock_timeout, now); });
}

boost::asio::awaitable<std::size_t> JobQueue::purge_succeeded(std::chrono::seconds older_than) {
    co_return co_await db_.tx([cutoff = clock_.now() - older_than](pqxx::work& tx) { return sql::purge_succeeded(tx, cutoff); });
}

}  // namespace poni::jobs
