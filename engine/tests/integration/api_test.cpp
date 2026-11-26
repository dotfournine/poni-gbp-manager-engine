// The dashboard's API over a real PostgreSQL: clients, locations and their switch, the review
// inbox and its filters, the audit log (and that it is append-only), and the
// system status.

#include "api_fixture.hpp"
#include "core/crypto.hpp"
#include "db/repo_auth.hpp"
#include "db/repo_reviews.hpp"
#include "jobs/sync.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <gmock/gmock.h>
#include <pqxx/pqxx>

#include <exception>
#include <vector>

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

    const auto path = "/api/clients/" + std::to_string(id);
    const auto renamed = patch(path, R"({"name": "LU DREAM SPA 路梦养生馆"})", if_match(etag(path, browser_)));
    ASSERT_EQ(renamed.status, 200u);
    EXPECT_EQ(json(renamed).at("notes").as_string(), "Two locations") << "fields that aren't sent stay";
    EXPECT_EQ(patch(path, R"({"archived": true})", if_match(header(renamed, "ETag"))).status, 200u);
    EXPECT_EQ(patch(path, R"({"archived": true})", if_match(etag(path, browser_))).status, 200u) << "no change, no error";

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
    EXPECT_EQ(del("/api/clients/999").status, 404u);
    EXPECT_EQ(del("/api/clients/abc").status, 404u);
    EXPECT_EQ(count("clients"), 1);
    EXPECT_EQ(count("audit_log WHERE object_type = 'client'"), 1);
}

// A client is bookkeeping: deleting it unlinks its locations and changes nothing else about them (D-102).
TEST_F(Api, DeletingAClientUnlinksItsLocationsAndChangesNothingElse) {
    discover_spa_and_buffet();
    const auto client = json(post("/api/clients", R"({"name": "LU DREAM SPA", "notes": "Two shops"})")).at("id").as_int64();
    const auto spa = location_id("9001");
    const auto buffet = location_id("9002");
    ASSERT_EQ(patch_current(std::format("/api/locations/{}", spa), std::format(R"({{"client_id": {}, "enabled": true, "industry_key": "wellness"}})", client))
                  .status,
              200u);
    ASSERT_EQ(patch_current(std::format("/api/locations/{}", buffet), std::format(R"({{"client_id": {}}})", client)).status, 200u);
    ASSERT_EQ(save_settings(spa, "voice", R"({"reply_name": "Willow Moon Head Spa"})").status, 200u);
    // A report of the location: a row that named the client before D-102, so that the delete failed on it.
    const auto made = post(std::format("/api/locations/{}/reports", spa), R"({"period_start": "2026-10-01", "period_end": "2026-10-31"})");
    ASSERT_EQ(made.status, 201u) << made.body;
    const auto report = json(made).at("id").as_int64();
    // The location and its settings, without the client they name.
    const auto without_the_client = [&](std::int64_t location) {
        boost::json::object out{{"location", read(std::format("/api/locations/{}", location))}, {"settings", read(settings_path(location))}};
        for (auto& [part, value] : out) {
            value.as_object().erase("client_id");
            value.as_object().erase("client_name");
        }
        return out;
    };
    const auto spa_before = without_the_client(spa);
    const auto buffet_before = without_the_client(buffet);
    const auto target = std::format("/api/clients/{}", client);
    EXPECT_EQ(call(verb::delete_, target, {.cookie = browser_.cookie, .csrf = ""}).status, 403u) << "a change needs the CSRF token";

    const auto deleted = del(target, if_match(etag(target, browser_)));
    EXPECT_EQ(deleted.status, 204u);
    EXPECT_TRUE(deleted.body.empty());
    EXPECT_EQ(count("clients"), 0);
    EXPECT_EQ(count("locations WHERE client_id IS NOT NULL"), 0);
    EXPECT_EQ(without_the_client(spa), spa_before);
    EXPECT_EQ(without_the_client(buffet), buffet_before);
    EXPECT_TRUE(read(std::format("/api/locations/{}", spa)).at("enabled").as_bool());
    EXPECT_EQ(read(std::format("/api/reports/{}", report)).at("location_id").as_int64(), spa) << "the location's report stays";
    const auto entry = read("/api/audit-log?action=client.delete").at("items").at(0);
    EXPECT_EQ(entry.at("object_id").as_string(), std::to_string(client));
    EXPECT_EQ(entry.at("actor").as_string(), "staff@xvisionmediaus.com");
    EXPECT_EQ(entry.at("before").at("name").as_string(), "LU DREAM SPA");
    EXPECT_EQ(entry.at("before").at("notes").as_string(), "Two shops");
    EXPECT_EQ(boost::json::serialize(entry.at("before").at("location_ids")), std::format("[{},{}]", spa, buffet));
    EXPECT_EQ(read("/api/audit-log?action=location.unlink").at("total").as_int64(), 2) << "one unlink per location";

    EXPECT_EQ(get(target, browser_).status, 404u);
    EXPECT_EQ(del(target).status, 404u);
}

