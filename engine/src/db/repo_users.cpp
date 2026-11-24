#include "db/repo_users.hpp"

#include "connectors/workspace/directory.hpp"
#include "db/audit.hpp"
#include "db/database.hpp"
#include "db/repo_auth.hpp"
#include "db/repo_notifications.hpp"
#include "db/sql.hpp"

#include <boost/json/object.hpp>

#include <format>
#include <map>
#include <ranges>
#include <stdexcept>
#include <string>

namespace poni::repo {

model::UserList list_users(pqxx::transaction_base& tx, std::string_view status) {
    const std::string select = "SELECT id, email, name, picture_url, status FROM users";
    const std::string order = " ORDER BY lower(name), id";
    if (status == "all") return {.items = db::rows_as<model::User>(tx, select + order)};
    return {.items = db::rows_as<model::User>(tx, select + " WHERE status = $1" + order, pqxx::params{status})};
}

// ---- people from the directory (D-106 point 8) ---------------------------------

namespace {

struct UserBySub {
    std::string google_sub;
    std::int64_t id = 0;
};
BOOST_DESCRIBE_STRUCT(UserBySub, (), (google_sub, id))

}  // namespace

std::map<std::string, std::int64_t> user_ids_by_google_sub(pqxx::transaction_base& tx, const std::vector<std::string>& ids) {
    std::map<std::string, std::int64_t> out;
    if (ids.empty()) return out;
    for (auto& user : db::rows_as<UserBySub>(tx, "SELECT google_sub, id FROM users WHERE google_sub IN (SELECT jsonb_array_elements_text($1::jsonb))",
                                             pqxx::params{db::json_list(ids)})) {
        out.emplace(std::move(user.google_sub), user.id);
    }
    return out;
}

CreatedUser create_directory_user(pqxx::transaction_base& tx, audit::Writer& audit, const workspace::DirectoryUser& person) {
    // No sign-in time, no photo, the column's default status (active): a row that gives nobody access before their own
    // sign-in. DO NOTHING: whoever the person is already (signed in or not, active or disabled) stays as they are.
    const auto inserted = tx.exec("INSERT INTO users (google_sub, email, name, created_at) VALUES ($1, $2, $3, $4::timestamptz) "
                                  "ON CONFLICT (google_sub) DO NOTHING RETURNING id",
                                  pqxx::params{person.id, person.primary_email, person.full_name, db::to_timestamptz(audit.now())});
    const bool created = !inserted.empty();
    if (created) {
        audit.record("user.create", "user", inserted[0]["id"].as<std::int64_t>(),
                     {.after = {{"email", person.primary_email}, {"directory_id", person.id}, {"source", "directory"}}});
    } else {
        audit.nothing_changed();
    }
    return {.user = db::one_as<model::User>(tx, "SELECT id, email, name, picture_url, status FROM users WHERE google_sub = $1",
                                            pqxx::params{person.id}),
            .created = created};
}

// ---- the staff check (D-106) ---------------------------------------------------

namespace {

// A user as the staff check holds them while it decides.
struct LockedUser {
    std::int64_t id = 0;
    std::string email;
    std::string name;
    std::string status;
};
BOOST_DESCRIBE_STRUCT(LockedUser, (), (id, email, name, status))

const audit::Actor& workspace_actor() {
    static const auto actor = audit::Actor::system("workspace");
    return actor;
}

// The reason a disable names: what Google said.
std::string_view reason_of(DirectoryAnswer answer) {
    switch (answer) {
        case DirectoryAnswer::suspended: return "suspended";
        case DirectoryAnswer::archived: return "archived";
        case DirectoryAnswer::missing: return "deleted";
        case DirectoryAnswer::present: break;
    }
    throw std::logic_error("a present account is no reason to disable anyone");
}

std::string_view google_said(DirectoryAnswer answer) {
    switch (answer) {
        case DirectoryAnswer::suspended: return "Google Workspace shows their account as suspended";
        case DirectoryAnswer::archived: return "Google Workspace shows their account as archived";
        case DirectoryAnswer::missing:
        case DirectoryAnswer::present: break;
    }
    return "their account is no longer in the Workspace directory's list";
}

// The directory's name for the user, when it names one and it is another (D-106 point 7). Never without an entry:
// the user row is not written without one.
void follow_name(pqxx::transaction_base& tx, const LockedUser& user, const std::string& full_name, core::TimePoint now) {
    if (full_name.empty() || full_name == user.name) return;
    tx.exec("UPDATE users SET name = $2 WHERE id = $1", pqxx::params{user.id, full_name}).no_rows();
    audit::record(tx, workspace_actor(),
                  {.action = "user.update",
                   .object_type = "user",
                   .object_id = std::to_string(user.id),
                   .before = {{"name", user.name}},
                   .after = {{"name", full_name}, {"source", "directory"}}},
                  now);
}

bool disable(pqxx::transaction_base& tx, const LockedUser& user, DirectoryAnswer answer, core::TimePoint now) {
    if (tx.exec("UPDATE users SET status = 'disabled' WHERE id = $1 AND status = 'active'", pqxx::params{user.id}).affected_rows() == 0) {
        return false;
    }
    // The codes before the sessions. The app's exchange (take_app_code) reads the user's status without locking the
    // row and inserts a session in the same transaction; it is this delete that waits for an exchange in flight,
    // which holds its code's row, so the delete of the sessions after it sees the session that exchange committed.
    // The other way round, a session of a disabled user could survive, and the next enable would bring it back.
    tx.exec("DELETE FROM app_sign_in_codes WHERE user_id = $1", pqxx::params{user.id}).no_rows();
    // Their devices go with their sessions (ON DELETE CASCADE).
    const auto ended = tx.exec("DELETE FROM sessions WHERE user_id = $1", pqxx::params{user.id}).affected_rows();
    const auto reason = reason_of(answer);
    audit::record(tx, workspace_actor(),
                  {.action = "user.disable",
                   .object_type = "user",
                   .object_id = std::to_string(user.id),
                   .before = {{"status", "active"}},
                   .after = {{"status", "disabled"}, {"reason", reason}, {"sessions_ended", ended}}},
                  now);
    std::string body = std::format(
        "The staff check disabled {}: {}. Their sessions in the dashboard and the phone app were ended.\n\n"
        "They still appear everywhere as inactive, and what they approved before still goes out. Reassign their open "
        "escalations, and take their address out of the notification rules: email to it is still sent.\n\n"
        "The engine enables them again by itself when the directory's list holds their account active.",
        user.email, google_said(answer));
    if (answer == DirectoryAnswer::missing) {
        body += "\n\nIf this person still works here, the connected administrator's role may not cover them (give the role for the "
                "whole organization, and the next check enables them again), or their account is in another Workspace than the "
                "administrator's, which the engine cannot see.";
    }
    emit(tx,
         {.type = std::string(kEventSystemProblem),
          .key = std::format("staff-disabled:{}:{}", user.id, db::to_timestamptz(now)),
          .location_id = std::nullopt,
          .review_id = std::nullopt,
          .subject = std::format("A staff account was disabled: {}", user.email),
          .body = std::move(body),
          .link_path = "/#/status"},
         now);
    return true;
}

bool enable(pqxx::transaction_base& tx, const LockedUser& user, core::TimePoint now) {
    if (tx.exec("UPDATE users SET status = 'active' WHERE id = $1 AND status = 'disabled'", pqxx::params{user.id}).affected_rows() == 0) {
        return false;
    }
    // No session is created: the person signs in, which asks Google once more.
    audit::record(tx, workspace_actor(),
                  {.action = "user.enable",
                   .object_type = "user",
                   .object_id = std::to_string(user.id),
                   .before = {{"status", "disabled"}},
                   .after = {{"status", "active"}, {"reason", "present"}}},
                  now);
    emit(tx,
         {.type = std::string(kEventSystemProblem),
          .key = std::format("staff-enabled:{}:{}", user.id, db::to_timestamptz(now)),
          .location_id = std::nullopt,
          .review_id = std::nullopt,
          .subject = std::format("A staff account is active again: {}", user.email),
          .body = std::format("The Workspace directory's list holds the account of {} as active again, so the staff check enabled "
                              "it. The person can sign in again.",
                              user.email),
          .link_path = "/#/status"},
         now);
    return true;
}

std::string unreadable_body(const StaffCheckError& error) {
    if (error.problem == StaffCheckProblem::no_administrator) {
        return "The Workspace directory's list was read, but it does not hold the connected administrator as an active "
               "administrator, so the staff check disabled and enabled nobody.\n\n"
               "Connect another administrator of this Workspace on the status page; or give the connected account's role for the "
               "whole organization, its own organizational unit included; or, if the account's address changed at Google, connect "
               "the same account again. Waiting does not help.";
    }
    return std::format("The staff check could not read the Workspace directory's list of users ({}). Nobody is disabled or "
                       "enabled until it works again. If it stays this way, connect the Workspace administrator again on the "
                       "status page.",
                       error.message);
}

}  // namespace

std::optional<StaffToCheck> staff_to_check(pqxx::transaction_base& tx) {
    auto credential = load_credential(tx, kWorkspaceCredential);
    if (!credential || !credential->account_email || credential->account_email->empty()) return std::nullopt;
    return StaffToCheck{.credential_updated_at = std::move(credential->updated_at),
                        .admin_email = std::move(*credential->account_email),
                        .staff = db::rows_as<StaffMember>(tx, "SELECT id, google_sub FROM users ORDER BY id")};
}

StaffCheckResult apply_staff_check(pqxx::transaction_base& tx, std::string_view credential_updated_at,
                                   const std::vector<StaffFinding>& findings, core::TimePoint now) {
    // Disconnected since the run began, or another account connected since it read the list.
    const auto credential = load_credential(tx, kWorkspaceCredential);
    if (!credential || credential->updated_at != credential_updated_at) return {.stale = true};

    std::map<std::int64_t, const StaffFinding*> by_user;
    for (const auto& finding : findings) by_user.emplace(finding.user_id, &finding);
    // In id order, so two runs cannot deadlock. FOR NO KEY UPDATE, not FOR UPDATE: every insert of a row that names
    // the user (a session, an app code, an audit entry, a decision) takes FOR KEY SHARE on the user's row, which
    // conflicts with FOR UPDATE only. With FOR UPDATE, the app's exchange (it holds its code's row, then inserts the
    // session) and this transaction (it holds the user's row, then deletes the code) would deadlock, and PostgreSQL
    // would abort one of them, perhaps this whole run. This lock lets the exchange finish and commit its session,
    // which the delete of the sessions then sees; it still conflicts with a sign-in's upsert and with another run's.
    const auto users = db::rows_as<LockedUser>(tx,
                                               "SELECT id, email, name, status FROM users "
                                               "WHERE id IN (SELECT jsonb_array_elements_text($1::jsonb)::bigint) "
                                               "ORDER BY id FOR NO KEY UPDATE",
                                               pqxx::params{db::json_list(std::views::keys(by_user))});

    StaffCheckResult result;
    for (const auto& user : users) {
        const auto& finding = *by_user.at(user.id);
        if (finding.answer != DirectoryAnswer::missing) follow_name(tx, user, finding.full_name, now);
        if (user.status == "active" && finding.answer != DirectoryAnswer::present) {
            if (disable(tx, user, finding.answer, now)) ++result.disabled;
        } else if (user.status == "disabled" && finding.answer == DirectoryAnswer::present) {
            if (enable(tx, user, now)) ++result.enabled;
        }
    }
    return result;
}

void record_staff_check(pqxx::transaction_base& tx, const StaffCheckResult& result, std::size_t listed,
                        const std::optional<StaffCheckError>& error, core::TimePoint now) {
    if (error) {
        mark_integration_error(tx, kWorkspaceStatus, error->message, now);
        emit(tx,
             {.type = std::string(kEventSystemProblem),
              .key = std::format("staff-check-unreadable:{:%F}", now),
              .location_id = std::nullopt,
              .review_id = std::nullopt,
              .subject = "The staff check could not use the Workspace directory",
              .body = unreadable_body(*error),
              .link_path = "/#/status"},
             now);
        return;
    }
    if (result.stale) return;
    mark_integration_ok(tx, kWorkspaceStatus, now, {{"listed", listed}, {"disabled", result.disabled}, {"enabled", result.enabled}});
}

}  // namespace poni::repo
