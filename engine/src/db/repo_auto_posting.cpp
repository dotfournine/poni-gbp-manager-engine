#include "db/repo_auto_posting.hpp"

#include "core/text.hpp"
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

// What a location's row does not hold: the timing comes from the reply policy
// and the state of the go-live test from its runs (to_auto_posting).
const db::FilledLater kFromPolicyAndGoLive{"delay_min_minutes", "delay_max_minutes", "window_start", "window_end", "timing_problem",
                                           "go_live_state"};

// $1: the start of the daily cap's window.
std::string location_select() {
    return std::format(R"sql(
        SELECT l.id AS location_id, l.title AS location_title, l.client_id, c.name AS client_name, l.auto_post_enabled AS enabled,
               l.auto_post_five_star AS five_star, l.auto_post_four_star AS four_star, l.auto_post_daily_cap AS daily_cap,
               l.time_zone, l.posting_enabled, l.auto_post_paused_at IS NOT NULL AS paused, {} AS paused_at,
               l.auto_post_pause_reason AS pause_reason, l.auto_post_pause_detail AS pause_detail, {} AS posted_last_24h,
               (SELECT count(*) FROM reply_drafts d JOIN reviews r ON r.id = d.review_id
                 WHERE r.location_id = l.id AND d.status = 'scheduled') AS scheduled,
               l.auto_post_pause_after_failures AS pause_after_failures
          FROM locations l LEFT JOIN clients c ON c.id = l.client_id)sql",
                       iso("l.auto_post_paused_at"), auto_posts_since("l.id", "$1"));
}

// Adds what the row does not hold. The timing comes from the location's reply
// policy; a problem with it is reported, not thrown, so the page can say what to fix.
model::AutoPosting to_auto_posting(pqxx::transaction_base& tx, model::AutoPosting a) {
    // Where the go-live test of this configuration stands (D-074).
    a.go_live_state = "not_tested";
    if (const auto setup = go_live_setup(tx, a.location_id)) {
        a.go_live_state = go_live_state(tx, setup->fingerprint.hash, setup->scope).state;
    }

    const auto settings = stored_reply_settings(tx, a.location_id);
    if (!settings) {
        a.timing_problem = "The location's reply policy can't be read.";
        return a;
    }
    a.delay_min_minutes = settings->policy.auto_post_delay_min_minutes;
    a.delay_max_minutes = settings->policy.auto_post_delay_max_minutes;
    a.window_start = settings->policy.auto_post_window_start;
    a.window_end = settings->policy.auto_post_window_end;
    const auto timing = domain::posting_timing(settings->policy);
    if (!timing) {
        a.timing_problem = std::format("The reply policy's timing can't be used: {}.", timing.error());
    } else if (timing->window && !a.time_zone) {
        a.timing_problem = "The reply policy has a posting window, which needs the location's time zone.";
    } else if (timing->window && !domain::known_time_zone(*a.time_zone)) {
        a.timing_problem = std::format("The time zone '{}' is not known on this server.", *a.time_zone);
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
                .object_id = location_id ? std::to_string(*location_id) : std::string("auto_posting"),
                .before = std::move(change.before),
                .after = std::move(change.after)};
    }
};

// Lifts the pause, if there is one. False if there is no such location.
bool resume(pqxx::transaction_base& tx, audit::Writer& audit, std::optional<std::int64_t> location_id) {
    const PauseScope scope{location_id};
    const auto rows = tx.exec(std::format("SELECT auto_post_pause_reason FROM {} WHERE {} FOR UPDATE", scope.table(), scope.row()),
                              pqxx::params{location_id});
    if (rows.empty()) return false;
    const auto reason = rows[0][0].as<OptString>();
    if (!reason) {
        audit.nothing_changed();
        return true;
    }
    tx.exec(std::format("UPDATE {} SET auto_post_paused_at = NULL, auto_post_pause_reason = NULL, auto_post_pause_detail = NULL, "
                        "auto_post_resumed_at = $2::timestamptz, updated_at = $2::timestamptz WHERE {}",
                        scope.table(), scope.row()),
            pqxx::params{location_id, to_timestamptz(audit.now())})
        .no_rows();
    audit.record(scope.entry("location.auto_post_resumed", "auto_posting.resumed",
                             {.before = {{"paused", true}, {"reason", *reason}}, .after = {{"paused", false}}}));
    return true;
}

