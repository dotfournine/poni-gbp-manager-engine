// The engine's check at start that the database has exactly the migrations in
// engine/migrations/ (D-079). The files are NNNN_name.sql, starting with
// 0001_baseline.sql (D-091 point 10); `poni-tools migrate` applies them and
// records each in schema_migrations with the SHA-256 of its bytes. The engine
// applies none.
#pragma once

#include <pqxx/connection>

#include <filesystem>
#include <optional>
#include <string>

namespace poni::db {

// What keeps the engine from serving this database, in one sentence, or
// nothing: a file that can't be read or is badly named, a migration that is
// pending, one that is not what the database recorded, or one the database
// has and the files don't. Reads only; a database never migrated has
// everything pending.
std::optional<std::string> migrations_problem(pqxx::connection& conn, const std::filesystem::path& dir);

}  // namespace poni::db
