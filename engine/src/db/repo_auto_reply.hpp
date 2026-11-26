// Auto reply's own state (D-072, D-110): the pauses the engine puts on
// itself, how a person lifts them, what the engine shows of each location,
// and the list of what was posted without a person. Blocking calls on a
// transaction the caller owns: run them through Database::run.
//
// The settings, the switch among them, are the location's reply settings
// (repo_settings), off after installation and switched on only by a person's
// save. The engine itself can only switch auto reply *off*: a pause, which a
// person lifts, or the location switched off. How a draft gets scheduled and
// posted is in repo_posting.
#pragma once

#include "core/clock.hpp"
#include "db/audit.hpp"
#include "db/failure.hpp"
#include "db/sql.hpp"
#include "domain/models.hpp"

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
// location is the location's own reply setting (auto_reply_pause_after_failures, D-076).

// What decides and starts a post no person approved, as the audit log names it (architecture 4.1, step 5).
inline audit::Actor auto_reply() { return audit::Actor::system("auto-reply"); }

// The daily cap counts the automatic replies of the last 24 hours.
inline constexpr std::chrono::hours kAutoReplyCapWindow{24};
// SQL: how many automatic replies were started at the location `location`
// (an expression of the query around it, like "l.id") since the time `since`
// (a placeholder). What the cap counts and what the page shows are this one count.
std::string auto_replies_since(std::string_view location, std::string_view since);

// The global state and every switched-on location.
model::AutoReplyStatus auto_reply_overview(pqxx::transaction_base& tx, core::TimePoint now);

// A person lifts the pause on a location, or the pause on every location at
// once (which leaves each location's own pause as it is). Drafts written
// before now no longer count towards the next pause for failing checks.
Result<model::AutoReplyLocation> resume_auto_reply(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t location_id);
model::AutoReplyStatus resume_all_auto_reply(pqxx::transaction_base& tx, audit::Writer& audit);

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
model::AutoReplyList list_auto_replies(pqxx::transaction_base& tx, const db::Page& page);

}  // namespace poni::repo
