// Jobs the engine itself needs.
#pragma once

#include "db/database.hpp"
#include "domain/session.hpp"
#include "jobs/queue.hpp"
#include "jobs/registry.hpp"

#include <chrono>
#include <string_view>

namespace poni::jobs {

// Does nothing but log: proves that the scheduler, queue and workers are alive.
inline constexpr std::string_view kHeartbeatKind = "system.heartbeat";
// Hourly: deletes succeeded jobs older than the retention period, sessions that ended, and
// sign-in codes of the phone app that nobody exchanged.
inline constexpr std::string_view kHousekeepingKind = "system.housekeeping";

struct BuiltinOptions {
    std::chrono::seconds heartbeat_interval{300};
    std::chrono::hours job_retention{24 * 7};
    // Sessions that ended, each by its own kind's lifetime, are deleted.
    domain::SessionLifetimes sessions;
};

// `db` and `queue` must outlive the registry.
void install_builtin(Registry& registry, db::Database& db, JobQueue& queue, const BuiltinOptions& options);

}  // namespace poni::jobs
