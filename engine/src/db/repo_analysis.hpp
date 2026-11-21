// Analyses, drafts and LLM calls: what the pipeline job reads and writes,
// and what the dashboard shows of it. Blocking calls on a transaction the
// caller owns: run them through Database::run.
#pragma once

#include "connectors/llm/client.hpp"
#include "core/clock.hpp"
#include "db/audit.hpp"
#include "db/failure.hpp"
#include "db/repo_go_live.hpp"
#include "db/sql.hpp"
#include "domain/models.hpp"
#include "domain/review_pipeline.hpp"

#include <boost/describe/class.hpp>
#include <boost/json/object.hpp>
#include <pqxx/pqxx>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace poni::repo {

inline constexpr std::string_view kLlmStatus = "llm";
// The job that analyzes one review and drafts its reply (jobs/analysis.hpp).
inline constexpr std::string_view kAnalyzeReviewKind = "reviews.analyze";
// On the last attempt a model that still gives no usable answer is recorded as a failed analysis or an
// empty draft, so the review reaches a person instead of being dropped.
inline constexpr int kAnalyzeReviewAttempts = 4;

// ---- the pipeline job ----------------------------------------------------------

struct PendingReview {
    std::int64_t id = 0;
    std::string updated_time;
    // False: the current version is analyzed and only the draft is missing.
    bool needs_analysis = true;
};
BOOST_DESCRIBE_STRUCT(PendingReview, (), (id, updated_time, needs_analysis))

// Reviews that lack an analysis of their current version, or a live draft
// while they have no reply, or an escalation although they are flagged and
// unanswered, and that no queued or running job is working on.
// Only reviews of an accessible location whose client has an industry and
// isn't archived. Unreplied reviews first, then newest first.
std::vector<PendingReview> pending_reviews(pqxx::transaction_base& tx, std::int64_t limit);

// Jobs of kind reviews.analyze that are queued or running.
std::int64_t active_analysis_jobs(pqxx::transaction_base& tx);

// Everything the job needs to process one review.
struct ReviewContext {
    std::int64_t review_id = 0;
    int stars = 0;                // 0: Google sent no rating
    std::string text;             // the reviewer's own words; empty for a rating-only review
    std::string updated_time;     // the version being processed
    bool has_reply = false;
    // A person decided that it needs no reply (D-068): analyzed, never drafted for.
    bool ignored = false;
    bool removed = false;
    // False: the location isn't linked to an active client, or was lost.
    bool synced = false;
    // Analysis is switched on (the AI settings).
    bool enabled = false;
    // What the review's location runs with: the client's names, the industry,
    // the reply settings and the analyzer's and the writer's model. It is the
    // setup a go-live test of the location tests, from the same loader.
    std::expected<GoLiveSetup, NoSetup> setup = std::unexpected(NoSetup{});
    // The location has nothing to run a review with, and that is its state
    // (no client, no industry profile), not a fault in its stored settings.
    bool unsupported() const { return !setup && !setup.error().settings_error; }
    // The newest analysis of this version, if there is one.
    std::optional<model::ReviewAnalysis> analysis;
    // This version of the review has a draft that settles it: one that is
    // waiting, on its way to Google, published, or rejected by a person.
    bool has_live_draft = false;
    // That draft is scheduled, approved or being posted: it must not be replaced.
    bool draft_in_flight = false;
    // Texts of the last few drafts for rating-only reviews at this location.
    std::vector<std::string> recent_rating_only_replies;
};

std::optional<ReviewContext> review_context(pqxx::transaction_base& tx, std::int64_t review_id);

// The review is still at the version the job read (`updated_time`) and was
// not removed. Locks its row, so two jobs for one review take turns.
bool review_is_current(pqxx::transaction_base& tx, std::int64_t review_id, const std::string& updated_time);

// "Analyze again": queues the analysis of one review, whatever was analyzed
// before, for the person who asked. Refused for a review the pipeline cannot work on.
Result<void> request_analysis(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t review_id);

