// A location's reports as jobs (FR-8, D-085, D-102), and what a person does with one.
//
//   reports.build     builds one report: reads the location's stored reviews,
//                     asks Google for the period's visibility numbers and
//                     search keywords, computes every figure in code
//                     (domain/report.hpp), asks the model for a summary of
//                     those figures, renders the page and stores both. If
//                     the report's schedule sends by itself, it is sent.
//   reports.schedule  every few minutes: starts the report of every
//                     schedule whose time has come.
//
// The only things written outside the database are objects in the public
// bucket: nothing here writes to Google Business Profile, and the calls to
// the Performance API are GETs. A report is built without visibility
// numbers when Google can't be asked and without a summary when the model
// gives none; neither stops it.
//
// send(), create_link() and revoke_link() are what the API calls for a
// signed-in person: they put a page into the public bucket (or overwrite
// one) and then record it.
#pragma once

#include "connectors/gbp/performance.hpp"
#include "connectors/llm/client.hpp"
#include "connectors/storage/store.hpp"
#include "core/clock.hpp"
#include "db/audit.hpp"
#include "db/database.hpp"
#include "db/repo_reports.hpp"
#include "jobs/registry.hpp"

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace poni::jobs {

inline constexpr std::string_view kReportScheduleKind = "reports.schedule";
// How often the schedules are looked at.
inline constexpr std::chrono::seconds kReportScheduleTick{300};

class Reporter {
public:
    // `public_store` holds the shared pages. It may be absent (the setting is
    // empty): then no report can be shared or sent.
    Reporter(db::Database& db, const core::Clock& clock, gbp::PerformanceReader& performance, llm::Client& llm,
             storage::ObjectStore* public_store)
        : db_(db), clock_(clock), performance_(performance), llm_(llm), public_store_(public_store) {}

    // Builds the report if it is still "building". Throws to be retried;
    // with `last_attempt` the report is marked failed first.
    boost::asio::awaitable<void> build(std::int64_t report_id, bool last_attempt);

    // Starts the reports that are due (repo::start_due_reports). Returns how many.
    boost::asio::awaitable<int> start_due();

    // `actor` the person, or system:report-schedule when its schedule sends the report.
    boost::asio::awaitable<repo::Result<model::Report>> send(std::int64_t report_id, audit::Actor actor);
    // A person only.
    boost::asio::awaitable<repo::Result<model::ReportShareLink>> create_link(std::int64_t report_id, audit::Actor actor);
    boost::asio::awaitable<repo::Result<model::ReportShareLink>> revoke_link(std::int64_t link_id, audit::Actor actor);

    // The address in front of a share link's object name; "" without a public bucket.
    std::string link_base() const;

private:
    boost::asio::awaitable<void> build_once(std::int64_t report_id, bool last_attempt);
    boost::asio::awaitable<domain::LocationPerformance> read_performance(const repo::ReportWork& work, bool last_attempt);
    boost::asio::awaitable<void> summarize(domain::ReportData& data);
    boost::asio::awaitable<void> hold(std::int64_t report_id, std::string reason);
    // Puts `html` under a new token. The token, or why the bucket refused.
    boost::asio::awaitable<repo::Result<std::string>> publish(std::string html);

    db::Database& db_;
    const core::Clock& clock_;
    gbp::PerformanceReader& performance_;
    llm::Client& llm_;
    storage::ObjectStore* public_store_;
};

void install(Registry& registry, Reporter& reporter);

}  // namespace poni::jobs