// ---- a client's ETag (D-108) -------------------------------------------------------

TEST_F(Api, AClientIsChangedOnlyAgainstTheEtagItsReaderHad) {
    const auto id = json(post("/api/clients", R"({"name": "Willow Moon", "notes": "One shop"})")).at("id").as_int64();
    const auto path = std::format("/api/clients/{}", id);
    const auto read_tag = etag(path, browser_);
    const auto audited = [&] { return count("audit_log WHERE object_type = 'client'"); };

    const auto renamed = patch(path, R"({"name": "Willow Moon Head Spa"})", if_match(read_tag));
    ASSERT_EQ(renamed.status, 200u) << renamed.body;
    const auto new_tag = header(renamed, "ETag");
    EXPECT_NE(new_tag, read_tag);
    EXPECT_EQ(new_tag, etag(path, browser_)) << "the change answers with the tag a GET gives";
    const auto after_rename = audited();

    // The same change again from the view that is now stale: refused, and nothing is written.
    const auto stale = patch(path, R"({"name": "Willow Moon Spa"})", if_match(read_tag));
    EXPECT_EQ(stale.status, 412u);
    EXPECT_EQ(error_code(stale), "precondition_failed");
    EXPECT_TRUE(header(stale, "ETag").empty());
    EXPECT_EQ(read(path).at("name").as_string(), "Willow Moon Head Spa");
    EXPECT_EQ(audited(), after_rename);

    const auto without = patch(path, R"({"name": "Willow Moon Spa"})");
    EXPECT_EQ(without.status, 428u);
    EXPECT_EQ(error_code(without), "precondition_required");
    for (const auto& refused : {std::string("*"), new_tag + ", " + new_tag, read_tag + ", " + new_tag, std::string(""), "W/" + new_tag}) {
        const auto response = patch(path, R"({"name": "Willow Moon Spa"})", if_match(refused));
        EXPECT_EQ(response.status, 412u) << "[" << refused << "]";
        EXPECT_EQ(error_code(response), "precondition_failed") << "[" << refused << "]";
    }
    EXPECT_EQ(read(path).at("name").as_string(), "Willow Moon Head Spa");
    EXPECT_EQ(audited(), after_rename);

    // A save that changes nothing: stale, it is refused like any other; up to date, it is a 200 that records nothing.
    EXPECT_EQ(patch(path, R"({"name": "Willow Moon Head Spa"})", if_match(read_tag)).status, 412u) << "a stale equal save";
    const auto equal = patch(path, R"({"name": "Willow Moon Head Spa", "notes": "One shop"})", if_match(new_tag));
    ASSERT_EQ(equal.status, 200u) << equal.body;
    EXPECT_EQ(header(equal, "ETag"), new_tag);
    EXPECT_EQ(audited(), after_rename);

    // What a compressing proxy in front of the engine makes of the tag is the same tag.
    const auto gzip = "\"" + new_tag.substr(1, 64) + "-gzip\"";
    EXPECT_EQ(patch(path, R"({"archived": true})", if_match(gzip)).status, 200u);
    EXPECT_EQ(audited(), after_rename + 1);
}

TEST_F(Api, AClientsEtagIsNotChangedByALinkMadeThroughTheLocation) {
    discover_spa_and_buffet();
    const auto id = json(post("/api/clients", R"({"name": "LU DREAM SPA"})")).at("id").as_int64();
    const auto path = std::format("/api/clients/{}", id);
    const auto before = etag(path, browser_);
    // Linking a location changes the client's count, its list of locations and nothing a PATCH of it sets.
    ASSERT_EQ(patch_current(std::format("/api/locations/{}", location_id("9001")), std::format(R"({{"client_id": {}}})", id)).status, 200u);
    EXPECT_EQ(read(path).at("location_count").as_int64(), 1);
    EXPECT_EQ(etag(path, browser_), before);
    EXPECT_EQ(patch(path, R"({"notes": "Two shops"})", if_match(before)).status, 200u);
}

