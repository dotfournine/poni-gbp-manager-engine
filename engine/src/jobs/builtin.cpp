#include "jobs/builtin.hpp"

#include "core/log.hpp"
#include "db/repo_auth.hpp"

namespace poni::jobs {

void install_builtin(Registry& registry, db::Database& db, JobQueue& queue, const BuiltinOptions& options) {
    registry.every(std::string(kHeartbeatKind), options.heartbeat_interval, 5, [](const Job& job) -> boost::asio::awaitable<void> {
        log::info("heartbeat", {{"job_id", job.id}});
        co_return;
    });
    registry.every(std::string(kHousekeepingKind), std::chrono::hours(1), 5,
                   [&db, &queue, options](const Job&) -> boost::asio::awaitable<void> {
                       const auto finished = co_await queue.purge_succeeded(options.job_retention);
                       log::info("purged finished jobs", {{"deleted", finished}});
                       const auto sessions = co_await db.tx([now = queue.clock().now(), lifetimes = options.sessions](pqxx::work& tx) {
                           repo::purge_app_codes(tx, now);
                           return repo::purge_sessions(tx, now, lifetimes);
                       });
                       if (sessions > 0) log::info("purged ended sessions", {{"deleted", sessions}});
                   });
}

}  // namespace poni::jobs
