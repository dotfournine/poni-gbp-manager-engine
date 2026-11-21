// The dashboard's API over a real PostgreSQL: clients, locations, the review
// inbox and its filters, the audit log (and that it is append-only), and the
// system status.

#include "api_fixture.hpp"
#include "core/crypto.hpp"
#include "db/repo_auth.hpp"
#include "db/repo_reviews.hpp"
#include "jobs/sync.hpp"

#include <gmock/gmock.h>

using namespace poni;
using namespace std::chrono_literals;
using test::make_review;

namespace {

class Api : public test::ApiTest {
protected:
    void SetUp() override {
        ApiTest::SetUp();
        browser_ = sign_in();
    }

    void store(const gbp::Review& review) {
        store_review(location_id(gbp::parse_review_name(review.name)->location_id), review, repo::kSourceFullSync);
    }

    // Google's ids of the reviews a list returns, in its order.
    std::vector<std::string> review_ids(const std::string& query) {
        std::vector<std::string> ids;
        for (const auto& item : read("/api/reviews" + query).at("items").as_array()) {
            ids.push_back(gbp_review_id(item.at("id").as_int64()));
        }
        return ids;
    }

};

}  // namespace

// ---- clients ---------------------------------------------------------------------

TEST_F(Api, ClientsAreCreatedChangedArchivedAndAudited) {
    const auto created = post("/api/clients", R"({"name": "  LU DREAM SPA  ", "notes": "Two locations"})");
    ASSERT_EQ(created.status, 201u) << created.body;
    const auto id = json(created).at("id").as_int64();
    EXPECT_EQ(json(created).at("name").as_string(), "LU DREAM SPA");
    EXPECT_EQ(json(created).at("location_count").as_int64(), 0);

    const auto renamed = patch("/api/clients/" + std::to_string(id), R"({"name": "LU DREAM SPA 路梦养生馆"})");
    ASSERT_EQ(renamed.status, 200u);
    EXPECT_EQ(json(renamed).at("notes").as_string(), "Two locations") << "fields that aren't sent stay";
    EXPECT_EQ(patch("/api/clients/" + std::to_string(id), R"({"archived": true})").status, 200u);
    EXPECT_EQ(patch("/api/clients/" + std::to_string(id), R"({"archived": true})").status, 200u) << "no change, no error";

    EXPECT_TRUE(read("/api/clients/" + std::to_string(id)).at("archived").as_bool());
    EXPECT_EQ(read("/api/clients?archived=false").at("total").as_int64(), 0);
    EXPECT_EQ(read("/api/clients?archived=true&q=dream").at("total").as_int64(), 1);

    const auto log = read("/api/audit-log?object_type=client").at("items").as_array();
    ASSERT_EQ(log.size(), 3u) << "the request that changed nothing left no entry";
    EXPECT_EQ(log[0].at("action").as_string(), "client.archive");
    EXPECT_EQ(log[1].at("action").as_string(), "client.update");
    EXPECT_EQ(log[2].at("action").as_string(), "client.create");
    EXPECT_EQ(log[1].at("actor").as_string(), "staff@xvisionmediaus.com");
    EXPECT_EQ(log[1].at("before").at("name").as_string(), "LU DREAM SPA");
    EXPECT_EQ(log[1].at("after").at("name").as_string(), "LU DREAM SPA 路梦养生馆");
    EXPECT_EQ(log[1].at("object_id").as_string(), std::to_string(id));
    EXPECT_EQ(log[1].at("ip").as_string(), "203.0.113.10");
}