TEST_F(Api, AClientIsDeletedOnlyAgainstItsEtag) {
    const auto id = json(post("/api/clients", R"({"name": "Willow Moon"})")).at("id").as_int64();
    const auto path = std::format("/api/clients/{}", id);
    const auto read_tag = etag(path, browser_);
    ASSERT_EQ(patch(path, R"({"notes": "Closing"})", if_match(read_tag)).status, 200u);

    EXPECT_EQ(del(path).status, 428u);
    EXPECT_EQ(del(path, if_match(read_tag)).status, 412u) << "a stale tag";
    EXPECT_EQ(del(path, if_match("*")).status, 412u);
    EXPECT_EQ(count(std::format("clients WHERE id = {}", id)), 1);

    const auto deleted = del(path, if_match(etag(path, browser_)));
    EXPECT_EQ(deleted.status, 204u);
    EXPECT_TRUE(header(deleted, "ETag").empty());
    EXPECT_EQ(count(std::format("clients WHERE id = {}", id)), 0);
    // Gone is a 404, whatever the tag, and never a 428.
    EXPECT_EQ(del(path, if_match(read_tag)).status, 404u);
    EXPECT_EQ(del(path).status, 404u);
}

TEST_F(Api, AClientChangeIsCheckedForItsBodyThenItsClientThenItsTag) {
    const auto id = json(post("/api/clients", R"({"name": "Willow Moon"})")).at("id").as_int64();
    const auto path = std::format("/api/clients/{}", id);
    const auto audited = count("audit_log");

    // The body first: a member the schema does not have is a 400, with or without a tag.
    for (const auto& headers : {Headers{}, if_match("\"stale\"")}) {
        const auto refused = patch(path, R"({"not_in_the_spec": true})", headers);
        EXPECT_EQ(refused.status, 400u);
        EXPECT_EQ(error_code(refused), "invalid_request");
    }
    // Then the client: one that does not exist is a 404, not a 428.
    const auto missing = patch("/api/clients/999999", R"({"name": "x"})");
    EXPECT_EQ(missing.status, 404u);
    EXPECT_EQ(error_code(missing), "not_found");
    EXPECT_EQ(patch("/api/clients/999999", R"({"name": "x"})", if_match("\"stale\"")).status, 404u);
    EXPECT_EQ(del("/api/clients/999999").status, 404u);
    // An If-Match on a route it does not guard is a header like any other.
    EXPECT_EQ(post("/api/clients", R"({"name": "Gangnam AYCE Buffet"})").status, 201u);
    EXPECT_EQ(call(verb::post, "/api/clients", browser_, R"({"name": "Another"})", if_match("\"stale\"")).status, 201u);
    EXPECT_EQ(count("audit_log"), audited + 2);
}

namespace {

// Keeps `lock` (a transaction that holds a row lock) until two other backends wait on a lock, or five seconds have
// passed, then commits it; `seen` is how many waited.
boost::asio::awaitable<void> release_when_two_wait(pqxx::work& lock, pqxx::connection& watcher, long& seen) {
    boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
    const auto waiting = [&watcher] {
        pqxx::nontransaction tx{watcher};
        return tx.query_value<long>("SELECT count(*) FROM pg_stat_activity WHERE datname = current_database() AND wait_event_type = 'Lock'");
    };
    for (int i = 0; i < 500 && (seen = waiting()) < 2; ++i) {
        timer.expires_after(10ms);
        co_await timer.async_wait(boost::asio::use_awaitable);
    }
    lock.commit();
}

}  // namespace

