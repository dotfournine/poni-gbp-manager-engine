// The users as the API lists them: every one with its status, chosen by
// status. Sign-in itself (creating a user, sessions, the app's codes) is in
// repo_auth. Blocking calls on a transaction the caller owns.
#pragma once

#include "domain/models.hpp"

#include <pqxx/pqxx>

#include <array>
#include <string_view>

namespace poni::repo {

inline constexpr std::array<std::string_view, 3> kUserStatusFilters{"active", "disabled", "all"};

// The users of one status (`active`: everyone who can be assigned), or of
// both (`all`), by name. `status` is one of kUserStatusFilters.
model::UserList list_users(pqxx::transaction_base& tx, std::string_view status);

}  // namespace poni::repo