TEST_F(Api, ClientRequestsAreValidated) {
    ASSERT_EQ(post("/api/clients", R"({"name": "Gangnam AYCE Buffet"})").status, 201u);

    const auto taken = post("/api/clients", R"({"name": "gangnam ayce buffet"})");
    EXPECT_EQ(taken.status, 409u);
    EXPECT_EQ(error_code(taken), "name_taken");
    for (const auto* body : {"", "[]", "{}", R"({"name": 5})", R"({"name": "   "})", R"({"name": "ok", "notes": 1})"}) {
        const auto response = post("/api/clients", body);
        EXPECT_EQ(response.status, 400u) << body;
        EXPECT_EQ(error_code(response), "invalid_request") << body;
    }
    EXPECT_EQ(patch("/api/clients/1", R"({"archived": "yes"})").status, 400u);
    EXPECT_EQ(patch("/api/clients/999", R"({"name": "x"})").status, 404u);
    EXPECT_EQ(get("/api/clients/abc", browser_).status, 404u);
    EXPECT_EQ(get("/api/clients/0", browser_).status, 404u);
    EXPECT_EQ(get("/api/clients?page=0", browser_).status, 400u);
    EXPECT_EQ(get("/api/clients?per_page=101", browser_).status, 400u);
    EXPECT_EQ(get("/api/clients?archived=maybe", browser_).status, 400u);
    EXPECT_EQ(del("/api/clients/1").status, 405u) << "nothing can be deleted through the API";
    EXPECT_EQ(count("clients"), 1);
    EXPECT_EQ(count("audit_log WHERE object_type = 'client'"), 1);
}

// ---- locations -------------------------------------------------------------------

TEST_F(Api, LocationsAreLinkedToClientsAndTheLinkIsAudited) {
    discover_spa_and_buffet();
    const auto client = json(post("/api/clients", R"({"name": "LU DREAM SPA"})")).at("id").as_int64();
    const auto spa = location_id("9001");

    EXPECT_EQ(read("/api/locations").at("total").as_int64(), 2);
    EXPECT_EQ(read("/api/locations?linked=false").at("total").as_int64(), 2);

    const auto linked = patch("/api/locations/" + std::to_string(spa), std::format(R"({{"client_id": {}}})", client));
    ASSERT_EQ(linked.status, 200u) << linked.body;
    EXPECT_EQ(json(linked).at("client_name").as_string(), "LU DREAM SPA");
    EXPECT_EQ(json(linked).at("gbp_location_id").as_string(), "9001");
    EXPECT_EQ(count("locations WHERE id = " + std::to_string(spa) + " AND next_review_poll_at IS NOT NULL AND next_full_sync_at IS NOT NULL"), 1)
        << "linking makes the reviews due for a sync at once";

    EXPECT_EQ(read("/api/locations?linked=true").at("items").as_array().size(), 1u);
    EXPECT_EQ(read(std::format("/api/locations?client_id={}", client)).at("total").as_int64(), 1);
    EXPECT_EQ(read("/api/locations?q=gangnam").at("items").at(0).at("title").as_string(), "Gangnam AYCE Buffet");
    EXPECT_EQ(read(std::format("/api/clients/{}", client)).at("location_count").as_int64(), 1);

    const auto entry = read("/api/audit-log?action=location.link").at("items").at(0);
    EXPECT_TRUE(entry.at("before").at("client_id").is_null());
    EXPECT_EQ(entry.at("after").at("client_name").as_string(), "LU DREAM SPA");

    ASSERT_EQ(patch("/api/locations/" + std::to_string(spa), R"({"client_id": null})").status, 200u);
    EXPECT_TRUE(read("/api/locations/" + std::to_string(spa)).at("client_id").is_null());
    EXPECT_EQ(read("/api/audit-log?action=location.unlink").at("total").as_int64(), 1);
}

