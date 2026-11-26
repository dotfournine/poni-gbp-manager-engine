#include "db/repo_auto_reply.hpp"

#include "db/database.hpp"
#include "db/repo_go_live.hpp"
#include "db/repo_notifications.hpp"
#include "db/repo_settings.hpp"
#include "domain/posting_schedule.hpp"

#include <format>

namespace poni::repo {

using db::iso;
using db::to_timestamptz;

namespace {

using OptString = std::optional<std::string>;

// What a location's row does not hold: whether the reply settings' timing
// can be used, and the state of the go-live test from its runs (complete).
db::FilledLater from_settings_and_go_live() { return db::FilledLater{"timing_problem", "go_live_state"}; }

// $1: the start of the daily cap's window.
std::string location_select() {
    return std::format(R"sql(
        SELECT l.id AS location_id, l.title AS location_title, l.posting_enabled, l.auto_reply_paused_at IS NOT NULL AS paused,
               {} AS paused_at, l.auto_reply_pause_reason AS pause_reason, l.auto_reply_pause_detail AS pause_detail,
               {} AS resumed_at, {} AS posted_last_24h,
               (SELECT count(*) FROM reply_drafts d JOIN reviews r ON r.id = d.review_id
                 WHERE r.location_id = l.id AND d.status = 'scheduled') AS scheduled
          FROM locations l)sql",
                       iso("l.auto_reply_paused_at"), iso("l.auto_reply_resumed_at"), auto_replies_since("l.id", "$1"));
}

// Adds what the row does not hold. A problem with the timing is reported,
// not thrown, so the page can say what to fix.
model::AutoReplyLocation complete(pqxx::transaction_base& tx, model::AutoReplyLocation a) {
    // Where the go-live test of this configuration stands (D-074).
    a.go_live_state = "not_tested";
    if (const auto setup = go_live_setup(tx, a.location_id)) {
        a.go_live_state = go_live_state(tx, setup->fingerprint.hash, setup->scope).state;
    }

    const auto settings = stored_settings(tx, a.location_id);
    if (!settings) {
        a.timing_problem = "The location's reply settings can't be read.";
        return a;
    }
    const auto zone = tx.query_value<std::string>("SELECT time_zone FROM locations WHERE id = $1", pqxx::params{a.location_id});
    const auto timing = domain::posting_timing(settings->reply);
    if (!timing) {
        a.timing_problem = std::format("The reply policy's timing can't be used: {}.", timing.error());
    } else if (timing->window && !domain::known_time_zone(zone)) {
        a.timing_problem = std::format("The time zone '{}' is not known on this server.", zone);
    }
    return a;
}

// A pause is on one location, or on every location at once (D-072): the same
// columns, in the location's row or in the one row of system_settings.
struct PauseScope {
    // $1 of every statement about the pause; nothing: every location.
    std::optional<std::int64_t> location_id;

    std::string_view table() const { return location_id ? "locations" : "system_settings"; }
    // The WHERE that finds the row, so that both tables take the same parameters.
    std::string_view row() const { return location_id ? "id = $1" : "$1::bigint IS NULL"; }
    // The audit entry about it: `action` if it is a location's, `action_for_all` otherwise.
    audit::Entry entry(std::string_view action, std::string_view action_for_all, audit::Change change) const {
        return {.action = std::string(location_id ? action : action_for_all),
                .object_type = location_id ? "location" : "system_settings",
                .object_id = location_id ? std::to_string(*location_id) : std::string("auto_reply"),
                .before = std::move(change.before),
                .after = std::move(change.after)};
    }
};

// Lifts the pause, if there is one. False if there is no such location.
bool resume(pqxx::transaction_base& tx, audit::Writer& audit, std::optional<std::int64_t> location_id) {
    const PauseScope scope{location_id};
    const auto rows = tx.exec(std::format("SELECT auto_reply_pause_reason FROM {} WHERE {} FOR UPDATE", scope.table(), scope.row()),
                              pqxx::params{location_id});
    if (rows.empty()) return false;
    const auto reason = rows[0][0].as<OptString>();
    if (!reason) {
        audit.nothing_changed();
        return true;
    }
    tx.exec(std::format("UPDATE {} SET auto_reply_paused_at = NULL, auto_reply_pause_reason = NULL, auto_reply_pause_detail = NULL, "
                        "auto_reply_resumed_at = $2::timestamptz, updated_at = $2::timestamptz WHERE {}",
                        scope.table(), scope.row()),
            pqxx::params{location_id, to_timestamptz(audit.now())})
        .no_rows();
    audit.record(scope.entry("location.auto_reply_resumed", "auto_reply.resumed",
                             {.before = {{"paused", true}, {"reason", *reason}}, .after = {{"paused", false}}}));
    return true;
}

std::string pause_words(std::string_view reason) {
    if (reason == kPauseChecksFailing) return "replies kept failing their checks";
    if (reason == kPausePostUnconfirmed) return "a reply was sent and did not appear on Google";
    return "Google refused a reply";
}

}  // namespace

