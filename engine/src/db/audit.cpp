#include "db/audit.hpp"

#include "db/database.hpp"

#include <boost/json/serialize.hpp>

#include <charconv>

namespace poni::audit {

namespace {

// The object's row id, when its id is one. Worked out here and not cast in
// SQL: PostgreSQL may fold a cast of a parameter before a CASE picks its branch,
// and `credential` or `user` entries have ids that are no number.
std::optional<std::int64_t> row_id(const std::string& object_id) {
    std::int64_t id = 0;
    const auto* end = object_id.data() + object_id.size();
    const auto [ptr, ec] = std::from_chars(object_id.data(), end, id);
    if (object_id.empty() || ec != std::errc{} || ptr != end || id <= 0) return std::nullopt;
    return id;
}

}  // namespace

void record(pqxx::transaction_base& tx, const Actor& actor, const Entry& entry, core::TimePoint now) {
    const auto json = [](const boost::json::value& v) -> std::optional<std::string> {
        return v.is_null() ? std::nullopt : std::optional(boost::json::serialize(v));
    };
    // The list of kinds is the one in AuditEntry.location_id (api/openapi.yaml)
    // and in the check of 0009_audit_location.sql.
    tx.exec(
          R"sql(
        INSERT INTO audit_log (occurred_at, actor_type, actor_user_id, actor, action, object_type, object_id,
                               before, after, ip, location_id)
        VALUES ($1::timestamptz, $2, $3, $4, $5, $6, $7, $8::jsonb, $9::jsonb, $10,
                COALESCE($11::bigint, CASE $6
                    WHEN 'location' THEN (SELECT id FROM locations WHERE id = $12::bigint)
                    WHEN 'review' THEN (SELECT location_id FROM reviews WHERE id = $12::bigint)
                    WHEN 'draft' THEN (SELECT r.location_id FROM reply_drafts d JOIN reviews r ON r.id = d.review_id
                                        WHERE d.id = $12::bigint)
                    WHEN 'escalation' THEN (SELECT r.location_id FROM cases c JOIN reviews r ON r.id = c.review_id
                                             WHERE c.id = $12::bigint)
                    WHEN 'local_post' THEN (SELECT location_id FROM local_posts WHERE id = $12::bigint)
                    WHEN 'report' THEN (SELECT location_id FROM reports WHERE id = $12::bigint)
                    WHEN 'report_share_link' THEN (SELECT r.location_id FROM report_share_links l
                                                    JOIN reports r ON r.id = l.report_id WHERE l.id = $12::bigint)
                    WHEN 'report_schedule' THEN (SELECT location_id FROM report_schedules WHERE id = $12::bigint)
                    WHEN 'go_live_run' THEN (SELECT location_id FROM go_live_runs WHERE id = $12::bigint)
                    WHEN 'notification_rule' THEN (SELECT location_id FROM notification_rules WHERE id = $12::bigint)
                END)))sql",
          pqxx::params{db::to_timestamptz(now), actor.type, actor.user_id, actor.label, entry.action, entry.object_type,
                       entry.object_id, json(entry.before), json(entry.after), actor.ip, entry.location_id,
                       row_id(entry.object_id)})
        .no_rows();
}

}  // namespace poni::audit