TEST_F(Api, ARefusedLocationChangeChangesNothing) {
    discover_spa_and_buffet();
    const auto spa = std::to_string(location_id("9001"));
    const auto archived = json(post("/api/clients", R"({"name": "Old client"})")).at("id").as_int64();
    patch(std::format("/api/clients/{}", archived), R"({"archived": true})");
    const auto entries_before = count("audit_log");

    // The interval is fine but the client isn't: neither is applied.
    const auto refused = patch("/api/locations/" + spa, R"({"review_poll_interval_s": 300, "client_id": 999})");
    EXPECT_EQ(refused.status, 409u);
    EXPECT_EQ(error_code(refused), "client_not_found");
    EXPECT_EQ(patch("/api/locations/" + spa, std::format(R"({{"client_id": {}}})", archived)).status, 409u);
    EXPECT_EQ(patch("/api/locations/" + spa, R"({"review_poll_interval_s": 5})").status, 400u);
    EXPECT_EQ(patch("/api/locations/" + spa, R"({"client_id": "one"})").status, 400u);
    EXPECT_EQ(patch("/api/locations/12345", R"({"client_id": null})").status, 404u);

    EXPECT_EQ(read("/api/locations/" + spa).at("review_poll_interval_s").as_int64(), 120);
    EXPECT_EQ(count("audit_log"), entries_before);

    ASSERT_EQ(patch("/api/locations/" + spa, R"({"review_poll_interval_s": 300})").status, 200u);
    EXPECT_EQ(read("/api/audit-log?action=location.update").at("items").at(0).at("after").at("review_poll_interval_s").as_int64(), 300);
}

TEST_F(Api, DiscoveryCanBeRequestedAndRunsAsAJob) {
    const auto response = post("/api/locations/discover", "");
    EXPECT_EQ(response.status, 202u);
    EXPECT_TRUE(json(response).at("enqueued").as_bool());
    EXPECT_EQ(count("jobs WHERE kind = 'locations.discover' AND status = 'queued'"), 1);
    EXPECT_EQ(count("audit_log WHERE action = 'location.discovery_requested' AND actor = 'staff@xvisionmediaus.com'"), 1);
}

// ---- reviews ---------------------------------------------------------------------

