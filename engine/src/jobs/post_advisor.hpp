// The model's help with local posts (FR-14, D-088 point 11): ideas for
// posts, and the text of one post. It works only when a signed-in person
// asks through the API; no job and no schedule calls it, and it stores
// nothing and publishes nothing: its answer goes into the form.
//
// Each call reads the location's usable reviews and posts from the
// database, asks Google for the profile's figures and search keywords (GETs
// of the Performance API; left out when Google can't be asked), and asks the
// model once, through domain/prompts.hpp. The model's task is "posts", with
// its own provider and model in the AI settings.
#pragma once

#include "connectors/gbp/performance.hpp"
#include "connectors/llm/client.hpp"
#include "core/clock.hpp"
#include "db/database.hpp"
#include "db/failure.hpp"
#include "db/repo_post_help.hpp"
#include "domain/models.hpp"
#include "domain/post_help.hpp"

#include <boost/asio/awaitable.hpp>

#include <cstdint>
#include <expected>
#include <string>

namespace poni::jobs {

class PostAdvisor {
public:
    PostAdvisor(db::Database& db, const core::Clock& clock, llm::Client& llm, gbp::PerformanceReader& performance)
        : db_(db), clock_(clock), llm_(llm), performance_(performance) {}

    // Refused (conflict, "too_few_reviews") with fewer than
    // domain::kPostHelpMinReviews usable reviews: then no model is asked.
    boost::asio::awaitable<repo::Result<model::LocalPostIdeas>> ideas(std::int64_t location_id);

    // `request` is what the API accepted: a known kind, a language tag, and a brief or a text.
    boost::asio::awaitable<repo::Result<model::LocalPostSuggestion>> draft(std::int64_t location_id, domain::PostDraftRequest request);

private:
    struct Prepared {
        repo::PostHelp help;
        domain::PostHelpInput input;
    };
    boost::asio::awaitable<repo::Result<Prepared>> prepare(std::int64_t location_id, bool for_ideas);
    boost::asio::awaitable<void> read_google(const repo::PostHelp& help, domain::PostHelpInput& input);
    boost::asio::awaitable<repo::Result<llm::ChatResult>> ask(const repo::PostHelp& help, std::vector<domain::ChatMessage> messages);

    db::Database& db_;
    const core::Clock& clock_;
    llm::Client& llm_;
    gbp::PerformanceReader& performance_;
};

}  // namespace poni::jobs
