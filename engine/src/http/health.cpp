#include "http/health.hpp"

#include "core/log.hpp"
#include "core/version.hpp"

#include <boost/json/value_from.hpp>
#include <pqxx/nontransaction>

#include <exception>
#include <optional>

namespace poni::http {

Handler make_health_handler(db::Database& db) {
    return [&db](const Request&) -> boost::asio::awaitable<Response> {
        std::optional<std::string> error;
        try {
            co_await db.run([](pqxx::connection& conn) {
                pqxx::nontransaction tx{conn};
                tx.exec("SELECT 1").one_row();
            });
        } catch (const std::exception& e) {
            error = e.what();
        }
        // The reason is logged, not sent: it can hold host names and user names.
        if (error) log::warn("health: database unreachable", {{"error", *error}});

        const Health health{
            .status = error ? "degraded" : "ok",
            .version = std::string(core::kVersion),
            .database = {.reachable = !error},
        };
        co_return Response::json(error ? 503 : 200, boost::json::value_from(health));
    };
}

}  // namespace poni::http
