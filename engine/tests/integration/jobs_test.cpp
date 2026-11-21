// The job queue against a real PostgreSQL: enqueue, claim, retry with
// backoff, idempotency, SKIP LOCKED, stale locks, and a job travelling the
// whole path scheduler -> queue -> worker -> handler.

#include "engine_fixture.hpp"
#include "jobs/backoff.hpp"
#include "jobs/builtin.hpp"
#include "jobs/queue.hpp"
#include "jobs/scheduler.hpp"
#include "jobs/worker.hpp"
#include "support.hpp"

#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <gmock/gmock.h>
#include <pqxx/pqxx>

#include <stdexcept>

using namespace poni;
using namespace std::chrono_literals;
namespace asio = boost::asio;

namespace {

struct JobRow {
    std::string status;
    int attempts = 0;
    std::optional<std::string> last_error;
    std::optional<std::string> locked_by;
};

class Jobs : public test::EngineTest {
protected:
    JobRow row(std::int64_t id) {
        pqxx::work tx{conn()};
        const auto r = tx.exec("SELECT status, attempts, last_error, locked_by FROM jobs WHERE id = $1",
                               pqxx::params{id})
                           .one_row();
        return {r[0].as<std::string>(), r[1].as<int>(), r[2].as<std::optional<std::string>>(),
                r[3].as<std::optional<std::string>>()};
    }

    // Jobs that match `where`.
    long jobs_where(const std::string& where) { return count("jobs WHERE " + where); }

    jobs::Worker make_worker(const jobs::Registry& registry, std::string id = "test-worker") {
        return jobs::Worker(io_.get_executor(), queue(), registry, {.id = std::move(id), .poll_interval = 10ms});
    }

};

}  // namespace

TEST_F(Jobs, EnqueueClaimComplete) {
    const auto id = await(queue().enqueue({.kind = "test.echo", .payload = {{"n", 7}, {"name", "路梦"}}}));
    ASSERT_TRUE(id);
    EXPECT_EQ(row(*id).status, "queued");

    const auto job = await(queue().claim("w1"));
    ASSERT_TRUE(job);
    EXPECT_EQ(job->id, *id);
    EXPECT_EQ(job->kind, "test.echo");
    EXPECT_EQ(job->payload.at("n").as_int64(), 7);
    EXPECT_EQ(job->payload.at("name").as_string(), "路梦");
    EXPECT_EQ(job->attempts, 1);
    EXPECT_EQ(row(*id).status, "running");
    EXPECT_EQ(row(*id).locked_by, "w1");

    EXPECT_FALSE(await(queue().claim("w2"))) << "a running job must not be claimed again";

    EXPECT_TRUE(await(queue().complete(*job)));
    EXPECT_EQ(row(*id).status, "succeeded");
    EXPECT_FALSE(row(*id).locked_by);
}

TEST_F(Jobs, JobsRunInOrderOfRunAt) {
    const auto later = await(queue().enqueue({.kind = "k", .run_at = test::t0() - 1s}));
    const auto sooner = await(queue().enqueue({.kind = "k", .run_at = test::t0() - 10s}));
    EXPECT_EQ(await(queue().claim("w"))->id, *sooner);
    EXPECT_EQ(await(queue().claim("w"))->id, *later);
}

TEST_F(Jobs, DelayedJobWaitsForRunAt) {
    const auto id = await(queue().enqueue({.kind = "k", .run_at = test::t0() + 10min}));
    EXPECT_FALSE(await(queue().claim("w")));

    clock_.advance(10min - 1s);
    EXPECT_FALSE(await(queue().claim("w")));

    clock_.advance(1s);
    const auto job = await(queue().claim("w"));
    ASSERT_TRUE(job);
    EXPECT_EQ(job->id, *id);
}