std::string auto_replies_since(std::string_view location, std::string_view since) {
    return std::format("(SELECT count(*) FROM reply_posts p JOIN reviews pr ON pr.id = p.review_id "
                       "WHERE pr.location_id = {} AND p.policy_version IS NOT NULL AND p.status <> 'failed' "
                       "AND p.started_at > {}::timestamptz)",
                       location, since);
}

namespace {

// One location's state, as its resume answers with it.
std::optional<model::AutoReplyLocation> get_location(pqxx::transaction_base& tx, std::int64_t location_id, core::TimePoint now) {
    auto row = db::row_as<model::AutoReplyLocation>(tx, location_select() + " WHERE l.id = $2",
                                                    pqxx::params{to_timestamptz(now - kAutoReplyCapWindow), location_id},
                                                    from_settings_and_go_live());
    if (!row) return std::nullopt;
    return complete(tx, std::move(*row));
}

}  // namespace

model::AutoReplyStatus auto_reply_overview(pqxx::transaction_base& tx, core::TimePoint now) {
    // The global state in one row; the locations are read after it.
    auto out = db::one_as<model::AutoReplyStatus>(
        tx,
        std::format("SELECT posting_enabled, auto_reply_paused_at IS NOT NULL AS paused, {} AS paused_at, "
                    "auto_reply_pause_reason AS pause_reason, auto_reply_pause_detail AS pause_detail, {} AS resumed_at "
                    "FROM system_settings",
                    iso("auto_reply_paused_at"), iso("auto_reply_resumed_at")),
        {}, db::FilledLater{"locations"});
    for (auto& row : db::rows_as<model::AutoReplyLocation>(tx, location_select() + " WHERE l.enabled ORDER BY lower(l.title), l.id",
                                                           pqxx::params{to_timestamptz(now - kAutoReplyCapWindow)},
                                                           from_settings_and_go_live())) {
        out.locations.push_back(complete(tx, std::move(row)));
    }
    return out;
}

Result<model::AutoReplyLocation> resume_auto_reply(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t location_id) {
    if (!resume(tx, audit, location_id)) return not_found("location");
    return *get_location(tx, location_id, audit.now());
}

model::AutoReplyStatus resume_all_auto_reply(pqxx::transaction_base& tx, audit::Writer& audit) {
    resume(tx, audit, std::nullopt);
    return auto_reply_overview(tx, audit.now());
}

// ---- the engine pauses itself -----------------------------------------------------