std::string pause_words(std::string_view reason) {
    if (reason == kPauseChecksFailing) return "replies kept failing their checks";
    if (reason == kPausePostUnconfirmed) return "a reply was sent and did not appear on Google";
    return "Google refused a reply";
}

}  // namespace

std::string auto_posts_since(std::string_view location, std::string_view since) {
    return std::format("(SELECT count(*) FROM reply_posts p JOIN reviews pr ON pr.id = p.review_id "
                       "WHERE pr.location_id = {} AND p.policy_version IS NOT NULL AND p.status <> 'failed' "
                       "AND p.started_at > {}::timestamptz)",
                       location, since);
}

namespace {

// A location's settings, as the change and the resume answer with them.
std::optional<model::AutoPosting> get_auto_posting(pqxx::transaction_base& tx, std::int64_t location_id, core::TimePoint now) {
    auto row = db::row_as<model::AutoPosting>(tx, location_select() + " WHERE l.id = $2",
                                              pqxx::params{to_timestamptz(now - kAutoPostCapWindow), location_id}, kFromPolicyAndGoLive);
    if (!row) return std::nullopt;
    return to_auto_posting(tx, std::move(*row));
}

}  // namespace

model::AutoPostingOverview auto_posting_overview(pqxx::transaction_base& tx, core::TimePoint now) {
    // The global state in one row; the locations are read after it.
    auto out = db::one_as<model::AutoPostingOverview>(
        tx,
        std::format("SELECT posting_enabled, auto_post_paused_at IS NOT NULL AS paused, {} AS paused_at, "
                    "auto_post_pause_reason AS pause_reason, auto_post_pause_detail AS pause_detail FROM system_settings",
                    iso("auto_post_paused_at")),
        {}, db::FilledLater{"locations"});
    for (auto& row : db::rows_as<model::AutoPosting>(
             tx, location_select() + " WHERE l.client_id IS NOT NULL AND NOT c.archived ORDER BY lower(l.title), l.id",
             pqxx::params{to_timestamptz(now - kAutoPostCapWindow)}, kFromPolicyAndGoLive)) {
        out.locations.push_back(to_auto_posting(tx, std::move(row)));
    }
    return out;
}