TEST_F(Jobs, IdempotencyKeyAllowsOneJobPerKind) {
    const auto first = await(queue().enqueue({.kind = "post_reply", .idempotency_key = "review-1"}));
    const auto again = await(queue().enqueue({.kind = "post_reply", .idempotency_key = "review-1"}));
    const auto other_key = await(queue().enqueue({.kind = "post_reply", .idempotency_key = "review-2"}));
    const auto other_kind = await(queue().enqueue({.kind = "ingest_review", .idempotency_key = "review-1"}));
    EXPECT_TRUE(first);
    EXPECT_FALSE(again) << "same kind and key: no second job";
    EXPECT_TRUE(other_key);
    EXPECT_TRUE(other_kind);

    // Still one job after the first has finished: the key outlives the run.
    await(queue().complete(*await(queue().claim("w"))));
    EXPECT_FALSE(await(queue().enqueue({.kind = "post_reply", .idempotency_key = "review-1"})));

    // Jobs without a key are never deduplicated.
    EXPECT_TRUE(await(queue().enqueue({.kind = "post_reply"})));
    EXPECT_TRUE(await(queue().enqueue({.kind = "post_reply"})));
}

TEST_F(Jobs, FailedJobIsRetriedWithBackoffThenFailsPermanently) {
    const auto id = await(queue().enqueue({.kind = "k", .max_attempts = 3}));

    for (int attempt = 1; attempt <= 2; ++attempt) {
        const auto job = await(queue().claim("w"));
        ASSERT_TRUE(job) << "attempt " << attempt;
        EXPECT_EQ(job->attempts, attempt);
        EXPECT_EQ(await(queue().fail(*job, "boom")), jobs::FailOutcome::retry_scheduled);
        EXPECT_EQ(row(*id).status, "queued");
        EXPECT_EQ(row(*id).last_error, "boom");

        // Not due until the backoff has passed.
        clock_.advance(jobs::retry_delay(attempt) - 1s);
        EXPECT_FALSE(await(queue().claim("w"))) << "attempt " << attempt;
        clock_.advance(1s);
    }

    const auto last = await(queue().claim("w"));
    ASSERT_TRUE(last);
    EXPECT_EQ(last->attempts, 3);
    EXPECT_EQ(await(queue().fail(*last, "still broken")), jobs::FailOutcome::failed_permanently);
    EXPECT_EQ(row(*id).status, "failed");
    EXPECT_EQ(row(*id).last_error, "still broken");

    clock_.advance(24h);
    EXPECT_FALSE(await(queue().claim("w"))) << "a failed job never runs again";
}

TEST_F(Jobs, SkipLockedGivesConcurrentWorkersDifferentJobs) {
    const auto a = await(queue().enqueue({.kind = "k"}));
    const auto b = await(queue().enqueue({.kind = "k"}));

    // Two workers claim inside transactions that are open at the same time.
    pqxx::connection conn1{url()};
    pqxx::connection conn2{url()};
    pqxx::work tx1{conn1};
    pqxx::work tx2{conn2};
    const auto job1 = jobs::sql::claim(tx1, "w1", clock_.now());
    const auto job2 = jobs::sql::claim(tx2, "w2", clock_.now());  // must not wait for tx1
    ASSERT_TRUE(job1);
    ASSERT_TRUE(job2);
    EXPECT_EQ(job1->id, *a);
    EXPECT_EQ(job2->id, *b);

    pqxx::connection conn3{url()};
    pqxx::work tx3{conn3};
    EXPECT_FALSE(jobs::sql::claim(tx3, "w3", clock_.now())) << "both jobs are locked";

    tx1.commit();
    tx2.commit();
}

TEST_F(Jobs, EnqueueJoinsTheCallersTransaction) {
    pqxx::connection conn{url()};
    {
        pqxx::work tx{conn};
        ASSERT_TRUE(jobs::sql::enqueue(tx, {.kind = "k"}, clock_.now()));
        // No commit: the change that wanted the job was rolled back.
    }
    EXPECT_EQ(jobs_where("true"), 0);
}

