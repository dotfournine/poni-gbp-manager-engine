// The staff check (D-106 points 4 to 7) against a real PostgreSQL and a fake
// Workspace directory: who is disabled and enabled for what the directory's
// list says, what a disable ends, and every way a run changes nothing.

#include "core/crypto.hpp"
#include "db/repo_auth.hpp"
#include "db/repo_users.hpp"
#include "engine_fixture.hpp"
#include "fakes.hpp"
#include "jobs/scheduler.hpp"
#include "jobs/staff_check.hpp"
#include "jobs/worker.hpp"

#include <boost/asio/error.hpp>
#include <boost/system/system_error.hpp>
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
        // The connected administrator, as a custom role shows them.
        directory_.users.push_back({.id = "100", .primary_email = std::string(kAdmin), .full_name = "Ada Admin", .is_delegated_admin = true});
        check_.emplace(db(), clock_, directory_, 300s);
    }

    // The directory's entry for a person.
    void list(std::string_view id, std::string_view email, std::string_view full_name, bool suspended = false, bool archived = false) {
        directory_.users.push_back({.id = std::string(id),
                                    .primary_email = std::string(email),
                                    .full_name = std::string(full_name),
                                    .suspended = suspended,
                                    .archived = archived});
    }

    void run() { await(check_->run()); }

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
    test::FakeDirectory directory_;
    std::optional<jobs::StaffCheck> check_;
};

class StaffCheckApply : public StaffCheckTest {};
class StaffCheck : public StaffCheckTest {};

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

// ---- the job, with the fake directory ---------------------------------------------------------

TEST_F(StaffCheck, OneRunReadsTheListOnceAndDisablesASuspendedUser) {
    const auto sam = add_user("200", "sam@example.com", "Sam Staff");
    sign_in_everywhere(sam);
    list("200", "sam@example.com", "Sam Staff", /*suspended=*/true);

    run();

    EXPECT_EQ(directory_.lists, 1) << "one list, every page, and no get of one user";
    EXPECT_EQ(status(sam), "disabled");
    EXPECT_EQ(sessions_of(sam), 0);
    EXPECT_EQ(status(admin_), "active");
    EXPECT_EQ(entries("user.disable", sam), 1);
    EXPECT_EQ(notices("A staff account was disabled: sam@example.com"), 1);
    // The status page shows the run's outcome under workspace.
    tx([&](pqxx::work& tx) {
        const auto workspace = repo::integration(tx, repo::kWorkspaceStatus);
        EXPECT_TRUE(workspace.last_ok_at);
        EXPECT_FALSE(workspace.last_error);
    });
    EXPECT_EQ(count("integration_status WHERE name = 'workspace' AND (detail->>'listed')::int = 2 AND (detail->>'disabled')::int = 1 "
                    "AND (detail->>'enabled')::int = 0"),
              1);
}

TEST_F(StaffCheck, AUserMissingFromTheListIsDisabledWhateverTheirDomain) {
    const auto own = add_user("200", "sam@example.com", "Sam Staff");
    const auto other = add_user("201", "kim@nobody-listed.example", "Kim Staff");
    run();
    EXPECT_EQ(status(own), "disabled");
    EXPECT_EQ(status(other), "disabled");
    EXPECT_EQ(status(admin_), "active");
}

TEST_F(StaffCheck, ANewUserMissingFromTheListIsDisabledAndEnabledOnceTheListHoldsThem) {
    clock_.advance(1h);
    const auto fresh = add_user("200", "new@example.com", "New Hire");
    clock_.advance(1min);

    run();
    EXPECT_EQ(status(fresh), "disabled") << "no grace for a new user (the owner)";

    list("200", "new@example.com", "New Hire");
    clock_.advance(5min);
    run();
    EXPECT_EQ(status(fresh), "active");
    EXPECT_EQ(entries("user.enable", fresh), 1);
}

TEST_F(StaffCheck, AListThatCouldNotBeReadChangesNobody) {
    const auto active = add_user("200", "sam@example.com", "Sam Staff");  // missing from the list
    const auto disabled = add_user("201", "kim@example.com", "Kim Staff", "disabled");
    list("201", "kim@example.com", "Kim Staff");  // listed active

    const std::vector<google::ApiError> failures{
        {google::ApiError::Kind::permission_denied, 403, "HTTP 403 Not Authorized to access this resource/api"},
        google::not_configured("no Workspace administrator is connected; connect one on the dashboard's status page"),
        {google::ApiError::Kind::unavailable, 500, "HTTP 500"},
    };
    for (const auto& failure : failures) {
        directory_.failure = failure;
        run();
        EXPECT_EQ(status(active), "active") << failure.message;
        EXPECT_EQ(status(disabled), "disabled") << failure.message;
        EXPECT_EQ(value<std::string>("SELECT last_error FROM integration_status WHERE name = 'workspace'"), failure.message);
        clock_.advance(5min);
    }
    // A network failure: google::guarded makes it an error.
    directory_.failure.reset();
    directory_.on_list = [] { throw boost::system::system_error(boost::asio::error::connection_reset); };
    run();
    EXPECT_EQ(status(active), "active");
    EXPECT_EQ(status(disabled), "disabled");
    EXPECT_EQ(count("integration_status WHERE name = 'workspace' AND last_error LIKE 'network error: %'"), 1);

    EXPECT_EQ(count("audit_log WHERE actor = 'system:workspace'"), 0);
    const auto unreadable = "notification_deliveries WHERE subject = 'The staff check could not use the Workspace directory'";
    EXPECT_EQ(count(unreadable), 1) << "one notice a day";
    EXPECT_EQ(count(std::string(unreadable) + " AND body LIKE 'The staff check could not read the Workspace directory''s list%' AND "
                                              "body LIKE '%Nobody is disabled or enabled until it works again%'"),
              1);
    clock_.advance(24h);
    run();
    EXPECT_EQ(count(unreadable), 2) << "and one the next day";
}

