#include "db/repo_auth.hpp"

#include "core/json.hpp"
#include "db/database.hpp"
#include "db/sql.hpp"

#include <boost/json/serialize.hpp>

#include <format>
#include <boost/json/value_from.hpp>

namespace poni::repo {

using db::to_timestamptz;

void save_credential(pqxx::transaction_base& tx, std::string_view name, const StoredCredential& credential,
                     core::TimePoint now) {
    tx.exec(
          R"sql(
        INSERT INTO credentials (name, ciphertext, key_id, created_at, updated_at)
        VALUES ($1, $2, $3, $4::timestamptz, $4::timestamptz)
        ON CONFLICT (name) DO UPDATE
           SET ciphertext = EXCLUDED.ciphertext, key_id = EXCLUDED.key_id, updated_at = EXCLUDED.updated_at)sql",
          pqxx::params{std::string(name), credential.ciphertext, credential.key_id, to_timestamptz(now)})
        .no_rows();
}

std::optional<StoredCredential> load_credential(pqxx::transaction_base& tx, std::string_view name) {
    const auto rows = tx.exec("SELECT ciphertext, key_id FROM credentials WHERE name = $1", pqxx::params{std::string(name)});
    if (rows.empty()) return std::nullopt;
    return StoredCredential{rows[0][0].as<std::string>(), rows[0][1].as<std::string>()};
}

void save_oauth_credentials(pqxx::transaction_base& tx, const core::SecretBox& box, std::string_view name,
                            const google::OAuthCredentials& credentials, core::TimePoint now) {
    const auto plaintext = boost::json::serialize(boost::json::value_from(credentials));
    save_credential(tx, name, {box.seal(plaintext, name), box.key_id()}, now);
}

std::expected<std::optional<google::OAuthCredentials>, std::string> load_oauth_credentials(pqxx::transaction_base& tx,
                                                                                           const core::SecretBox& box,
                                                                                           std::string_view name) {
    const auto stored = load_credential(tx, name);
    if (!stored) return std::nullopt;
    if (stored->key_id != box.key_id()) {
        return std::unexpected(std::format("the stored {} token was encrypted with another PONI_SECRET_KEY; connect the account "
                                           "again on the dashboard's status page",
                                           name));
    }
    const auto plaintext = box.open(stored->ciphertext, name);
    if (!plaintext) return std::unexpected(plaintext.error());
    auto credentials = core::parse_json<google::OAuthCredentials>(*plaintext);
    if (!credentials) return std::unexpected(std::format("the stored {} token is not readable", name));
    return std::optional(std::move(*credentials));
}

void save_gbp_credentials(pqxx::transaction_base& tx, const core::SecretBox& box,
                          const google::OAuthCredentials& credentials, core::TimePoint now) {
    save_oauth_credentials(tx, box, kGbpCredential, credentials, now);
}

std::expected<std::optional<google::OAuthCredentials>, std::string> load_gbp_credentials(pqxx::transaction_base& tx,
                                                                                         const core::SecretBox& box) {
    return load_oauth_credentials(tx, box, kGbpCredential);
}

void mark_integration_ok(pqxx::transaction_base& tx, std::string_view name, core::TimePoint now,
                         const boost::json::object& detail) {
    tx.exec(
          R"sql(
        INSERT INTO integration_status (name, last_ok_at, detail, updated_at)
        VALUES ($1, $2::timestamptz, $3::jsonb, $2::timestamptz)
        ON CONFLICT (name) DO UPDATE
           SET last_ok_at = EXCLUDED.last_ok_at, last_error = NULL, last_error_at = NULL,
               detail = integration_status.detail || EXCLUDED.detail, updated_at = EXCLUDED.updated_at)sql",
          pqxx::params{std::string(name), to_timestamptz(now), boost::json::serialize(detail)})
        .no_rows();
}