TEST_F(Jobs, StaleLockIsRequeuedAndTheOldWorkerIsFencedOff) {
    const auto id = await(queue().enqueue({.kind = "k"}));
    const auto first = await(queue().claim("crashed-worker"));
    ASSERT_TRUE(first);

    clock_.advance(14min);
    EXPECT_EQ(await(queue().requeue_stale(15min)).count, 0u);
    clock_.advance(2min);
    EXPECT_EQ(await(queue().requeue_stale(15min)).count, 1u);
    EXPECT_EQ(row(*id).status, "queued");

    const auto second = await(queue().claim("new-worker"));
    ASSERT_TRUE(second);
    EXPECT_EQ(second->attempts, 2);

    // The first worker comes back late: its result must not be recorded.
    EXPECT_FALSE(await(queue().complete(*first)));
    EXPECT_EQ(await(queue().fail(*first, "late")), jobs::FailOutcome::lock_lost);
    EXPECT_EQ(row(*id).status, "running");
    EXPECT_EQ(row(*id).locked_by, "new-worker");

    EXPECT_TRUE(await(queue().complete(*second)));
}

TEST_F(Jobs, StaleJobOutOfAttemptsFails) {
    const auto id = await(queue().enqueue({.kind = "k", .max_attempts = 1}));
    ASSERT_TRUE(await(queue().claim("w")));
    clock_.advance(1h);
    const auto requeued = await(queue().requeue_stale(15min));
    EXPECT_EQ(requeued.count, 1u);
    ASSERT_EQ(requeued.failed.size(), 1u);
    EXPECT_EQ(requeued.failed[0].id, *id);
    EXPECT_EQ(row(*id).status, "failed");
}

TEST_F(Jobs, PurgeDeletesOnlyOldSucceededJobs) {
    const auto old_done = await(queue().enqueue({.kind = "k"}));
    await(queue().complete(*await(queue().claim("w"))));
    const auto old_failed = await(queue().enqueue({.kind = "k", .max_attempts = 1}));
    await(queue().fail(*await(queue().claim("w")), "x"));

    clock_.advance(8 * 24h);
    const auto fresh_done = await(queue().enqueue({.kind = "k"}));
    await(queue().complete(*await(queue().claim("w"))));
    const auto queued = await(queue().enqueue({.kind = "k"}));

    EXPECT_EQ(await(queue().purge_succeeded(7 * 24h)), 1u);
    EXPECT_EQ(jobs_where(std::format("id = {}", *old_done)), 0);
    EXPECT_EQ(jobs_where(std::format("id IN ({}, {}, {})", *old_failed, *fresh_done, *queued)), 3);
}

TEST_F(Jobs, WorkerRunsHandlerAndRecordsSuccess) {
    std::vector<std::string> seen;
    jobs::Registry registry;
    registry.add("test.greet", [&](const jobs::Job& job) -> asio::awaitable<void> {
        seen.emplace_back(job.payload.at("name").as_string());
        co_return;
    });
    auto worker = make_worker(registry);

    EXPECT_FALSE(await(worker.run_once())) << "nothing queued yet";
    const auto id = await(queue().enqueue({.kind = "test.greet", .payload = {{"name", "Gangnam"}}}));
    EXPECT_TRUE(await(worker.run_once()));
    EXPECT_THAT(seen, ::testing::ElementsAre("Gangnam"));
    EXPECT_EQ(row(*id).status, "succeeded");
    EXPECT_EQ(row(*id).attempts, 1);
    EXPECT_FALSE(await(worker.run_once()));
}

TEST_F(Jobs, WorkerRetriesAThrowingHandlerUntilItSucceeds) {
    int calls = 0;
    jobs::Registry registry;
    registry.add("test.flaky", [&](const jobs::Job&) -> asio::awaitable<void> {
        if (++calls < 3) throw std::runtime_error("upstream said 503");
        co_return;
    });
    auto worker = make_worker(registry);
    const auto id = await(queue().enqueue({.kind = "test.flaky"}));

    EXPECT_TRUE(await(worker.run_once()));
    EXPECT_EQ(row(*id).status, "queued");
    EXPECT_EQ(row(*id).last_error, "upstream said 503");
    EXPECT_FALSE(await(worker.run_once())) << "backoff not over";

    clock_.advance(jobs::retry_delay(1));
    EXPECT_TRUE(await(worker.run_once()));
    clock_.advance(jobs::retry_delay(2));
    EXPECT_TRUE(await(worker.run_once()));

    EXPECT_EQ(calls, 3);
    EXPECT_EQ(row(*id).status, "succeeded");
    EXPECT_EQ(row(*id).attempts, 3);
    EXPECT_FALSE(row(*id).last_error);
}

