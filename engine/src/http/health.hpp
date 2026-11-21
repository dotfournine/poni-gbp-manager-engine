// GET /api/health: is the engine up, which version is it, can it reach the database.
#pragma once

#include "db/database.hpp"
#include "http/router.hpp"

#include <boost/describe/class.hpp>

#include <string>

namespace poni::http {

// Response body; must match the Health schema in api/openapi.yaml.
struct DatabaseHealth {
    bool reachable = false;
};
BOOST_DESCRIBE_STRUCT(DatabaseHealth, (), (reachable))

struct Health {
    std::string status;  // "ok" or "degraded"
    std::string version;
    DatabaseHealth database;
};
BOOST_DESCRIBE_STRUCT(Health, (), (status, version, database))

// 200 when everything works; 503 with the same body when the database is unreachable.
Handler make_health_handler(db::Database& db);

}  // namespace poni::http
