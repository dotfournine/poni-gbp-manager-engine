// People of the Workspace directory found and made users before their first
// sign-in (D-106 point 8), through the router as main assembles it, with a
// fake directory: what the search answers, what making a user writes and
// refuses, and that the person's own first sign-in finds that row.

#include "api_fixture.hpp"
#include "core/crypto.hpp"
#include "db/repo_auth.hpp"
#include "jobs/analysis.hpp"

#include <format>
#include <optional>
#include <string>
#include <vector>

using namespace poni;
using test::FakeLlm;
using test::make_review;

namespace {

// A Google account's id: 21 digits, as a sign-in's `sub` and the directory's id are.
constexpr std::string_view kRobin = "100000000000000000042";
constexpr std::string_view kRobinEmail = "robin.newhire@xvisionmediaus.com";

constexpr std::string_view kSevere = R"({
    "language": "en", "script": "Latn", "sentiment": "negative", "contains_complaint": true, "topics": [], "items": [],
    "severity_flags": [{"id": "injury", "evidence": "my neck has hurt for a week"}],
    "manipulation_suspected": false, "manipulation_evidence": "", "summary_en": "A guest was hurt."})";

class UsersDirectory : public test::ApiTest {
protected:
    void SetUp() override {
        ApiTest::SetUp();
        if (HasFatalFailure()) return;
        browser_ = sign_in();
        connect();
        list(kRobin, kRobinEmail, "Robin Newhire");
    }

    // The Workspace administrator's connection, as the connect callback stores it.
    void connect() {
        tx([&](pqxx::work& tx) {
            repo::save_oauth_credentials(tx, box_, repo::kWorkspaceCredential, {"client", "secret", "1//refresh"},
                                         "admin@xvisionmediaus.com", clock_.now());
        });
    }

    // The directory's entry for a person.
    void list(std::string_view id, std::string_view email, std::string_view full_name, bool suspended = false, bool archived = false) {
        directory_.users.push_back({.id = std::string(id),
                                    .primary_email = std::string(email),
                                    .full_name = std::string(full_name),
                                    .suspended = suspended,
                                    .archived = archived});
    }

    http::Response search(std::string_view q) { return get(std::format("/api/users/directory?q={}", q), browser_); }
    http::Response create(std::string_view directory_id) {
        return post("/api/users", boost::json::serialize(boost::json::object{{"directory_id", directory_id}}));
    }

    // The addresses a search answers, in its order.
    std::vector<std::string> emails(std::string_view q) {
        const auto answer = search(q);
        EXPECT_EQ(answer.status, 200u) << answer.body;
        std::vector<std::string> out;
        for (const auto& person : json(answer).at("items").as_array()) out.emplace_back(person.at("email").as_string());
        return out;
    }

    long users() { return count("users"); }
    long created_entries() { return count("audit_log WHERE action = 'user.create'"); }
    // The `first_sign_in` of the newest entry of `action` about the user with this Google id.
    bool first_sign_in(std::string_view action, std::string_view sub) {
        return value<bool>(std::format("SELECT (after->>'first_sign_in')::boolean FROM audit_log WHERE action = '{}' AND object_type = 'user' "
                                       "AND object_id = (SELECT id::text FROM users WHERE google_sub = '{}') ORDER BY id DESC LIMIT 1",
                                       action, sub));
    }

    // An open escalation, made as the approval queue's tests make one: a review the analyzer flags.
    std::int64_t escalation() {
        llm_.analyzer = FakeLlm::always(kSevere);
        pipeline_.emplace(db(), clock_, llm_, queue(), jobs::AnalysisOptions{});
        gbp_.add_location("9001", "LU DREAM SPA - Chelsea");
        discover();
        const auto client = make_client("LU DREAM SPA");
        const auto location = link("9001", client);
        become_test_client(client, "wellness");
        const auto review = make_review(gbp_.account_id, "9001", "hurt", "ONE", "The therapist pressed so hard that my neck has hurt for a week.",
                                        "2026-11-09T10:00:00Z", "2026-11-09T10:00:00Z");
        gbp_.put(review);
        await(pipeline_->process(store_review(location, review), false, false));
        return read("/api/escalations").at("items").at(0).at("id").as_int64();
    }

    static identity::Identity robin() {
        return {.subject = std::string(kRobin),
                .email = std::string(kRobinEmail),
                .email_verified = true,
                .hosted_domain = "xvisionmediaus.com",
                .name = "Robin Newhire",
                .picture_url = "https://lh3.googleusercontent.com/a/robin"};
    }

    core::SecretBox box_ = core::SecretBox::from_base64_key(core::base64_encode(std::string(32, 'k'))).value();
    FakeLlm llm_;
    std::optional<jobs::ReviewPipeline> pipeline_;
};

}  // namespace

