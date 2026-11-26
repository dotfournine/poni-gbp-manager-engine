// Small helpers for writing SQL by hand: timestamps and lists as parameters,
// WHERE conditions, and rows read into Boost.Describe structs by column name
// (rows_as, row_as, one_as, page).
#pragma once

#include "core/json.hpp"
#include "domain/paged.hpp"

#include <boost/json/array.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value_from.hpp>
#include <pqxx/pqxx>

#include <algorithm>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace poni::db {

// SQL expression for a timestamptz column as RFC 3339 text in UTC with
// microseconds: the same form as domain::canonical_timestamp().
inline std::string iso(std::string_view column) {
    return std::format(R"(to_char({} AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.US"Z"'))", column);
}

// A list as one jsonb parameter. libpqxx expands a container into separate
// parameters, so lists travel as JSON:
//     ... WHERE id IN (SELECT jsonb_array_elements_text($1::jsonb)::bigint)
template <class Range>
std::string json_list(const Range& values) {
    boost::json::array out;
    for (const auto& value : values) out.emplace_back(value);
    return boost::json::serialize(out);
}

// A text[] column written from one json_list parameter, in the list's order, with $2 bound to json_list(values):
//     std::format("INSERT INTO t (words) VALUES ({})", text_array("$2"))
inline std::string text_array(std::string_view parameter) {
    return std::format("ARRAY(SELECT e FROM jsonb_array_elements_text({}::jsonb) WITH ORDINALITY AS t(e, ord) ORDER BY ord)", parameter);
}

// Collects WHERE conditions and their numbered parameters.
class Conditions {
public:
    // Adds a parameter and returns its placeholder ("$3").
    template <class T>
    std::string bind(T&& value) {
        params_.append(std::forward<T>(value));
        return std::format("${}", ++count_);
    }
    void add(std::string clause) { clauses_.push_back(std::move(clause)); }

    // "" or " WHERE a AND b".
    std::string where() const {
        std::string out;
        for (const auto& clause : clauses_) {
            out += out.empty() ? " WHERE " : " AND ";
            out += clause;
        }
        return out;
    }
    const pqxx::params& params() const { return params_; }

private:
    std::vector<std::string> clauses_;
    pqxx::params params_;
    int count_ = 0;
};

struct Page {
    int page = 1;       // 1-based
    int per_page = 25;  // at most kMaxPerPage

    static constexpr int kMaxPerPage = 100;
    std::int64_t limit() const { return std::clamp(per_page, 1, kMaxPerPage); }
    std::int64_t offset() const { return static_cast<std::int64_t>(std::max(page, 1) - 1) * limit(); }
};

// The members of a struct that no column carries, because the code fills them
// in after the read (what another table, a setting or a computation says):
//
//     db::rows_as<model::Escalation>(tx, select, params, db::FilledLater{"review"})
//
// Such a member starts as a default-constructed T has it. Every other member
// still needs its column. A name that is not a member of T, and a column of
// such a name in the SELECT, are mistakes and throw.
class FilledLater {
public:
    FilledLater() = default;
    FilledLater(std::initializer_list<std::string_view> members) : members_(members) {}
    const std::vector<std::string_view>& members() const { return members_; }

private:
    std::vector<std::string_view> members_;
};

namespace detail {

// What the members named in `later` hold before they are filled in, by name.
template <class T>
boost::json::object defaults_of(const FilledLater& later) {
    boost::json::object out;
    if (later.members().empty()) return out;
    const auto all = boost::json::value_from(T{}).as_object();
    for (const auto member : later.members()) {
        const auto* value = all.if_contains(member);
        if (!value) throw std::logic_error(std::format("'{}' is to be filled in later, but the struct has no such member", member));
        out.emplace(member, *value);
    }
    return out;
}

// One row, as PostgreSQL's to_jsonb wrote it, as a T. `later`: defaults_of the members no column carries.
template <class T>
T from_row(std::string_view json, const boost::json::object& later = {}) {
    // Precise: a float8 must come out as the same double the column holds.
    boost::json::parse_options options;
    options.numbers = boost::json::number_precision::precise;
    auto row = boost::json::parse(json, {}, options);
    for (const auto& member : later) {
        // A column the code would overwrite: one of the two is a mistake.
        if (!row.as_object().emplace(member.key(), member.value()).second) {
            throw std::logic_error(
                std::format("'{}' is to be filled in later, but the row has a column of that name", std::string_view(member.key())));
        }
    }
    auto parsed = core::from_json<T>(row);
    // A mistake in the SELECT, not in the data: the message names the member.
    if (!parsed) throw std::logic_error(std::format("a row does not fit its struct: {}", parsed.error()));
    return std::move(*parsed);
}

}  // namespace detail

