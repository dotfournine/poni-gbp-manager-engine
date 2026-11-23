#include "db/repo_catalog.hpp"

#include "core/text.hpp"
#include "db/database.hpp"
#include "db/repo_analysis.hpp"
#include "db/repo_auth.hpp"
#include "db/repo_knowledge.hpp"
#include "db/repo_notifications.hpp"
#include "db/repo_posting.hpp"
#include "db/repo_settings.hpp"
#include "domain/dates.hpp"
#include "jobs/queue.hpp"

#include <boost/json/serialize.hpp>
#include <boost/json/value_from.hpp>

#include <format>

namespace poni::repo {

using db::iso;
using db::to_timestamptz;

namespace {

using OptString = std::optional<std::string>;

std::string trimmed(std::string_view s) {
    constexpr std::string_view ws = " \t\r\n";
    const auto first = s.find_first_not_of(ws);
    if (first == std::string_view::npos) return {};
    return std::string(s.substr(first, s.find_last_not_of(ws) - first + 1));
}

// ---- clients ---------------------------------------------------------------

std::string client_select() {
    return std::format(R"sql(
        SELECT c.id, c.name, c.notes, c.archived,
               (SELECT count(*) FROM locations l WHERE l.client_id = c.id) AS location_count,
               (SELECT coalesce(jsonb_agg(l.id ORDER BY l.id), '[]') FROM locations l WHERE l.client_id = c.id) AS location_ids,
               {} AS created_at, {} AS updated_at
          FROM clients c)sql",
                       iso("c.created_at"), iso("c.updated_at"));
}

bool client_name_taken(pqxx::transaction_base& tx, const std::string& name, std::int64_t except_id) {
    return !tx.exec("SELECT 1 FROM clients WHERE lower(name) = lower($1) AND id <> $2", pqxx::params{name, except_id}).empty();
}

constexpr std::string_view kClientLimits = "A client needs a name of 1 to 200 characters; notes may be up to 5000 characters.";

boost::json::object client_json(const std::string& name, const std::string& notes, bool archived) {
    return {{"name", name}, {"notes", notes}, {"archived", archived}};
}

// ---- locations -------------------------------------------------------------

std::string location_select() {
    return std::format(R"sql(
        SELECT l.id, l.gbp_location_id, l.gbp_account_id, l.client_id, c.name AS client_name, l.enabled, l.industry_key,
               to_char(l.analyze_from, 'YYYY-MM-DD') AS analyze_from, l.audience, l.title, l.store_code,
               l.primary_category, l.address, l.locality, l.administrative_area, l.postal_code, l.region_code,
               l.phone, l.website_uri, l.maps_uri, l.access_status, {} AS access_lost_at, {} AS first_seen_at, {} AS last_seen_at,
               l.review_poll_interval_s,
               (SELECT count(*) FROM reviews r WHERE r.location_id = l.id AND r.removed_at IS NULL) AS review_count,
               (SELECT count(*) FROM reviews r WHERE r.location_id = l.id AND r.removed_at IS NULL AND r.reply_text IS NULL)
                   AS unreplied_count,
               (SELECT avg(r.rating)::float8 FROM reviews r WHERE r.location_id = l.id AND r.removed_at IS NULL) AS average_rating,
               {} AS reviews_polled_at, {} AS reviews_full_synced_at, l.last_sync_error, {} AS last_sync_error_at, l.posting_enabled,
               {} AS local_posts_synced_at, l.local_posts_sync_error, {} AS local_posts_sync_error_at, l.time_zone
          FROM locations l LEFT JOIN clients c ON c.id = l.client_id)sql",
                       iso("l.access_lost_at"), iso("l.first_seen_at"), iso("l.last_seen_at"), iso("l.reviews_polled_at"),
                       iso("l.reviews_full_synced_at"), iso("l.last_sync_error_at"), iso("l.local_posts_synced_at"),
                       iso("l.local_posts_sync_error_at"));
}

boost::json::value optional_json(const OptString& s) { return s ? boost::json::value(*s) : boost::json::value(nullptr); }

boost::json::object link_json(const std::optional<std::int64_t>& client_id, const OptString& client_name) {
    return {{"client_id", client_id ? boost::json::value(*client_id) : boost::json::value(nullptr)},
            {"client_name", optional_json(client_name)}};
}

std::string join_lines(const std::optional<std::vector<std::string>>& lines) {
    std::string out;
    for (const auto& line : lines.value_or(std::vector<std::string>{})) {
        if (!out.empty()) out += ", ";
        out += line;
    }
    return out;
}

