// PostgreSQL access (D-014). libpqxx is blocking, so every database call runs
// on a dedicated thread pool and the calling coroutine resumes on its own
// executor when the call returns. Nearly every call is one transaction:
//
//     auto n = co_await db.tx([](pqxx::work& tx) { return tx.query_value<long>("SELECT count(*) FROM jobs"); });
//
// and a change a person makes is one transaction that must leave an audit entry:
//
//     auto client = co_await db.audited(actor, now, [=](pqxx::work& tx, audit::Writer& audit) {
//         return repo::update_client(tx, audit, id, patch);
//     });
//
// run() is what both stand on, for the few calls that are not one committed
// transaction (no transaction at all, or one that is rolled back on purpose).
//
// The callable runs on a pool thread: it must not touch state owned by the
// io_context thread, and anything it captures must outlive the co_await.
// Exceptions thrown by the callable (pqxx::sql_error, pqxx::broken_connection,
// ...) are rethrown from the co_await.
#pragma once

#include "core/clock.hpp"
#include "db/audit.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <pqxx/connection>
#include <pqxx/transaction>

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace poni::db {

// A time point as text that PostgreSQL casts to timestamptz ($n::timestamptz).
// The engine passes its own clock's time instead of using SQL now().
std::string to_timestamptz(core::TimePoint t);

class Database {
public:
    // `pool_size` threads, each call using one connection; connections are
    // opened on first use and reused.
    Database(std::string conninfo, std::size_t pool_size);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    template <class F>
    auto run(F f) -> boost::asio::awaitable<std::invoke_result_t<F&, pqxx::connection&>> {
        using R = std::invoke_result_t<F&, pqxx::connection&>;
        co_return co_await boost::asio::co_spawn(
            pool_,
            [this, f = std::move(f)]() mutable -> boost::asio::awaitable<R> {
                Lease lease(*this);
                co_return f(lease.connection());
            },
            boost::asio::use_awaitable);
    }

    // `f(tx)` in one transaction, committed when `f` returns; `f` may return nothing.
    template <class F>
    auto tx(F f) {
        return run([f = std::move(f)](pqxx::connection& conn) mutable {
            pqxx::work work{conn};
            if constexpr (std::is_void_v<std::invoke_result_t<F&, pqxx::work&>>) {
                f(work);
                work.commit();
            } else {
                auto result = f(work);
                work.commit();
                return result;
            }
        });
    }

    // `f(tx, audit)` in one transaction that commits only with an audit entry
    // in `actor`'s name or when `f` declared that nothing changed; a refusal
    // (an expected with its error) is rolled back (audit::transact).
    template <class F>
    auto audited(audit::Actor actor, core::TimePoint now, F f) {
        return run([actor = std::move(actor), now, f = std::move(f)](pqxx::connection& conn) mutable {
            return audit::transact(conn, std::move(actor), now, f);
        });
    }

    // Waits for running calls to finish. Called by the destructor.
    void join();

private:
    // Borrows a connection for one call. A connection that is no longer open
    // when the call ends (the server went away) is dropped, not reused.
    class Lease {
    public:
        explicit Lease(Database& db) : db_(db), conn_(db.acquire()) {}
        ~Lease() { db_.release(std::move(conn_)); }
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        pqxx::connection& connection() { return *conn_; }

    private:
        Database& db_;
        std::unique_ptr<pqxx::connection> conn_;
    };

    std::unique_ptr<pqxx::connection> acquire();
    void release(std::unique_ptr<pqxx::connection> conn) noexcept;

    std::string conninfo_;
    std::mutex mutex_;
    std::vector<std::unique_ptr<pqxx::connection>> idle_;
    boost::asio::thread_pool pool_;  // last member: joined before the connections go away
};

}  // namespace poni::db