TEST_F(Api, TheReviewInboxFiltersSortsAndPages) {
    discover_spa_and_buffet();
    const auto spa_client = json(post("/api/clients", R"({"name": "LU DREAM SPA"})")).at("id").as_int64();
    const auto buffet_client = json(post("/api/clients", R"({"name": "Gangnam AYCE Buffet"})")).at("id").as_int64();
    patch(std::format("/api/locations/{}", location_id("9001")), std::format(R"({{"client_id": {}}})", spa_client));
    patch(std::format("/api/locations/{}", location_id("9002")), std::format(R"({{"client_id": {}}})", buffet_client));

    store(make_review("111", "9001", "s1", "FIVE", "Wonderful massage", "2026-11-01T10:00:00Z", "2026-11-01T10:00:00Z", "Thank you!"));
    store(make_review("111", "9001", "s2", "FIVE", "(Translated by Google) Very comfortable\n\n(Original)\n很舒服", "2026-11-02T10:00:00Z",
                      "2026-11-02T10:00:00Z"));
    store(make_review("111", "9001", "s3", "FOUR", std::nullopt, "2026-11-03T10:00:00Z", "2026-11-03T10:00:00Z"));
    store(make_review("111", "9002", "b1", "ONE", "Cold food and rude staff", "2026-11-04T10:00:00Z", "2026-11-05T10:00:00Z",
                      "We are sorry."));
    store(make_review("111", "9002", "b2", "TWO", "Meh", "2026-11-05T09:00:00Z", "2026-11-05T09:00:00Z"));
    {
        pqxx::connection conn{url()};
        pqxx::work tx{conn};
        ASSERT_TRUE(repo::mark_review_removed(tx, location_id("9002"), "b2", repo::kSourceFullSync, clock_.now()));
        tx.commit();
    }
    using Ids = std::vector<std::string>;

    EXPECT_EQ(review_ids(""), (Ids{"b1", "s3", "s2", "s1"})) << "newest first; removed reviews are left out by default";
    EXPECT_EQ(read("/api/reviews").at("total").as_int64(), 4);
    EXPECT_EQ(review_ids("?status=removed"), (Ids{"b2"}));
    EXPECT_EQ(review_ids("?status=all").size(), 5u);
    EXPECT_EQ(review_ids(std::format("?client_id={}", spa_client)), (Ids{"s3", "s2", "s1"}));
    EXPECT_EQ(review_ids(std::format("?location_id={}", location_id("9002"))), (Ids{"b1"}));
    EXPECT_EQ(review_ids("?rating=1,2"), (Ids{"b1"}));
    EXPECT_EQ(review_ids("?rating=4&rating=1"), (Ids{"b1", "s3"}));
    EXPECT_EQ(review_ids("?has_reply=false"), (Ids{"s3", "s2"}));
    EXPECT_EQ(review_ids("?has_reply=true"), (Ids{"b1", "s1"}));
    EXPECT_EQ(review_ids("?language=zh"), (Ids{"s2"}));
    EXPECT_EQ(review_ids("?language=none"), (Ids{"s3"}));
    EXPECT_EQ(review_ids("?q=RUDE"), (Ids{"b1"}));
    EXPECT_EQ(review_ids("?q=reviewer+s1"), (Ids{"s1"}));
    EXPECT_EQ(review_ids("?q=100%25"), Ids{}) << "wildcard characters are plain text";
    EXPECT_EQ(review_ids("?sort=rating&order=asc"), (Ids{"b1", "s3", "s1", "s2"}));
    EXPECT_EQ(review_ids("?sort=updated_time"), (Ids{"b1", "s3", "s2", "s1"}));
    EXPECT_EQ(review_ids("?per_page=2&page=2"), (Ids{"s2", "s1"}));
    EXPECT_EQ(read("/api/reviews?per_page=2&page=2").at("total").as_int64(), 4);
    EXPECT_EQ(review_ids("?has_reply=false&rating=5&client_id=" + std::to_string(spa_client)), (Ids{"s2"}));

    for (const auto* bad : {"?rating=6", "?rating=x", "?status=gone", "?has_reply=1", "?sort=id", "?order=up", "?client_id=x",
                            "?per_page=0"}) {
        EXPECT_EQ(get(std::string("/api/reviews") + bad, browser_).status, 400u) << bad;
    }

    const auto id = read("/api/reviews?language=zh").at("items").at(0).at("id").as_int64();
    const auto review = read(std::format("/api/reviews/{}", id));
    EXPECT_EQ(review.at("original_text").as_string(), "很舒服");
    EXPECT_EQ(review.at("translated_text").as_string(), "Very comfortable");
    EXPECT_EQ(review.at("location_title").as_string(), "LU DREAM SPA - Chelsea");
    EXPECT_EQ(review.at("client_name").as_string(), "LU DREAM SPA");
    EXPECT_EQ(review.at("created_time").as_string(), "2026-11-02T10:00:00.000000Z");
    EXPECT_FALSE(review.at("has_reply").as_bool());
    EXPECT_TRUE(review.at("reply").is_null());
    EXPECT_EQ(review.at("status").as_string(), "active");

    const auto replied = read("/api/reviews?q=rude").at("items").at(0);
    EXPECT_EQ(replied.at("reply").at("text").as_string(), "We are sorry.");
    EXPECT_EQ(replied.at("reply").at("origin").as_string(), "external");

    const auto removed = read("/api/reviews?status=removed").at("items").at(0);
    EXPECT_EQ(removed.at("status").as_string(), "removed");
    const auto events = read(std::format("/api/reviews/{}/events", removed.at("id").as_int64())).at("items").as_array();
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].at("kind").as_string(), "discovered");
    EXPECT_EQ(events[1].at("kind").as_string(), "removed");

    EXPECT_EQ(get("/api/reviews/999999", browser_).status, 404u);
    EXPECT_EQ(get("/api/reviews/999999/events", browser_).status, 404u);
    EXPECT_EQ(post("/api/reviews", "{}").status, 405u) << "the inbox is read-only";

    const auto buffet = read(std::format("/api/locations/{}", location_id("9002")));
    EXPECT_EQ(buffet.at("review_count").as_int64(), 1) << "removed reviews are not counted";
    EXPECT_EQ(buffet.at("unreplied_count").as_int64(), 0);
    EXPECT_DOUBLE_EQ(buffet.at("average_rating").as_double(), 1.0);
}