TEST_F(Api, OfTwoChangesAgainstTheSameEtagOneIsRefused) {
    const auto id = json(post("/api/clients", R"({"name": "Willow Moon"})")).at("id").as_int64();
    const auto path = std::format("/api/clients/{}", id);
    const auto tag = etag(path, browser_);

    // The row is held while both are sent, so that both have read the tag's client and wait for it at once.
    pqxx::connection holder{url()};
    pqxx::connection watcher{url()};
    pqxx::work lock{holder};
    lock.exec("SELECT 1 FROM clients WHERE id = $1 FOR UPDATE", pqxx::params{id}).one_row();
    std::vector<unsigned> statuses;
    for (const auto* name : {"Willow Moon Head Spa", "Willow Moon Day Spa"}) {
        http::Request request;
        request.method = verb::patch;
        request.path = path;
        request.remote_ip = "203.0.113.10";
        request.headers = {{"cookie", browser_.cookie}, {"x-csrf-token", browser_.csrf}, {"if-match", tag}};
        request.body = boost::json::serialize(boost::json::object{{"name", name}});
        boost::asio::co_spawn(io_, router_->dispatch(std::move(request)), [&statuses](std::exception_ptr error, http::Response response) {
            EXPECT_FALSE(error);
            statuses.push_back(response.status);
        });
    }
    long seen = 0;
    boost::asio::co_spawn(io_, release_when_two_wait(lock, watcher, seen), boost::asio::detached);
    io_.run();
    io_.restart();
    EXPECT_EQ(seen, 2) << "both changes waited on the row";
    EXPECT_THAT(statuses, testing::UnorderedElementsAre(200u, 412u));
    EXPECT_EQ(count("audit_log WHERE action = 'client.update'"), 1);
    EXPECT_THAT(std::string(read(path).at("name").as_string()), testing::AnyOf("Willow Moon Head Spa", "Willow Moon Day Spa"));
}

// ---- locations -------------------------------------------------------------------

TEST_F(Api, LocationsAreGroupedUnderClientsAndTheLinkChangesNothingElse) {
    discover_spa_and_buffet();
    const auto client = json(post("/api/clients", R"({"name": "LU DREAM SPA"})")).at("id").as_int64();
    const auto other = json(post("/api/clients", R"({"name": "Willow Moon"})")).at("id").as_int64();
    const auto spa = location_id("9001");
    const auto target = "/api/locations/" + std::to_string(spa);

    EXPECT_EQ(read("/api/locations").at("total").as_int64(), 2);
    EXPECT_EQ(read("/api/locations?linked=false").at("total").as_int64(), 2);
    // A location the engine works on, with settings and auto reply of its own.
    ASSERT_EQ(patch_current(target, R"({"enabled": true, "industry_key": "wellness", "audience": "Office workers"})").status, 200u);
    ASSERT_EQ(save_settings(spa, "voice", R"({"reply_name": "Willow Moon Head Spa"})").status, 200u);
    const auto on = patch(target + "/auto-posting", R"({"enabled": true, "five_star": true, "time_zone": "America/New_York"})");
    ASSERT_EQ(on.status, 200u) << on.body;
    // All of it but the client, as it is before and after each change of the link.
    const auto auto_reply_state = [&] {
        return value<std::string>(std::format("SELECT concat(auto_reply_enabled, auto_reply_five_star, auto_reply_four_star, time_zone) "
                                              "FROM locations WHERE id = {}",
                                              spa));
    };
    const auto all_but_the_client = [&] {
        auto location = read(target).as_object();
        location.erase("client_id");
        location.erase("client_name");
        return boost::json::object{{"location", location}, {"settings", read(settings_path(spa)).at("voice")}, {"auto_reply", auto_reply_state()}};
    };
    const auto before = all_but_the_client();
    const auto syncs = value<std::string>(std::format("SELECT concat(next_review_poll_at, next_full_sync_at) FROM locations WHERE id = {}", spa));

    const auto linked = patch_current(target, std::format(R"({{"client_id": {}}})", client));
    ASSERT_EQ(linked.status, 200u) << linked.body;
    EXPECT_EQ(json(linked).at("client_name").as_string(), "LU DREAM SPA");
    EXPECT_EQ(all_but_the_client(), before);
    EXPECT_EQ(read("/api/locations?linked=true").at("items").as_array().size(), 1u);
    EXPECT_EQ(read(std::format("/api/locations?client_id={}", client)).at("total").as_int64(), 1);
    EXPECT_EQ(read("/api/locations?q=gangnam").at("items").at(0).at("title").as_string(), "Gangnam AYCE Buffet");
    const auto grouped = read(std::format("/api/clients/{}", client));
    EXPECT_EQ(grouped.at("location_count").as_int64(), 1);
    EXPECT_EQ(boost::json::serialize(grouped.at("location_ids")), std::format("[{}]", spa));
    const auto entry = read("/api/audit-log?action=location.link").at("items").at(0);
    EXPECT_TRUE(entry.at("before").at("client_id").is_null());
    EXPECT_EQ(entry.at("after").at("client_name").as_string(), "LU DREAM SPA");
    EXPECT_FALSE(entry.at("after").as_object().contains("reply_settings_from")) << "nothing is copied";

    // A move to another client, archiving the client, and unlinking: the grouping alone changes (D-102).
    ASSERT_EQ(patch_current(target, std::format(R"({{"client_id": {}}})", other)).status, 200u);
    EXPECT_EQ(all_but_the_client(), before);
    const auto other_path = std::format("/api/clients/{}", other);
    ASSERT_EQ(patch(other_path, R"({"archived": true})", if_match(etag(other_path, browser_))).status, 200u);
    EXPECT_EQ(all_but_the_client(), before);
    ASSERT_EQ(patch_current(target, R"({"client_id": null})").status, 200u);
    EXPECT_TRUE(read(target).at("client_id").is_null());
    EXPECT_EQ(all_but_the_client(), before);
    EXPECT_EQ(read("/api/audit-log?action=location.unlink").at("total").as_int64(), 1);
    EXPECT_EQ(value<std::string>(std::format("SELECT concat(next_review_poll_at, next_full_sync_at) FROM locations WHERE id = {}", spa)), syncs)
        << "a link starts no sync";
    EXPECT_EQ(count("go_live_runs"), 0);
}

