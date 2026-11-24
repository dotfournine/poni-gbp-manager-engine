// The staff check (D-106 points 4 to 7) against a real PostgreSQL: who is
// disabled and enabled for what the Workspace directory's list says, what a
// disable ends, and every way a run changes nothing.

#include "core/crypto.hpp"
#include "db/repo_auth.hpp"
#include "db/repo_users.hpp"
#include "engine_fixture.hpp"

#include <gtest/gtest.h>

#include <format>
#include <string>
#include <vector>

using namespace poni;
using namespace std::chrono_literals;
using repo::DirectoryAnswer;

namespace {

constexpr std::string_view kAdmin = "admin@example.com";

class StaffCheckTest : public test::EngineTest {
protected:
    void SetUp() override {
        EngineTest::SetUp();
        if (HasFatalFailure()) return;
        connect(kAdmin);
        admin_ = add_user("100", kAdmin, "Ada Admin");
    }

    // The Workspace administrator's connection, as the connect callback stores it, at the clock's now.
    void connect(std::string_view email) {
        tx([&](pqxx::work& tx) {
            repo::save_oauth_credentials(tx, box_, repo::kWorkspaceCredential, {"client", "secret", "1//refresh"}, std::string(email),
                                         clock_.now());
        });
    }

    // A user as a sign-in leaves one, with a Google id of digits.
    std::int64_t add_user(std::string_view sub, std::string_view email, std::string_view name, std::string_view status = "active") {
        const auto now = db::to_timestamptz(clock_.now());
        return stored(std::format("INSERT INTO users (google_sub, email, name, status, created_at, last_login_at) "
                                  "VALUES ('{}', '{}', '{}', '{}', '{}', '{}') RETURNING id",
                                  sub, email, name, status, now, now));
    }

    // A dashboard session, a phone app session with its device, and an unspent app code of `user`.
    void sign_in_everywhere(std::int64_t user) {
        const auto now = db::to_timestamptz(clock_.now());
        const auto id = std::to_string(user);
        exec(std::format("INSERT INTO sessions (token_hash, user_id, kind, csrf_token, created_at, last_seen_at, expires_at) "
                         "VALUES ('browser-{0}', {0}, 'browser', 'csrf', '{1}', '{1}', '{1}'::timestamptz + interval '7 days')",
                         id, now));
        exec(std::format("INSERT INTO sessions (token_hash, user_id, kind, csrf_token, created_at, last_seen_at, expires_at) "
                         "VALUES ('app-{0}', {0}, 'app', NULL, '{1}', '{1}', '{1}'::timestamptz + interval '90 days')",
                         id, now));
        exec(std::format("INSERT INTO devices (session_token_hash, apns_token, app_version, created_at, updated_at) "
                         "VALUES ('app-{0}', '{1}', '1.0', '{2}', '{2}')",
                         id, std::format("{:064x}", user), now));
        exec(std::format("INSERT INTO app_sign_in_codes (code_hash, user_id, code_challenge, first_sign_in, expires_at) "
                         "VALUES ('code-{0}', {0}, 'challenge', false, '{1}'::timestamptz + interval '60 seconds')",
                         id, now));
    }

    // The credential's `updated_at` as staff_to_check reads it: the connection a run read the list through.
    std::string read_through() {
        return value<std::string>("SELECT " + db::iso("updated_at") + " FROM credentials WHERE name = 'workspace'");
    }

    repo::StaffCheckResult apply(const std::vector<repo::StaffFinding>& findings) { return apply(read_through(), findings); }
    repo::StaffCheckResult apply(const std::string& read_through, const std::vector<repo::StaffFinding>& findings) {
        return tx([&](pqxx::work& tx) { return repo::apply_staff_check(tx, read_through, findings, clock_.now()); });
    }

