// A location's reports (FR-8, D-085, D-102, D-104): the reports themselves,
// their sends and the schedules that ask for them. A report row is written
// once as "building"; its job names its page, puts the page into the public
// bucket and stores the numbers, after which neither changes. A send checks
// first, under the report's row lock, and writes its delivery rows after its
// emails went out; it changes nothing on the report. Blocking calls on a
// transaction the caller owns.
//
// Nothing here talks to a bucket, a mailbox or a model: jobs/reports.hpp
// does, and records the outcome through these functions.
#pragma once

#include "core/body.hpp"
#include "core/clock.hpp"
#include "db/audit.hpp"
#include "db/failure.hpp"
#include "db/repo_notifications.hpp"
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
    std::int64_t location_id = 0;
    domain::Period period;
    std::string time_zone;
    std::optional<std::int64_t> schedule_id;
    bool auto_send = false;
};

// Writes the report as "building" and enqueues its job, requested by the
// audit's actor (a person, or nobody: a schedule). Without a time zone it
// takes the location's, else UTC. Refuses a location that is unknown or
// switched off, a time zone the system does not know, a period that is
// upside down, longer than 400 days or not over yet, and any report while
// there is no public bucket (`link_base` empty): its page would have nowhere to go.
Result<model::Report> create_report(pqxx::transaction_base& tx, audit::Writer& audit, const NewReport& report, std::string_view link_base);

struct ReportLocationRef {
    std::int64_t id = 0;
    std::string gbp_location_id;
    bool accessible = false;
};

// What the build job needs: everything the numbers are computed from that
// is in the database, and the report's location to ask Google about.
struct ReportWork {
    std::int64_t report_id = 0;
    domain::ReportInput input;
    ReportLocationRef location;
};

// Nothing unless the report is still "building". A report whose location was
// switched off meanwhile is marked failed instead, and nothing is returned.
std::optional<ReportWork> load_report_work(pqxx::transaction_base& tx, std::int64_t report_id, core::TimePoint now);

// The name of the report's page in the public bucket, written now if the row
// has none yet (`candidate`, "r/<token>.<ext>"), or nothing when the report is
// no longer "building". Called right before the page is uploaded and nowhere
// else: a row names a page only if an upload of it was at least begun, and a
// run that was overtaken uploads nothing. A retried build keeps the name.
std::optional<std::string> name_report_page(pqxx::transaction_base& tx, std::int64_t report_id, const std::string& candidate);

// ---- sending (D-104) ------------------------------------------------------------------

// What a send needs once its checks passed.
struct ReportMail {
    std::int64_t location_id = 0;
    std::vector<std::string> recipients;  // sorted, each once
    std::string subject;
    std::string body;
};

// The checks of a send, under the report's row lock, in this order: `only`, when given, names at
// least one address; the report is ready; its location is switched on; email sending is on and a
// mailbox is connected; there is a public bucket (`link_base`); the location has recipients; and
// each address of `only` is one of them (trimmed and lower-cased first). Then the mail: to every
// recipient, or to those of `only`, with the report's link in its body. Writes nothing.
Result<ReportMail> prepare_report_send(pqxx::transaction_base& tx, std::int64_t report_id,
                                       const std::optional<std::vector<std::string>>& only, std::string_view link_base);

// After the sends: one delivery row per address as it ended, under `event_key` and in the name of
// the audit's actor (a person, or nobody: its schedule), the audit entry with the counts, and the
// report as the API serves it. Checks nothing again and changes nothing on the report.
Result<model::Report> record_report_send(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t report_id, const ReportMail& mail,
                                         const std::vector<ReportDelivery>& outcomes, std::string_view event_key,
                                         std::string_view link_base);

// building -> ready, with the numbers. A report whose schedule sends it by itself ("pending") is
// checked as a send in the same transaction: if the checks pass it becomes "done" and the mail is
// returned for the caller to send, once. Nothing is returned when the report was no longer
// building, when its schedule does not send it by itself, and when the send's checks refused
// (it stays "pending" for a person to send).
std::optional<ReportMail> store_built_report(pqxx::transaction_base& tx, std::int64_t report_id, const domain::ReportData& data,
                                             std::string_view link_base, core::TimePoint now);
// building -> failed.
void fail_report(pqxx::transaction_base& tx, std::int64_t report_id, const std::string& error, core::TimePoint now);

// ---- reading -------------------------------------------------------------------------

struct ReportFilter {
    std::optional<std::int64_t> location_id;
    std::string status;
    db::Page page;
};

// `link_base` is the address of the public bucket, in front of a page's object name: a report's
// `url` while it is ready and its `archive_url` once it is withdrawn. Without a bucket both are null.
// A report of the list has no `recipients` and no `deliveries`; get_report fills both.
model::ReportList list_reports(pqxx::transaction_base& tx, const ReportFilter& filter, std::string_view link_base);
std::optional<model::Report> get_report(pqxx::transaction_base& tx, std::int64_t id, std::string_view link_base);

// ---- schedules ------------------------------------------------------------------------

// Both are the API's request bodies (POST and PATCH /api/report-schedules).
struct NewSchedule {
    std::int64_t location_id = 0;  // an enabled location
    std::string frequency;
    // Each for its frequency; null or left out otherwise.
    core::Nullable<std::int64_t> weekday;
    core::Nullable<std::int64_t> month_day;
    std::string time;  // "HH:MM"
    std::string time_zone;
    std::optional<bool> auto_send;  // nothing: off
    std::optional<bool> enabled;    // nothing: on
};
BOOST_DESCRIBE_STRUCT(NewSchedule, (), (location_id, frequency, weekday, month_day, time, time_zone, auto_send, enabled))
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

// Every schedule, by location title.
model::ReportScheduleList list_schedules(pqxx::transaction_base& tx);
Result<model::ReportSchedule> create_schedule(pqxx::transaction_base& tx, audit::Writer& audit, const NewSchedule& schedule);
Result<model::ReportSchedule> update_schedule(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id, const SchedulePatch& patch);
Result<void> delete_schedule(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id);

// A report that has been building for longer than its job can take has lost
// the job (a restart during the last attempt, say): it is marked failed, so
// that it can be discarded and generated again. Returns how many.
inline constexpr std::chrono::minutes kReportStalledAfter{60};
int fail_stalled_reports(pqxx::transaction_base& tx, core::TimePoint now);

// Builds the report of every enabled schedule of an enabled location whose
// latest run has come and has not been built yet: one report per schedule and run day, however often
// this is called. A schedule made or changed after a run's moment does not
// run for it. `link_base` as for create_report. Returns how many reports were started.
int start_due_reports(pqxx::transaction_base& tx, std::string_view link_base, core::TimePoint now);

}  // namespace poni::repo
