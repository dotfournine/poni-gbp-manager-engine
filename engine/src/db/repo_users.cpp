#include "db/repo_users.hpp"

#include "db/sql.hpp"

#include <string>

namespace poni::repo {

model::UserList list_users(pqxx::transaction_base& tx, std::string_view status) {
    const std::string select = "SELECT id, email, name, picture_url, status FROM users";
    const std::string order = " ORDER BY lower(name), id";
    if (status == "all") return {.items = db::rows_as<model::User>(tx, select + order)};
    return {.items = db::rows_as<model::User>(tx, select + " WHERE status = $1" + order, pqxx::params{status})};
}

}  // namespace poni::repo