// D-102 point 2: only a person switches a location on, and only one the managing account still sees.
TEST_F(Api, ALocationIsSwitchedOnAndOffByAPerson) {
    discover_spa_and_buffet();
    const auto spa = location_id("9001");
    const auto target = "/api/locations/" + std::to_string(spa);
    EXPECT_FALSE(read(target).at("enabled").as_bool()) << "off at discovery";
    EXPECT_EQ(read("/api/locations?enabled=false").at("total").as_int64(), 2);
    EXPECT_EQ(get("/api/locations?enabled=yes", browser_).status, 400u);

    // Switching on starts a full sync, and needs no client.
    const auto on = patch_current(target, R"({"enabled": true})");
    ASSERT_EQ(on.status, 200u) << on.body;
    EXPECT_TRUE(json(on).at("enabled").as_bool());
    EXPECT_TRUE(json(on).at("client_id").is_null());
    EXPECT_EQ(read("/api/locations?enabled=true").at("total").as_int64(), 1);
    EXPECT_EQ(read("/api/audit-log?action=location.enable").at("items").at(0).at("actor").as_string(), "staff@xvisionmediaus.com");
    jobs::Sync sync(db(), clock_, gbp_, queue(), {});
    await(sync.poll_reviews());
    EXPECT_EQ(count(std::format("jobs WHERE kind = 'reviews.full_sync' AND (payload->>'location_id')::bigint = {}", spa)), 1);
    ASSERT_EQ(patch_current(target, R"({"enabled": true})").status, 200u);
    EXPECT_EQ(read("/api/audit-log?action=location.enable").at("total").as_int64(), 1) << "no change, no entry";

    // Switching off switches auto reply off with it, and a person switches that on again (D-070).
    ASSERT_EQ(patch(target + "/auto-posting", R"({"enabled": true, "five_star": true, "time_zone": "America/New_York"})").status, 200u);
    const auto off = patch_current(target, R"({"enabled": false})");
    ASSERT_EQ(off.status, 200u) << off.body;
    EXPECT_FALSE(json(off).at("enabled").as_bool());
    EXPECT_FALSE(value<bool>(std::format("SELECT auto_reply_enabled OR auto_reply_five_star FROM locations WHERE id = {}", spa)));
    const auto entry = read("/api/audit-log?action=location.disable").at("items").at(0);
    EXPECT_TRUE(entry.at("before").at("auto_reply_enabled").as_bool());
    EXPECT_FALSE(entry.at("after").at("auto_reply_enabled").as_bool());
    EXPECT_EQ(patch(target + "/auto-posting", R"({"enabled": true, "five_star": true})").status, 400u)
        << "auto reply needs a location that is switched on";

    // A location the managing account no longer sees can't be switched on, and the refusal changes nothing else.
    exec(std::format("UPDATE locations SET access_status = 'lost' WHERE id = {}", spa));
    const auto lost = patch_current(target, R"({"enabled": true, "audience": "Families"})");
    EXPECT_EQ(lost.status, 409u);
    EXPECT_EQ(error_code(lost), "location_not_accessible");
    EXPECT_FALSE(read(target).at("enabled").as_bool());
    EXPECT_EQ(read(target).at("audience").as_string(), "");
    EXPECT_EQ(patch(target, R"({"enabled": null})").status, 400u);
}

