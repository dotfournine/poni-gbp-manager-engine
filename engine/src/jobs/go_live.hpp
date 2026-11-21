// The go-live test as jobs (D-074, D-075): one job per case of a run.
//
//   golive.case   runs one case with the location's real configuration, as
//                 often as the run says: analyzer call, writer call, reply
//                 checks and policy gate, exactly the decisions of the
//                 pipeline (domain/review_pipeline.hpp). Then one judge call
//                 rates the replies that were routed to auto-post, and the
//                 verdict is stored. Storing it queues the next case or
//                 settles the run (repo::finish_go_live_case).
//
// Nothing here writes a draft, a post or a notification, and nothing is sent
// to Google: a test only reads reviews and asks models. Before every model
// call the job checks that the run is still running, so a cancelled run
// makes no further calls. A model that can't be reached or gives no usable
// answer makes the run inconclusive, never failed and never passed.
#pragma once

#include "connectors/llm/client.hpp"
#include "core/clock.hpp"
#include "db/database.hpp"
#include "db/repo_go_live.hpp"
#include "jobs/registry.hpp"

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace poni::jobs {

class GoLiveTester {
public:
    GoLiveTester(db::Database& db, const core::Clock& clock, llm::Client& llm) : db_(db), clock_(clock), llm_(llm) {}

    // Runs the case if its run is still running and it has no verdict yet.
    // An exception (the database, a bug) is rethrown so the job is retried;
    // with `last_attempt` the case is stored as inconclusive instead, so a
    // run never stays "running" for ever.
    boost::asio::awaitable<void> run_case(std::int64_t case_id, bool last_attempt);

    // Ends runs that stopped making progress (repo::end_stalled_go_live_runs). Returns how many.
    boost::asio::awaitable<std::size_t> sweep();

private:
    // What became of one model call.
    struct Asked {
        // The run ended (cancelled, or settled by another case) before the call was made.
        bool stopped = false;
        // The model's JSON; nothing when the provider refused the request.
        std::optional<boost::json::object> json;
        // The provider refused the request (a 4xx, or the model declined).
        // For a review written to be alarming that is its content filter,
        // which the pipeline also carries on without. For an ordinary review
        // it is the setup (an unknown model, a member the provider doesn't
        // take), and a test that went on would pass with nothing tested.
        bool refused = false;
        // The model gave no answer for a reason that is not the review's:
        // unreachable, a wrong key, nonsense after every retry.
        std::optional<std::string> outage;
        std::string error;
        // The provider's fallback model answered instead of the one asked.
        std::optional<std::string> fallback_model;
    };
    boost::asio::awaitable<Asked> ask(std::int64_t run_id, llm::ChatRequest request);
    boost::asio::awaitable<void> run(const repo::GoLiveCaseWork& work);
    boost::asio::awaitable<void> finish(std::int64_t case_id, std::string verdict, std::string detail,
                                        std::vector<model::GoLiveAttempt> attempts);

    db::Database& db_;
    const core::Clock& clock_;
    llm::Client& llm_;
};

inline constexpr std::string_view kGoLiveSweepKind = "golive.sweep";
// A run without a finished case for this long has lost its jobs. Longer than
// a case can take with every retry, and than the queue's lock timeout.
inline constexpr std::chrono::minutes kGoLiveStalledAfter{45};

void install(Registry& registry, GoLiveTester& tester);

}  // namespace poni::jobs
