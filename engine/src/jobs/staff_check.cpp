#include "jobs/staff_check.hpp"

#include "core/log.hpp"
#include "core/text.hpp"
#include "db/repo_users.hpp"
#include "http/client.hpp"

#include <algorithm>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace poni::jobs {

namespace asio = boost::asio;
using repo::DirectoryAnswer;

namespace {

// What a run makes of the list (D-106 point 5): nothing, unless the list holds the connected administrator as an
// active administrator, which an empty list, another account's list, and a list read through a role that does not
// reach the administrator's own unit do not; then a finding for every staff member with a Google id.
std::expected<std::vector<repo::StaffFinding>, repo::StaffCheckError> judge(const repo::StaffToCheck& staff,
                                                                            const std::vector<workspace::DirectoryUser>& list) {
    const auto admin = text::ascii_lower(staff.admin_email);
    const bool administrator_listed = std::ranges::any_of(list, [&](const workspace::DirectoryUser& user) {
        return text::ascii_lower(user.primary_email) == admin && !user.suspended && !user.archived &&
               (user.is_admin || user.is_delegated_admin);
    });
    if (!administrator_listed) {
        return std::unexpected(repo::StaffCheckError{
            repo::StaffCheckProblem::no_administrator, "the directory's list does not hold the connected administrator as an active administrator"});
    }
    std::map<std::string_view, const workspace::DirectoryUser*> by_id;
    for (const auto& user : list) by_id.emplace(user.id, &user);

    std::vector<repo::StaffFinding> findings;
    for (const auto& member : staff.staff) {
        // A row without a Google id (development sign-in's "dev:<address>") is never judged.
        if (!http::is_plain_id(member.google_sub)) continue;
        const auto found = by_id.find(member.google_sub);
        if (found == by_id.end()) {
            findings.push_back({.user_id = member.id, .answer = DirectoryAnswer::missing});
            continue;
        }
        const auto& user = *found->second;
        const auto answer = user.suspended ? DirectoryAnswer::suspended : user.archived ? DirectoryAnswer::archived : DirectoryAnswer::present;
        findings.push_back({.user_id = member.id, .answer = answer, .full_name = user.full_name});
    }
    return findings;
}

}  // namespace

asio::awaitable<void> StaffCheck::run() {
    // The staff before the list: a user made after this read is judged at the next run, never on a list older than they are.
    const auto to_check = co_await db_.tx([](pqxx::work& tx) { return repo::staff_to_check(tx); });
    if (!to_check) co_return;

    const auto list = co_await google::guarded(directory_.list_users());
    auto findings = list ? judge(*to_check, *list)
                         : std::expected<std::vector<repo::StaffFinding>, repo::StaffCheckError>(std::unexpected(
                               repo::StaffCheckError{repo::StaffCheckProblem::unreadable, list.error().message}));
    const auto now = clock_.now();
    if (!findings) {
        const auto error = findings.error();
        co_await db_.tx([error, now](pqxx::work& tx) { repo::record_staff_check(tx, {}, 0, error, now); });
        log::warn("the staff check could not use the directory", {{"error", error.message}});
        co_return;
    }

    const auto listed = list->size();
    const auto judged = findings->size();
    const auto result = co_await db_.tx(
        [updated_at = to_check->credential_updated_at, findings = std::move(*findings), listed, now](pqxx::work& tx) {
            const auto result = repo::apply_staff_check(tx, updated_at, findings, now);
            if (!result.stale) repo::record_staff_check(tx, result, listed, std::nullopt, now);
            return result;
        });
    log::info("staff checked", {{"listed", listed},
                                {"judged", judged},
                                {"disabled", result.disabled},
                                {"enabled", result.enabled},
                                {"stale", result.stale}});
}

void install(Registry& registry, StaffCheck& check) {
    registry.every(std::string(kStaffCheckKind), check.interval(), 1,
                   [&check](const Job&) -> asio::awaitable<void> { co_await check.run(); });
}

}  // namespace poni::jobs
