// db/sql.hpp's reads by column name, against a real PostgreSQL: a row fits
// its struct by the names of its columns, a member without a column is a
// mistake that is named, and the members the code fills in afterwards are
// said so at the call (db::FilledLater).

#include "db/sql.hpp"

#include "db_fixture.hpp"

#include <boost/describe/class.hpp>
#include <gmock/gmock.h>
#include <pqxx/pqxx>

#include <exception>
#include <optional>
#include <string>
#include <vector>

using namespace poni;
using ::testing::ElementsAre;
using ::testing::HasSubstr;

namespace {

struct Part {
    std::int64_t id = 0;
    std::string name;
};
BOOST_DESCRIBE_STRUCT(Part, (), (id, name))

// `state` and `parts` are what a repository would add after the read.
struct Thing {
    std::int64_t id = 0;
    std::string name;
    std::optional<std::string> note;
    std::string state = "unknown";
    std::vector<Part> parts;
};
BOOST_DESCRIBE_STRUCT(Thing, (), (id, name, note, state, parts))

constexpr std::string_view kWhole = R"sql(SELECT 'new' AS state, 'First' AS name, 7 AS id, NULL AS note, 'ignored' AS extra,
                                                 '[{"id": 1, "name": "a"}]'::jsonb AS parts)sql";
constexpr std::string_view kRowOnly = "SELECT 7 AS id, 'First' AS name, 'a note' AS note";

class Sql : public test::DbTest {
protected:
    // What a read that must throw says.
    template <class F>
    std::string refusal(F&& read) {
        return tx([&](pqxx::work& tx) -> std::string {
            try {
                read(tx);
            } catch (const std::exception& e) {
                return e.what();
            }
            return "(nothing was thrown)";
        });
    }
};

}  // namespace

TEST_F(Sql, ARowFitsItsStructByTheNamesOfItsColumns) {
    const auto things = tx([](pqxx::work& tx) { return db::rows_as<Thing>(tx, kWhole); });
    ASSERT_EQ(things.size(), 1u);
    EXPECT_EQ(things[0].id, 7);
    EXPECT_EQ(things[0].name, "First");
    EXPECT_FALSE(things[0].note);
    EXPECT_EQ(things[0].state, "new");
    ASSERT_EQ(things[0].parts.size(), 1u);
    EXPECT_EQ(things[0].parts[0].name, "a");
    EXPECT_FALSE(tx([](pqxx::work& tx) { return db::row_as<Thing>(tx, std::string(kWhole) + " WHERE false"); }));
}

TEST_F(Sql, AMemberWithoutAColumnIsNamed) {
    EXPECT_THAT(refusal([](pqxx::work& tx) { db::rows_as<Thing>(tx, kRowOnly); }), HasSubstr("'state'"));
}

TEST_F(Sql, AMemberTheCodeFillsInLaterNeedsNoColumnAndStartsAsItsDefault) {
    const auto thing = tx([](pqxx::work& tx) { return db::row_as<Thing>(tx, kRowOnly, {}, db::FilledLater{"state", "parts"}); });
    ASSERT_TRUE(thing);
    EXPECT_EQ(thing->id, 7);
    EXPECT_EQ(thing->note, "a note");
    EXPECT_EQ(thing->state, "unknown") << "as a default-constructed Thing has it";
    EXPECT_TRUE(thing->parts.empty());
    // Every other member still needs its column.
    EXPECT_THAT(refusal([](pqxx::work& tx) { db::rows_as<Thing>(tx, "SELECT 7 AS id", {}, db::FilledLater{"state", "parts"}); }),
                HasSubstr("'name'"));
    EXPECT_THAT(refusal([](pqxx::work& tx) { db::rows_as<Thing>(tx, kRowOnly, {}, db::FilledLater{"state"}); }), HasSubstr("'parts'"));
}

TEST_F(Sql, AMemberToFillInLaterMustBeAMemberAndMustNotBeAColumn) {
    // A typo does not pass for "nothing to fill in".
    EXPECT_THAT(refusal([](pqxx::work& tx) { db::rows_as<Thing>(tx, kRowOnly, {}, db::FilledLater{"stat", "parts"}); }),
                HasSubstr("'stat' is to be filled in later, but the struct has no such member"));
    // Even when no row comes back: the mistake is in the call, not in the data.
    EXPECT_THAT(refusal([](pqxx::work& tx) { db::rows_as<Thing>(tx, std::string(kRowOnly) + " WHERE false", {}, db::FilledLater{"stat"}); }),
                HasSubstr("no such member"));
    // A column the code would overwrite.
    EXPECT_THAT(refusal([](pqxx::work& tx) { db::rows_as<Thing>(tx, kWhole, {}, db::FilledLater{"state"}); }),
                HasSubstr("'state' is to be filled in later, but the row has a column of that name"));
}

TEST_F(Sql, OneAsNeedsExactlyOneRow) {
    EXPECT_EQ(tx([](pqxx::work& tx) { return db::one_as<Thing>(tx, kWhole).name; }), "First");
    EXPECT_THAT(refusal([](pqxx::work& tx) { db::one_as<Thing>(tx, std::string(kWhole) + " WHERE false"); }), HasSubstr("found 0"));
    EXPECT_THAT(refusal([](pqxx::work& tx) { db::one_as<Thing>(tx, std::string(kWhole) + " UNION ALL " + std::string(kWhole)); }),
                HasSubstr("found 2"));
}

TEST_F(Sql, APageCountsEveryRowOfTheFilterAndFillsInLaterToo) {
    const auto page = tx([](pqxx::work& tx) {
        db::Conditions where;
        where.add(std::format("g > {}", where.bind(1)));
        return db::page<Thing>(tx, "SELECT g AS id, 'n' || g AS name, NULL AS note FROM generate_series(1, 6) g", where, "g DESC",
                               {.page = 2, .per_page = 2}, db::FilledLater{"state", "parts"});
    });
    EXPECT_EQ(page.total, 5);
    std::vector<std::int64_t> ids;
    for (const auto& thing : page.items) ids.push_back(thing.id);
    EXPECT_THAT(ids, ElementsAre(4, 3));
    EXPECT_EQ(page.items[0].name, "n4");
    EXPECT_EQ(page.items[0].state, "unknown");
}
