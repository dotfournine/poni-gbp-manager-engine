#include "db/database.hpp"

#include <chrono>
#include <format>

namespace poni::db {

std::string to_timestamptz(core::TimePoint t) {
    return std::format("{:%F %T}+00", t);
}

Database::Database(std::string conninfo, std::size_t pool_size)
    : conninfo_(std::move(conninfo)), pool_(pool_size) {}

Database::~Database() { join(); }

void Database::join() { pool_.join(); }

std::unique_ptr<pqxx::connection> Database::acquire() {
    {
        std::scoped_lock lock(mutex_);
        if (!idle_.empty()) {
            auto conn = std::move(idle_.back());
            idle_.pop_back();
            return conn;
        }
    }
    return std::make_unique<pqxx::connection>(conninfo_);
}

void Database::release(std::unique_ptr<pqxx::connection> conn) noexcept {
    if (!conn || !conn->is_open()) return;
    try {
        std::scoped_lock lock(mutex_);
        idle_.push_back(std::move(conn));
    } catch (...) {
        // Out of memory while returning the connection: let it close.
    }
}

}  // namespace poni::db
