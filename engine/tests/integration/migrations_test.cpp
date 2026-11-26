// The engine's check at start (D-079): it applies no migration, it only
// refuses to serve a database that is behind the files, ahead of them, or
// migrated from other files. Applying is poni-tools' business and is tested
// there (tools/tests/test_migrate.py); the database these tests start from
// was migrated by it.

#include "db/migrations.hpp"
#include "db/repo_knowledge.hpp"

#include "db_fixture.hpp"

#include <gmock/gmock.h>
#include <pqxx/pqxx>

#include <unistd.h>

#include <filesystem>
#include <fstream>

using namespace poni;
using ::testing::HasSubstr;
using ::testing::Optional;

namespace {

class Migrations : public test::DbTest {
protected:
    void SetUp() override {
        test::DbTest::SetUp();
        files_ = std::filesystem::temp_directory_path() /
                 std::format("poni-migrations-test-{}-{}", ::getpid(),
                             ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(files_);
        std::filesystem::copy(PONI_DEFAULT_MIGRATIONS_DIR, files_);
    }
    void TearDown() override {
        std::filesystem::remove_all(files_);
        test::DbTest::TearDown();
    }

    void write(const std::string& name, std::string_view text) { std::ofstream(files_ / name) << text; }

    std::optional<std::string> problem() { return problem_in(files_); }
    std::optional<std::string> problem_in(const std::filesystem::path& dir) {
        pqxx::connection conn{url()};
        return db::migrations_problem(conn, dir);
    }

    std::filesystem::path files_;  // a copy of engine/migrations/ that a test may change
};

}  // namespace

TEST_F(Migrations, TheDatabaseTheScriptsPreparedHasNone) {
    EXPECT_EQ(problem_in(PONI_DEFAULT_MIGRATIONS_DIR), std::nullopt);
}

TEST_F(Migrations, AFileTheDatabaseLacksIsPending) {
    // A number no shipped migration has, however many there are.
    write("9999_the_next_one.sql", "SELECT 1;");
    EXPECT_THAT(problem(), Optional(HasSubstr("1 migration(s) pending (9999_the_next_one); run `poni-tools migrate`")));
}

TEST_F(Migrations, ADatabaseNeverMigratedHasEverythingPendingAndIsNotChanged) {
    pqxx::connection conn{url()};
    {
        // The table the check reads does not exist here.
        pqxx::work tx{conn};
        tx.exec("CREATE SCHEMA never_migrated").no_rows();
        tx.exec("SET search_path TO never_migrated").no_rows();
        tx.commit();
    }
    // Every file is pending, from the first.
    EXPECT_THAT(db::migrations_problem(conn, files_), Optional(HasSubstr("pending (0001_baseline")));
    pqxx::work tx{conn};
    EXPECT_EQ(tx.query_value<std::string>("SELECT coalesce(to_regclass('never_migrated.schema_migrations')::text, 'none')"),
              "none");
}

TEST_F(Migrations, AnEditedFileIsRefused) {
    write("0001_baseline.sql", "-- edited after it was applied\n");
    EXPECT_THAT(problem(), Optional(HasSubstr("migration 0001_baseline is not the one the database recorded")));
}

TEST_F(Migrations, ADatabaseNewerThanTheFilesIsRefused) {
    std::filesystem::remove(files_ / "0001_baseline.sql");
    EXPECT_THAT(problem(), Optional(HasSubstr("the database has migration 0001, which the files don't have")));
}

TEST_F(Migrations, FilesAreNamedNnnnName) {
    for (const char* bad : {"1_short.sql", "0001-dash.sql", "abcd_x.sql", "0000_zero.sql", "0001_.sql"}) {
        write(bad, "SELECT 1;");
        EXPECT_THAT(problem(), Optional(HasSubstr(bad)));
        std::filesystem::remove(files_ / bad);
    }
    write("0001_again.sql", "SELECT 1;");
    EXPECT_THAT(problem(), Optional(HasSubstr("two migration files have number 0001")));
    std::filesystem::remove(files_ / "0001_again.sql");
    write("README.md", "not a migration");
    EXPECT_EQ(problem(), std::nullopt);
}

TEST_F(Migrations, AMissingDirectoryIsAProblem) {
    EXPECT_THAT(problem_in(files_ / "nope"), Optional(HasSubstr("migrations directory")));
}

// The start check refuses a knowledge base whose defaults a migration rebuilt and no seed has filled yet (D-110).
TEST_F(Migrations, AKnowledgeBaseWithoutItsDefaultsRowsIsNotSeeded) {
    const auto knowledge = [&] {
        pqxx::connection conn{url()};
        pqxx::work tx{conn};
        return repo::knowledge_problem(tx);
    };
    EXPECT_EQ(knowledge(), std::nullopt) << "the seeded database";
    // Emptied one after the other, the last checked first, so each problem names the table just emptied.
    for (const std::string table : {"kb_report_defaults", "kb_reply_defaults", "kb_content_defaults"}) {
        {
            pqxx::connection conn{url()};
            pqxx::work tx{conn};
            tx.exec("DELETE FROM " + table).no_rows();
            tx.commit();
        }
        EXPECT_THAT(knowledge(), Optional(HasSubstr(table + "; run `poni-tools seed`"))) << table;
    }
}
