// A migrated and seeded database, a clock the test controls, the job
// queue, and the world most tests start from: a fake Google with the spa
// and the buffet, their clients, and a staff member.
#pragma once

#include "core/clock.hpp"
#include "db/audit.hpp"
#include "db/database.hpp"
#include "db/repo_catalog.hpp"
#include "db/repo_go_live.hpp"
#include "db/repo_knowledge.hpp"
#include "db/repo_reviews.hpp"
#include "db/repo_settings.hpp"
#include "domain/reply_settings.hpp"
#include "ai/ai_fixture.hpp"
#include "db_fixture.hpp"
#include "fakes.hpp"
#include "jobs/queue.hpp"
#include "jobs/sync.hpp"
#include "jobs/worker.hpp"
#include "support.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include <optional>
#include <string>

namespace poni::test {

inline constexpr std::string_view kSpa = "9001";
inline constexpr std::string_view kBuffet = "9002";

class EngineTest : public DbTest {
protected:
    void SetUp() override {
        DbTest::SetUp();
        if (HasFatalFailure()) return;
        database_.emplace(url(), 4);
        queue_.emplace(*database_, clock_);
    }

    void TearDown() override {
        queue_.reset();
        database_.reset();  // closes the pool's connections before the database is dropped
        DbTest::TearDown();
    }

    db::Database& db() { return *database_; }
    jobs::JobQueue& queue() { return *queue_; }

    template <class T>
    T await(boost::asio::awaitable<T> task) {
        return test::run(io_, std::move(task));
    }

    // One value from the database.
    template <class T>
    T value(const std::string& sql) {
        pqxx::work tx{conn()};
        return tx.query_value<T>(sql);
    }

    long count(const std::string& from_where) { return value<long>("SELECT count(*) FROM " + from_where); }

    void exec(const std::string& sql) {
        tx([&](pqxx::work& tx) { tx.exec(sql); });
    }

    // Runs f(tx, audit) through audit::transact on conn(), recorded as `actor`.
    template <class F>
    auto transact(audit::Actor actor, F&& f) {
        return audit::transact(conn(), std::move(actor), clock_.now(), std::forward<F>(f));
    }
    // ... as the tests' own setup.
    template <class F>
    auto as_test(F&& f) {
        return transact(audit::Actor::system("test"), std::forward<F>(f));
    }
    // ... as the staff member (user_), signed in from the tests' address.
    template <class F>
    auto as_staff(F&& f) {
        return transact(audit::Actor::user(user_, "staff@xvisionmediaus.com", "203.0.113.10"), std::forward<F>(f));
    }
    // ... as the staff member, with no address recorded.
    template <class F>
    auto as_staff_without_ip(F&& f) {
        return transact(audit::Actor::user(user_, "staff@xvisionmediaus.com", ""), std::forward<F>(f));
    }

    std::int64_t make_client(const std::string& name) {
        return as_test([&](pqxx::work& tx, audit::Writer& audit) { return repo::create_client(tx, audit, {.name = name}).value().id; });
    }

    // Makes the client the made-up business of an industry's pack in the
    // knowledge base (knowledge/industries/<key>/test_client.json), the way
    // a person would in the dashboard: give it the industry, then save the
    // business's settings (the defaults, the voice template it names, what
    // it sets itself) for each of its locations. A location that is linked
    // afterwards copies them from the first (D-093).
    void become_test_client(std::int64_t client_id, const std::string& industry_key) {
        as_test([&](pqxx::work& tx, audit::Writer& audit) {
            const auto business = boost::json::parse(tx.query_value<std::string>(
                                                         "SELECT test_client::text FROM kb_industries WHERE key = $1", pqxx::params{industry_key}))
                                      .as_object();
            (void)repo::update_client(tx, audit, client_id, {.industry_key = industry_key}).value();
            const auto defaults = repo::knowledge_defaults(tx);
            std::optional<model::KnowledgeVoiceTemplate> taken;
            if (const auto* chosen = business.if_contains("template")) {
                taken = db::row_as<model::KnowledgeVoiceTemplate>(
                    tx, "SELECT key, label, description, voice FROM kb_voice_templates WHERE key = $1",
                    pqxx::params{std::string(chosen->as_string())});
            }
            const auto settings =
                business_settings({{"voice", defaults.voice}, {"localization", defaults.localization}, {"policy", defaults.policy}}, taken, business);
            for (const auto& row : tx.exec("SELECT id FROM locations WHERE client_id = $1 ORDER BY id", pqxx::params{client_id})) {
                (void)repo::replace_reply_settings(tx, audit, row[0].as<std::int64_t>(), settings).value();
            }
            return 0;
        });
    }

