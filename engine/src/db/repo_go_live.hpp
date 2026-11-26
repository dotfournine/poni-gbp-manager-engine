// The go-live test (D-074, D-075): a location's fingerprint, the gate that
// auto reply asks, and the runs. Blocking calls on a transaction the caller
// owns: run them through Database::run.
//
// A run tests one location's fingerprint, and its pass counts for every
// location with that fingerprint. It is started by a person for one
// location, never by a change, and it ends as passed, failed, inconclusive or cancelled. Only a pass
// counts, and all it does is remove one obstacle: nothing here switches
// auto reply on or lifts a pause.
#pragma once

#include "core/clock.hpp"
#include "db/audit.hpp"
#include "db/failure.hpp"
#include "db/sql.hpp"
#include "domain/go_live.hpp"
#include "domain/industry.hpp"
#include "domain/models.hpp"
#include "domain/reply_settings.hpp"

#include <pqxx/pqxx>

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace poni::repo {

// The job that runs one case of a run (jobs/go_live.hpp).
inline constexpr std::string_view kGoLiveCaseKind = "golive.case";
// How many cases of one run are in the queue at a time is the number of job
// workers the engine runs (PONI_JOB_WORKERS), passed in as `parallel`. Other
// jobs take their turn between cases: each next case is queued only when one
// finishes, behind whatever is already waiting.
// The location's own reviews among the cases (D-075): the newest with 4 or 5 stars.
inline constexpr int kGoLiveRealWithText = 20;
inline constexpr int kGoLiveRealWithoutText = 2;

// ---- the fingerprint and the gate ---------------------------------------------------

// Everything the models are asked with for one location, as it is now: what
// the pipeline runs a review with (ReviewContext::setup) and what a test of
// the location tests. One loader builds it for both, so a test cannot test
// something else than the pipeline runs.
struct GoLiveSetup {
    std::int64_t location_id = 0;
    std::string location_title;
    std::string business_name;
    domain::IndustryRules industry;
    domain::ResolvedSettings settings;
    domain::ModelSetup analyzer;
    domain::ModelSetup writer;
    domain::ModelSetup judge;
    domain::Fingerprint fingerprint;
    domain::AutoReplyScope scope;
};

// Why a location has no setup.
struct NoSetup {
    // As a sentence, for the page.
    std::string reason;
    // Set when the location's stored reply settings can't be read (there is
    // no row, or a part lacks a field), in the reader's own words. A save is
    // checked and discovery writes complete settings, so it is a fault to raise
    // and not a state a location can be in.
    std::optional<std::string> settings_error;
};

// Nothing, with the reason, for a location without an industry profile or
// whose reply settings can't be read. Whether it is switched on is its
// callers' to ask: this is what it would run with.
std::expected<GoLiveSetup, NoSetup> go_live_setup(pqxx::transaction_base& tx, std::int64_t location_id);
// The same for a caller that has read the AI settings in this transaction
// already (one row for every location), so that they are not read again.
// Everything of the location itself is always read here: a setup, and so a
// fingerprint, is never built from what a caller hands in.
std::expected<GoLiveSetup, NoSetup> go_live_setup(pqxx::transaction_base& tx, std::int64_t location_id, const model::AiSettings& ai);

struct GoLiveGate {
    bool passed = false;
    // Why not, as a sentence; empty when passed.
    std::string reason;
    // Empty when the location has no fingerprint.
    std::string fingerprint;
};

// May this location auto reply as far as the go-live test goes: does its
// fingerprint have a passing run at least as wide as its policy today?
// The gate reads the location's configuration itself, as it is now.
GoLiveGate go_live_gate(pqxx::transaction_base& tx, std::int64_t location_id);

// "passed", "not_tested", "running", "failed", "inconclusive" or "cancelled"
// for a fingerprint and the policy's width, with a sentence where the word
// alone doesn't say why.
struct GoLiveState {
    std::string state;
    std::optional<std::string> detail;
    std::optional<std::int64_t> pass_run_id;
    std::optional<std::int64_t> latest_run_id;
};
GoLiveState go_live_state(pqxx::transaction_base& tx, const std::string& fingerprint, const domain::AutoReplyScope& scope);

// ---- what people do -------------------------------------------------------------------

// Every enabled location, by title, with its configuration's state.
// `configured_providers` as for start_go_live: the overview says whether a test could run.
model::GoLiveOverview go_live_overview(pqxx::transaction_base& tx, const std::vector<std::string>& configured_providers, int parallel);

// Starts a run of the location's configuration at its policy's width
// (D-102 point 5), with the location's own reviews, and queues its first
// cases. Refused unless the location is switched on and can be tested, and
// a conflict while its fingerprint has a run already. Only for a change a
// person makes (audit.user_id()): the engine's own restart of a test whose
// configuration changed does not come through here.
// `configured_providers`: the test needs the analyzer's, the writer's and the
// judge's provider to have an address and a key.
Result<model::GoLiveRun> start_go_live(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t location_id,
                                       const std::vector<std::string>& configured_providers, int parallel);

// A person stops a running test: its jobs make no further model calls, what
// was spent stays recorded, and the result counts for nothing.
Result<model::GoLiveRun> cancel_go_live(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t run_id);

// One run with its cases.
std::optional<model::GoLiveRun> get_go_live_run(pqxx::transaction_base& tx, std::int64_t run_id);

// ---- the job --------------------------------------------------------------------------

// One case and what it runs with.
struct GoLiveCaseWork {
    std::int64_t case_id = 0;
    std::int64_t run_id = 0;
    std::string case_key;
    std::string category;
    int stars = 0;
    // The review's words; empty for a rating without text.
    std::string text;
    std::optional<std::int64_t> review_id;
    int repeats = 0;
    GoLiveSetup setup;
};

// The case, if it still has to be run: its run is running, it has no verdict
// yet, its location is switched on, and the location's fingerprint is still
// the one the run tests. A run whose location was switched off is cancelled
// here. A run whose configuration changed under it is cancelled too, and a
// new run of the new configuration is started in its place for the same
// location (D-077 point 3): the person who submitted the test asked for the
// location to be tested, and what the old run would go on to test no longer
// exists.
std::optional<GoLiveCaseWork> go_live_case(pqxx::transaction_base& tx, std::int64_t case_id, core::TimePoint now);

// Ends every run that has been running without a case finishing for `idle`:
// its job died for good (a crash on its last attempt), so nothing would ever
// settle it. Inconclusive, or settled if every case already has a verdict.
// Returns how many.
std::size_t end_stalled_go_live_runs(pqxx::transaction_base& tx, std::chrono::seconds idle, core::TimePoint now);

// Is the run still running? Asked before every model call, so that a
// cancelled run makes no further ones.
bool go_live_running(pqxx::transaction_base& tx, std::int64_t run_id);

// Stores a case's verdict and, in the same transaction, queues the next case
// or settles the run when this was the last. An inconclusive case ends the
// run at once as inconclusive (or failed, if a case already failed): the
// model is not answering, and further calls would be spent for nothing.
// Does nothing if the run has ended meanwhile or the case already has a verdict.
void finish_go_live_case(pqxx::transaction_base& tx, std::int64_t case_id, std::string_view verdict, const std::string& detail,
                         const std::vector<model::GoLiveAttempt>& attempts, core::TimePoint now);

}  // namespace poni::repo