TEST_F(Api, ARefusedLocationChangeChangesNothing) {
    discover_spa_and_buffet();
    const auto spa = std::to_string(location_id("9001"));
    const auto archived = json(post("/api/clients", R"({"name": "Old client"})")).at("id").as_int64();
    const auto archived_path = std::format("/api/clients/{}", archived);
    ASSERT_EQ(patch(archived_path, R"({"archived": true})", if_match(etag(archived_path, browser_))).status, 200u);
    const auto entries_before = count("audit_log");

    // The audience is fine but the client isn't: neither is applied.
    const auto refused = patch_current("/api/locations/" + spa, R"({"audience": "Spa guests", "client_id": 999})");
    EXPECT_EQ(refused.status, 409u);
    EXPECT_EQ(error_code(refused), "client_not_found");
    EXPECT_EQ(patch_current("/api/locations/" + spa, std::format(R"({{"client_id": {}}})", archived)).status, 409u);
    EXPECT_EQ(patch("/api/locations/" + spa, R"({"client_id": "one"})").status, 400u);
    EXPECT_EQ(patch_current("/api/locations/" + spa, R"({"enabled": true, "industry_key": "no_such_industry"})").status, 400u);
    EXPECT_EQ(patch_current("/api/locations/" + spa, R"({"enabled": true, "analyze_from": "2026-02-30"})").status, 400u);
    EXPECT_EQ(patch_current("/api/locations/" + spa, R"({"enabled": true, "audience": ")" + std::string(501, 'x') + R"("})").status, 400u);
    EXPECT_EQ(patch("/api/locations/12345", R"({"client_id": null})").status, 404u);

    EXPECT_EQ(read("/api/locations/" + spa).at("audience").as_string(), "");
    EXPECT_FALSE(read("/api/locations/" + spa).at("enabled").as_bool());
    EXPECT_EQ(count("audit_log"), entries_before);

    ASSERT_EQ(patch_current("/api/locations/" + spa, R"({"audience": "Spa guests"})").status, 200u);
    EXPECT_EQ(read("/api/audit-log?action=location.update").at("items").at(0).at("after").at("audience").as_string(), "Spa guests");
}

// D-108: a location's change needs the ETag of what the person read, and a stale one changes nothing.
TEST_F(Api, ALocationIsChangedOnlyAgainstItsCurrentEtag) {
    discover_spa_and_buffet();
    const auto target = "/api/locations/" + std::to_string(location_id("9001"));
    const auto read_tag = etag(target, browser_);

    const auto without = patch(target, R"({"audience": "Spa guests"})");
    EXPECT_EQ(without.status, 428u);
    EXPECT_EQ(error_code(without), "precondition_required");
    const auto changed = patch(target, R"({"audience": "Spa guests"})", if_match(read_tag));
    ASSERT_EQ(changed.status, 200u) << changed.body;
    const auto new_tag = header(changed, "ETag");
    EXPECT_NE(new_tag, read_tag);
    EXPECT_EQ(etag(target, browser_), new_tag) << "the change answers with the tag its GET then gives";

    const auto entries = count("audit_log");
    const auto stale = patch(target, R"({"posting_enabled": false})", if_match(read_tag));
    EXPECT_EQ(stale.status, 412u);
    EXPECT_EQ(error_code(stale), "precondition_failed");
    EXPECT_TRUE(read(target).at("posting_enabled").as_bool());
    EXPECT_EQ(patch(target, R"({"audience": "Spa guests"})", if_match(read_tag)).status, 412u) << "a stale equal save";
    EXPECT_EQ(count("audit_log"), entries);
    EXPECT_EQ(patch("/api/locations/12345", R"({"audience": "x"})", if_match(read_tag)).status, 404u);
}