Result<model::AutoPosting> update_auto_posting(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t location_id,
                                               const AutoPostingPatch& patch) {
    const auto rows = tx.exec(
        "SELECT l.auto_post_enabled, l.auto_post_five_star, l.auto_post_four_star, l.auto_post_daily_cap, l.time_zone, "
        "l.client_id IS NOT NULL AND l.access_status = 'active' AND NOT coalesce(c.archived, true), l.auto_post_pause_after_failures "
        "FROM locations l LEFT JOIN clients c ON c.id = l.client_id WHERE l.id = $1 FOR UPDATE OF l",
        pqxx::params{location_id});
    if (rows.empty()) return not_found("location");
    const bool old_enabled = rows[0][0].as<bool>();
    const bool old_five = rows[0][1].as<bool>();
    const bool old_four = rows[0][2].as<bool>();
    const int old_cap = rows[0][3].as<int>();
    const auto old_zone = rows[0][4].as<OptString>();
    const bool synced = rows[0][5].as<bool>();
    const int old_failures = rows[0][6].as<int>();

    const bool enabled = patch.enabled.value_or(old_enabled);
    const bool five = patch.five_star.value_or(old_five);
    const bool four = patch.four_star.value_or(old_four);
    const int cap = patch.daily_cap.value_or(old_cap);
    const int failures = patch.pause_after_failures.value_or(old_failures);
    OptString zone = old_zone;
    if (patch.time_zone.given()) {
        const auto& given = patch.time_zone.value();
        zone = given ? OptString{std::string(text::trim(*given))} : std::nullopt;
        if (zone && zone->empty()) zone.reset();
    }

    // Check everything before writing anything: a refused request changes nothing.
    if (cap < 1 || cap > 1000) return invalid("\"daily_cap\" must be between 1 and 1000.");
    if (failures < 1 || failures > 20) return invalid("\"pause_after_failures\" must be between 1 and 20.");
    if (zone && zone != old_zone && !domain::known_time_zone(*zone)) {
        return invalid(std::format("\"{}\" is not a time zone this server knows. Use a name like \"America/New_York\".", *zone));
    }
    if (enabled) {
        // What must hold while auto-posting is on, checked on every change.
        if (!synced) return invalid("Auto-posting needs a location that is linked to an active client and still accessible.");
        if (!five && !four) return invalid("Choose which star ratings may be answered automatically before switching auto-posting on.");
        const auto settings = stored_reply_settings(tx, location_id);
        if (!settings) return invalid("The location's reply policy can't be read.");
        const auto timing = domain::posting_timing(settings->policy);
        if (!timing) return invalid(std::format("The reply policy's timing can't be used: {}.", timing.error()));
        if (timing->window && !zone) return invalid("The reply policy has a posting window, so the location needs a time zone first.");
    }

    const bool settings_changed = five != old_five || four != old_four || cap != old_cap || zone != old_zone || failures != old_failures;
    if (enabled == old_enabled && !settings_changed) {
        audit.nothing_changed();
        return *get_auto_posting(tx, location_id, audit.now());
    }
    tx.exec("UPDATE locations SET auto_post_enabled = $2, auto_post_five_star = $3, auto_post_four_star = $4, auto_post_daily_cap = $5, "
            "time_zone = $6, updated_at = $7::timestamptz, auto_post_pause_after_failures = $8 WHERE id = $1",
            pqxx::params{location_id, enabled, five, four, cap, zone, to_timestamptz(audit.now()), failures})
        .no_rows();
    const auto object_id = std::to_string(location_id);
    const auto json = [](bool on, bool five_star, bool four_star, int daily_cap, const OptString& time_zone, int pause_after) {
        return boost::json::object{{"enabled", on},
                                   {"five_star", five_star},
                                   {"four_star", four_star},
                                   {"daily_cap", daily_cap},
                                   {"time_zone", time_zone ? boost::json::value(*time_zone) : boost::json::value(nullptr)},
                                   {"pause_after_failures", pause_after}};
    };
    // Its own action name when the switch itself moves: who switched auto-posting on must be easy to find.
    audit.record({.action = enabled == old_enabled ? "location.auto_post_settings"
                            : enabled              ? "location.auto_post_enabled"
                                                   : "location.auto_post_disabled",
                  .object_type = "location",
                  .object_id = object_id,
                  .before = json(old_enabled, old_five, old_four, old_cap, old_zone, old_failures),
                  .after = json(enabled, five, four, cap, zone, failures)});
    return *get_auto_posting(tx, location_id, audit.now());
}

Result<model::AutoPosting> resume_auto_posting(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t location_id) {
    if (!resume(tx, audit, location_id)) return not_found("location");
    return *get_auto_posting(tx, location_id, audit.now());
}

model::AutoPostingOverview resume_all_auto_posting(pqxx::transaction_base& tx, audit::Writer& audit) {
    resume(tx, audit, std::nullopt);
    return auto_posting_overview(tx, audit.now());
}

// ---- the engine pauses itself -----------------------------------------------------

