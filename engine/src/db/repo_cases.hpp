// Escalations (FR-6), stored as the simplest form of a case (D-054): opened
// when the analyzer raises a severity flag on a review that has no reply,
// assigned to one person, closed with a reason. The table is `cases`; the
// statuses, the ways of opening and the kinds it knows are the first values
// of the lists FR-12 will extend. Blocking calls on a transaction the caller
// owns.
#pragma once

#include "core/clock.hpp"
#include "db/audit.hpp"
#include "db/failure.hpp"
#include "db/sql.hpp"
#include "domain/models.hpp"
#include "domain/review_pipeline.hpp"

#include <array>
#include <pqxx/pqxx>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace poni::repo {

struct OpenedCase {
    std::int64_t id = 0;
    // False: the case was open already and only took the new flags.
    bool alert = false;
};

// Opens the review's case for these flags, or opens it again if it was
// closed and this is a newer analysis. One review has one case. Records the
// audit entry (actor system:review-analysis) when something was opened.
OpenedCase open_case_for_flags(pqxx::transaction_base& tx, std::int64_t review_id, std::int64_t analysis_id,
                               const std::vector<domain::RaisedFlag>& flags, core::TimePoint now);

inline constexpr std::array<std::string_view, 2> kEscalationStatuses{"open", "closed"};

struct EscalationFilter {
    std::string status;  // one of kEscalationStatuses; empty: both
    std::optional<std::int64_t> client_id;
    std::optional<std::int64_t> location_id;
    std::optional<std::int64_t> assignee_id;
    db::Page page;
};

model::EscalationList list_escalations(pqxx::transaction_base& tx, const EscalationFilter& filter);
std::optional<model::Escalation> get_escalation(pqxx::transaction_base& tx, std::int64_t id);

// `assignee_id` nothing: nobody.
Result<model::Escalation> assign_escalation(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id,
                                            std::optional<std::int64_t> assignee_id);
Result<model::Escalation> close_escalation(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id, const std::string& reason,
                                           const std::string& note);

// Everyone who can be assigned: users that aren't disabled, by name.
model::UserList list_users(pqxx::transaction_base& tx);

}  // namespace poni::repo