TEST_F(UsersDirectory, TheSearchFindsByNameAndAddressWithoutCaseSortedAndSaysWhoIsAUser) {
    list("100000000000000000043", "sam.brooks@xvisionus.com", "Sam Lee");  // by the address
    list("100000000000000000044", "aaron.ross@xvisionmediaus.com", "aaron Ross");
    list("100000000000000000045", "avery.stone@xvisionmediaus.com", "Avery Stone");  // no "ro" anywhere
    const auto aaron = stored("INSERT INTO users (google_sub, email, name, created_at, last_login_at) "
                              "VALUES ('100000000000000000044', 'aaron.ross@xvisionmediaus.com', 'aaron Ross', now(), now()) RETURNING id");

    const auto answer = search("RO");
    ASSERT_EQ(answer.status, 200u) << answer.body;
    const auto items = json(answer).at("items").as_array();
    ASSERT_EQ(items.size(), 3u);
    // By the name without case: "aaron Ross" first, where a sort by bytes would put it last.
    EXPECT_EQ(items[0].at("email").as_string(), "aaron.ross@xvisionmediaus.com");
    EXPECT_EQ(items[1].at("email").as_string(), kRobinEmail);
    EXPECT_EQ(items[2].at("email").as_string(), "sam.brooks@xvisionus.com");
    EXPECT_EQ(items[0].at("user_id").as_int64(), aaron);
    EXPECT_TRUE(items[1].at("user_id").is_null());
    EXPECT_EQ(items[1].at("directory_id").as_string(), kRobin);
    EXPECT_EQ(items[1].at("name").as_string(), "Robin Newhire");
    EXPECT_EQ(items[1].as_object().size(), 4u) << "directory_id, email, name and user_id";
    EXPECT_EQ(emails("ROBIN.new"), std::vector<std::string>{std::string(kRobinEmail)});
    EXPECT_TRUE(emails("nobody").empty());
    EXPECT_EQ(directory_.lists, 3) << "one list per search";
    EXPECT_EQ(count("audit_log WHERE action LIKE 'user.%'"), 0) << "a search changes nothing";
}

TEST_F(UsersDirectory, TheSearchLeavesOutTheSuspendedTheArchivedAndOtherDomains) {
    list("100000000000000000050", "rory.here@xvisionmediaus.com", "Rory Here");
    list("100000000000000000051", "rory.suspended@xvisionmediaus.com", "Rory Suspended", true);
    list("100000000000000000052", "rory.archived@xvisionmediaus.com", "Rory Archived", false, true);
    list("100000000000000000053", "rory@example.com", "Rory Outside");
    EXPECT_EQ(emails("rory"), std::vector<std::string>{"rory.here@xvisionmediaus.com"});
}

TEST_F(UsersDirectory, TheSearchAnswersAtMostFifty) {
    for (int i = 0; i < 60; ++i) {
        list(std::format("2{:020d}", i), std::format("person.{:02d}@xvisionmediaus.com", i), std::format("Person {:02d}", 59 - i));
    }
    const auto found = emails("person");
    ASSERT_EQ(found.size(), 50u);
    EXPECT_EQ(found.front(), "person.59@xvisionmediaus.com") << "Person 00";
    EXPECT_EQ(found.back(), "person.10@xvisionmediaus.com") << "Person 49";
}

TEST_F(UsersDirectory, TheSearchRefusesABadQueryAndAnswersNoListWithoutADirectory) {
    const std::vector<std::string> bad = {"r", "%20r%20", "", std::string(101, 'r')};
    for (const auto& q : bad) {
        const auto refused = search(q);
        EXPECT_EQ(refused.status, 400u) << q;
        EXPECT_EQ(error_code(refused), "invalid_request") << q;
    }
    EXPECT_EQ(get("/api/users/directory", browser_).status, 400u);
    EXPECT_EQ(directory_.lists, 0) << "a bad query reads nothing";
    EXPECT_EQ(search(std::string(100, 'r')).status, 200u);
    EXPECT_EQ(search("%E6%97%A5%E6%9C%AC").status, 200u) << "two characters, six bytes";

    directory_.failure = google::ApiError{google::ApiError::Kind::permission_denied, 403, "Not Authorized to access this resource/api"};
    const auto unreadable = search("robin");
    EXPECT_EQ(unreadable.status, 502u);
    EXPECT_EQ(error_code(unreadable), "directory_unavailable");
    EXPECT_FALSE(json(unreadable).as_object().contains("items"));

    exec("DELETE FROM credentials WHERE name = 'workspace'");
    const int read_before = directory_.lists;
    const auto unconnected = search("robin");
    EXPECT_EQ(unconnected.status, 409u);
    EXPECT_EQ(error_code(unconnected), "workspace_not_connected");
    EXPECT_EQ(directory_.lists, read_before) << "nothing to read the directory with";
}