void mark_integration_error(pqxx::transaction_base& tx, std::string_view name, std::string_view error,
                            core::TimePoint now) {
    tx.exec(
          R"sql(
        INSERT INTO integration_status (name, last_error, last_error_at, updated_at)
        VALUES ($1, $2, $3::timestamptz, $3::timestamptz)
        ON CONFLICT (name) DO UPDATE
           SET last_error = EXCLUDED.last_error, last_error_at = EXCLUDED.last_error_at,
               updated_at = EXCLUDED.updated_at)sql",
          pqxx::params{std::string(name), std::string(error), to_timestamptz(now)})
        .no_rows();
}

void record_integration(pqxx::transaction_base& tx, std::string_view name, const std::optional<std::string>& error, core::TimePoint now,
                        const boost::json::object& detail) {
    if (error) {
        mark_integration_error(tx, name, *error, now);
    } else {
        mark_integration_ok(tx, name, now, detail);
    }
}

Integration integration(pqxx::transaction_base& tx, std::string_view name) {
    return db::row_as<Integration>(tx,
                                   std::format("SELECT {} AS last_ok_at, last_error, {} AS last_error_at, "
                                               "detail->>'last_message_at' AS last_message_at FROM integration_status WHERE name = $1",
                                               db::iso("last_ok_at"), db::iso("last_error_at")),
                                   pqxx::params{std::string(name)})
        .value_or(Integration{});
}

UpsertedUser upsert_user(pqxx::transaction_base& tx, const NewUser& user, core::TimePoint now) {
    const auto row = tx.exec(
                           R"sql(
        INSERT INTO users (google_sub, email, name, picture_url, created_at, last_login_at)
        VALUES ($1, $2, $3, $4, $5::timestamptz, $5::timestamptz)
        ON CONFLICT (google_sub) DO UPDATE
           SET email = EXCLUDED.email, name = EXCLUDED.name, picture_url = EXCLUDED.picture_url,
               last_login_at = CASE WHEN users.status = 'active' THEN EXCLUDED.last_login_at ELSE users.last_login_at END
        RETURNING id, email, name, picture_url, status, (xmax = 0) AS created)sql",
                           pqxx::params{user.google_sub, user.email, user.name, user.picture_url, to_timestamptz(now)})
                         .one_row();
    return {
        .user = {.id = row[0].as<std::int64_t>(),
                 .email = row[1].as<std::string>(),
                 .name = row[2].as<std::string>(),
                 .picture_url = row[3].as<std::optional<std::string>>()},
        .created = row[5].as<bool>(),
        .disabled = row[4].as<std::string>() != "active",
    };
}

void create_session(pqxx::transaction_base& tx, std::int64_t user_id, const NewSession& session, core::TimePoint now) {
    tx.exec(
          R"sql(
        INSERT INTO sessions (token_hash, user_id, kind, csrf_token, created_at, last_seen_at, expires_at, user_agent, ip)
        VALUES ($1, $2, $3, $4, $5::timestamptz, $5::timestamptz, $6::timestamptz, $7, $8))sql",
          pqxx::params{session.token_hash, user_id, std::string(domain::name(session.kind)), session.csrf_token,
                       to_timestamptz(now), to_timestamptz(session.expires_at), session.user_agent, session.ip})
        .no_rows();
}

std::string live_session_sql(int now, int idle_cutoff, int kind) {
    return std::format("s.kind = ${2} AND s.expires_at > ${0}::timestamptz AND s.last_seen_at > ${1}::timestamptz AND u.status = 'active'",
                       now, idle_cutoff, kind);
}

