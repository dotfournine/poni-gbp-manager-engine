// The users as the API lists them: every one with its status, chosen by
// status; a person of the Workspace directory made a user before their first
// sign-in (D-106 point 8); and the staff check's side of them (D-106 points 4
// to 7), the one function that writes a user's status. Sign-in itself
// (creating a user at sign-in, sessions, the app's codes) is in repo_auth.
// Blocking calls on a transaction the caller owns.
#pragma once

#include "core/clock.hpp"
#include "domain/models.hpp"

#include <boost/describe/class.hpp>
#include <pqxx/pqxx>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace poni::audit {
class Writer;
}
namespace poni::workspace {
struct DirectoryUser;
}

namespace poni::repo {

inline constexpr std::array<std::string_view, 3> kUserStatusFilters{"active", "disabled", "all"};

// The users of one status (`active`: everyone who can be assigned), or of
// both (`all`), by name. `status` is one of kUserStatusFilters.
model::UserList list_users(pqxx::transaction_base& tx, std::string_view status);

// ---- people from the directory (D-106 point 8) ---------------------------------

// The engine's users for these Directory ids: google_sub -> users.id.
std::map<std::string, std::int64_t> user_ids_by_google_sub(pqxx::transaction_base& tx, const std::vector<std::string>& ids);

struct CreatedUser {
    model::User user;
    bool created = false;
};
// Makes the person a user who has not signed in yet, or returns the user they already are. `person` is the entry the
// engine itself just read from the directory's list, never anything a request said: their address and name come from it,
// and their Directory id becomes the row's google_sub, which their first sign-in carries as its `sub`.
CreatedUser create_directory_user(pqxx::transaction_base& tx, audit::Writer& audit, const workspace::DirectoryUser& person);

// ---- the staff check (D-106) ---------------------------------------------------

struct StaffMember {
    std::int64_t id = 0;
    std::string google_sub;
};
BOOST_DESCRIBE_STRUCT(StaffMember, (), (id, google_sub))

struct StaffToCheck {
    std::string credential_updated_at;  // which connection this run reads through
    std::string admin_email;            // the connected administrator's address
    std::vector<StaffMember> staff;     // every user, active or disabled, by id
};
// Nothing while no Workspace administrator is connected, or while the connection has no address.
std::optional<StaffToCheck> staff_to_check(pqxx::transaction_base& tx);

// What the directory's list says about one user: holds their id as active,
// suspended or archived, or does not hold it.
enum class DirectoryAnswer { present, suspended, archived, missing };
struct StaffFinding {
    std::int64_t user_id = 0;
    DirectoryAnswer answer = DirectoryAnswer::present;
    std::string full_name;  // the directory's; empty for `missing`, or when Google names none
};
struct StaffCheckResult {
    int disabled = 0;
    int enabled = 0;
    bool stale = false;  // another connection (or none) is stored since the list was read: nothing changed
};
// Carries one run's findings out (D-106 points 4 to 7). The only function that writes users.status.
// A disable ends the person's codes, sessions and devices, with an audit entry and a notification;
// an enable creates no session. Nothing happens unless the stored connection is still the one whose
// `updated_at` the run read the list through.
StaffCheckResult apply_staff_check(pqxx::transaction_base& tx, std::string_view credential_updated_at,
                                   const std::vector<StaffFinding>& findings, core::TimePoint now);

// Why a run could not use the list: the two cases tell staff different things.
enum class StaffCheckProblem { unreadable, no_administrator };
struct StaffCheckError {
    StaffCheckProblem problem = StaffCheckProblem::unreadable;
    std::string message;
};
// What a run came to, for the status page; tells staff once a day when the list could not be used.
// A stale result records nothing.
void record_staff_check(pqxx::transaction_base& tx, const StaffCheckResult& result, std::size_t listed,
                        const std::optional<StaffCheckError>& error, core::TimePoint now);

}  // namespace poni::repo
