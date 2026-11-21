#include "db/audit.hpp"

#include "db/database.hpp"

#include <boost/json/serialize.hpp>

namespace poni::audit {

void record(pqxx::transaction_base& tx, const Actor& actor, const Entry& entry, core::TimePoint now) {
    const auto json = [](const boost::json::value& v) -> std::optional<std::string> {
        return v.is_null() ? std::nullopt : std::optional(boost::json::serialize(v));
    };
    tx.exec(
          R"sql(
        INSERT INTO audit_log (occurred_at, actor_type, actor_user_id, actor, action, object_type, object_id,
                               before, after, ip)
        VALUES ($1::timestamptz, $2, $3, $4, $5, $6, $7, $8::jsonb, $9::jsonb, $10))sql",
          pqxx::params{db::to_timestamptz(now), actor.type, actor.user_id, actor.label, entry.action, entry.object_type,
                       entry.object_id, json(entry.before), json(entry.after), actor.ip})
        .no_rows();
}

}  // namespace poni::audit