TEST_F(UsersDirectory, APersonIsMadeAUserFromTheDirectorysOwnEntry) {
    const auto before = users();
    const auto made = create(kRobin);
    ASSERT_EQ(made.status, 201u) << made.body;
    const auto user = json(made);
    EXPECT_EQ(user.at("email").as_string(), kRobinEmail);
    EXPECT_EQ(user.at("name").as_string(), "Robin Newhire");
    EXPECT_TRUE(user.at("picture_url").is_null());
    EXPECT_EQ(user.at("status").as_string(), "active");
    const auto id = user.at("id").as_int64();
    EXPECT_EQ(users(), before + 1);
    const auto where = std::format("users WHERE id = {} ", id);
    EXPECT_EQ(count(where + std::format("AND google_sub = '{}' AND email = '{}' AND name = 'Robin Newhire'", kRobin, kRobinEmail)), 1);
    EXPECT_EQ(count(where + "AND last_login_at IS NULL AND picture_url IS NULL AND status = 'active'"), 1) << "no sign-in yet";
    EXPECT_EQ(count(where + std::format("AND created_at = '{}'::timestamptz", db::to_timestamptz(clock_.now()))), 1) << "the engine's clock";
    EXPECT_EQ(count(std::format("sessions WHERE user_id = {}", id)), 0);
    EXPECT_EQ(count(std::format("audit_log WHERE action = 'user.create' AND actor_type = 'user' AND actor = 'staff@xvisionmediaus.com' "
                                "AND object_type = 'user' AND object_id = '{}' AND after->>'email' = '{}' "
                                "AND after->>'directory_id' = '{}' AND after->>'source' = 'directory'",
                                id, kRobinEmail, kRobin)),
              1);
    EXPECT_EQ(directory_.lists, 1);
}

TEST_F(UsersDirectory, TheSamePersonAgainIsTheSameUser) {
    const auto first = create(kRobin);
    ASSERT_EQ(first.status, 201u) << first.body;
    const auto again = create(kRobin);
    ASSERT_EQ(again.status, 200u) << again.body;
    EXPECT_EQ(json(again).at("id").as_int64(), json(first).at("id").as_int64());
    EXPECT_EQ(created_entries(), 1) << "no second entry";
    EXPECT_EQ(count(std::format("users WHERE google_sub = '{}'", kRobin)), 1);

    // A disabled user is answered as they are, and stays so.
    exec(std::format("UPDATE users SET status = 'disabled' WHERE google_sub = '{}'", kRobin));
    const auto disabled = create(kRobin);
    ASSERT_EQ(disabled.status, 200u) << disabled.body;
    EXPECT_EQ(json(disabled).at("status").as_string(), "disabled");
    EXPECT_EQ(created_entries(), 1);
}

TEST_F(UsersDirectory, TheRequestCannotNameThePerson) {
    const auto before = users();
    for (const auto* body : {R"({"directory_id": "100000000000000000042", "email": "someone.else@xvisionmediaus.com"})",
                             R"({"directory_id": "100000000000000000042", "name": "Someone Else"})", R"({"email": "robin.newhire@xvisionmediaus.com"})"}) {
        const auto refused = post("/api/users", body);
        EXPECT_EQ(refused.status, 400u) << body;
        EXPECT_EQ(error_code(refused), "invalid_request") << body;
    }
    EXPECT_EQ(users(), before);
    EXPECT_EQ(directory_.lists, 0);
}

TEST_F(UsersDirectory, NobodyInactiveOrOfAnotherDomainOrUnknownIsMadeAUser) {
    list("100000000000000000060", "sus.pended@xvisionmediaus.com", "Sus Pended", true);
    list("100000000000000000061", "ar.chived@xvisionmediaus.com", "Ar Chived", false, true);
    list("100000000000000000062", "out.side@example.com", "Out Side");
    const auto before = users();
    const auto refused = [&](std::string_view id, unsigned status, std::string_view code) {
        const auto answer = create(id);
        EXPECT_EQ(answer.status, status) << id << ": " << answer.body;
        EXPECT_EQ(error_code(answer), code) << id;
    };
    refused("100000000000000000060", 409, "directory_user_inactive");
    refused("100000000000000000061", 409, "directory_user_inactive");
    refused("100000000000000000062", 409, "domain_not_allowed");
    refused("100000000000000000099", 404, "not_found");
    directory_.failure = google::ApiError{google::ApiError::Kind::unauthenticated, 401, "Invalid Credentials"};
    refused(kRobin, 502, "directory_unavailable");
    directory_.failure.reset();
    exec("DELETE FROM credentials WHERE name = 'workspace'");
    refused(kRobin, 409, "workspace_not_connected");
    EXPECT_EQ(users(), before);
    EXPECT_EQ(created_entries(), 0);
}