    std::string status(std::int64_t user) { return value<std::string>(std::format("SELECT status FROM users WHERE id = {}", user)); }
    std::string name(std::int64_t user) { return value<std::string>(std::format("SELECT name FROM users WHERE id = {}", user)); }
    long sessions_of(std::int64_t user) { return count(std::format("sessions WHERE user_id = {}", user)); }
    long devices_of(std::int64_t user) {
        return count(std::format("devices d WHERE d.session_token_hash IN ('browser-{0}', 'app-{0}')", user));
    }
    long codes_of(std::int64_t user) { return count(std::format("app_sign_in_codes WHERE user_id = {}", user)); }
    long entries(std::string_view action, std::int64_t user) {
        return count(std::format("audit_log WHERE action = '{}' AND actor = 'system:workspace' AND object_type = 'user' AND object_id = '{}'",
                                 action, user));
    }
    long notices(std::string_view subject) { return count(std::format("notification_deliveries WHERE subject = '{}'", subject)); }
    long all_entries() { return count("audit_log"); }
    long all_notices() { return count("notification_deliveries"); }

    core::SecretBox box_ = core::SecretBox::from_base64_key(core::base64_encode(std::string(32, 'k'))).value();
    std::int64_t admin_ = 0;
};

class StaffCheckApply : public StaffCheckTest {};

}  // namespace

// ---- apply_staff_check, called with findings -------------------------------------------------

TEST_F(StaffCheckApply, ASuspendedAccountIsDisabledAndItsSessionsDevicesAndCodesAreGone) {
    const auto sam = add_user("200", "sam@example.com", "Sam Staff");
    sign_in_everywhere(sam);
    ASSERT_EQ(sessions_of(sam), 2);
    ASSERT_EQ(devices_of(sam), 1);
    ASSERT_EQ(codes_of(sam), 1);

    const auto result = apply({{.user_id = sam, .answer = DirectoryAnswer::suspended, .full_name = "Sam Staff"}});

    EXPECT_EQ(result.disabled, 1);
    EXPECT_FALSE(result.stale);
    EXPECT_EQ(status(sam), "disabled");
    EXPECT_EQ(sessions_of(sam), 0);
    EXPECT_EQ(devices_of(sam), 0);
    EXPECT_EQ(codes_of(sam), 0);
    EXPECT_EQ(count("users WHERE email = 'sam@example.com' AND name = 'Sam Staff' AND google_sub = '200'"), 1)
        << "a disabled user's row is never deleted";
    EXPECT_EQ(count(std::format("audit_log WHERE action = 'user.disable' AND object_id = '{}' AND after->>'reason' = 'suspended'", sam)),
              1);
    EXPECT_EQ(notices("A staff account was disabled: sam@example.com"), 1);
    EXPECT_EQ(count("notification_deliveries WHERE event_type = 'system_problem' AND link_path = '/#/status' AND body LIKE "
                    "'%suspended%' AND body LIKE '%still appear everywhere as inactive%' AND body LIKE '%still goes out%'"),
              1);

    // Their old cookie and app token find no session: a 401.
    const domain::SessionLifetimes lifetimes;
    tx([&](pqxx::work& tx) {
        EXPECT_FALSE(repo::find_session(tx, std::format("browser-{}", sam), domain::SessionKind::browser, clock_.now(), lifetimes));
        EXPECT_FALSE(repo::find_session(tx, std::format("app-{}", sam), domain::SessionKind::app, clock_.now(), lifetimes));
        // Still listed, as disabled, for everyone who asks for all.
        const auto all = repo::list_users(tx, "all").items;
        const auto listed = std::ranges::find(all, sam, &model::User::id);
        ASSERT_NE(listed, all.end());
        EXPECT_EQ(listed->status, "disabled");
    });
}

TEST_F(StaffCheckApply, AnArchivedAccountIsDisabled) {
    const auto sam = add_user("200", "sam@example.com", "Sam Staff");
    EXPECT_EQ(apply({{.user_id = sam, .answer = DirectoryAnswer::archived, .full_name = "Sam Staff"}}).disabled, 1);
    EXPECT_EQ(status(sam), "disabled");
    EXPECT_EQ(count(std::format("audit_log WHERE action = 'user.disable' AND object_id = '{}' AND after->>'reason' = 'archived'", sam)),
              1);
}