constexpr std::string_view kBadAnalyzeFrom = "\"analyze_from\" must be a date, YYYY-MM-DD, or null.";

// Whether the day analysis starts from is a day the calendar has, or none.
bool is_day_or_none(const OptString& day) { return !day || domain::parse_report_day(*day); }

OptString non_empty(std::string s) { return s.empty() ? std::nullopt : OptString(std::move(s)); }

// Google's posts are short; a line is enough to say who reads them.
constexpr std::size_t kMaxAudienceLength = 500;

}  // namespace

// ---- clients -------------------------------------------------------------------

model::ClientList list_clients(pqxx::transaction_base& tx, const ClientFilter& filter) {
    db::Conditions where;
    if (filter.archived) where.add(std::format("c.archived = {}", where.bind(*filter.archived)));
    if (!filter.q.empty()) where.add(std::format("position(lower({}) in lower(c.name)) > 0", where.bind(filter.q)));
    return db::page<model::Client>(tx, client_select(), where, "lower(c.name), c.id", filter.page);
}

std::optional<model::Client> get_client(pqxx::transaction_base& tx, std::int64_t id) {
    return db::row_as<model::Client>(tx, client_select() + " WHERE c.id = $1", pqxx::params{id});
}

Result<model::Client> create_client(pqxx::transaction_base& tx, audit::Writer& audit, const NewClient& client) {
    const auto clean = trimmed(client.name);
    const auto notes = client.notes.value_or("");
    if (clean.empty() || clean.size() > 200 || notes.size() > 5000) return invalid(std::string(kClientLimits));
    if (client_name_taken(tx, clean, 0)) return conflict("name_taken", "Another client already has this name.");

    const auto id = tx.query_value<std::int64_t>(
        "INSERT INTO clients (name, notes, created_at, updated_at) VALUES ($1, $2, $3::timestamptz, $3::timestamptz) RETURNING id",
        pqxx::params{clean, notes, to_timestamptz(audit.now())});
    audit.record("client.create", "client", id, {.after = client_json(clean, notes, false)});
    return *get_client(tx, id);
}

Result<model::Client> update_client(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id, const ClientPatch& patch) {
    const auto rows = tx.exec("SELECT name, notes, archived FROM clients WHERE id = $1 FOR UPDATE", pqxx::params{id});
    if (rows.empty()) return not_found("client");
    const auto old_name = rows[0][0].as<std::string>();
    const auto old_notes = rows[0][1].as<std::string>();
    const auto old_archived = rows[0][2].as<bool>();

    const auto name = patch.name ? trimmed(*patch.name) : old_name;
    const auto notes = patch.notes.value_or(old_notes);
    const auto archived = patch.archived.value_or(old_archived);
    if (name.empty() || name.size() > 200 || notes.size() > 5000) return invalid(std::string(kClientLimits));
    if (name != old_name && client_name_taken(tx, name, id)) return conflict("name_taken", "Another client already has this name.");

    if (name == old_name && notes == old_notes && archived == old_archived) {
        audit.nothing_changed();
        return *get_client(tx, id);
    }
    // Archiving changes the grouping only: its locations keep everything (D-102).
    tx.exec("UPDATE clients SET name = $2, notes = $3, archived = $4, updated_at = $5::timestamptz WHERE id = $1",
            pqxx::params{id, name, notes, archived, to_timestamptz(audit.now())})
        .no_rows();
    audit.record(archived != old_archived ? (archived ? "client.archive" : "client.restore") : "client.update", "client", id,
                 {.before = client_json(old_name, old_notes, old_archived), .after = client_json(name, notes, archived)});
    return *get_client(tx, id);
}

Result<void> delete_client(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id) {
    // The client's row is the lock: a location can't be linked to it while it goes.
    if (tx.exec("SELECT 1 FROM clients WHERE id = $1 FOR UPDATE", pqxx::params{id}).empty()) return not_found("client");
    const auto client = *get_client(tx, id);
    for (const auto location : client.location_ids) {
        if (auto unlinked = update_location(tx, audit, location, {.client_id = std::nullopt}); !unlinked) return std::unexpected(unlinked.error());
    }
    tx.exec("DELETE FROM clients WHERE id = $1", pqxx::params{id}).no_rows();
    audit.record("client.delete", "client", id,
                 {.before = {{"name", client.name},
                             {"notes", client.notes},
                             {"archived", client.archived},
                             {"location_ids", boost::json::value_from(client.location_ids)}}});
    return {};
}

