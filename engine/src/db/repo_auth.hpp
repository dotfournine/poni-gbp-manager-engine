// Stored secrets, integration health, users and sessions. Blocking calls on a
// transaction the caller owns: run them through Database::run.
#pragma once

#include "connectors/google/auth.hpp"
#include "core/clock.hpp"
#include "core/crypto.hpp"
#include "domain/models.hpp"
#include "domain/session.hpp"

#include <boost/describe/class.hpp>
#include <boost/json/object.hpp>
#include <pqxx/pqxx>

#include <chrono>
#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace poni::repo {

// ---- credentials ---------------------------------------------------------------

inline constexpr std::string_view kGbpCredential = "gbp";
inline constexpr std::string_view kWorkspaceCredential = "workspace";

struct StoredCredential {
    std::string ciphertext;  // core::SecretBox::seal output
    std::string key_id;
    // The Google account's address, from the connection's ID token (D-100); none for a credential stored before.
    std::optional<std::string> account_email;
};

void save_credential(pqxx::transaction_base& tx, std::string_view name, const StoredCredential& credential,
                     core::TimePoint now);
std::optional<StoredCredential> load_credential(pqxx::transaction_base& tx, std::string_view name);

// An OAuth client and refresh token stored under `name` ("gbp", "gmail", "workspace"),
// encrypted with `box`, with the address of the account they belong to,
// which replaces the one stored before (none: unknown). Loading returns
// nothing if none is stored, and an error message if the stored value can't
// be read with this key.
void save_oauth_credentials(pqxx::transaction_base& tx, const core::SecretBox& box, std::string_view name,
                            const google::OAuthCredentials& credentials, std::optional<std::string> account_email,
                            core::TimePoint now);
std::expected<std::optional<google::OAuthCredentials>, std::string> load_oauth_credentials(pqxx::transaction_base& tx,
                                                                                           const core::SecretBox& box,
                                                                                           std::string_view name);

// The GBP managing account's OAuth client and refresh token, encrypted with
// `box`. Loading returns nothing if none is stored, and an error message if
// the stored value can't be read with this key.
void save_gbp_credentials(pqxx::transaction_base& tx, const core::SecretBox& box,
                          const google::OAuthCredentials& credentials, core::TimePoint now);
std::expected<std::optional<google::OAuthCredentials>, std::string> load_gbp_credentials(pqxx::transaction_base& tx,
                                                                                         const core::SecretBox& box);

// ---- integration status --------------------------------------------------------

inline constexpr std::string_view kGbpTokenStatus = "gbp_token";
inline constexpr std::string_view kWorkspaceStatus = "workspace";
inline constexpr std::string_view kDiscoveryStatus = "location_discovery";
inline constexpr std::string_view kPubsubStatus = "pubsub";

// Records a success and clears the last error. `detail` keys are merged into the stored detail.
void mark_integration_ok(pqxx::transaction_base& tx, std::string_view name, core::TimePoint now,
                         const boost::json::object& detail = {});
void mark_integration_error(pqxx::transaction_base& tx, std::string_view name, std::string_view error,
                            core::TimePoint now);
// For a caller that holds what a call to the integration came to: the error,
// or nothing for a success (and then `detail`, as for mark_integration_ok).
void record_integration(pqxx::transaction_base& tx, std::string_view name, const std::optional<std::string>& error, core::TimePoint now,
                        const boost::json::object& detail = {});

// An integration_status row as the status page shows it; all empty if the integration never reported.
struct Integration {
    std::optional<std::string> last_ok_at;
    std::optional<std::string> last_error;
    std::optional<std::string> last_error_at;
    std::optional<std::string> last_message_at;  // Pub/Sub only
};
BOOST_DESCRIBE_STRUCT(Integration, (), (last_ok_at, last_error, last_error_at, last_message_at))
Integration integration(pqxx::transaction_base& tx, std::string_view name);

// ---- users and sessions --------------------------------------------------------

struct NewUser {
    std::string google_sub;
    std::string email;
    std::string name;
    std::optional<std::string> picture_url;
};

struct UpsertedUser {
    model::User user;
    bool created = false;
    bool disabled = false;
};

// Creates the user on first sign-in, otherwise refreshes email, name and picture.
UpsertedUser upsert_user(pqxx::transaction_base& tx, const NewUser& user, core::TimePoint now);

struct NewSession {
    std::string token_hash;
    domain::SessionKind kind = domain::SessionKind::browser;
    // A browser session's CSRF token; an app session has none (the table checks both).
    std::optional<std::string> csrf_token;
    core::TimePoint expires_at;
    std::string user_agent;
    std::string ip;
};

void create_session(pqxx::transaction_base& tx, std::int64_t user_id, const NewSession& session, core::TimePoint now);

struct SessionUser {
    model::User user;
    std::optional<std::string> csrf_token;
};

// The session's user if a session of this kind exists, hasn't expired, was
// used within its kind's idle limit and belongs to an active user. Also notes
// that it was just used. The caller says the kind from how the token came (a
// cookie: browser; a bearer token: app), so neither is taken for the other.
// The condition, in SQL, that the session `s` of the user `u` has not
// ended: of its kind, not expired, used within its kind's idle limit, of an
// active user. The arguments are the numbers of the parameters that hold
// now, now less the kind's idle limit, and the kind. The one place a
// session's life is written in SQL, for find_session and for push.
std::string live_session_sql(int now, int idle_cutoff, int kind);

std::optional<SessionUser> find_session(pqxx::transaction_base& tx, std::string_view token_hash, domain::SessionKind kind,
                                        core::TimePoint now, const domain::SessionLifetimes& lifetimes);

bool delete_session(pqxx::transaction_base& tx, std::string_view token_hash);

// Deletes sessions that have expired or sat idle for longer than their kind may. Returns how many.
std::size_t purge_sessions(pqxx::transaction_base& tx, core::TimePoint now, const domain::SessionLifetimes& lifetimes);

// ---- the phone app's one-time sign-in codes (D-095) -----------------------------

struct NewAppCode {
    std::string code_hash;
    std::int64_t user_id = 0;
    std::string code_challenge;  // PKCE, S256, as the app sent it at the start
    bool first_sign_in = false;
    core::TimePoint expires_at;
};

void create_app_code(pqxx::transaction_base& tx, const NewAppCode& code);

// A code that was presented, and so is gone from the table.
struct TakenAppCode {
    model::User user;
    bool user_active = false;
    std::string code_challenge;
    bool first_sign_in = false;
    bool expired = false;
};

// Takes the code out of the table in one statement, so that of two requests
// with the same code only one gets it. Nothing for a code that is not there.
// The caller checks what it got; the code is spent either way.
std::optional<TakenAppCode> take_app_code(pqxx::transaction_base& tx, std::string_view code_hash, core::TimePoint now);

// Deletes codes nobody exchanged in time. Returns how many.
std::size_t purge_app_codes(pqxx::transaction_base& tx, core::TimePoint now);

}  // namespace poni::repo