bool pause_auto_posting(pqxx::transaction_base& tx, std::optional<std::int64_t> location_id, std::string_view reason,
                        const std::string& detail, core::TimePoint now) {
    const PauseScope scope{location_id};
    const auto now_text = to_timestamptz(now);
    const auto rows = tx.exec(std::format("UPDATE {} SET auto_post_paused_at = $2::timestamptz, auto_post_pause_reason = $3, "
                                          "auto_post_pause_detail = $4, updated_at = $2::timestamptz "
                                          "WHERE {} AND auto_post_paused_at IS NULL RETURNING {}",
                                          scope.table(), scope.row(), location_id ? "title, client_id" : "1"),
                              pqxx::params{location_id, now_text, std::string(reason), detail});
    if (rows.empty()) return false;
    audit::record(tx, auto_reply(),
                  scope.entry("location.auto_post_paused", "auto_posting.paused", {.after = {{"reason", std::string(reason)}, {"detail", detail}}}),
                  now);
    if (location_id) {
        const auto title = rows[0][0].as<std::string>();
        emit(tx,
             {.type = std::string(kEventSystemProblem),
              .key = std::format("auto-post-pause:location:{}:{}", *location_id, now_text),
              .client_id = rows[0][1].as<std::optional<std::int64_t>>(),
              .location_id = location_id,
              .review_id = std::nullopt,
              .subject = std::format("Auto-posting paused: {}", title),
              .body = std::format("Auto-posting at {} was paused because {}.\n\n{}\n\nNothing is posted there without a person until "
                                  "someone resumes it in the dashboard. Replies can still be approved by hand.",
                                  title, pause_words(reason), detail),
              .link_path = "/#/auto-posting"},
             now);
    } else {
        emit(tx,
             {.type = std::string(kEventSystemProblem),
              .key = std::format("auto-post-pause:all:{}", now_text),
              .client_id = std::nullopt,
              .location_id = std::nullopt,
              .review_id = std::nullopt,
              .subject = "Auto-posting paused for every location",
              .body = std::format("Auto-posting was paused for every location because {}.\n\n{}\n\nNothing is posted without a person "
                                  "until someone resumes it in the dashboard. Replies can still be approved by hand.",
                                  pause_words(reason), detail),
              .link_path = "/#/auto-posting"},
             now);
    }
    return true;
}

bool pause_if_checks_keep_failing(pqxx::transaction_base& tx, std::int64_t location_id, core::TimePoint now) {
    // Only where auto-posting is on: elsewhere a failing check already means
    // the approval queue and there is nothing to pause.
    const auto limits = tx.exec("SELECT auto_post_pause_after_failures FROM locations WHERE id = $1 AND auto_post_enabled "
                                "AND auto_post_paused_at IS NULL",
                                pqxx::params{location_id});
    if (limits.empty()) return false;
    const auto limit = limits[0][0].as<int>();
    // The newest `limit` drafts the pipeline wrote here since the last resume: all of them failed a check?
    const auto row = tx.exec(R"sql(
        SELECT count(*), count(*) FILTER (WHERE x.failed)
          FROM (SELECT EXISTS (SELECT 1 FROM jsonb_array_elements(d.checks) k WHERE NOT (k->>'passed')::boolean) AS failed
                  FROM reply_drafts d JOIN reviews r ON r.id = d.review_id JOIN locations l ON l.id = r.location_id
                 WHERE l.id = $1 AND d.created_by IS NULL
                   AND (l.auto_post_resumed_at IS NULL OR d.created_at > l.auto_post_resumed_at)
                 ORDER BY d.id DESC LIMIT $2) x)sql",
                             pqxx::params{location_id, limit})
                         .one_row();
    if (row[0].as<int>() < limit || row[1].as<int>() < limit) return false;
    return pause_auto_posting(tx, location_id, kPauseChecksFailing,
                              std::format("The last {} written for this location each failed a reply check. Look at them in "
                                          "the approval queue, and at the voice settings and the model, before resuming.",
                                          limit == 1 ? std::string("reply") : std::format("{} replies in a row", limit)),
                              now);
}

// ---- what was auto-posted -----------------------------------------------------------

model::AutoPostList list_auto_posts(pqxx::transaction_base& tx, const db::Page& page) {
    return db::page<model::AutoPost>(
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