// ---- locations -----------------------------------------------------------------

model::LocationList list_locations(pqxx::transaction_base& tx, const LocationFilter& filter) {
    db::Conditions where;
    if (filter.client_id) where.add(std::format("l.client_id = {}", where.bind(*filter.client_id)));
    if (filter.linked) where.add(*filter.linked ? "l.client_id IS NOT NULL" : "l.client_id IS NULL");
    if (filter.enabled) where.add(std::format("l.enabled = {}", where.bind(*filter.enabled)));
    if (filter.access_status) where.add(std::format("l.access_status = {}", where.bind(*filter.access_status)));
    if (!filter.q.empty()) where.add(std::format("position(lower({}) in lower(l.title)) > 0", where.bind(filter.q)));
    return db::page<model::Location>(tx, location_select(), where, "lower(l.title), l.id", filter.page);
}

std::optional<model::Location> get_location(pqxx::transaction_base& tx, std::int64_t id) {
    return db::row_as<model::Location>(tx, location_select() + " WHERE l.id = $1", pqxx::params{id});
}

Result<model::Location> update_location(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id, const LocationPatch& patch) {
    const auto rows = tx.exec(
        "SELECT l.client_id, c.name, l.review_poll_interval_s, l.posting_enabled, l.enabled, l.access_status, l.industry_key, "
        "to_char(l.analyze_from, 'YYYY-MM-DD'), l.audience, l.auto_post_enabled, l.auto_post_five_star, l.auto_post_four_star "
        "FROM locations l LEFT JOIN clients c ON c.id = l.client_id "
        "WHERE l.id = $1 FOR UPDATE OF l",
        pqxx::params{id});
    if (rows.empty()) return not_found("location");
    const auto old_client = rows[0][0].as<std::optional<std::int64_t>>();
    const auto old_client_name = rows[0][1].as<OptString>();
    const auto old_interval = rows[0][2].as<int>();
    const auto old_posting = rows[0][3].as<bool>();
    const auto old_enabled = rows[0][4].as<bool>();
    const bool accessible = rows[0][5].as<std::string>() == "active";
    const auto old_industry = rows[0][6].as<OptString>();
    const auto old_from = rows[0][7].as<OptString>();
    const auto old_audience = rows[0][8].as<std::string>();
    const auto old_auto_post = rows[0][9].as<bool>(), old_five_star = rows[0][10].as<bool>(), old_four_star = rows[0][11].as<bool>();
    const auto now = to_timestamptz(audit.now());

    // Check everything before writing anything: a refused request changes nothing.
    const auto interval = patch.review_poll_interval_s.value_or(old_interval);
    if (interval != old_interval && (interval < 60 || interval > 86400)) return invalid("review_poll_interval_s must be between 60 and 86400.");
    const auto industry = patch.industry_key.over(old_industry);
    // Only an industry the knowledge base supports can be given; one the location already has may stay.
    if (industry && industry != old_industry && !industry_supported(tx, *industry)) {
        return invalid("\"industry_key\" must be the key of an industry in the knowledge base, or null.");
    }
    const auto from = patch.analyze_from.over(old_from);
    if (!is_day_or_none(from)) return invalid(std::string(kBadAnalyzeFrom));
    const auto audience = patch.audience ? trimmed(*patch.audience) : old_audience;
    if (text::length(audience) > kMaxAudienceLength) {
        return invalid(std::format("\"audience\" may be up to {} characters.", kMaxAudienceLength));
    }
    const auto& client_id = patch.client_id.value();
    const bool new_client = patch.client_id.given() && client_id != old_client;
    OptString new_client_name;
    if (new_client && client_id) {
        const auto client = tx.exec("SELECT name FROM clients WHERE id = $1 AND NOT archived", pqxx::params{*client_id});
        if (client.empty()) return conflict("client_not_found", "That client doesn't exist or is archived.");
        new_client_name = client[0][0].as<std::string>();
    }
    const bool new_enabled = patch.enabled && *patch.enabled != old_enabled;
    if (new_enabled && *patch.enabled && !accessible) {
        return conflict("location_not_accessible", "The managing account no longer sees this location, so it can't be switched on.");
    }

    // What the location is told about itself, in one entry.
    boost::json::object before, after;
    const auto changed = [&](std::string_view name, boost::json::value was, boost::json::value is) {
        if (was == is) return;
        before[name] = std::move(was);
        after[name] = std::move(is);
    };
    changed("review_poll_interval_s", old_interval, interval);
    changed("industry_key", optional_json(old_industry), optional_json(industry));
    changed("analyze_from", optional_json(old_from), optional_json(from));
    changed("audience", boost::json::value(old_audience), boost::json::value(audience));
    const bool new_fields = !after.empty();
    if (new_fields) {
        tx.exec("UPDATE locations SET review_poll_interval_s = $2, industry_key = $3, analyze_from = $4::date, audience = $5, "
                "updated_at = $6::timestamptz WHERE id = $1",
                pqxx::params{id, interval, industry, from, audience, now})
            .no_rows();
        audit.record("location.update", "location", id, {.before = std::move(before), .after = std::move(after)});
    }
    if (new_client) {
        // Bookkeeping only (D-102): the location keeps its settings, switches, tests and reports.
        tx.exec("UPDATE locations SET client_id = $2, updated_at = $3::timestamptz WHERE id = $1", pqxx::params{id, client_id, now})
            .no_rows();
        audit.record(client_id ? "location.link" : "location.unlink", "location", id,
                     {.before = link_json(old_client, old_client_name), .after = link_json(client_id, new_client_name)});
    }
    if (new_enabled && *patch.enabled) {
        // Polled and fully synced at once (D-037 point 2); nothing else is switched on and no pause is lifted.
        tx.exec("UPDATE locations SET enabled = true, next_review_poll_at = $2::timestamptz, next_full_sync_at = $2::timestamptz, "
                "updated_at = $2::timestamptz WHERE id = $1",
                pqxx::params{id, now})
            .no_rows();
        audit.record("location.enable", "location", id, {.before = {{"enabled", false}}, .after = {{"enabled", true}}});
    } else if (new_enabled) {
        // Auto-posting goes with it, and a person switches it on again (D-070).
        tx.exec("UPDATE locations SET enabled = false, auto_post_enabled = false, auto_post_five_star = false, "
                "auto_post_four_star = false, updated_at = $2::timestamptz WHERE id = $1",
                pqxx::params{id, now})
            .no_rows();
        audit.record("location.disable", "location", id,
                     {.before = {{"enabled", true},
                                 {"auto_post_enabled", old_auto_post},
                                 {"auto_post_five_star", old_five_star},
                                 {"auto_post_four_star", old_four_star}},
                      .after = {{"enabled", false}, {"auto_post_enabled", false}, {"auto_post_five_star", false}, {"auto_post_four_star", false}}});
    }

    const bool new_posting = patch.posting_enabled && *patch.posting_enabled != old_posting;
    if (new_posting) {
        tx.exec("UPDATE locations SET posting_enabled = $2, updated_at = $3::timestamptz WHERE id = $1",
                pqxx::params{id, *patch.posting_enabled, now})
            .no_rows();
        // Its own action name: who paused or resumed posting must be easy to find.
        audit.record(*patch.posting_enabled ? "location.posting_resumed" : "location.posting_paused", "location", id,
                     {.before = {{"posting_enabled", old_posting}}, .after = {{"posting_enabled", *patch.posting_enabled}}});
    }

    if (!new_fields && !new_client && !new_enabled && !new_posting) audit.nothing_changed();
    return *get_location(tx, id);
}

