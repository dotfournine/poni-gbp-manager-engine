// What the API handler files share: the Api context a handler talks to the
// database through, answers in the API's shapes, and reading path ids, paging
// and JSON bodies. Internal to src/http.
//
// A handler reads with api.query and changes with api.change, and answers
// with respond():
//
//     const auto id = path_id(request);
//     if (!id) co_return not_found("client");
//     co_return respond(co_await api.change(request, [id = *id, patch](pqxx::work& tx, audit::Writer& audit) {
//         return repo::update_client(tx, audit, id, patch);
//     }));
#pragma once

#include "core/body.hpp"
#include "core/clock.hpp"
#include "db/audit.hpp"
#include "db/database.hpp"
#include "db/failure.hpp"
#include "db/sql.hpp"
#include "http/api.hpp"
#include "http/router.hpp"

#include <boost/json/value_from.hpp>
#include <pqxx/pqxx>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace poni::http {

// The database, the clock and the options, as every handler gets them.
// Handlers hold a copy; `db` and `clock` outlive the router.
struct Api {
    db::Database& db;
    const core::Clock& clock;
    ApiOptions options;

    // The signed-in person, for the audit log. Access::user routes always have one.
    static audit::Actor actor(const Request& request) {
        const auto& principal = *request.principal;
        return audit::Actor::user(principal.user_id, principal.email, request.remote_ip);
    }

    // A read: `f(tx)` in one transaction.
    template <class F>
    auto query(F f) const {
        return db.tx(std::move(f));
    }

    // A change the signed-in person makes: `f(tx, audit)` in one transaction,
    // which commits only with an audit entry in that person's name; a result
    // that is a refusal commits nothing (audit::transact). The time of the
    // change is audit.now(), and the person is audit.user_id().
    template <class F>
    auto change(const Request& request, F f) const {
        return db.audited(actor(request), clock.now(), std::move(f));
    }
};

namespace api_support {

template <class T>
inline Response ok(const T& body, unsigned status = 200) {
    return Response::json(status, boost::json::value_from(body));
}

// What a route answers after it queued a job.
inline Response enqueued() { return Response::json(202, boost::json::object{{"enqueued", true}}); }

// The one place a repo::Failure becomes an answer.
inline Response respond(const repo::Failure& failure) {
    using Kind = repo::Failure::Kind;
    unsigned status = 400;
    switch (failure.kind) {
        case Kind::not_found: status = 404; break;
        case Kind::invalid: status = 400; break;
        case Kind::conflict: status = 409; break;
        case Kind::unavailable: status = 502; break;
    }
    return Response::error(status, failure.code, failure.message);
}

// The two answers a handler gives by itself, in the repositories' own words (db/failure.hpp).
inline Response invalid(std::string_view message) { return respond(repo::invalid(std::string(message)).error()); }
inline Response not_found(std::string_view what) { return respond(repo::not_found(what).error()); }
inline Response bad_query() { return invalid("A query parameter is not valid."); }

// What a function that may refuse came to: its value with `status`, or the failure's answer.
template <class T>
Response respond(const repo::Result<T>& result, unsigned status = 200) {
    if (!result) return respond(result.error());
    return ok(*result, status);
}

// The same for a change that has nothing to return: `done` if it was carried out.
inline Response respond(const repo::Result<void>& result, Response done) {
    if (!result) return respond(result.error());
    return done;
}

// What a lookup found, or "No such <what>.".
template <class T>
Response respond(const std::optional<T>& found, std::string_view what, unsigned status = 200) {
    if (!found) return not_found(what);
    return ok(*found, status);
}

// The "{id}" path parameter as a positive number.
inline std::optional<std::int64_t> path_id(const Request& request) {
    const auto it = request.params.find("id");
    if (it == request.params.end()) return std::nullopt;
    std::int64_t id = 0;
    const auto& text = it->second;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), id);
    if (ec != std::errc{} || end != text.data() + text.size() || id <= 0) return std::nullopt;
    return id;
}

// A GET of one thing by its "{id}": `find(tx, id)` gives it or nothing, and `what` names it in the 404.
template <class Find>
void add_get_by_id(Router& router, const Api& api, std::string path, std::string action, std::string what, Find find) {
    router.add(boost::beast::http::verb::get, std::move(path), {Access::user, std::move(action)},
               [api, what = std::move(what), find = std::move(find)](const Request& request) -> boost::asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found(what);
                   co_return respond(co_await api.query([id = *id, find](pqxx::work& tx) { return find(tx, id); }), what);
               });
}

// A query parameter that must be one of `allowed` (a list from a repository
// or domain header): `target` takes it if present, and `bad` is set if it is
// anything else. Checked here, before a value reaches a query.
template <class Allowed, class Target>
void read_choice(const QueryParams& query, std::string_view name, const Allowed& allowed, Target& target, bool& bad) {
    if (auto value = query.get(name)) {
        if (!std::ranges::contains(allowed, std::string_view(*value))) bad = true;
        target = std::move(*value);
    }
}

// The same for a list, written "a,b" (OpenAPI's form style, not exploded) or
// as the parameter repeated: `target` takes every value, and `bad` is set if
// one is not in `allowed`. An empty value is no value.
template <class Allowed>
void read_choices(const QueryParams& query, std::string_view name, const Allowed& allowed, std::vector<std::string>& target, bool& bad) {
    for (auto& value : query.all(name)) {
        if (!std::ranges::contains(allowed, std::string_view(value))) bad = true;
        target.push_back(std::move(value));
    }
}

// page and per_page; false (and `bad` filled in) if they are out of range.
inline bool read_page(const QueryParams& query, db::Page& page, bool& bad) {
    if (const auto n = query.integer("page", bad)) {
        if (*n < 1 || *n > 1'000'000) bad = true;
        page.page = static_cast<int>(*n);
    }
    if (const auto n = query.integer("per_page", bad)) {
        if (*n < 1 || *n > db::Page::kMaxPerPage) bad = true;
        page.per_page = static_cast<int>(*n);
    }
    return !bad;
}

// The body as T, a Boost.Describe struct that says what the route's schema says, read the one
// way every body is read (core/body.hpp, D-092); or the 400 that names the member.
template <class T>
std::expected<T, Response> body_as(const Request& request) {
    auto parsed = core::parse_body<T>(request.body);
    if (!parsed) return std::unexpected(invalid(parsed.error()));
    return std::move(*parsed);
}

// The same for a route whose body may be left out altogether (`required: false` in the spec): none is a T that says nothing.
template <class T>
std::expected<T, Response> optional_body_as(const Request& request) {
    static_assert(core::nothing_required<T>(), "a body that may be left out has no member that must be there");
    if (request.body.empty()) return T{};
    return body_as<T>(request);
}

}  // namespace api_support

}  // namespace poni::http
