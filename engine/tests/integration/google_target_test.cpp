// Which Google a database belongs to (D-087): the engine marks a new
// database with its mode when it starts, and refuses to start when the mark
// and PONI_GOOGLE_MOCK_URL disagree, in either direction.

#include "core/clock.hpp"
#include "db/repo_google_target.hpp"
#include "db_fixture.hpp"
#include "support.hpp"

#include <gmock/gmock.h>

using namespace poni;
using testing::HasSubstr;

namespace {

class GoogleTarget : public test::DbTest {
protected:
    // What the start check says, committed as the engine commits it.
    std::optional<std::string> start(bool mock) {
        pqxx::work tx{conn()};
        auto problem = repo::claim_google_target(tx, mock, test::t0());
        if (!problem) tx.commit();
        return problem;
    }

    std::optional<std::string> mark() {
        pqxx::work tx{conn()};
        const auto value = tx.exec("SELECT google_target FROM system_settings").one_field();
        return value.is_null() ? std::nullopt : std::optional(value.as<std::string>());
    }

    void exec(const std::string& sql) {
        pqxx::work tx{conn()};
        tx.exec(sql).no_rows();
        tx.commit();
    }
};

}  // namespace

TEST_F(GoogleTarget, ANewDatabaseTakesTheModeOfItsFirstStartAndKeepsIt) {
    EXPECT_EQ(mark(), std::nullopt);
    EXPECT_EQ(start(true), std::nullopt);
    EXPECT_EQ(mark(), "mock");
    EXPECT_EQ(start(true), std::nullopt) << "the same mode starts again";

    pqxx::work tx{conn()};
    EXPECT_EQ(tx.exec("SELECT count(*) FROM audit_log WHERE action = 'system.google_target' AND actor = 'system:start'")
                  .one_field()
                  .as<int>(),
              1)
        << "marked once, with an audit entry";
}

TEST_F(GoogleTarget, AMockDatabaseNeverStartsAgainstGoogle) {
    ASSERT_EQ(start(true), std::nullopt);
    const auto problem = start(false);
    ASSERT_TRUE(problem);
    EXPECT_THAT(*problem, HasSubstr("mock database"));
    EXPECT_EQ(mark(), "mock");
}

TEST_F(GoogleTarget, AGoogleDatabaseNeverStartsAgainstTheMock) {
    ASSERT_EQ(start(false), std::nullopt);
    EXPECT_EQ(mark(), "google");
    const auto problem = start(true);
    ASSERT_TRUE(problem);
    EXPECT_THAT(*problem, HasSubstr("belongs to Google Business Profile"));
    EXPECT_EQ(mark(), "google");
}

TEST_F(GoogleTarget, AnUnmarkedDatabaseWithDataFromGoogleCannotBecomeAMockDatabase) {
    // As if the engine had run before the mark existed; the migration marks such a database, this is the second guard.
    exec("INSERT INTO credentials (name, ciphertext, key_id, created_at, updated_at) "
         "VALUES ('gbp_oauth', 'sealed', 'key', now(), now())");
    const auto problem = start(true);
    ASSERT_TRUE(problem);
    EXPECT_THAT(*problem, HasSubstr("came from Google"));
    EXPECT_EQ(mark(), std::nullopt);
    EXPECT_EQ(start(false), std::nullopt) << "it starts against Google, and is marked so";
    EXPECT_EQ(mark(), "google");
}

TEST_F(GoogleTarget, OnlyTheTwoMarksExist) {
    EXPECT_THROW(exec("UPDATE system_settings SET google_target = 'staging'"), pqxx::check_violation);
}