// ---- location discovery --------------------------------------------------------

void request_discovery(pqxx::transaction_base& tx, audit::Writer& audit) {
    jobs::sql::enqueue(tx, {.kind = std::string(kDiscoverLocationsKind), .max_attempts = 1}, audit.now());
    audit.record({.action = "location.discovery_requested", .object_type = "location"});
}

DiscoveryOutcome apply_discovery(pqxx::transaction_base& tx, const std::vector<SeenLocation>& seen, core::TimePoint now) {
    audit::Writer log{tx, audit::Actor::system("location-discovery"), now};
    const auto now_text = to_timestamptz(now);
    DiscoveryOutcome outcome;
    std::vector<std::string> seen_ids;

    for (const auto& [account_id, location] : seen) {
        const auto location_id = gbp::strip_prefix(location.name, "locations/");
        if (!location_id) continue;
        seen_ids.push_back(*location_id);
        ++outcome.seen;

        const auto address = location.storefrontAddress.value_or(gbp::PostalAddress{});
        const auto metadata = location.metadata.value_or(gbp::LocationMetadata{});
        const auto title = location.title.value_or(*location_id);
        const pqxx::params fields{
            *location_id,
            account_id,
            title,
            location.storeCode,
            location.categories.value_or(gbp::Categories{}).primaryCategory.value_or(gbp::Category{}).displayName,
            non_empty(join_lines(address.addressLines)),
            address.locality,
            address.administrativeArea,
            address.postalCode,
            address.regionCode,
            location.phoneNumbers.value_or(gbp::PhoneNumbers{}).primaryPhone,
            location.websiteUri,
            metadata.placeId,
            metadata.mapsUri,
            boost::json::serialize(location.raw),
            now_text,
        };

        const auto existing = tx.exec(
            "SELECT id, access_status, gbp_account_id FROM locations WHERE gbp_location_id = $1 FOR UPDATE",
            pqxx::params{*location_id});
        if (existing.empty()) {
            const auto id = tx.query_value<std::int64_t>(
                R"sql(
                INSERT INTO locations (gbp_location_id, gbp_account_id, title, store_code, primary_category, address,
                                       locality, administrative_area, postal_code, region_code, phone, website_uri,
                                       place_id, maps_uri, raw, first_seen_at, last_seen_at, updated_at)
                VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15::jsonb,
                        $16::timestamptz, $16::timestamptz, $16::timestamptz)
                RETURNING id)sql",
                fields);
            // Every location has its reply settings from the start, enabled or not (D-093, D-102).
            write_default_reply_settings(tx, id, now);
            log.record("location.discovered", "location", id,
                       {.after = {{"title", title}, {"gbp_location_id", *location_id}, {"gbp_account_id", account_id}}});
            ++outcome.created;
            continue;
        }

        const auto id = existing[0][0].as<std::int64_t>();
        const bool was_lost = existing[0][1].as<std::string>() == "lost";
        const auto old_account = existing[0][2].as<std::string>();
        tx.exec(
              R"sql(
            UPDATE locations
               SET gbp_account_id = $2, title = $3, store_code = $4, primary_category = $5, address = $6,
                   locality = $7, administrative_area = $8, postal_code = $9, region_code = $10, phone = $11,
                   website_uri = $12, place_id = $13, maps_uri = $14, raw = $15::jsonb,
                   last_seen_at = $16::timestamptz, updated_at = $16::timestamptz,
                   missed_discoveries = 0, access_status = 'active', access_lost_at = NULL,
                   -- Back after a gap: poll at once.
                   next_review_poll_at = CASE WHEN access_status = 'lost' THEN $16::timestamptz ELSE next_review_poll_at END
             WHERE gbp_location_id = $1)sql",
              fields)
            .no_rows();
        if (was_lost) {
            log.record("location.access_restored", "location", id, {.before = {{"access_status", "lost"}}, .after = {{"access_status", "active"}}});
            ++outcome.restored;
        }
        if (old_account != account_id) {
            log.record("location.account_changed", "location", id,
                       {.before = {{"gbp_account_id", old_account}}, .after = {{"gbp_account_id", account_id}}});
        }
    }

    // One miss can be a hiccup on Google's side; two complete listings in a
    // row without the location means our access is gone.
    const auto missing = tx.exec(
        R"sql(
        UPDATE locations
           SET missed_discoveries = missed_discoveries + 1, updated_at = $2::timestamptz
         WHERE access_status = 'active'
           AND gbp_location_id NOT IN (SELECT jsonb_array_elements_text($1::jsonb))
        RETURNING id, title, missed_discoveries, enabled)sql",
        pqxx::params{db::json_list(seen_ids), now_text});
    for (const auto& row : missing) {
        if (row[2].as<int>() < 2) continue;
        const auto id = row[0].as<std::int64_t>();
        tx.exec("UPDATE locations SET access_status = 'lost', access_lost_at = $2::timestamptz WHERE id = $1",
                pqxx::params{id, now_text})
            .no_rows();
        log.record("location.access_lost", "location", id,
                   {.before = {{"access_status", "active"}}, .after = {{"access_status", "lost"}, {"title", row[1].as<std::string>()}}});
        // A location we work on that we can no longer reach is a system
        // problem (FR-7); one that is switched off is not.
        if (row[3].as<bool>()) {
            emit(tx,
                 {.type = std::string(kEventSystemProblem),
                  .key = std::format("location:{}:access_lost:{}", id, now_text),
                  .location_id = id,
                  .review_id = std::nullopt,
                  .subject = "Lost access to a location",
                  .body = std::format("The managing Google account can no longer see \"{}\": two location listings in a row did "
                                      "not include it. Its reviews are not synced and no reply can be posted until access is back.",
                                      row[1].as<std::string>()),
                  .link_path = "/#/status"},
                 now);
        }
        ++outcome.lost;
    }
    return outcome;
}

