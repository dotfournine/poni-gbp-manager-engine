// A location's reports as jobs (FR-8, D-085, D-102, D-104).
//
//   reports.build     builds one report: reads the location's stored reviews,
//                     asks Google for the period's visibility numbers and
//                     search keywords, computes every figure in code
//                     (domain/report.hpp), asks the model for a summary of
//                     those figures, renders the page, names it on the row,
//                     puts it into the public bucket and stores the numbers.
//   reports.schedule  every few minutes: starts the report of every
//                     schedule whose time has come.
//
// A ready report is sent by email (D-104): by a person, as often as they
// like, to the location's recipients or some of them (send), and once by the
// build itself where a person set the report's schedule to send by itself.
// Each address gets its own email, one after another, and every address that
// was tried gets its delivery row, sent or failed, once the last was tried.
// No transaction is open while the emails go out.
//
// What is written outside the database is a report's page in the public
// bucket and its emails: nothing here writes to Google Business Profile, and
// the calls to the Performance API are GETs. A report is built without
// visibility numbers when Google can't be asked and without a summary when
// the model gives none; neither stops it. Without a public bucket no report
// is started, none is built and none is sent.
#pragma once

#include "connectors/gbp/performance.hpp"
#include "connectors/llm/client.hpp"
#include "connectors/notify/channel.hpp"
#include "connectors/storage/store.hpp"
#include "core/clock.hpp"
#include "db/audit.hpp"
#include "db/database.hpp"
#include "db/failure.hpp"
#include "db/repo_reports.hpp"
#include "domain/models.hpp"
#include "jobs/registry.hpp"

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace poni::jobs {

inline constexpr std::string_view kReportScheduleKind = "reports.schedule";
// How often the schedules are looked at.
inline constexpr std::chrono::seconds kReportScheduleTick{300};

class Reporter {
public:
    // `email` sends the reports' emails. `public_store` holds the reports' pages. It may be absent (the
    // setting is empty): then no report is started, built or sent.
    Reporter(db::Database& db, const core::Clock& clock, gbp::PerformanceReader& performance, llm::Client& llm, notify::Channel& email,
             storage::ObjectStore* public_store)
        : db_(db), clock_(clock), performance_(performance), llm_(llm), email_(email), public_store_(public_store) {}

    // Builds the report if it is still "building". Throws to be retried;
    // with `last_attempt` the report is marked failed first.
    boost::asio::awaitable<void> build(std::int64_t report_id, bool last_attempt);

    // Sends a ready report to its location's recipients, or to those of `only`, for `actor`, a signed-in
    // person: refused as repo::prepare_report_send refuses, else every address is tried and the answer is
    // the report with its deliveries, whatever came of each address.
    boost::asio::awaitable<repo::Result<model::Report>> send(std::int64_t report_id, std::optional<std::vector<std::string>> only,
                                                             audit::Actor actor);

    // Starts the reports that are due (repo::start_due_reports). Returns how many.
    boost::asio::awaitable<int> start_due();

    // The address in front of a page's object name; "" without a public bucket.
    std::string link_base() const;

private:
    boost::asio::awaitable<void> build_once(std::int64_t report_id, bool last_attempt);
    boost::asio::awaitable<domain::LocationPerformance> read_performance(const repo::ReportWork& work, bool last_attempt);
    boost::asio::awaitable<void> summarize(domain::ReportData& data);
    // Emails every address of `mail`, one after another, then writes the rows and the audit entry.
    boost::asio::awaitable<repo::Result<model::Report>> deliver(std::int64_t report_id, repo::ReportMail mail, audit::Actor actor);

    db::Database& db_;
    const core::Clock& clock_;
    gbp::PerformanceReader& performance_;
    llm::Client& llm_;
    notify::Channel& email_;
    storage::ObjectStore* public_store_;
};

void install(Registry& registry, Reporter& reporter);

}  // namespace poni::jobs
