// The audit log: who did what to which object, with the values before and
// after (architecture section 12.1, FR-11). Entries are written on the caller's
// transaction, so an entry exists exactly when its change was committed.
//
// Changes made through the API go through audit::transact(), which refuses to
// commit a change that recorded nothing. Callers reach it through
// db::Database::audited() (db/database.hpp):
//
//     co_await db.audited(actor, now, [=](pqxx::work& tx, audit::Writer& audit) {
//         ... change rows ...
//         audit.record("client.update", "client", id, {.before = before, .after = after});
//         return result;
//     });
#pragma once

#include "core/clock.hpp"

#include <boost/json/value.hpp>
#include <pqxx/pqxx>

#include <cstdint>
#include <expected>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace poni::audit {

struct Actor {
    std::string type;  // "user", "system" or "anonymous"
    std::optional<std::int64_t> user_id;
    std::string label;  // the user's email, or "system:<component>"
    std::optional<std::string> ip;

    static Actor user(std::int64_t id, std::string email, std::string ip) {
        return {"user", id, std::move(email), ip.empty() ? std::nullopt : std::optional(std::move(ip))};
    }
    // The engine acting on its own; `component` names the part and the rule.
    static Actor system(std::string component) { return {"system", std::nullopt, "system:" + std::move(component), std::nullopt}; }
    static Actor anonymous(std::string label, std::string ip) {
        return {"anonymous", std::nullopt, std::move(label), ip.empty() ? std::nullopt : std::optional(std::move(ip))};
    }
};

struct Entry {
    std::string action;  // "<object>.<verb>", e.g. "location.link"
    std::string object_type;
    std::string object_id;
    boost::json::value before = nullptr;
    boost::json::value after = nullptr;
};

// The values before and after, for the short form of Writer::record.
struct Change {
    boost::json::value before = nullptr;
    boost::json::value after = nullptr;
};

// Inserts one entry on `tx`.
void record(pqxx::transaction_base& tx, const Actor& actor, const Entry& entry, core::TimePoint now);

class Writer {
public:
    Writer(pqxx::transaction_base& tx, Actor actor, core::TimePoint now) : tx_(tx), actor_(std::move(actor)), now_(now) {}

    void record(const Entry& entry) {
        audit::record(tx_, actor_, entry, now_);
        ++count_;
    }
    // The usual entry: about the row `id` of one kind of object.
    void record(std::string action, std::string object_type, std::int64_t id, Change change = {}) {
        record({.action = std::move(action),
                .object_type = std::move(object_type),
                .object_id = std::to_string(id),
                .before = std::move(change.before),
                .after = std::move(change.after)});
    }
    // The request turned out to change nothing (not found, same values), so no entry is due.
    void nothing_changed() { nothing_changed_ = true; }
    bool satisfied() const { return count_ > 0 || nothing_changed_; }
    core::TimePoint now() const { return now_; }
    // The signed-in person the change is made for. Only for a change a person makes: throws for any other actor.
    std::int64_t user_id() const { return actor_.user_id.value(); }
    // The person, or nothing for a change the engine makes (a schedule): where a change can come from either.
    std::optional<std::int64_t> person_id() const { return actor_.user_id; }

private:
    pqxx::transaction_base& tx_;
    Actor actor_;
    core::TimePoint now_;
    int count_ = 0;
    bool nothing_changed_ = false;
};

namespace detail {

// A result that says by itself that the request was refused: an expected
// that holds its error (repo::Result<T> with a Failure). Not an empty
// optional: "nothing" has no fixed meaning, and some functions return it for success.
template <class T>
bool refused(const T&) {
    return false;
}
template <class T, class E>
bool refused(const std::expected<T, E>& result) {
    return !result;
}

}  // namespace detail

// Runs `f(tx, writer)` in one transaction; `f` may return nothing. The
// transaction commits only if `f` recorded an entry or declared that nothing
// changed. Without either, a refusal (an expected with its error) is returned
// and nothing is committed, so a refused request cannot leave an unrecorded
// change behind; anything else is a change without an audit entry, and throws.
template <class F>
auto transact(pqxx::connection& conn, Actor actor, core::TimePoint now, F&& f) {
    pqxx::work tx{conn};
    Writer writer{tx, std::move(actor), now};
    if constexpr (std::is_void_v<std::invoke_result_t<F&, pqxx::work&, Writer&>>) {
        f(tx, writer);
        if (!writer.satisfied()) throw std::logic_error("a change was made without an audit entry");
        tx.commit();
    } else {
        auto result = f(tx, writer);
        if (writer.satisfied()) {
            tx.commit();
        } else if (!detail::refused(result)) {
            throw std::logic_error("a change was made without an audit entry");
        }
        return result;  // refused, and nothing recorded: `tx` rolls back as it goes
    }
}

}  // namespace poni::audit
