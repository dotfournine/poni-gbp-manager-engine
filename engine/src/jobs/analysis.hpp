// The analysis and drafting pipeline as jobs (architecture 4.1, steps 2-4):
// every review of an enabled location with an industry gets an analysis, and
// every review without a reply gets a draft and a route. A draft routed to
// auto-post is scheduled for posting where a person switched auto-posting
// on (D-069); every other draft waits for a person.
//
//   reviews.analyze_pending   every minute, and right after a sync stored
//                             something new: finds the reviews that still
//                             need work and enqueues one job each, a batch
//                             at a time so the sync is never starved.
//   reviews.analyze           one review: analyzer call,
//                             generator call, mechanical checks, policy gate.
//
// "Needs work" is read from the data, not from events: a review whose
// current version has no analysis, or that has no reply and no live draft.
// So a review that was edited, lost its reply, or whose job died is picked
// up again by the next pass, and running a job twice changes nothing.
#pragma once

#include "connectors/llm/client.hpp"
#include "core/clock.hpp"
#include "db/audit.hpp"
#include "db/database.hpp"
#include "db/repo_analysis.hpp"
#include "domain/go_live.hpp"
#include "domain/prompts.hpp"
#include "domain/review_pipeline.hpp"
#include "jobs/queue.hpp"
#include "jobs/registry.hpp"

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace poni::jobs {

inline constexpr std::string_view kAnalyzePendingKind = "reviews.analyze_pending";

// ---- what the models are asked ----------------------------------------------------
// One construction for the pipeline and for the go-live test (jobs/go_live.cpp):
// both ask through these functions, with a setup from the one loader
// (repo::go_live_setup), so a test puts before the models what the pipeline
// will. The pipeline alone adds to the writer's input what only it has: the
// location's recent replies to rating-only reviews, and a person's
// instructions for a draft they have written again. Neither is part of what
// a test covers, and a draft with instructions never auto-posts.

// What a call is recorded under besides its location (llm_calls). Never sent.
struct AskedFor {
    std::optional<std::int64_t> review_id;
    std::optional<std::int64_t> go_live_run_id;
};

// A request to one of the setup's models: its provider, its model name, its
// temperature (or none), its token limit and the provider's request extras.
// The caller adds the messages.
llm::ChatRequest request_for(const domain::ModelSetup& model, std::string task, const repo::GoLiveSetup& setup, const AskedFor& ids);
// The analyzer's request for a review with these stars and these words.
llm::ChatRequest analyzer_request(const repo::GoLiveSetup& setup, int stars, std::string_view review_text, const AskedFor& ids);
// The writer's request for `input`.
llm::ChatRequest writer_request(const repo::GoLiveSetup& setup, const domain::GeneratorInput& input, const AskedFor& ids);

struct AnalysisOptions {
    std::chrono::seconds tick{60};
    // Where in the policy's delay range an auto-post falls, in [0, 1).
    // Empty: random. Tests set it.
    std::function<double()> delay_roll;
};

class ReviewPipeline {
public:
    ReviewPipeline(db::Database& db, const core::Clock& clock, llm::Client& llm,
                   JobQueue& queue, AnalysisOptions options)
        : db_(db), clock_(clock), llm_(llm), queue_(queue), options_(options) {}

    // Enqueues a job for each review that needs work, up to the batch size
    // from the settings minus the jobs already waiting. Returns how many.
    boost::asio::awaitable<std::size_t> enqueue_pending();

    // Analyzes the review (unless its current version already is) and
    // drafts a reply if it has none. `force` redoes both. Throws when the
    // model can't be reached or answers nonsense, so the job is retried;
    // with `last_attempt` the failure is stored instead.
    boost::asio::awaitable<void> process(std::int64_t review_id, bool force, bool last_attempt);

    // Writes a waiting draft again with a staff member's instructions added
    // to the prompt (FR-5). The new draft replaces the old one and always
    // waits for a person. Nothing is stored when the model gives no usable
    // answer. Returns the new draft's id.
    // `actor`: the signed-in person who asked.
    boost::asio::awaitable<repo::Result<std::int64_t>> regenerate(std::int64_t draft_id, std::string instructions, audit::Actor actor);

    const AnalysisOptions& options() const { return options_; }

private:
    // A person's request for a new draft.
    struct Regeneration {
        std::int64_t draft_id = 0;  // the draft being replaced
        std::string instructions;
        audit::Actor actor;  // a person
    };

    boost::asio::awaitable<llm::Result<llm::ChatResult>> ask(llm::ChatRequest request);

    // Opens the review's escalation and raises its alert, once.
    boost::asio::awaitable<void> escalate(const repo::ReviewContext& context, const domain::Analysis& analysis,
                                          std::int64_t analysis_id);

    // The writer's part for a review whose location has a setup
    // (context.setup): plan, generator call, checks and gate. Gives the
    // draft to store. For the pipeline it throws when the model fails and the
    // job should be retried, and gives an empty draft on the last attempt.
    // For a regeneration a person is waiting for the answer, so it gives
    // nothing when the model gave no usable one (why is in llm_calls).
    boost::asio::awaitable<std::optional<repo::NewDraft>> compose_draft(
        const repo::ReviewContext& context, const domain::Analysis& analysis, std::int64_t analysis_id, bool last_attempt,
        const Regeneration* regeneration);

    // Stores the pipeline's own draft, pauses a location whose checks keep
    // failing, and schedules the auto-post or tells people that a draft
    // waits. False, and nothing stored, if the review changed or was replied
    // to while the draft was written.
    boost::asio::awaitable<bool> store_own_draft(const repo::ReviewContext& context, const domain::Analysis& analysis,
                                                 const repo::NewDraft& draft);

    // Stores the draft a person asked for in place of the one they looked
    // at, audited as theirs. Nothing if that draft or its review changed
    // while the new one was written.
    boost::asio::awaitable<std::optional<std::int64_t>> store_regenerated_draft(const repo::NewDraft& draft,
                                                                                const Regeneration& regeneration);

    db::Database& db_;
    const core::Clock& clock_;
    llm::Client& llm_;
    JobQueue& queue_;
    AnalysisOptions options_;
};

void install(Registry& registry, ReviewPipeline& pipeline);

}  // namespace poni::jobs