// ---- audit log and status --------------------------------------------------------

TEST_F(Api, TheAuditLogCanOnlyGrow) {
    post("/api/clients", R"({"name": "A"})");
    ASSERT_GE(count("audit_log"), 2) << "the sign-in and the new client";

    EXPECT_THROW(exec("UPDATE audit_log SET actor = 'someone else'"), pqxx::sql_error);
    EXPECT_THROW(exec("DELETE FROM audit_log"), pqxx::sql_error);
    EXPECT_THROW(exec("TRUNCATE audit_log"), pqxx::sql_error);
    EXPECT_EQ(count("audit_log WHERE actor = 'someone else'"), 0);

    const auto log = read("/api/audit-log?per_page=1");
    EXPECT_EQ(log.at("items").as_array().size(), 1u);
    EXPECT_EQ(log.at("items").at(0).at("action").as_string(), "client.create") << "newest first";
    EXPECT_GE(log.at("total").as_int64(), 2);
    EXPECT_EQ(read("/api/audit-log?actor=STAFF@").at("total").as_int64(), log.at("total").as_int64());
    EXPECT_EQ(read("/api/audit-log?actor=system:").at("total").as_int64(), 0);
    EXPECT_EQ(post("/api/audit-log", "{}").status, 405u);
}

TEST_F(Api, AChangeWithoutAnAuditEntryIsRolledBack) {
    EXPECT_THROW(audit::transact(conn(), audit::Actor::system("test"), clock_.now(),
                                 [](pqxx::work& tx, audit::Writer&) {
                                     tx.exec("INSERT INTO clients (name, created_at, updated_at) VALUES ('sneaky', now(), now())");
                                     return 0;
                                 }),
                 std::logic_error);
    EXPECT_EQ(count("clients WHERE name = 'sneaky'"), 0);

    // A refusal needs no entry, and nothing of it is committed either.
    const auto refused = audit::transact(conn(), audit::Actor::system("test"), clock_.now(),
                                         [](pqxx::work& tx, audit::Writer&) -> repo::Result<int> {
                                             tx.exec("INSERT INTO clients (name, created_at, updated_at) VALUES ('refused', now(), now())");
                                             return repo::conflict("refused", "No.");
                                         });
    EXPECT_FALSE(refused);
    EXPECT_EQ(count("clients WHERE name = 'refused'"), 0);

    // A function that wrote, recorded its entry and then answers with a failure keeps what it wrote.
    const auto recorded = audit::transact(conn(), audit::Actor::system("test"), clock_.now(),
                                          [](pqxx::work& tx, audit::Writer& audit) -> repo::Result<int> {
                                              tx.exec("INSERT INTO clients (name, created_at, updated_at) VALUES ('recorded', now(), now())");
                                              audit.record({.action = "client.test", .object_type = "client"});
                                              return repo::conflict("refused", "No.");
                                          });
    EXPECT_FALSE(recorded);
    EXPECT_EQ(count("clients WHERE name = 'recorded'"), 1);
    EXPECT_EQ(count("audit_log WHERE action = 'client.test'"), 1);
}

