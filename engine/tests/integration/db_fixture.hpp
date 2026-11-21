// Gives each test its own database: a copy of the one this test run
// prepared with the real scripts (migrated and seeded by `poni-tools
// test-db`, D-079), which PostgreSQL makes in a few milliseconds. So tests
// start from exactly what production starts from, can run in parallel, and
// never touch the development database's data.
//
// The server comes from PONI_TEST_DATABASE_URL; the default is the
// development database of deploy/docker-compose.dev.yml. ctest prepares the
// database before the first integration test (tests/CMakeLists.txt); to run
// a test binary by hand, prepare it once from tools/:
//
//   uv run poni-tools test-db --name <the name in the failure message>
//
// A missing server or a missing prepared database fails the test:
// integration tests are never skipped silently.
#pragma once

#include <boost/url/url.hpp>
#include <gtest/gtest.h>
#include <pqxx/connection>
#include <pqxx/nontransaction>
#include <pqxx/transaction>

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

namespace poni::test {

inline std::string base_database_url() {
    const char* url = std::getenv("PONI_TEST_DATABASE_URL");
    return url && *url ? url : "postgresql://poni:poni@127.0.0.1:55432/poni";
}

// The database `poni-tools test-db` prepared for this build.
inline std::string prepared_database() {
    const char* name = std::getenv("PONI_TEST_TEMPLATE");
    return name && *name ? name : PONI_TEST_TEMPLATE;
}

class DbTest : public ::testing::Test {
protected:
    void SetUp() override {
        static std::atomic<int> counter{0};
        // The prefix is what `poni-tools test-db` clears away when a crashed run left copies behind.
        name_ = std::format("{}_c{}_{}", prepared_database(), ::getpid(), ++counter);
        pqxx::connection conn{base_database_url()};
        pqxx::nontransaction tx{conn};
        tx.exec(std::format("DROP DATABASE IF EXISTS {} WITH (FORCE)", name_)).no_rows();
        try {
            tx.exec(std::format("CREATE DATABASE {} TEMPLATE {}", name_, prepared_database())).no_rows();
        } catch (const std::exception& e) {
            FAIL() << "cannot copy the prepared test database " << prepared_database() << " (" << e.what()
                   << "). ctest prepares it; by hand, in tools/: uv run poni-tools test-db --name " << prepared_database();
        }
        boost::urls::url url(base_database_url());
        url.set_path("/" + name_);
        url_ = url.buffer();
    }

    void TearDown() override {
        conn_.reset();
        if (url_.empty()) return;
        pqxx::connection conn{base_database_url()};
        pqxx::nontransaction tx{conn};
        tx.exec(std::format("DROP DATABASE IF EXISTS {} WITH (FORCE)", name_)).no_rows();
    }

    // Every connection made with url() works inside this test's own database.
    const std::string& url() const { return url_; }

    // The test's own connection to its database, opened on first use. A test
    // that races two connections opens the others itself.
    pqxx::connection& conn() {
        // Without the test's own database (SetUp failed), an empty URL would reach libpq's default database.
        if (url_.empty()) throw std::logic_error("this test has no database of its own");
        if (!conn_) conn_.emplace(url_);
        return *conn_;
    }

    // Runs f(tx) in one transaction on conn() and commits it; returns what f returns.
    template <class F>
    auto tx(F&& f) {
        pqxx::work tx{conn()};
        if constexpr (std::is_void_v<std::invoke_result_t<F&, pqxx::work&>>) {
            f(tx);
            tx.commit();
        } else {
            auto result = f(tx);
            tx.commit();
            return result;
        }
    }

    // Two workers reach the same rows together: `first` runs in a transaction that is still open when
    // `second` starts in its own, on a connection and a thread of its own; the first commits 200 ms later,
    // while the second waits for its locks. Returns what each got.
    template <class First, class Second>
    auto race(First&& first, Second&& second) {
        pqxx::connection one{url_};
        pqxx::work one_tx{one};
        auto mine = first(one_tx);
        std::optional<std::invoke_result_t<Second&, pqxx::work&>> theirs;
        std::thread other([&] {
            pqxx::connection conn{url_};
            pqxx::work tx{conn};
            theirs.emplace(second(tx));
            tx.commit();
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        one_tx.commit();
        other.join();
        return std::pair{std::move(mine), std::move(*theirs)};
    }

private:
    std::optional<pqxx::connection> conn_;
    std::string name_;
    std::string url_;
};

}  // namespace poni::test
