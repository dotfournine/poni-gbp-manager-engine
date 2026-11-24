#include "db/repo_cases.hpp"

#include "core/text.hpp"
#include "db/database.hpp"
#include "db/repo_reviews.hpp"

#include <boost/json/serialize.hpp>
#include <boost/json/value_from.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <map>

namespace poni::repo {

using db::iso;
using db::to_timestamptz;

namespace {

using OptId = std::optional<std::int64_t>;

constexpr std::array<std::string_view, 5> kCloseReasons{"resolved", "review_removed", "review_updated", "no_response", "dropped"};

// A case's row does not hold its review: with_reviews adds it.
const db::FilledLater kReview{"review"};

std::string case_select() {
    return std::format(R"sql(
        SELECT k.id, k.review_id, k.status, k.severity_flags,
               CASE WHEN a.id IS NOT NULL
                    THEN jsonb_build_object('id', a.id, 'email', a.email, 'name', a.name, 'picture_url', a.picture_url, 'status', a.status) END AS assignee,
               {} AS assigned_at, k.close_reason, k.close_note, (SELECT u.email FROM users u WHERE u.id = k.closed_by) AS closed_by,
               {} AS closed_at, {} AS opened_at, {} AS updated_at
          FROM cases k
          JOIN reviews r ON r.id = k.review_id
          JOIN locations l ON l.id = r.location_id
          LEFT JOIN users a ON a.id = k.assignee_id)sql",
                       iso("k.assigned_at"), iso("k.closed_at"), iso("k.opened_at"), iso("k.updated_at"));
}

// Gives each escalation its review, the newest analysis and draft included,
// so the page needs one call. The reviews are read together, not one by one.
std::vector<model::Escalation> with_reviews(pqxx::transaction_base& tx, std::vector<model::Escalation> escalations) {
    std::vector<std::int64_t> review_ids;
    for (const auto& escalation : escalations) review_ids.push_back(escalation.review_id);
    std::map<std::int64_t, model::Review> reviews;
    for (auto& review : get_reviews(tx, review_ids)) {
        const auto id = review.id;
        reviews.emplace(id, std::move(review));
    }
    for (auto& escalation : escalations) escalation.review = std::move(reviews.at(escalation.review_id));
    return escalations;
}

boost::json::value optional_json(const OptId& id) { return id ? boost::json::value(*id) : boost::json::value(nullptr); }

}  // namespace

OpenedCase open_case_for_flags(pqxx::transaction_base& tx, std::int64_t review_id, std::int64_t analysis_id,
                               const std::vector<domain::RaisedFlag>& flags, core::TimePoint now) {
    const auto now_text = to_timestamptz(now);
    const auto flags_json = boost::json::serialize(boost::json::value_from(flags));
    const auto actor = audit::Actor::system("review-analysis");
    boost::json::array flag_ids;
    for (const auto& flag : flags) flag_ids.emplace_back(flag.id);

    const auto existing = tx.exec("SELECT id, status, analysis_id FROM cases WHERE review_id = $1 FOR UPDATE", pqxx::params{review_id});
    if (existing.empty()) {
        const auto rows = tx.exec(
            "INSERT INTO cases (review_id, opened_by, analysis_id, severity_flags, opened_at, updated_at) "
            "VALUES ($1, 'severity_flag', $2, $3::jsonb, $4::timestamptz, $4::timestamptz) "
            "ON CONFLICT (review_id) DO NOTHING RETURNING id",
            pqxx::params{review_id, analysis_id, flags_json, now_text});
        // No row: another job opened it a moment ago, and alerted.
        if (rows.empty()) return {.id = tx.query_value<std::int64_t>("SELECT id FROM cases WHERE review_id = $1", pqxx::params{review_id})};
        const auto id = rows[0][0].as<std::int64_t>();
        audit::record(tx, actor,
                      {.action = "escalation.open",
                       .object_type = "escalation",
                       .object_id = std::to_string(id),
                       .after = {{"review_id", review_id}, {"severity_flags", flag_ids}}},
                      now);
        return {.id = id, .alert = true};
    }

    const auto id = existing[0][0].as<std::int64_t>();
    const bool closed = existing[0][1].as<std::string>() == "closed";
    const bool newer_analysis = existing[0][2].as<OptId>() != OptId(analysis_id);
    if (closed && newer_analysis) {
        // The review was analyzed again (it was edited, or staff asked) and is
        // still flagged: the matter is not finished after all.
        tx.exec("UPDATE cases SET status = 'open', close_reason = NULL, close_note = '', closed_by = NULL, closed_at = NULL, "
                "analysis_id = $2, severity_flags = $3::jsonb, opened_at = $4::timestamptz, updated_at = $4::timestamptz WHERE id = $1",
                pqxx::params{id, analysis_id, flags_json, now_text})
            .no_rows();
        audit::record(tx, actor,
                      {.action = "escalation.reopen",
                       .object_type = "escalation",
                       .object_id = std::to_string(id),
                       .after = {{"review_id", review_id}, {"severity_flags", flag_ids}}},
                      now);
        return {.id = id, .alert = true};
    }
    if (!closed && newer_analysis) {
        tx.exec("UPDATE cases SET analysis_id = $2, severity_flags = $3::jsonb, updated_at = $4::timestamptz WHERE id = $1",
                pqxx::params{id, analysis_id, flags_json, now_text})
            .no_rows();
    }
    return {.id = id};
}