TEST_F(Api, SystemStatusShowsTokenSyncAndFailedJobs) {
    discover_spa_and_buffet();
    auto status = read("/api/system/status");
    EXPECT_FALSE(status.at("gbp").at("connected").as_bool());
    EXPECT_TRUE(status.at("database").at("reachable").as_bool());
    EXPECT_FALSE(status.at("pubsub").at("enabled").as_bool());
    EXPECT_FALSE(status.at("location_discovery").at("last_ok_at").is_null());
    EXPECT_EQ(status.at("locations").as_array().size(), 2u);
    EXPECT_FALSE(status.at("locations").at(0).at("syncing").as_bool()) << "not linked yet";
    EXPECT_EQ(status.at("jobs").at("failed").as_int64(), 0);

    const auto client = make_client("Gangnam AYCE Buffet");
    link("9002", client);
    {
        const auto box = core::SecretBox::from_base64_key(core::base64_encode(std::string(32, 'k'))).value();
        pqxx::connection conn{url()};
        pqxx::work tx{conn};
        repo::save_gbp_credentials(tx, box, {"id", "secret", "refresh"}, clock_.now());
        repo::mark_integration_error(tx, repo::kGbpTokenStatus, "HTTP 400 invalid_grant", clock_.now());
        repo::record_sync_error(tx, {location_id("9002")}, "HTTP 403 PERMISSION_DENIED", clock_.now(), false);
        tx.exec("INSERT INTO jobs (kind, status, run_at, attempts, last_error, created_at, updated_at, finished_at) "
                "VALUES ('reviews.full_sync', 'failed', $1::timestamptz, 5, 'boom', $1::timestamptz, $1::timestamptz, $1::timestamptz)",
                pqxx::params{db::to_timestamptz(clock_.now())});
        EXPECT_EQ(tx.query_value<std::string>("SELECT ciphertext FROM credentials").find("refresh"), std::string::npos);
        tx.commit();
    }

    status = read("/api/system/status");
    EXPECT_TRUE(status.at("gbp").at("connected").as_bool());
    EXPECT_EQ(status.at("gbp").at("last_error").as_string(), "HTTP 400 invalid_grant");
    const auto& buffet = status.at("locations").at(0);
    EXPECT_EQ(buffet.at("title").as_string(), "Gangnam AYCE Buffet");
    EXPECT_TRUE(buffet.at("syncing").as_bool());
    EXPECT_EQ(buffet.at("last_sync_error").as_string(), "HTTP 403 PERMISSION_DENIED");
    EXPECT_EQ(status.at("jobs").at("failed").as_int64(), 1);
    EXPECT_EQ(status.at("jobs").at("failures").at(0).at("last_error").as_string(), "boom");
    EXPECT_EQ(status.at("version").as_string(), PONI_VERSION);
}

TEST_F(Api, TheStoredGbpTokenRoundTripsOnlyWithTheSameKey) {
    const auto box = core::SecretBox::from_base64_key(core::base64_encode(std::string(32, 'k'))).value();
    const auto other = core::SecretBox::from_base64_key(core::base64_encode(std::string(32, 'z'))).value();
    pqxx::work tx{conn()};
    EXPECT_FALSE(repo::load_gbp_credentials(tx, box).value()) << "nothing stored yet";

    repo::save_gbp_credentials(tx, box, {"client-id", "client-secret", "the-refresh-token"}, clock_.now());
    const auto loaded = repo::load_gbp_credentials(tx, box);
    ASSERT_TRUE(loaded && *loaded);
    EXPECT_EQ((*loaded)->refresh_token, "the-refresh-token");

    const auto wrong_key = repo::load_gbp_credentials(tx, other);
    ASSERT_FALSE(wrong_key);
    EXPECT_THAT(wrong_key.error(), testing::HasSubstr("another PONI_SECRET_KEY"));

    // Moving the ciphertext to another row doesn't make it readable there.
    tx.exec("INSERT INTO credentials SELECT 'other', ciphertext, key_id, created_at, updated_at FROM credentials");
    const auto moved = repo::load_credential(tx, "other");
    ASSERT_TRUE(moved);
    EXPECT_FALSE(box.open(moved->ciphertext, "other"));
}