    // ---- a location's reply settings, changed without a person's save ----

    // One part of the location's stored settings ("voice", "localization" or "policy"), as JSON text.
    std::string settings_part(std::int64_t location, std::string_view part) {
        return value<std::string>(std::format("SELECT {}::text FROM reply_settings WHERE location_id = {}", part, location));
    }
    // Lays `members` on that part: objects merge member by member, everything else is replaced.
    void set_part(std::int64_t location, std::string_view part, std::string_view members) {
        const auto merged =
            merge_layer(boost::json::parse(settings_part(location, part)).as_object(), boost::json::parse(members).as_object());
        tx([&](pqxx::work& tx) {
            tx.exec(std::format("UPDATE reply_settings SET {} = $2::jsonb WHERE location_id = $1", part),
                    pqxx::params{location, boost::json::serialize(merged)})
                .no_rows();
        });
    }
    void set_voice(std::int64_t location, std::string_view members) { set_part(location, "voice", members); }
    // The policy's rules (delay, window, what is routed to auto-post).
    void set_policy(std::int64_t location, std::string_view rules) { set_part(location, "policy", rules); }

    // What a seed of a new knowledge base version does to the tables, in this
    // test's own copy of the database: the change, and a new version. The
    // engine's tests cannot run poni-tools; that the real seed replaces the
    // tables like this is tested there (tools/tests/test_seed.py).
    void reseed(const std::string& change) {
        static int version = 0;
        exec(change);
        exec(std::format("UPDATE kb_meta SET version = 'test-seed-{}', seeded_at = now()", ++version));
    }

    // Links the location with this Google id to the client and returns the location's id.
    std::int64_t link(std::string_view gbp_location_id, std::int64_t client_id) {
        const auto id = location_id(gbp_location_id);
        as_test([&](pqxx::work& tx, audit::Writer& audit) {
            return repo::update_location(tx, audit, id, {.client_id = client_id}).has_value();
        });
        return id;
    }

    std::int64_t location_id(std::string_view gbp_location_id) {
        return value<std::int64_t>(std::format("SELECT id FROM locations WHERE gbp_location_id = '{}'", gbp_location_id));
    }

    // ---- the world ----

    // The spa (9001) and the buffet (9002) on the fake Google.
    void add_spa_and_buffet(std::string spa_title = "LU DREAM SPA - Chelsea") {
        gbp_.add_location(std::string(kSpa), std::move(spa_title));
        gbp_.add_location(std::string(kBuffet), "Gangnam AYCE Buffet");
    }
    // Finds the fake Google's locations, as the sync's discovery does.
    void discover() {
        jobs::Sync discovery(db(), clock_, gbp_, queue(), {});
        await(discovery.discover_locations());
    }
    // Each of the two gets a client of its own: spa_client_ and buffet_client_.
    void link_spa_and_buffet() {
        spa_client_ = make_client("LU DREAM SPA");
        buffet_client_ = make_client("Gangnam AYCE Buffet");
        spa_ = link(kSpa, spa_client_);
        buffet_ = link(kBuffet, buffet_client_);
    }
    // The two locations, discovered.
    void discover_spa_and_buffet() {
        add_spa_and_buffet();
        discover();
    }
    // ... and linked.
    void spa_and_buffet() {
        discover_spa_and_buffet();
        link_spa_and_buffet();
    }
    // The two clients are the made-up businesses of the knowledge base's packs.
    void become_test_clients() {
        become_test_client(spa_client_, "wellness");
        become_test_client(buffet_client_, "food_beverage");
    }
    // The staff member, as a row in users (user_).
    void add_staff() {
        exec("INSERT INTO users (google_sub, email, name, created_at, last_login_at) "
             "VALUES ('sub-1', 'staff@xvisionmediaus.com', 'Staff Member', now(), now())");
        user_ = value<std::int64_t>("SELECT id FROM users WHERE google_sub = 'sub-1'");
    }

    // A session of the phone app for `email` (its user made once per address),
    // named `token_hash`, as a sign-in of the app leaves one (D-095).
    void add_app_session(const std::string& email, const std::string& token_hash) {
        const auto now = db::to_timestamptz(clock_.now());
        tx([&](pqxx::work& tx) {
            tx.exec("INSERT INTO users (google_sub, email, name, created_at, last_login_at) VALUES ('sub-' || $1, $1, 'Staff', "
                    "$2::timestamptz, $2::timestamptz) ON CONFLICT (google_sub) DO NOTHING",
                    pqxx::params{email, now});
            tx.exec("INSERT INTO sessions (token_hash, user_id, csrf_token, created_at, last_seen_at, expires_at, kind) "
                    "SELECT $1, id, NULL, $2::timestamptz, $2::timestamptz, $2::timestamptz + interval '90 days', 'app' "
                    "FROM users WHERE google_sub = 'sub-' || $3",
                    pqxx::params{token_hash, now, email});
        });
    }