TEST_F(Jobs, UnknownKindFailsWithAClearError) {
    const jobs::Registry registry;
    auto worker = make_worker(registry);
    const auto id = await(queue().enqueue({.kind = "test.unknown", .max_attempts = 1}));
    EXPECT_TRUE(await(worker.run_once()));
    EXPECT_EQ(row(*id).status, "failed");
    EXPECT_THAT(row(*id).last_error.value_or(""), ::testing::HasSubstr("no handler registered"));
}

// A job runs through the whole path. The
// scheduler enqueues the built-in heartbeat, a worker claims it, the handler
// runs and the job is recorded as succeeded -- once per slot, however often
// the scheduler ticks.
TEST_F(Jobs, HeartbeatRunsFromSchedulerThroughQueueToWorker) {
    const jobs::BuiltinOptions options{.heartbeat_interval = 300s};
    jobs::Registry registry;
    jobs::install_builtin(registry, db(), queue(), options);
    jobs::Scheduler scheduler(io_.get_executor(), queue(), registry.recurring(), {});
    auto worker = make_worker(registry);

    EXPECT_EQ(await(scheduler.tick()), 2u) << "heartbeat and housekeeping for the current slot";
    EXPECT_EQ(await(scheduler.tick()), 0u) << "same slot: nothing new";

    // A second engine process (or a restart) must not duplicate the slot's jobs.
    jobs::Scheduler other(io_.get_executor(), queue(), registry.recurring(), {});
    EXPECT_EQ(await(other.tick()), 0u);

    while (await(worker.run_once())) {}
    EXPECT_EQ(jobs_where("kind = 'system.heartbeat' AND status = 'succeeded'"), 1);
    EXPECT_EQ(jobs_where("kind = 'system.housekeeping' AND status = 'succeeded'"), 1);
    EXPECT_EQ(jobs_where("status <> 'succeeded'"), 0);

    // Next heartbeat slot.
    clock_.advance(300s);
    EXPECT_EQ(await(scheduler.tick()), 1u);
    while (await(worker.run_once())) {}
    EXPECT_EQ(jobs_where("kind = 'system.heartbeat' AND status = 'succeeded'"), 2);
}

// The real loops with real timers: run() polls, picks the job up and stops on request.
TEST_F(Jobs, WorkerAndSchedulerLoopsRunAndStop) {
    core::SystemClock system_clock;
    jobs::JobQueue queue(db(), system_clock);
    const jobs::BuiltinOptions options{.heartbeat_interval = 300s};
    jobs::Registry registry;
    jobs::install_builtin(registry, db(), queue, options);

    bool ran = false;
    jobs::Worker worker(io_.get_executor(), queue, registry, {.id = "loop-worker", .poll_interval = 10ms});
    jobs::Scheduler scheduler(io_.get_executor(), queue, registry.recurring(), {.tick_interval = 10ms});
    registry.add("test.stop", [&](const jobs::Job&) -> asio::awaitable<void> {
        ran = true;
        worker.stop();
        scheduler.stop();
        co_return;
    });

    asio::co_spawn(io_, worker.run(), asio::detached);
    asio::co_spawn(io_, scheduler.run(), asio::detached);
    asio::co_spawn(
        io_,
        [&]() -> asio::awaitable<void> {
            // Enqueued after the loops are already polling an empty queue.
            asio::steady_timer timer(io_, 50ms);
            co_await timer.async_wait(asio::use_awaitable);
            co_await queue.enqueue({.kind = "test.stop", .run_at = system_clock.now() + 50ms});
        },
        asio::detached);

    io_.run_for(30s);  // returns as soon as both loops have ended
    EXPECT_TRUE(ran);
    EXPECT_TRUE(io_.stopped()) << "the loops did not stop";
    io_.restart();
    EXPECT_GE(jobs_where("kind = 'system.heartbeat'"), 1);
}
