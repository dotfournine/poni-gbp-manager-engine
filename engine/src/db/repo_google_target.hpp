// Which Google a database belongs to: the real one, or the local mock
// (PONI_GOOGLE_MOCK_URL, D-087). Checked once, when the engine starts.
// Blocking calls on a transaction the caller owns.
#pragma once

#include "core/clock.hpp"

#include <pqxx/pqxx>

#include <optional>
#include <string>

namespace poni::repo {

// Marks a database that has no mark yet with the engine's mode, and says why
// the engine must not start if the mark disagrees with it. A database without
// a mark that already holds locations, reviews or a stored token came from
// Google, and is never marked as a mock database. Commit the transaction when
// nothing is returned.
std::optional<std::string> claim_google_target(pqxx::transaction_base& tx, bool mock, core::TimePoint now);

}  // namespace poni::repo