// The rows of `select`, each as a T (a Boost.Describe struct), matched by
// column name instead of by position:
//
//     rows_as<model::Client>(tx, "SELECT c.id, c.name, " + iso("c.created_at") + " AS created_at FROM clients c WHERE c.id = $1",
//                            pqxx::params{id});
//
// Every member of T needs a column of its name, so anything that is not a
// plain column gets an alias, and a timestamp is written `iso(column) AS
// member`. A NULL becomes an empty std::optional, a jsonb column a
// boost::json::value, and a column T does not have is ignored. A member
// without a column is a mistake in the SELECT and throws, naming the member,
// unless `later` names it as one the caller fills in (FilledLater, above).
// An ORDER BY goes inside `select`: the rows come back in its order. So does
// a FOR UPDATE: the wrapper adds no condition and no limit of its own, so the
// rows it returns are the rows `select` locks (the claims rely on that).
template <class T>
std::vector<T> rows_as(pqxx::transaction_base& tx, std::string_view select, const pqxx::params& params = {}, const FilledLater& later = {}) {
    const auto defaults = detail::defaults_of<T>(later);
    std::vector<T> out;
    for (const auto& row : tx.exec(std::format("SELECT to_jsonb(q)::text FROM ({}) q", select), params)) {
        out.push_back(detail::from_row<T>(row[0].view(), defaults));
    }
    return out;
}

// The first row of `select` as a T, or nothing: for a read by id.
template <class T>
std::optional<T> row_as(pqxx::transaction_base& tx, std::string_view select, const pqxx::params& params = {}, const FilledLater& later = {}) {
    auto rows = rows_as<T>(tx, select, params, later);
    if (rows.empty()) return std::nullopt;
    return std::move(rows.front());
}

// The one row of `select` as a T: for a read that always finds exactly one
// (the row of system_settings, a SELECT of counts). Anything else throws and says so.
template <class T>
T one_as(pqxx::transaction_base& tx, std::string_view select, const pqxx::params& params = {}, const FilledLater& later = {}) {
    auto rows = rows_as<T>(tx, select, params, later);
    if (rows.size() != 1) throw std::runtime_error(std::format("a read that must find one row found {}", rows.size()));
    return std::move(rows.front());
}

// One page of a filtered list, with the number of rows the filter matches:
//
//     db::Conditions where;
//     if (filter.archived) where.add(std::format("c.archived = {}", where.bind(*filter.archived)));
//     return db::page<model::Client>(tx, client_select(), where, "lower(c.name), c.id", filter.page);
//
// `select` is the columns and the FROM, as for rows_as; `order` is the ORDER
// BY list. The total is counted over that same SELECT with the same
// conditions, so it cannot disagree with the rows. PostgreSQL drops the
// columns the count does not need as long as it can flatten `select` (plain
// columns, joins, correlated subqueries); one with DISTINCT, GROUP BY or a
// window function is computed in full to be counted, so count that yourself.
template <class T>
model::Paged<T> page(pqxx::transaction_base& tx, std::string_view select, Conditions where, std::string_view order, const Page& page,
                     const FilledLater& later = {}) {
    model::Paged<T> out;
    out.total = tx.query_value<std::int64_t>(std::format("SELECT count(*) FROM ({}{}) q", select, where.where()), where.params());
    const auto limit = where.bind(page.limit());
    const auto offset = where.bind(page.offset());
    out.items = rows_as<T>(tx, std::format("{}{} ORDER BY {} LIMIT {} OFFSET {}", select, where.where(), order, limit, offset), where.params(),
                           later);
    return out;
}

}  // namespace poni::db
