#include "db/migrations.hpp"

#include "core/crypto.hpp"

#include <pqxx/pqxx>

#include <charconv>
#include <format>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

namespace poni::db {

std::optional<std::string> migrations_problem(pqxx::connection& conn, const std::filesystem::path& dir) {
    struct File {
        std::string name;  // NNNN_name, without .sql
        std::string checksum;
    };
    std::map<int, File> files;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        const auto file = entry.path().filename().string();
        if (!entry.is_regular_file() || entry.path().extension() != ".sql") continue;
        int version = 0;
        if (file.size() < 10 || file[4] != '_' ||
            std::from_chars(file.data(), file.data() + 4, version).ptr != file.data() + 4 || version < 1) {
            return std::format("{}: migration files are named NNNN_name.sql, starting at 0001", file);
        }
        std::ifstream in(entry.path(), std::ios::binary);
        if (!in) return std::format("cannot read {}", entry.path().string());
        std::ostringstream bytes;
        bytes << in.rdbuf();
        if (!files.try_emplace(version, File{file.substr(0, file.size() - 4), core::sha256_hex(bytes.str())}).second) {
            return std::format("two migration files have number {:04}", version);
        }
    }
    if (ec) return std::format("migrations directory {}: {}", dir.string(), ec.message());

    pqxx::read_transaction tx{conn};
    std::map<int, std::string> recorded;
    if (tx.query_value<bool>("SELECT to_regclass('schema_migrations') IS NOT NULL")) {
        for (auto [version, checksum] : tx.query<int, std::string>("SELECT version, checksum FROM schema_migrations")) {
            recorded.emplace(version, std::move(checksum));
        }
    }

    std::vector<std::string> pending;
    for (const auto& [version, file] : files) {
        const auto it = recorded.find(version);
        if (it == recorded.end()) {
            pending.push_back(file.name);
        } else if (it->second != file.checksum) {
            return std::format(
                "migration {} is not the one the database recorded: it was changed after it was applied, or the "
                "database was migrated from other files (`poni-tools migrate --status` says more)",
                file.name);
        } else {
            recorded.erase(it);
        }
    }
    if (!recorded.empty()) {
        return std::format("the database has migration {:04}, which the files don't have; is this engine older than "
                           "the database?",
                           recorded.begin()->first);
    }
    if (!pending.empty()) {
        std::string names;
        for (const auto& name : pending) names += (names.empty() ? "" : ", ") + name;
        return std::format("{} migration(s) pending ({}); run `poni-tools migrate` first", pending.size(), names);
    }
    return std::nullopt;
}

}  // namespace poni::db