TEST_F(StaffCheckApply, AMissingAccountIsDisabledAsDeletedAndItsNoticeNamesTheRoleAndAnotherWorkspace) {
    const auto sam = add_user("200", "sam@other.example", "Sam Staff");
    EXPECT_EQ(apply({{.user_id = sam, .answer = DirectoryAnswer::missing}}).disabled, 1);
    EXPECT_EQ(status(sam), "disabled");
    EXPECT_EQ(count(std::format("audit_log WHERE action = 'user.disable' AND object_id = '{}' AND after->>'reason' = 'deleted'", sam)),
              1);
    EXPECT_EQ(count("notification_deliveries WHERE subject = 'A staff account was disabled: sam@other.example' AND body LIKE "
                    "'%no longer in the Workspace directory''s list%' AND body LIKE '%role may not cover them%' AND body LIKE "
                    "'%another Workspace than the administrator''s%'"),
              1);
}

TEST_F(StaffCheckApply, EveryOneOfTenMissingAtOnceIsDisabled) {
    std::vector<repo::StaffFinding> findings;
    for (int i = 0; i < 10; ++i) {
        findings.push_back({.user_id = add_user(std::to_string(300 + i), std::format("staff{}@example.com", i), "Staff"),
                            .answer = DirectoryAnswer::missing});
    }
    EXPECT_EQ(apply(findings).disabled, 10) << "no limit per run (the owner)";
    EXPECT_EQ(count("users WHERE status = 'disabled'"), 10);
    EXPECT_EQ(count("notification_deliveries WHERE subject LIKE 'A staff account was disabled: %'"), 10);
}

TEST_F(StaffCheckApply, AReinstatedAccountIsEnabledAndNoSessionComesBack) {
    const auto sam = add_user("200", "sam@example.com", "Sam Staff");
    sign_in_everywhere(sam);
    apply({{.user_id = sam, .answer = DirectoryAnswer::suspended, .full_name = "Sam Staff"}});
    clock_.advance(5min);

    const auto result = apply({{.user_id = sam, .answer = DirectoryAnswer::present, .full_name = "Sam Staff"}});

    EXPECT_EQ(result.enabled, 1);
    EXPECT_EQ(result.disabled, 0);
    EXPECT_EQ(status(sam), "active");
    EXPECT_EQ(sessions_of(sam), 0) << "an enable creates no session, and the old ones are gone for good";
    EXPECT_EQ(entries("user.enable", sam), 1);
    EXPECT_EQ(count(std::format("audit_log WHERE action = 'user.enable' AND object_id = '{}' AND after->>'reason' = 'present'", sam)), 1);
    EXPECT_EQ(notices("A staff account is active again: sam@example.com"), 1);

    // A new sign-in works.
    tx([&](pqxx::work& tx) {
        const auto signed_in = repo::upsert_user(tx, {.google_sub = "200", .email = "sam@example.com", .name = "Sam Staff"}, clock_.now());
        EXPECT_FALSE(signed_in.disabled);
    });
}

TEST_F(StaffCheckApply, ADisabledAccountStillSuspendedOrStillMissingStaysDisabledWithNoSecondEntryOrNotice) {
    const auto sam = add_user("200", "sam@example.com", "Sam Staff");
    const auto kim = add_user("201", "kim@example.com", "Kim Staff");
    apply({{.user_id = sam, .answer = DirectoryAnswer::suspended, .full_name = "Sam Staff"}, {.user_id = kim, .answer = DirectoryAnswer::missing}});
    clock_.advance(5min);

    const auto result =
        apply({{.user_id = sam, .answer = DirectoryAnswer::suspended, .full_name = "Sam Staff"}, {.user_id = kim, .answer = DirectoryAnswer::missing}});

    EXPECT_EQ(result.disabled, 0);
    EXPECT_EQ(result.enabled, 0);
    EXPECT_EQ(status(sam), "disabled");
    EXPECT_EQ(status(kim), "disabled");
    EXPECT_EQ(entries("user.disable", sam), 1);
    EXPECT_EQ(entries("user.disable", kim), 1);
    EXPECT_EQ(count("notification_deliveries WHERE subject LIKE 'A staff account was disabled: %'"), 2);
}