std::optional<SessionUser> find_session(pqxx::transaction_base& tx, std::string_view token_hash, domain::SessionKind kind,
                                        core::TimePoint now, const domain::SessionLifetimes& lifetimes) {
    const auto rows = tx.exec(
        "SELECT u.id, u.email, u.name, u.picture_url, s.csrf_token, "
        "       s.last_seen_at < $2::timestamptz - interval '60 seconds' AS stale "
        "  FROM sessions s JOIN users u ON u.id = s.user_id "
        " WHERE s.token_hash = $1 AND " + live_session_sql(2, 3, 4),
        pqxx::params{std::string(token_hash), to_timestamptz(now), to_timestamptz(now - lifetimes.of(kind).idle),
                     std::string(domain::name(kind))});
    if (rows.empty()) return std::nullopt;
    const auto row = rows[0];
    // At most one write a minute per session, not one per request.
    if (row[5].as<bool>()) {
        tx.exec("UPDATE sessions SET last_seen_at = $2::timestamptz WHERE token_hash = $1",
                pqxx::params{std::string(token_hash), to_timestamptz(now)})
            .no_rows();
    }
    return SessionUser{
        .user = {.id = row[0].as<std::int64_t>(),
                 .email = row[1].as<std::string>(),
                 .name = row[2].as<std::string>(),
                 .picture_url = row[3].as<std::optional<std::string>>()},
        .csrf_token = row[4].as<std::optional<std::string>>(),
    };
}

bool delete_session(pqxx::transaction_base& tx, std::string_view token_hash) {
    return tx.exec("DELETE FROM sessions WHERE token_hash = $1", pqxx::params{std::string(token_hash)}).affected_rows() == 1;
}

std::size_t purge_sessions(pqxx::transaction_base& tx, core::TimePoint now, const domain::SessionLifetimes& lifetimes) {
    std::size_t deleted = 0;
    for (const auto kind : {domain::SessionKind::browser, domain::SessionKind::app}) {
        const auto result = tx.exec(
            "DELETE FROM sessions WHERE kind = $1 AND (expires_at <= $2::timestamptz OR last_seen_at <= $3::timestamptz)",
            pqxx::params{std::string(domain::name(kind)), to_timestamptz(now), to_timestamptz(now - lifetimes.of(kind).idle)});
        deleted += static_cast<std::size_t>(result.affected_rows());
    }
    return deleted;
}

void create_app_code(pqxx::transaction_base& tx, const NewAppCode& code) {
    tx.exec(
          R"sql(
        INSERT INTO app_sign_in_codes (code_hash, user_id, code_challenge, first_sign_in, expires_at)
        VALUES ($1, $2, $3, $4, $5::timestamptz))sql",
          pqxx::params{code.code_hash, code.user_id, code.code_challenge, code.first_sign_in, to_timestamptz(code.expires_at)})
        .no_rows();
}

std::optional<TakenAppCode> take_app_code(pqxx::transaction_base& tx, std::string_view code_hash, core::TimePoint now) {
    // A DELETE cannot stand inside the SELECT that db::row_as wraps a query in, so the row is read here, by name.
    const auto rows = tx.exec(
        R"sql(
        DELETE FROM app_sign_in_codes c USING users u
         WHERE c.code_hash = $1 AND u.id = c.user_id
        RETURNING u.id, u.email, u.name, u.picture_url, u.status = 'active' AS user_active, c.code_challenge,
                  c.first_sign_in, c.expires_at <= $2::timestamptz AS expired)sql",
        pqxx::params{std::string(code_hash), to_timestamptz(now)});
    if (rows.empty()) return std::nullopt;
    const auto row = rows[0];
    return TakenAppCode{.user = {.id = row["id"].as<std::int64_t>(),
                                 .email = row["email"].as<std::string>(),
                                 .name = row["name"].as<std::string>(),
                                 .picture_url = row["picture_url"].as<std::optional<std::string>>()},
                        .user_active = row["user_active"].as<bool>(),
                        .code_challenge = row["code_challenge"].as<std::string>(),
                        .first_sign_in = row["first_sign_in"].as<bool>(),
                        .expired = row["expired"].as<bool>()};
}

std::size_t purge_app_codes(pqxx::transaction_base& tx, core::TimePoint now) {
    const auto result = tx.exec("DELETE FROM app_sign_in_codes WHERE expires_at <= $1::timestamptz", pqxx::params{to_timestamptz(now)});
    return static_cast<std::size_t>(result.affected_rows());
}

}  // namespace poni::repo
