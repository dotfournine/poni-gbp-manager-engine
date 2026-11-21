// Clients, locations, the audit log and the system status, as the API reads
// and changes them, plus what location discovery writes. Blocking calls on a
// transaction the caller owns: run them through Database::run.
#pragma once

#include "connectors/gbp/reader.hpp"
#include "core/body.hpp"
#include "core/clock.hpp"
#include "db/audit.hpp"
#include "db/failure.hpp"
#include "db/sql.hpp"
#include "domain/models.hpp"

#include <array>
#include <boost/describe/class.hpp>
#include <pqxx/pqxx>

#include <optional>
#include <string>
#include <vector>

namespace poni::repo {

// ---- clients -------------------------------------------------------------------

struct ClientFilter {
    std::optional<bool> archived;
    std::string q;  // part of the name
    db::Page page;
};

// The bodies of POST /api/clients and PATCH /api/clients/{id}, member for member.
struct NewClient {
    std::string name;
    std::optional<std::string> notes;
    // Who the customers are: at most 500 characters.
    std::optional<std::string> audience;
    // The day analysis starts from, "YYYY-MM-DD"; null or left out for every review.
    core::Nullable<std::string> analyze_from;
};
BOOST_DESCRIBE_STRUCT(NewClient, (), (name, notes, audience, analyze_from))

struct ClientPatch {
    std::optional<std::string> name;
    std::optional<std::string> notes;
    std::optional<std::string> audience;
    std::optional<bool> archived;
    // Null: no industry.
    core::Nullable<std::string> industry_key;
    // Null: every review.
    core::Nullable<std::string> analyze_from;
};
BOOST_DESCRIBE_STRUCT(ClientPatch, (), (name, notes, audience, archived, industry_key, analyze_from))

model::ClientList list_clients(pqxx::transaction_base& tx, const ClientFilter& filter);
std::optional<model::Client> get_client(pqxx::transaction_base& tx, std::int64_t id);
Result<model::Client> create_client(pqxx::transaction_base& tx, audit::Writer& audit, const NewClient& client);
Result<model::Client> update_client(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id, const ClientPatch& patch);

// ---- locations -----------------------------------------------------------------

// The values of locations.access_status.
inline constexpr std::array<std::string_view, 2> kAccessStatuses{"active", "lost"};

struct LocationFilter {
    std::optional<std::int64_t> client_id;
    std::optional<bool> linked;
    std::optional<std::string> access_status;  // one of kAccessStatuses
    std::string q;  // part of the title
    db::Page page;
};

// The body of PATCH /api/locations/{id}, member for member.
struct LocationPatch {
    // The client to link to; null unlinks.
    core::Nullable<std::int64_t> client_id;
    std::optional<int> review_poll_interval_s;
    // The location's kill switch for posting.
    std::optional<bool> posting_enabled;
};
BOOST_DESCRIBE_STRUCT(LocationPatch, (), (client_id, review_poll_interval_s, posting_enabled))

model::LocationList list_locations(pqxx::transaction_base& tx, const LocationFilter& filter);
std::optional<model::Location> get_location(pqxx::transaction_base& tx, std::int64_t id);
// Linking a location to a client makes its reviews due for a full sync at once.
Result<model::Location> update_location(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id, const LocationPatch& patch);

// ---- location discovery --------------------------------------------------------

struct SeenLocation {
    std::string account_id;
    gbp::Location location;
};

struct DiscoveryOutcome {
    int seen = 0;
    int created = 0;
    int restored = 0;
    int lost = 0;
};


// The job that lists every location under every account (jobs/sync.hpp runs it).
inline constexpr std::string_view kDiscoverLocationsKind = "locations.discover";

// "Look for new locations": queues one discovery, now, for the person who asked.
void request_discovery(pqxx::transaction_base& tx, audit::Writer& audit);

// Stores the complete result of one discovery: every location under every
// account. New locations are added, known ones refreshed, and an active
// location missing from two complete discoveries in a row is marked lost.
// New, lost and restored locations get an audit entry. Call this only with a
// complete listing: a partial one would count locations as missing.
DiscoveryOutcome apply_discovery(pqxx::transaction_base& tx, const std::vector<SeenLocation>& seen, core::TimePoint now);

// ---- audit log and status ------------------------------------------------------

struct AuditFilter {
    std::string action;
    std::string object_type;
    std::string object_id;
    std::string actor;  // part of the actor label
    db::Page page;
};

model::AuditList list_audit(pqxx::transaction_base& tx, const AuditFilter& filter);

// Everything on the system status page except `version` and `pubsub.enabled`.
model::SystemStatus system_status(pqxx::transaction_base& tx, core::TimePoint now);

}  // namespace poni::repo