TEST_F(StaffCheckApply, AConnectionMadeSinceTheReadChangesNothing) {
    const auto sam = add_user("200", "sam@example.com", "Sam Staff");
    const auto old = read_through();
    // save_credential writes the caller's now: without the clock moving, the second save would look like the first.
    clock_.advance(1s);
    connect("other-admin@example.com");
    const auto before = all_entries();

    const auto result = apply(old, {{.user_id = sam, .answer = DirectoryAnswer::suspended, .full_name = "Another Name"}});

    EXPECT_TRUE(result.stale);
    EXPECT_EQ(result.disabled, 0);
    EXPECT_EQ(status(sam), "active");
    EXPECT_EQ(name(sam), "Sam Staff");
    EXPECT_EQ(all_entries(), before);
    EXPECT_EQ(all_notices(), 0);
}

TEST_F(StaffCheckApply, NoConnectionChangesNothing) {
    const auto sam = add_user("200", "sam@example.com", "Sam Staff");
    const auto old = read_through();
    exec("DELETE FROM credentials WHERE name = 'workspace'");

    const auto result = apply(old, {{.user_id = sam, .answer = DirectoryAnswer::missing}});

    EXPECT_TRUE(result.stale);
    EXPECT_EQ(status(sam), "active");
    EXPECT_EQ(all_notices(), 0);
}

TEST_F(StaffCheckApply, TheSameFindingsTwiceChangeAndRecordNothingTheSecondTime) {
    const auto sam = add_user("200", "sam@example.com", "Sam Staff");
    const auto kim = add_user("201", "kim@example.com", "Kim Staff", "disabled");
    const std::vector<repo::StaffFinding> findings{{.user_id = sam, .answer = DirectoryAnswer::suspended, .full_name = "Sam Q. Staff"},
                                                   {.user_id = kim, .answer = DirectoryAnswer::present, .full_name = "Kim Staff"}};
    const auto first = apply(findings);
    EXPECT_EQ(first.disabled, 1);
    EXPECT_EQ(first.enabled, 1);
    const auto entries_after_first = all_entries();
    const auto notices_after_first = all_notices();

    const auto second = apply(findings);

    EXPECT_EQ(second.disabled, 0);
    EXPECT_EQ(second.enabled, 0);
    EXPECT_EQ(all_entries(), entries_after_first);
    EXPECT_EQ(all_notices(), notices_after_first);
}

TEST_F(StaffCheckApply, ANameFollowsTheDirectory) {
    const auto sam = add_user("200", "sam@example.com", "Sam Staff");
    apply({{.user_id = sam, .answer = DirectoryAnswer::present, .full_name = "Samantha Staff"}});

    EXPECT_EQ(name(sam), "Samantha Staff");
    EXPECT_EQ(status(sam), "active");
    EXPECT_EQ(count(std::format("audit_log WHERE action = 'user.update' AND actor = 'system:workspace' AND object_id = '{}' AND "
                                "before->>'name' = 'Sam Staff' AND after->>'name' = 'Samantha Staff' AND after->>'source' = 'directory'",
                                sam)),
              1);
    EXPECT_EQ(all_notices(), 0) << "a name is no notice";
}

TEST_F(StaffCheckApply, AnEmptyNameInTheDirectoryChangesNothing) {
    const auto sam = add_user("200", "sam@example.com", "Sam Staff");
    const auto before = all_entries();
    apply({{.user_id = sam, .answer = DirectoryAnswer::present, .full_name = ""},
           {.user_id = admin_, .answer = DirectoryAnswer::present, .full_name = "Ada Admin"}});
    EXPECT_EQ(name(sam), "Sam Staff");
    EXPECT_EQ(name(admin_), "Ada Admin");
    EXPECT_EQ(all_entries(), before) << "neither an empty name nor the same name is written";
}

TEST_F(StaffCheckApply, ADisableIsOneAuditEntryByTheWorkspaceWithItsReasonAndTheSessionsEnded) {
    const auto sam = add_user("200", "sam@example.com", "Sam Staff");
    sign_in_everywhere(sam);
    apply({{.user_id = sam, .answer = DirectoryAnswer::suspended, .full_name = "Sam Staff"}});

    EXPECT_EQ(entries("user.disable", sam), 1);
    EXPECT_EQ(count(std::format("audit_log WHERE action = 'user.disable' AND object_id = '{}' AND actor_type = 'system' AND "
                                "before->>'status' = 'active' AND after->>'status' = 'disabled' AND after->>'reason' = 'suspended' AND "
                                "(after->>'sessions_ended')::int = 2",
                                sam)),
              1);
}