bool pause_auto_reply(pqxx::transaction_base& tx, std::optional<std::int64_t> location_id, std::string_view reason,
                        const std::string& detail, core::TimePoint now) {
    const PauseScope scope{location_id};
    const auto now_text = to_timestamptz(now);
    const auto rows = tx.exec(std::format("UPDATE {} SET auto_reply_paused_at = $2::timestamptz, auto_reply_pause_reason = $3, "
                                          "auto_reply_pause_detail = $4, updated_at = $2::timestamptz "
                                          "WHERE {} AND auto_reply_paused_at IS NULL RETURNING {}",
                                          scope.table(), scope.row(), location_id ? "title" : "1"),
                              pqxx::params{location_id, now_text, std::string(reason), detail});
    if (rows.empty()) return false;
    audit::record(tx, auto_reply(),
                  scope.entry("location.auto_reply_paused", "auto_reply.paused", {.after = {{"reason", std::string(reason)}, {"detail", detail}}}),
                  now);
    if (location_id) {
        const auto title = rows[0][0].as<std::string>();
        emit(tx,
             {.type = std::string(kEventSystemProblem),
              .key = std::format("auto-reply-pause:location:{}:{}", *location_id, now_text),
              .location_id = location_id,
              .review_id = std::nullopt,
              .subject = std::format("Auto reply paused: {}", title),
              .body = std::format("Auto reply at {} was paused because {}.\n\n{}\n\nNothing is posted there without a person until "
                                  "someone resumes it in the dashboard. Replies can still be approved by hand.",
                                  title, pause_words(reason), detail),
              .link_path = "/#/auto-reply"},
             now);
    } else {
        emit(tx,
             {.type = std::string(kEventSystemProblem),
              .key = std::format("auto-reply-pause:all:{}", now_text),
              .location_id = std::nullopt,
              .review_id = std::nullopt,
              .subject = "Auto reply paused for every location",
              .body = std::format("Auto reply was paused for every location because {}.\n\n{}\n\nNothing is posted without a person "
                                  "until someone resumes it in the dashboard. Replies can still be approved by hand.",
                                  pause_words(reason), detail),
              .link_path = "/#/auto-reply"},
             now);
    }
    return true;
}

bool pause_if_checks_keep_failing(pqxx::transaction_base& tx, std::int64_t location_id, core::TimePoint now) {
    // Only where auto reply is on: elsewhere a failing check already means
    // the approval queue and there is nothing to pause.
    const auto limits = tx.exec("SELECT s.auto_reply_pause_after_failures FROM locations l JOIN reply_settings s ON s.location_id = l.id "
                                "WHERE l.id = $1 AND s.auto_reply_enabled AND l.auto_reply_paused_at IS NULL",
                                pqxx::params{location_id});
    if (limits.empty()) return false;
    const auto limit = limits[0][0].as<int>();
    // The newest `limit` drafts the pipeline wrote here since the last resume: all of them failed a check?
    const auto row = tx.exec(R"sql(
        SELECT count(*), count(*) FILTER (WHERE x.failed)
          FROM (SELECT EXISTS (SELECT 1 FROM jsonb_array_elements(d.checks) k WHERE NOT (k->>'passed')::boolean) AS failed
                  FROM reply_drafts d JOIN reviews r ON r.id = d.review_id JOIN locations l ON l.id = r.location_id
                 WHERE l.id = $1 AND d.created_by IS NULL
                   AND (l.auto_reply_resumed_at IS NULL OR d.created_at > l.auto_reply_resumed_at)
                 ORDER BY d.id DESC LIMIT $2) x)sql",
                             pqxx::params{location_id, limit})
                         .one_row();
    if (row[0].as<int>() < limit || row[1].as<int>() < limit) return false;
    return pause_auto_reply(tx, location_id, kPauseChecksFailing,
                              std::format("The last {} written for this location each failed a reply check. Look at them in "
                                          "the approval queue, and at the content and reply settings and the model, before resuming.",
                                          limit == 1 ? std::string("reply") : std::format("{} replies in a row", limit)),
                              now);
}

// ---- what was posted by auto reply --------------------------------------------------

model::AutoReplyList list_auto_replies(pqxx::transaction_base& tx, const db::Page& page) {
    return db::page<model::AutoReply>(
        tx,
        std::format("SELECT p.id, p.review_id, r.location_id, l.title AS location_title, r.rating, r.comment AS review_text, "
                    "p.text AS reply_text, p.status, p.policy_version, {} AS started_at, {} AS posted_at, "
                    "{} AS published_at, extract(epoch FROM p.posted_at - r.first_seen_at)::bigint AS seconds_to_post "
                    "FROM reply_posts p JOIN reviews r ON r.id = p.review_id JOIN locations l ON l.id = r.location_id "
                    "WHERE p.policy_version IS NOT NULL",
                    iso("p.started_at"), iso("p.posted_at"), iso("p.published_at")),
        {}, "p.id DESC", page);
}

}  // namespace poni::repo