TEST_F(UsersDirectory, AnIdThatIsNotPlainIsRefusedBeforeTheDirectoryIsRead) {
    for (const auto* id : {"someone@xvisionmediaus.com", "..", "", "1/2"}) {
        const auto refused = create(id);
        EXPECT_EQ(refused.status, 400u) << id;
        EXPECT_EQ(error_code(refused), "invalid_request") << id;
    }
    EXPECT_EQ(directory_.lists, 0);
}

TEST_F(UsersDirectory, TheMadeUserIsListedAndCanBeAssigned) {
    const auto made = create(kRobin);
    ASSERT_EQ(made.status, 201u) << made.body;
    const auto id = json(made).at("id").as_int64();
    bool listed = false;
    for (const auto& user : read("/api/users").at("items").as_array()) listed = listed || user.at("id").as_int64() == id;
    EXPECT_TRUE(listed);

    const auto path = std::format("/api/escalations/{}/assign", escalation());
    const auto assigned = post(path, boost::json::serialize(boost::json::object{{"assignee_id", id}}));
    ASSERT_EQ(assigned.status, 200u) << assigned.body;
    EXPECT_EQ(json(assigned).at("assignee").at("email").as_string(), kRobinEmail);
}

TEST_F(UsersDirectory, ThePersonsFirstSignInFindsTheRow) {
    const auto made = create(kRobin);
    ASSERT_EQ(made.status, 201u) << made.body;
    const auto id = json(made).at("id").as_int64();
    const auto escalation_id = escalation();
    ASSERT_EQ(post(std::format("/api/escalations/{}/assign", escalation_id), boost::json::serialize(boost::json::object{{"assignee_id", id}})).status,
              200u);

    const auto robin_browser = sign_in(robin());
    ASSERT_FALSE(robin_browser.cookie.empty());
    EXPECT_EQ(json(get("/api/session", robin_browser)).at("user").at("id").as_int64(), id);
    EXPECT_EQ(count(std::format("users WHERE email = '{}'", kRobinEmail)), 1) << "one row before and after";
    EXPECT_TRUE(first_sign_in("session.login", kRobin));
    EXPECT_EQ(count(std::format("users WHERE id = {} AND last_login_at = '{}'::timestamptz "
                                "AND picture_url = 'https://lh3.googleusercontent.com/a/robin'",
                                id, db::to_timestamptz(clock_.now()))),
              1);
    EXPECT_EQ(read(std::format("/api/escalations/{}", escalation_id)).at("assignee").at("id").as_int64(), id) << "still theirs";

    sign_in(robin());
    EXPECT_FALSE(first_sign_in("session.login", kRobin)) << "a second sign-in is no first one";
}

TEST_F(UsersDirectory, ThePersonsFirstSignInFromThePhoneAppFindsTheRow) {
    const auto made = create(kRobin);
    ASSERT_EQ(made.status, 201u) << made.body;
    const auto token = app_sign_in(robin());
    ASSERT_FALSE(token.empty());
    EXPECT_EQ(json(as_app(verb::get, "/api/session", token)).at("user").at("id").as_int64(), json(made).at("id").as_int64());
    EXPECT_EQ(count(std::format("users WHERE email = '{}'", kRobinEmail)), 1);
    EXPECT_TRUE(first_sign_in("session.app_code_issued", kRobin));
    EXPECT_TRUE(first_sign_in("session.login", kRobin));

    ASSERT_FALSE(app_sign_in(robin()).empty());
    EXPECT_FALSE(first_sign_in("session.app_code_issued", kRobin));
    EXPECT_FALSE(first_sign_in("session.login", kRobin));
}

TEST_F(UsersDirectory, ABearerTokenMaySearchAndCreate) {
    const auto token = app_sign_in(staff("colleague@xvisionmediaus.com"));
    ASSERT_FALSE(token.empty());
    EXPECT_EQ(as_app(verb::get, "/api/users/directory?q=robin", token).status, 200u);
    const auto made = as_app(verb::post, "/api/users", token, std::format(R"({{"directory_id": "{}"}})", kRobin));
    ASSERT_EQ(made.status, 201u) << made.body;
    EXPECT_EQ(count("audit_log WHERE action = 'user.create' AND actor = 'colleague@xvisionmediaus.com'"), 1);
}
