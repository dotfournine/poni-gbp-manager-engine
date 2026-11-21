#include "db/repo_google_target.hpp"

#include "db/audit.hpp"

#include <boost/json/object.hpp>

namespace poni::repo {

std::optional<std::string> claim_google_target(pqxx::transaction_base& tx, bool mock, core::TimePoint now) {
    const std::string wanted = mock ? "mock" : "google";
    const auto row = tx.exec("SELECT google_target FROM system_settings FOR UPDATE").one_row();
    if (!row[0].is_null()) {
        const auto target = row[0].as<std::string>();
        if (target == wanted) return std::nullopt;
        if (target == "mock") {
            return "this database is a mock database (made-up locations and reviews): start the engine against it only with "
                   "PONI_GOOGLE_MOCK_URL, or use another database for Google";
        }
        return "this database belongs to Google Business Profile (real locations and reviews): PONI_GOOGLE_MOCK_URL must not "
               "be set for it; give the mock a database of its own";
    }
    if (mock) {
        const bool has_data = tx.exec("SELECT EXISTS (SELECT 1 FROM locations) OR EXISTS (SELECT 1 FROM reviews) "
                                      "OR EXISTS (SELECT 1 FROM credentials)")
                                  .one_field()
                                  .as<bool>();
        if (has_data) {
            return "this database already holds locations, reviews or a token, so it came from Google: it can't become a mock "
                   "database; give the mock a new database";
        }
    }
    tx.exec("UPDATE system_settings SET google_target = $1", pqxx::params{wanted});
    audit::record(tx, audit::Actor::system("start"),
                  {.action = "system.google_target",
                   .object_type = "system_settings",
                   .object_id = "google_target",
                   .after = boost::json::object{{"google_target", wanted}}},
                  now);
    return std::nullopt;
}

}  // namespace poni::repo
