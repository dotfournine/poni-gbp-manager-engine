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
// The only thing written outside the database is a report's page in the
// public bucket: nothing here writes to Google Business Profile, and the
// calls to the Performance API are GETs. A report is built without
// visibility numbers when Google can't be asked and without a summary when
// the model gives none; neither stops it. Without a public bucket no report
// is started and none is built.
#pragma once

#include "connectors/gbp/performance.hpp"
#include "connectors/llm/client.hpp"
#include "connectors/storage/store.hpp"
#include "core/clock.hpp"
#include "db/database.hpp"
#include "db/repo_reports.hpp"
#include "jobs/registry.hpp"

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace poni::jobs {

inline constexpr std::string_view kReportScheduleKind = "reports.schedule";
// How often the schedules are looked at.
inline constexpr std::chrono::seconds kReportScheduleTick{300};

class Reporter {
public:
    // `public_store` holds the reports' pages. It may be absent (the setting is
    // empty): then no report is started or built.
    Reporter(db::Database& db, const core::Clock& clock, gbp::PerformanceReader& performance, llm::Client& llm,
             storage::ObjectStore* public_store)
        : db_(db), clock_(clock), performance_(performance), llm_(llm), public_store_(public_store) {}

    // Builds the report if it is still "building". Throws to be retried;
    // with `last_attempt` the report is marked failed first.
    boost::asio::awaitable<void> build(std::int64_t report_id, bool last_attempt);

    // Starts the reports that are due (repo::start_due_reports). Returns how many.
    boost::asio::awaitable<int> start_due();

    // The address in front of a page's object name; "" without a public bucket.
    std::string link_base() const;

private:
    boost::asio::awaitable<void> build_once(std::int64_t report_id, bool last_attempt);
    boost::asio::awaitable<domain::LocationPerformance> read_performance(const repo::ReportWork& work, bool last_attempt);
    boost::asio::awaitable<void> summarize(domain::ReportData& data);

    db::Database& db_;
    const core::Clock& clock_;
    gbp::PerformanceReader& performance_;
    llm::Client& llm_;
    storage::ObjectStore* public_store_;
};

void install(Registry& registry, Reporter& reporter);

}  // namespace poni::jobs