model::EscalationList list_escalations(pqxx::transaction_base& tx, const EscalationFilter& filter) {
    db::Conditions where;
    if (!filter.status.empty()) where.add(std::format("k.status = {}", where.bind(filter.status)));
    if (filter.location_id) where.add(std::format("r.location_id = {}", where.bind(*filter.location_id)));
    if (filter.assignee_id) where.add(std::format("k.assignee_id = {}", where.bind(*filter.assignee_id)));
    auto page = db::page<model::Escalation>(tx, case_select(), where, "k.opened_at DESC, k.id DESC", filter.page, kReview);
    page.items = with_reviews(tx, std::move(page.items));
    return page;
}

std::optional<model::Escalation> get_escalation(pqxx::transaction_base& tx, std::int64_t id) {
    auto found = db::rows_as<model::Escalation>(tx, case_select() + " WHERE k.id = $1", pqxx::params{id}, kReview);
    if (found.empty()) return std::nullopt;
    return std::move(with_reviews(tx, std::move(found)).front());
}

Result<model::Escalation> assign_escalation(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id,
                                            std::optional<std::int64_t> assignee_id) {
    const auto rows = tx.exec("SELECT status, assignee_id FROM cases WHERE id = $1 FOR UPDATE", pqxx::params{id});
    if (rows.empty()) return not_found("escalation");
    if (rows[0][0].as<std::string>() != "open") return conflict("escalation_closed", "The escalation is closed.");
    const auto before = rows[0][1].as<OptId>();
    if (assignee_id && tx.exec("SELECT 1 FROM users WHERE id = $1 AND status = 'active'", pqxx::params{*assignee_id}).empty()) {
        return conflict("user_not_found", "There is no such user, or the user is disabled.");
    }
    if (before == assignee_id) {
        audit.nothing_changed();
        return *get_escalation(tx, id);
    }
    tx.exec("UPDATE cases SET assignee_id = $2, assigned_at = CASE WHEN $2::bigint IS NULL THEN NULL ELSE $3::timestamptz END, "
            "updated_at = $3::timestamptz WHERE id = $1",
            pqxx::params{id, assignee_id, to_timestamptz(audit.now())})
        .no_rows();
    audit.record("escalation.assign", "escalation", id,
                 {.before = {{"assignee_id", optional_json(before)}},
                  .after = {{"assignee_id", optional_json(assignee_id)}}});
    return *get_escalation(tx, id);
}

Result<model::Escalation> close_escalation(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id, const std::string& reason,
                                           const std::string& note) {
    const auto rows = tx.exec("SELECT status FROM cases WHERE id = $1 FOR UPDATE", pqxx::params{id});
    if (rows.empty()) return not_found("escalation");
    if (rows[0][0].as<std::string>() != "open") return conflict("escalation_closed", "The escalation is closed.");
    const std::string cleaned_note(text::trim(note));
    if (!std::ranges::contains(kCloseReasons, std::string_view(reason)) || text::length(cleaned_note) > 2000) {
        return invalid("\"reason\" must be resolved, review_removed, review_updated, no_response or dropped, and \"note\" at most 2000 characters.");
    }
    tx.exec("UPDATE cases SET status = 'closed', close_reason = $2, close_note = $3, closed_by = $4, closed_at = $5::timestamptz, "
            "updated_at = $5::timestamptz WHERE id = $1",
            pqxx::params{id, reason, cleaned_note, audit.user_id(), to_timestamptz(audit.now())})
        .no_rows();
    audit.record("escalation.close", "escalation", id,
                 {.before = {{"status", "open"}},
                  .after = {{"status", "closed"}, {"reason", reason}, {"note", cleaned_note}}});
    return *get_escalation(tx, id);
}

}  // namespace poni::repo