TEST_F(StaffCheck, AListThatDoesNotHoldTheAdministratorAsAnActiveAdministratorChangesNobody) {
    const auto active = add_user("200", "sam@example.com", "Sam Staff");  // missing from every list below
    const auto disabled = add_user("201", "kim@example.com", "Kim Staff", "disabled");
    const workspace::DirectoryUser kim{.id = "201", .primary_email = "kim@example.com", .full_name = "Kim Staff"};
    const auto admin = directory_.users.front();
    auto suspended_admin = admin;
    suspended_admin.suspended = true;
    auto archived_admin = admin;
    archived_admin.archived = true;
    auto plain_admin = admin;
    plain_admin.is_delegated_admin = false;

    const std::vector<std::vector<workspace::DirectoryUser>> lists{
        {kim},                   // the administrator is not in it
        {suspended_admin, kim},  // suspended
        {archived_admin, kim},   // archived
        {plain_admin, kim},      // no administrator
        {},                      // an empty list
    };
    for (const auto& users : lists) {
        directory_.users = users;
        run();
        EXPECT_EQ(status(active), "active");
        EXPECT_EQ(status(disabled), "disabled");
        EXPECT_EQ(status(admin_), "active");
        EXPECT_EQ(value<std::string>("SELECT last_error FROM integration_status WHERE name = 'workspace'"),
                  "the directory's list does not hold the connected administrator as an active administrator");
    }
    EXPECT_EQ(count("audit_log WHERE actor = 'system:workspace'"), 0);
    EXPECT_EQ(count("notification_deliveries WHERE subject = 'The staff check could not use the Workspace directory' AND "
                    "body LIKE 'The Workspace directory''s list was read, but it does not hold the connected administrator%' AND "
                    "body LIKE '%Connect another administrator of this Workspace%' AND body LIKE '%Waiting does not help.'"),
              1);
}

TEST_F(StaffCheck, TheAdministratorIsFoundByAddressWhateverItsCaseAndAsASuperAdministrator) {
    const auto sam = add_user("200", "sam@example.com", "Sam Staff");
    directory_.users.front().primary_email = "Admin@Example.com";
    directory_.users.front().is_delegated_admin = false;
    directory_.users.front().is_admin = true;
    list("200", "sam@example.com", "Sam Staff", false, /*archived=*/true);
    run();
    EXPECT_EQ(status(sam), "disabled");
}

TEST_F(StaffCheck, ARowFromDevelopmentSignInIsNeverJudged) {
    const auto dev = stored(std::format("INSERT INTO users (google_sub, email, name, created_at, last_login_at) VALUES "
                                        "('dev:someone@localhost', 'someone@localhost', 'Someone', '{0}', '{0}') RETURNING id",
                                        db::to_timestamptz(clock_.now())));
    run();
    EXPECT_EQ(status(dev), "active");
    EXPECT_EQ(count("audit_log WHERE action = 'user.disable'"), 0);
}

TEST_F(StaffCheck, NothingConnectedReadsNothingAndRecordsNothing) {
    const auto sam = add_user("200", "sam@example.com", "Sam Staff");
    exec("DELETE FROM credentials WHERE name = 'workspace'");
    run();
    // A connection without an address (stored before D-100) is none either.
    exec("INSERT INTO credentials (name, ciphertext, key_id, created_at, updated_at) VALUES ('workspace', 'x', 'k', now(), now())");
    run();

    EXPECT_EQ(directory_.lists, 0);
    EXPECT_EQ(status(sam), "active");
    EXPECT_EQ(count("integration_status WHERE name = 'workspace'"), 0);
    EXPECT_EQ(all_entries(), 0);
    EXPECT_EQ(all_notices(), 0);
}

TEST_F(StaffCheck, AnotherAccountConnectedWhileTheListIsReadChangesNothing) {
    const auto sam = add_user("200", "sam@example.com", "Sam Staff");
    list("200", "sam@example.com", "Sam Staff", /*suspended=*/true);
    const auto before = all_entries();
    directory_.on_list = [&] {
        clock_.advance(1s);
        connect("other-admin@example.com");
    };

    run();

    EXPECT_EQ(directory_.lists, 1);
    EXPECT_EQ(status(sam), "active");
    EXPECT_EQ(all_entries(), before);
    EXPECT_EQ(all_notices(), 0);
    EXPECT_EQ(count("integration_status WHERE name = 'workspace'"), 0) << "nothing is recorded";
}

TEST_F(StaffCheck, TheSchedulerAndAWorkerCarryARunAtTheInterval) {
    const auto sam = add_user("200", "sam@example.com", "Sam Staff");
    list("200", "sam@example.com", "Sam Staff", /*suspended=*/true);
    jobs::Registry registry;
    jobs::install(registry, *check_);
    jobs::Scheduler scheduler(io_.get_executor(), queue(), registry.recurring(), {});
    jobs::Worker worker(io_.get_executor(), queue(), registry, {.id = "test-worker"});
    const auto run_all = [&] {
        await(scheduler.tick());
        while (await(worker.run_once())) {
        }
    };

    run_all();
    EXPECT_EQ(directory_.lists, 1);
    EXPECT_EQ(status(sam), "disabled");
    run_all();
    EXPECT_EQ(directory_.lists, 1) << "one run per slot";

    directory_.users.back().suspended = false;
    clock_.advance(300s);
    run_all();
    EXPECT_EQ(directory_.lists, 2);
    EXPECT_EQ(status(sam), "active");
    EXPECT_EQ(count("jobs WHERE kind = 'workspace.check_staff' AND status = 'failed'"), 0);
}