    // Stores a review in the mirror as the sync would. Returns its id.
    std::int64_t store_review(std::int64_t location, const gbp::Review& review, std::string_view source = repo::kSourcePoll) {
        return tx([&](pqxx::work& tx) {
            EXPECT_TRUE(repo::apply_review(tx, location, review, source, clock_.now()));
            return tx.query_value<std::int64_t>("SELECT id FROM reviews WHERE gbp_review_id = $1", pqxx::params{review.reviewId});
        });
    }

    // Google's id of a review in the mirror.
    std::string gbp_review_id(std::int64_t review_id) {
        return value<std::string>("SELECT gbp_review_id FROM reviews WHERE id = " + std::to_string(review_id));
    }

    // How many system problems were reported to people.
    long problems() { return count("notification_deliveries WHERE event_type = 'system_problem'"); }

    // Runs an INSERT ... RETURNING id and keeps the row.
    std::int64_t stored(const std::string& sql) {
        return tx([&](pqxx::work& tx) { return tx.query_value<std::int64_t>(sql); });
    }

    // The one raw INSERT INTO local_posts: a row written straight into the
    // table at the clock's now. `columns` and `values` add to the required ones.
    std::int64_t insert_local_post(std::int64_t location, const std::string& columns, const std::string& values) {
        return stored(std::format("INSERT INTO local_posts (location_id, created_at, updated_at, {}) VALUES ({}, '{}', '{}', {}) RETURNING id",
                                  columns, location, db::to_timestamptz(clock_.now()), db::to_timestamptz(clock_.now()), values));
    }
    // An upload of the staff member's (a local_post_media row), made `age` before the clock's now.
    struct Upload {
        std::string object = "p/abc123.jpg";
        std::string format = "photo";
        std::string content_type = "image/jpeg";
        long bytes = 204800;
        int width = 1200, height = 900;
        std::string duration_ms = "NULL";
        std::string age = "0 days";
    };
    std::int64_t insert_upload(const Upload& upload) {
        return stored(std::format("INSERT INTO local_post_media (object_name, format, content_type, bytes, width, height, duration_ms, "
                                  "created_by, created_at) VALUES ('{}', '{}', '{}', {}, {}, {}, {}, {}, '{}'::timestamptz - interval '{}') "
                                  "RETURNING id",
                                  upload.object, upload.format, upload.content_type, upload.bytes, upload.width, upload.height,
                                  upload.duration_ms, user_, db::to_timestamptz(clock_.now()), upload.age));
    }

    // Runs queued jobs until none is due.
    void run_jobs(const jobs::Registry& registry) {
        jobs::Worker worker(io_.get_executor(), queue(), registry, {.id = "test-worker"});
        while (await(worker.run_once())) {
        }
    }
    void run_jobs() { run_jobs(registry_); }

    // The location's configuration, as it is now, gets a passing go-live test
    // (D-074), written straight into this test's throwaway schema: what a
    // person's test run leaves behind when the model behaves. Tests about
    // auto-posting start from here; go_live_test.cpp earns the pass.
    void certify(std::int64_t location) {
        tx([&](pqxx::work& tx) {
            const auto setup = repo::go_live_setup(tx, location).value();
            tx.exec("INSERT INTO go_live_runs (client_id, fingerprint, fingerprint_inputs, location_ids, status, auto_post_positive, "
                    "auto_post_rating_only, auto_post_min_stars, corpus_version, repeats, started_at, finished_at) "
                    "VALUES ($1, $2, '{}', '[]', 'passed', true, true, 4, 'test', 3, now(), now())",
                    pqxx::params{setup.client_id, setup.fingerprint.hash});
        });
    }

    boost::asio::io_context io_;
    core::ManualClock clock_{t0()};
    FakeGbp gbp_;
    jobs::Registry registry_;
    std::int64_t spa_client_ = 0, buffet_client_ = 0, spa_ = 0, buffet_ = 0, user_ = 0;

private:
    std::optional<db::Database> database_;
    std::optional<jobs::JobQueue> queue_;
};

}  // namespace poni::test