// D-108: what the engine writes about a location by itself (a discovery, a review poll) leaves a person's view of it current.
TEST_F(Api, ALocationsEtagIsNotChangedByADiscoveryOrAReviewPoll) {
    discover_spa_and_buffet();
    const auto spa = location_id("9001");
    const auto target = "/api/locations/" + std::to_string(spa);
    ASSERT_EQ(patch_current(target, R"({"enabled": true, "audience": "Office workers"})").status, 200u);
    const auto read_tag = etag(target, browser_);
    const auto written = [&] { return value<std::string>(std::format("SELECT concat(title, updated_at) FROM locations WHERE id = {}", spa)); };
    const auto before = written();

    // Google renamed the location, and the discovery writes the row again.
    gbp_.locations[0].title = "Willow Moon Head Spa (Midtown)";
    clock_.advance(20min);
    discover();
    EXPECT_NE(written(), before) << "the discovery rewrote the row";
    EXPECT_EQ(etag(target, browser_), read_tag);

    // Its full sync, then a poll a while later: each writes the row, and neither changes the tag.
    gbp_.put(make_review(gbp_.account_id, "9001", "p1", "FIVE", "Lovely", "2026-11-09T10:00:00Z", "2026-11-09T10:00:00Z"));
    jobs::Sync sync(db(), clock_, gbp_, queue(), {});
    await(sync.full_sync(spa));
    const auto synced = read(target);
    EXPECT_FALSE(synced.at("reviews_full_synced_at").is_null()) << "the full sync wrote the row";
    EXPECT_EQ(synced.at("review_count").as_int64(), 1);
    EXPECT_EQ(etag(target, browser_), read_tag);
    clock_.advance(2h);
    gbp_.put(make_review(gbp_.account_id, "9001", "p2", "FOUR", "Nice", "2026-11-09T11:00:00Z", "2026-11-09T11:00:00Z"));
    await(sync.poll_reviews());
    EXPECT_NE(read(target).at("reviews_polled_at"), synced.at("reviews_polled_at")) << "the poll wrote the row";
    EXPECT_EQ(etag(target, browser_), read_tag);
    EXPECT_EQ(patch(target, R"({"audience": "Families"})", if_match(read_tag)).status, 200u);
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
    patch_current(std::format("/api/locations/{}", location_id("9001")), std::format(R"({{"client_id": {}}})", spa_client));
    patch_current(std::format("/api/locations/{}", location_id("9002")), std::format(R"({{"client_id": {}}})", buffet_client));

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
    EXPECT_EQ(review_ids("?has_reply=false&rating=5&location_id=" + std::to_string(location_id("9001"))), (Ids{"s2"}));

    for (const auto* bad : {"?rating=6", "?rating=x", "?status=gone", "?has_reply=1", "?sort=id", "?order=up", "?location_id=x",
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
    EXPECT_FALSE(status.at("workspace").at("connected").as_bool());
    EXPECT_TRUE(status.at("workspace").at("account_email").is_null());
    EXPECT_TRUE(status.at("database").at("reachable").as_bool());
    EXPECT_FALSE(status.at("pubsub").at("enabled").as_bool());
    EXPECT_FALSE(status.at("location_discovery").at("last_ok_at").is_null());
    EXPECT_EQ(status.at("locations").as_array().size(), 2u);
    EXPECT_FALSE(status.at("locations").at(0).at("syncing").as_bool()) << "not switched on yet";
    EXPECT_EQ(status.at("jobs").at("failed").as_int64(), 0);

    switch_location(location_id("9002"), true);
    {
        const auto box = core::SecretBox::from_base64_key(core::base64_encode(std::string(32, 'k'))).value();
        pqxx::connection conn{url()};
        pqxx::work tx{conn};
        repo::save_gbp_credentials(tx, box, {"id", "secret", "refresh"}, clock_.now());
        repo::mark_integration_error(tx, repo::kGbpTokenStatus, "HTTP 400 invalid_grant", clock_.now());
        repo::record_sync_error(tx, {location_id("9002")}, "HTTP 403 PERMISSION_DENIED", clock_.now(), 120s, false);
        tx.exec("INSERT INTO jobs (kind, status, run_at, attempts, last_error, created_at, updated_at, finished_at) "
                "VALUES ('reviews.full_sync', 'failed', $1::timestamptz, 5, 'boom', $1::timestamptz, $1::timestamptz, $1::timestamptz)",
                pqxx::params{db::to_timestamptz(clock_.now())});
        EXPECT_EQ(tx.query_value<std::string>("SELECT ciphertext FROM credentials").find("refresh"), std::string::npos);
        tx.commit();
    }

    status = read("/api/system/status");
    EXPECT_TRUE(status.at("gbp").at("connected").as_bool());
    EXPECT_TRUE(status.at("gbp").at("account_email").is_null()) << "a token stored without its address: unknown until connected again";
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