struct NewAnalysis {
    std::int64_t review_id = 0;
    std::string review_updated_time;
    domain::Analysis analysis;
    std::string industry_key;
    std::optional<std::string> model;
    std::string prompt_version;
    // The model's answer as it came; null when there was none.
    boost::json::value raw;
};

// Stores the analysis and, when it found a language, updates reviews.language
// (the analyzer replaces the guess made when the review was stored, D-044). Returns the stored row.
model::ReviewAnalysis store_analysis(pqxx::transaction_base& tx, const NewAnalysis& analysis, core::TimePoint now);

struct NewDraft {
    std::int64_t review_id = 0;
    std::int64_t analysis_id = 0;
    std::string review_updated_time;
    std::string mode;
    std::string language;
    std::optional<std::string> script;
    domain::DraftOutcome outcome;
    std::string model;
    std::string prompt_version;
    boost::json::object inputs;
    // A draft a person asked for: their instructions and who they are.
    std::optional<std::string> instructions;
    std::optional<std::int64_t> created_by;
};

// Stores the draft as the review's live one, if it is still wanted; the
// previous live draft becomes superseded. Wanted: the review is at the
// version the draft was written for (review_is_current, so its row is locked
// first, the order every writer of drafts uses) and has no reply, and
//   - for the pipeline's own draft: no draft of the review is scheduled,
//     approved or on its way to Google (that reply stands);
//   - for a draft a person had written again (`replaces`, the draft they
//     looked at): that draft is still waiting. Its row is locked.
// Nothing, and nothing stored, otherwise.
std::optional<std::int64_t> store_draft_if_wanted(pqxx::transaction_base& tx, const NewDraft& draft, std::optional<std::int64_t> replaces,
                                                  core::TimePoint now);

// One draft with its inputs and its newest post.
std::optional<model::ReplyDraft> get_draft(pqxx::transaction_base& tx, std::int64_t id);

// What writing a draft again starts from: its status, its review and the analysis it was written from.
struct DraftBasis {
    std::string status;
    std::int64_t review_id = 0;
    std::int64_t analysis_id = 0;
};
BOOST_DESCRIBE_STRUCT(DraftBasis, (), (status, review_id, analysis_id))
std::optional<DraftBasis> draft_basis(pqxx::transaction_base& tx, std::int64_t draft_id);

// The review's draft that is waiting or approved (but not yet being posted),
// if any, gets `status` ("cancelled" or "superseded") and the reason.
bool close_live_draft(pqxx::transaction_base& tx, std::int64_t review_id, std::string_view status, std::string_view reason,
                      core::TimePoint now);

// ---- LLM calls -----------------------------------------------------------------

// One llm_calls row; the cost estimate comes from the knowledge base's model prices.
void record_llm_call(pqxx::transaction_base& tx, const llm::CallRecord& call);

// Totals from `from` (inclusive) to `to` (exclusive), by day (UTC), model, task and client.
model::LlmUsage llm_usage(pqxx::transaction_base& tx, core::TimePoint from, core::TimePoint to);

// Newest first.
model::LlmCallList list_llm_calls(pqxx::transaction_base& tx, const db::Page& page);

// ---- what the dashboard reads --------------------------------------------------

// SQL for two columns of a review's row: the analysis and the draft with
// these ids (SQL expressions that name the outer query's columns), each as an
// object with the members of the API's struct, or NULL. The draft comes
// without its inputs, as in every list.
std::string analysis_json(std::string_view analysis_id);
std::string draft_json(std::string_view draft_id);

// Every draft of the review, newest first, with its inputs.
model::ReplyDraftList list_review_drafts(pqxx::transaction_base& tx, std::int64_t review_id);

// Everything except `llm_configured` and `providers`, which the caller knows.
model::AiStatus ai_status(pqxx::transaction_base& tx);

}  // namespace poni::repo