// ---- audit log and status ------------------------------------------------------

model::AuditList list_audit(pqxx::transaction_base& tx, const AuditFilter& filter) {
    db::Conditions where;
    if (!filter.action.empty()) where.add(std::format("a.action = {}", where.bind(filter.action)));
    if (!filter.object_type.empty()) where.add(std::format("a.object_type = {}", where.bind(filter.object_type)));
    if (!filter.object_id.empty()) where.add(std::format("a.object_id = {}", where.bind(filter.object_id)));
    if (!filter.actor.empty()) where.add(std::format("position(lower({}) in lower(a.actor)) > 0", where.bind(filter.actor)));
    if (filter.location_id) where.add(std::format("a.location_id = {}", where.bind(*filter.location_id)));
    return db::page<model::AuditEntry>(
        tx,
        std::format("SELECT a.id, {} AS occurred_at, a.actor_type, a.actor, a.action, a.object_type, a.object_id, "
                    "a.location_id, l.title AS location_title, a.before, a.after, a.ip "
                    "FROM audit_log a LEFT JOIN locations l ON l.id = a.location_id",
                    iso("a.occurred_at")),
        where, "a.occurred_at DESC, a.id DESC", filter.page);
}

model::SystemStatus system_status(pqxx::transaction_base& tx, core::TimePoint now) {
    model::SystemStatus status;
    status.database.reachable = true;
    if (const auto meta = knowledge_meta(tx)) status.knowledge = {.version = meta->version, .seeded_at = meta->seeded_at};

    const auto token = integration(tx, kGbpTokenStatus);
    const auto gbp = load_credential(tx, kGbpCredential);
    status.gbp = {.connected = gbp.has_value(),
                  .account_email = gbp ? gbp->account_email : std::nullopt,
                  .last_ok_at = token.last_ok_at,
                  .last_error = token.last_error,
                  .last_error_at = token.last_error_at};
    const auto discovery = integration(tx, kDiscoveryStatus);
    status.location_discovery = {.last_ok_at = discovery.last_ok_at, .last_error = discovery.last_error, .last_error_at = discovery.last_error_at};
    const auto pubsub = integration(tx, kPubsubStatus);
    status.pubsub = {.enabled = false,  // the API fills it in: the repository does not know the configuration
                     .last_ok_at = pubsub.last_ok_at,
                     .last_error = pubsub.last_error,
                     .last_error_at = pubsub.last_error_at,
                     .last_message_at = pubsub.last_message_at};
    status.ai = ai_status(tx);
    status.posting = posting_status(tx);
    status.email = email_status(tx, now);

    status.locations = db::rows_as<model::LocationSyncStatus>(
        tx, std::format(
                R"sql(
        SELECT l.id, l.title, c.name AS client_name, l.access_status, l.enabled AND l.access_status = 'active' AS syncing,
               (SELECT count(*) FROM reviews r WHERE r.location_id = l.id AND r.removed_at IS NULL) AS review_count,
               {} AS reviews_polled_at, {} AS reviews_full_synced_at, l.last_sync_error, {} AS last_sync_error_at
          FROM locations l LEFT JOIN clients c ON c.id = l.client_id
         ORDER BY lower(l.title), l.id)sql",
                iso("l.reviews_polled_at"), iso("l.reviews_full_synced_at"), iso("l.last_sync_error_at")));

    const auto week_ago = to_timestamptz(now - std::chrono::hours(24 * 7));
    const auto counts = tx.exec(
                              "SELECT count(*) FILTER (WHERE status = 'queued'), count(*) FILTER (WHERE status = 'running'), "
                              "count(*) FILTER (WHERE status = 'failed' AND finished_at > $1::timestamptz) FROM jobs",
                              pqxx::params{week_ago})
                            .one_row();
    status.jobs.queued = counts[0].as<std::int64_t>();
    status.jobs.running = counts[1].as<std::int64_t>();
    status.jobs.failed = counts[2].as<std::int64_t>();
    status.jobs.failures = db::rows_as<model::JobFailure>(
        tx,
        std::format("SELECT id, kind, attempts, left(last_error, 500) AS last_error, {} AS failed_at FROM jobs "
                    "WHERE status = 'failed' AND finished_at > $1::timestamptz ORDER BY finished_at DESC, id DESC LIMIT 20",
                    iso("finished_at")),
        pqxx::params{week_ago});
    return status;
}

}  // namespace poni::repo
