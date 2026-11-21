// Client reports (FR-8, D-085): the reports themselves, their share links
// and the schedules that ask for them. A report row is written once as
// "building"; its job fills in the numbers and the page, after which they
// never change. Blocking calls on a transaction the caller owns.
//
// Nothing here talks to a bucket or to a model: jobs/reports.hpp does, and
// records the outcome through these functions.
#pragma once

#include "core/body.hpp"
#include "core/clock.hpp"
#include "db/audit.hpp"
#include "db/failure.hpp"
#include "db/sql.hpp"
#include "domain/models.hpp"
#include "domain/report.hpp"
#include "domain/report_schedule.hpp"

#include <boost/describe/class.hpp>

#include <pqxx/pqxx>

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace poni::repo {

inline constexpr std::string_view kBuildReportKind = "reports.build";
inline constexpr int kBuildReportAttempts = 4;

// ---- creating and building ---------------------------------------------------------

struct NewReport {
    std::int64_t client_id = 0;
    domain::Period period;
    std::string time_zone;
    std::optional<std::int64_t> schedule_id;
    bool auto_send = false;
};

// Writes the report as "building" and enqueues its job, requested by the
// audit's actor (a person, or nobody: a schedule). Without a time zone it
// takes that of the client's first location that has one, else UTC. Refuses
// a client that is unknown or archived, a time zone the system does not
// know, and a period that is upside down, longer than 400 days or not over yet.
Result<model::Report> create_report(pqxx::transaction_base& tx, audit::Writer& audit, const NewReport& report, std::string_view link_base);

struct ReportLocationRef {
    std::int64_t id = 0;
    std::string gbp_location_id;
    bool accessible = false;
};

// What the build job needs: everything the numbers are computed from that
// is in the database, and the locations to ask Google about.
struct ReportWork {
    std::int64_t report_id = 0;
    bool auto_send = false;
    domain::ReportInput input;
    std::vector<ReportLocationRef> locations;
};

// Nothing unless the report is still "building".
std::optional<ReportWork> load_report_work(pqxx::transaction_base& tx, std::int64_t report_id, core::TimePoint now);

// building -> draft, with the numbers and the page. False if the report was no longer building.
bool store_built_report(pqxx::transaction_base& tx, std::int64_t report_id, const domain::ReportData& data, const std::string& html,
                        core::TimePoint now);
// building -> failed.
void fail_report(pqxx::transaction_base& tx, std::int64_t report_id, const std::string& error, core::TimePoint now);

// ---- reading -------------------------------------------------------------------------

struct ReportFilter {
    std::optional<std::int64_t> client_id;
    std::string status;
    db::Page page;
};

// `link_base` is the address of the public bucket, in front of "r/<token>.html".
model::ReportList list_reports(pqxx::transaction_base& tx, const ReportFilter& filter);
std::optional<model::Report> get_report(pqxx::transaction_base& tx, std::int64_t id, std::string_view link_base);
// The stored page; nothing while the report has none.
std::optional<std::string> report_html(pqxx::transaction_base& tx, std::int64_t id);
// The stored numbers as JSON text.
std::optional<std::string> report_data(pqxx::transaction_base& tx, std::int64_t id);

// ---- sending, rejecting and share links ---------------------------------------------

// The object a share link's token names in the public bucket.
std::string share_object_name(std::string_view token);

// The page to send, if the report can be sent now: it is a draft, email
// sending is switched on, and a rule names recipients for the client's reports.
Result<std::string> check_send(pqxx::transaction_base& tx, std::int64_t report_id);

// After the page was put in the public bucket under `token`: records the
// link, raises the client_report event (one email per recipient) and marks
// the report sent by the audit's actor (nobody when its schedule sends it,
// actor system:report-schedule). Checks again what check_send checked.
Result<model::Report> mark_report_sent(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t report_id, const std::string& token,
                                       std::string_view link_base);

// A built report its schedule could not send keeps the reason for the person who looks at it.
void hold_report(pqxx::transaction_base& tx, std::int64_t report_id, const std::string& reason, core::TimePoint now);

// draft or failed -> rejected. A failed report stays without numbers and page.
Result<model::Report> reject_report(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t report_id, std::string_view link_base);

// The page of a report a link may be made for (a draft or a sent report).
Result<std::string> shareable_html(pqxx::transaction_base& tx, std::int64_t report_id);
// After the page was put in the bucket under `token`.
model::ReportShareLink add_share_link(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t report_id, const std::string& token,
                                      std::string_view link_base);

struct ShareLinkRef {
    std::int64_t report_id = 0;
    std::string token;
    bool revoked = false;
};
std::optional<ShareLinkRef> share_link_ref(pqxx::transaction_base& tx, std::int64_t link_id);
// After the link's object was overwritten.
Result<model::ReportShareLink> revoke_share_link(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t link_id,
                                                 std::string_view link_base);

// ---- schedules ------------------------------------------------------------------------

// Both are the API's request bodies (POST and PATCH /api/report-schedules).
struct NewSchedule {
    std::int64_t client_id = 0;
    std::string frequency;
    // Each for its frequency; null or left out otherwise.
    core::Nullable<std::int64_t> weekday;
    core::Nullable<std::int64_t> month_day;
    std::string time;  // "HH:MM"
    std::string time_zone;
    std::optional<bool> auto_send;  // nothing: off
    std::optional<bool> enabled;    // nothing: on
};
BOOST_DESCRIBE_STRUCT(NewSchedule, (), (client_id, frequency, weekday, month_day, time, time_zone, auto_send, enabled))
// weekday and month_day can't be cleared by a PATCH: the one the frequency does not use is dropped.
struct SchedulePatch {
    std::optional<std::string> frequency;
    std::optional<std::int64_t> weekday;
    std::optional<std::int64_t> month_day;
    std::optional<std::string> time;
    std::optional<std::string> time_zone;
    std::optional<bool> auto_send;
    std::optional<bool> enabled;
};
BOOST_DESCRIBE_STRUCT(SchedulePatch, (), (frequency, weekday, month_day, time, time_zone, auto_send, enabled))

// Every schedule, by client.
model::ReportScheduleList list_schedules(pqxx::transaction_base& tx);
Result<model::ReportSchedule> create_schedule(pqxx::transaction_base& tx, audit::Writer& audit, const NewSchedule& schedule);
Result<model::ReportSchedule> update_schedule(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id, const SchedulePatch& patch);
Result<void> delete_schedule(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id);

// A report that has been building for longer than its job can take has lost
// the job (a restart during the last attempt, say): it is marked failed, so
// that it can be rejected and generated again. Returns how many.
inline constexpr std::chrono::minutes kReportStalledAfter{60};
int fail_stalled_reports(pqxx::transaction_base& tx, core::TimePoint now);

// Builds the report of every enabled schedule whose latest run has come and
// has not been built yet: one report per schedule and run day, however often
// this is called. A schedule made or changed after a run's moment does not
// run for it. Returns how many reports were started.
int start_due_reports(pqxx::transaction_base& tx, core::TimePoint now);

}  // namespace poni::repo
