#include "db/repo_devices.hpp"

#include "core/text.hpp"
#include "db/database.hpp"
#include "db/repo_auth.hpp"
#include "db/sql.hpp"
#include "domain/push.hpp"

#include <algorithm>
#include <format>

namespace poni::repo {

using db::iso;
using db::to_timestamptz;

namespace {

bool valid_version(std::string_view version) {
    return !version.empty() && text::length(version) <= 64 &&
           std::ranges::none_of(version, [](unsigned char c) { return c < 0x20 || c == 0x7F; });
}

model::Device get_device(pqxx::transaction_base& tx, std::int64_t id, core::ApnsEnvironment environment) {
    auto device = *db::row_as<model::Device>(
        tx,
        std::format("SELECT d.id, d.app_version, {} AS created_at, {} AS last_seen_at "
                    "FROM devices d JOIN sessions s ON s.token_hash = d.session_token_hash WHERE d.id = $1",
                    iso("d.created_at"), iso("s.last_seen_at")),
        pqxx::params{id}, db::FilledLater{"environment"});
    device.environment = core::name(environment);
    return device;
}

}  // namespace

Result<model::Device> register_device(pqxx::transaction_base& tx, audit::Writer& audit, std::string_view session_token_hash,
                                      const NewDevice& device, std::optional<core::ApnsEnvironment> engine) {
    if (!engine) return invalid("This engine has no push settings, so it registers no device.");
    if (device.environment != core::name(*engine)) {
        return invalid(std::format("This engine sends to APNs {}; a token of \"{}\" can't be reached from it.", core::name(*engine),
                                   device.environment));
    }
    const auto token = text::ascii_lower(device.apns_token);
    if (!domain::is_device_token(token)) return invalid("\"apns_token\" must be the device token APNs gave, in hex.");
    if (!valid_version(device.app_version)) return invalid("\"app_version\" must be 1 to 64 characters of text.");

    const auto now = to_timestamptz(audit.now());
    // A shared phone changes hands: the token leaves the session that had it.
    const auto moved = tx.exec("DELETE FROM devices WHERE apns_token = $1 AND session_token_hash <> $2 RETURNING id",
                               pqxx::params{token, std::string(session_token_hash)});
    const auto existing = tx.exec("SELECT id, apns_token, app_version FROM devices WHERE session_token_hash = $1 FOR UPDATE",
                                  pqxx::params{std::string(session_token_hash)});
    if (moved.empty() && !existing.empty() && existing[0][1].as<std::string>() == token &&
        existing[0][2].as<std::string>() == device.app_version) {
        audit.nothing_changed();
        return get_device(tx, existing[0][0].as<std::int64_t>(), *engine);
    }
    const auto id = tx.exec(R"sql(
        INSERT INTO devices (session_token_hash, apns_token, app_version, created_at, updated_at)
        VALUES ($1, $2, $3, $4::timestamptz, $4::timestamptz)
        ON CONFLICT (session_token_hash) DO UPDATE SET apns_token = EXCLUDED.apns_token, app_version = EXCLUDED.app_version,
                                                       updated_at = EXCLUDED.updated_at
        RETURNING id)sql",
                            pqxx::params{std::string(session_token_hash), token, device.app_version, now})
                        .one_row()[0]
                        .as<std::int64_t>();
    // The token is in no entry: what is recorded is that this session can be told.
    audit.record("device.register", "device", id,
                 {.after = {{"app_version", device.app_version},
                            {"environment", std::string(core::name(*engine))},
                            {"replaced", !existing.empty()},
                            {"taken_from_another_session", !moved.empty()}}});
    return get_device(tx, id, *engine);
}

Result<void> unregister_device(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t user_id, std::int64_t id) {
    const auto rows = tx.exec("DELETE FROM devices d USING sessions s WHERE d.id = $1 AND s.token_hash = d.session_token_hash "
                              "AND s.user_id = $2 RETURNING d.app_version",
                              pqxx::params{id, user_id});
    if (rows.empty()) return not_found("device");
    audit.record("device.unregister", "device", id, {.before = {{"app_version", rows[0][0].as<std::string>()}}});
    return {};
}

std::vector<PushDevice> push_devices(pqxx::transaction_base& tx, std::string_view email, core::TimePoint now,
                                     const domain::SessionLifetimes& lifetimes) {
    const auto rows = tx.exec("SELECT d.id, d.apns_token FROM devices d "
                              "JOIN sessions s ON s.token_hash = d.session_token_hash JOIN users u ON u.id = s.user_id "
                              "WHERE lower(u.email) = lower($1) AND " + live_session_sql(2, 3, 4) + " ORDER BY d.id",
                              pqxx::params{std::string(email), to_timestamptz(now), to_timestamptz(now - lifetimes.app.idle),
                                           std::string(domain::name(domain::SessionKind::app))});
    std::vector<PushDevice> out;
    for (const auto& row : rows) out.push_back({row[0].as<std::int64_t>(), row[1].as<std::string>()});
    return out;
}

void drop_device(pqxx::transaction_base& tx, std::int64_t id, std::string_view apns_token) {
    tx.exec("DELETE FROM devices WHERE id = $1 AND apns_token = $2", pqxx::params{id, std::string(apns_token)});
}

}  // namespace poni::repo
