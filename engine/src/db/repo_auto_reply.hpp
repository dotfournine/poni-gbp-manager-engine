// Auto reply per location (D-070 to D-072): the switches
// a person flips, the pauses the engine puts on itself, and the list of what
// was posted without a person. Blocking calls on a transaction the caller
// owns: run them through Database::run.
//
// Everything is off after installation. Only update_auto_posting switches a
// location on, and it is reached only from the API, as a signed-in person.
// The engine itself can only switch auto reply *off*: a pause, which a
// person lifts. How a draft gets scheduled and posted is in repo_posting.
#pragma once

#include "core/body.hpp"
#include "core/clock.hpp"
#include "db/audit.hpp"
#include "db/failure.hpp"
#include "db/sql.hpp"
#include "domain/models.hpp"

#include <boost/describe/class.hpp>
#include <pqxx/pqxx>

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace poni::repo {

// Why the engine paused auto reply.
inline constexpr std::string_view kPauseChecksFailing = "checks_failing";
inline constexpr std::string_view kPausePostUnconfirmed = "post_unconfirmed";
inline constexpr std::string_view kPausePostRefused = "post_refused";

// How many drafts in a row with a failed reply check pause auto reply at a
// location is the location's own setting (auto_reply_pause_after_failures, D-076).

// What decides and starts a post no person approved, as the audit log names it (architecture 4.1, step 5).
inline audit::Actor auto_reply() { return audit::Actor::system("auto-reply"); }

// The daily cap counts the automatic replies of the last 24 hours.
inline constexpr std::chrono::hours kAutoReplyCapWindow{24};
// SQL: how many automatic replies were started at the location `location`
// (an expression of the query around it, like "l.id") since the time `since`
// (a placeholder). What the cap counts and what the page shows are this one count.
std::string auto_replies_since(std::string_view location, std::string_view since);

// The body of the PATCH, member for member.
struct AutoPostingPatch {
    std::optional<bool> enabled;
    std::optional<bool> five_star;
    std::optional<bool> four_star;
    std::optional<int> daily_cap;
    std::optional<int> pause_after_failures;
    // Null clears the time zone; left out, it stays as it is.
    core::Nullable<std::string> time_zone;
};
BOOST_DESCRIBE_STRUCT(AutoPostingPatch, (), (enabled, five_star, four_star, daily_cap, pause_after_failures, time_zone))

// The global state and every enabled location.
model::AutoPostingOverview auto_reply_overview(pqxx::transaction_base& tx, core::TimePoint now);

// A person changes a location's auto reply settings. Switching it on is
// refused unless a star rating is chosen and the timing the reply policy
// asks for can be used (a posting window needs a time zone). It does not
// wait for a passing go-live test (D-076): without one nothing is posted by auto reply
// whatever the switch says, and the page shows that. Switching on or off is
// audited under its own name.
Result<model::AutoPosting> update_auto_posting(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t location_id,
                                               const AutoPostingPatch& patch);

// A person lifts the pause on a location, or the pause on every location at
// once (which leaves each location's own pause as it is). Drafts written
// before now no longer count towards the next pause for failing checks.
Result<model::AutoPosting> resume_auto_reply(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t location_id);
model::AutoPostingOverview resume_all_auto_reply(pqxx::transaction_base& tx, audit::Writer& audit);

// ---- the engine pauses itself -----------------------------------------------------

// Pauses auto reply at the location, or without one at every location at
// once (D-072), and raises a system-problem notification. False if it was
// paused already. Drafts already scheduled are stopped when their job
// claims them.
bool pause_auto_reply(pqxx::transaction_base& tx, std::optional<std::int64_t> location_id, std::string_view reason,
                        const std::string& detail, core::TimePoint now);

// Called after the pipeline stored a draft that failed a reply check:
// pauses the location if its newest drafts all did, as many in a row as the
// location's setting says. True if it paused.
bool pause_if_checks_keep_failing(pqxx::transaction_base& tx, std::int64_t location_id, core::TimePoint now);

// ---- what was posted by auto reply --------------------------------------------------

// Newest first.
model::AutoPostList list_auto_replies(pqxx::transaction_base& tx, const db::Page& page);

}  // namespace poni::repo
