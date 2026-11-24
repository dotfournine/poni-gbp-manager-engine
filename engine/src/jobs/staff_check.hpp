// The staff check (D-106 points 4 to 7): every few minutes the Workspace
// directory's list of users decides who may use the engine.
//
// A run reads the staff first, then the list once, every page, through the
// Workspace administrator's connection. It judges nobody unless the list was
// read whole and holds the connected administrator as an active
// administrator; then a staff member with a Google id whom the list does not
// show as active is disabled, and a disabled one it holds active is enabled
// (repo::apply_staff_check, the only writer of users.status). The job's one
// request is the connector's GET: nothing is written at Google.
#pragma once

#include "connectors/workspace/directory.hpp"
#include "core/clock.hpp"
#include "db/database.hpp"
#include "jobs/registry.hpp"

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <string_view>

namespace poni::jobs {

inline constexpr std::string_view kStaffCheckKind = "workspace.check_staff";

class StaffCheck {
public:
    StaffCheck(db::Database& db, const core::Clock& clock, workspace::Directory& directory, std::chrono::seconds interval)
        : db_(db), clock_(clock), directory_(directory), interval_(interval) {}

    // One run. Nothing connected: nothing is read or recorded. A list that
    // could not be used is recorded for the status page and changes nobody.
    boost::asio::awaitable<void> run();

    std::chrono::seconds interval() const { return interval_; }

private:
    db::Database& db_;
    const core::Clock& clock_;
    workspace::Directory& directory_;
    std::chrono::seconds interval_;
};

// One attempt per slot, as the other polling jobs: the next run repeats it.
void install(Registry& registry, StaffCheck& check);

}  // namespace poni::jobs
