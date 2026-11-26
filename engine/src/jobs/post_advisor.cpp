#include "jobs/post_advisor.hpp"

#include "core/log.hpp"
#include "db/repo_settings.hpp"
#include "domain/prompts.hpp"
#include "domain/report_schedule.hpp"
#include "jobs/search_keywords.hpp"

#include <chrono>
#include <format>
#include <stdexcept>

namespace poni::jobs {

namespace asio = boost::asio;

namespace {

// How many days of Google's figures the model is given, and how many search keywords.
constexpr int kPerformanceDays = 28;
constexpr std::size_t kKeywords = 20;
// Keywords are monthly, and a month's are there only when it is nearly over: this month's or one of the two before.
constexpr int kKeywordMonths = 3;

std::unexpected<repo::Failure> no_model(std::string message) { return repo::unavailable("model_unavailable", std::move(message)); }

}  // namespace

asio::awaitable<repo::Result<PostAdvisor::Prepared>> PostAdvisor::prepare(std::int64_t location_id, bool for_ideas) {
    const auto now = clock_.now();
    auto loaded = co_await db_.tx([location_id, now](pqxx::work& tx) { return repo::load_post_help(tx, location_id, now); });
    if (!loaded) co_return repo::not_found("location");
    auto& help = *loaded;
    if (!help.enabled) co_return repo::conflict("location_not_synced", "The location is switched off.");
    if (for_ideas && std::ssize(help.usable) < domain::kPostHelpMinReviews) {
        co_return repo::conflict("too_few_reviews",
                                 std::format("Ideas need at least {} analysed reviews from the last {} days; this location has {}.",
                                             domain::kPostHelpMinReviews, domain::kPostHelpDays, help.usable.size()));
    }
    if (!help.settings.enabled) co_return no_model("The models are switched off.");
    if (!llm_.configured(help.settings.posts_provider)) {
        co_return no_model(std::format("The model provider '{}' is not configured.", help.settings.posts_provider));
    }
    auto& settings = help.location_settings;
    // A save is checked and discovery writes complete settings, which prevent this.
    if (!settings) throw std::runtime_error(std::format("post help: the location's settings can't be read: {}", settings.error()));

    const auto* industry = help.industry ? &*help.industry : nullptr;
    domain::PostHelpInput input;
    input.business_name = help.location_title;
    input.location_title = help.location_title;
    input.industry_label = industry ? industry->label : std::string{};
    if (industry) {
        for (const auto& topic : industry->topics) input.topic_labels[topic.id] = topic.label;
    }
    input.audience = settings->content.audience;
    input.today = domain::day_in_words(help.period_end);
    input.settings = std::move(*settings);
    input.reviews = domain::reviews_for_model(help.usable, industry);
    input.counts = domain::count_post_help(help.usable, industry);
    input.recent_posts = help.posts;
    co_await read_google(help, input);
    Prepared prepared{.help = std::move(help), .input = std::move(input)};
    co_return prepared;
}

// Google's figures make an idea better and are not needed for one: whatever goes wrong here, the model is asked without them.
asio::awaitable<void> PostAdvisor::read_google(const repo::PostHelp& help, domain::PostHelpInput& input) {
    using namespace std::chrono;
    if (!help.accessible) co_return;
    const auto to = help.period_end - days{1};
    const auto from = to - days{kPerformanceDays - 1};
    std::string problem;
    try {
        auto metrics = co_await performance_.daily_metrics(help.gbp_location_id, from, to);
        if (!metrics) {
            problem = metrics.error().message;
        } else if (!metrics->empty()) {
            // Google is a few days behind: the figures end with the last day it has.
            auto last = from;
            for (const auto& day : *metrics) last = std::max(last, day.day);
            input.performance = domain::PostHelpPerformance{.from = domain::format_day(from),
                                                            .to = domain::format_day(last),
                                                            .totals = domain::sum_metrics(*metrics)};
        }
        if (problem.empty()) {
            const year_month_day end{help.period_end};
            auto found = co_await newest_search_keywords(performance_, help.gbp_location_id, end.year() / end.month(), kKeywordMonths);
            if (!found) {
                problem = found.error().message;
            } else {
                if (found->keywords.size() > kKeywords) found->keywords.resize(kKeywords);
                input.keywords_month = std::move(found->month);
                input.keywords = std::move(found->keywords);
            }
        }
    } catch (const std::exception& e) {
        problem = e.what();
    }
    if (!problem.empty()) log::warn("post help: Google's figures not read", {{"location_id", help.location_id}, {"error", problem}});
}

asio::awaitable<repo::Result<llm::ChatResult>> PostAdvisor::ask(const repo::PostHelp& help, std::vector<domain::ChatMessage> messages) {
    const auto& settings = help.settings;
    llm::ChatRequest request;
    request.task = "posts";
    request.provider = settings.posts_provider;
    request.model = settings.posts_model;
    request.messages = std::move(messages);
    request.max_tokens = settings.max_output_tokens;
    request.extras = repo::extras_for(settings, settings.posts_provider);
    request.location_id = help.location_id;
    auto answer = co_await llm_.chat_json(std::move(request));
    if (!answer) {
        // The client retried what is worth retrying; the person can press the button again.
        log::warn("post help: the model gave no answer", {{"location_id", help.location_id}, {"kind", llm::to_string(answer.error().kind)}});
        co_return no_model(std::format("The model gave no answer ({}).", llm::to_string(answer.error().kind)));
    }
    co_return std::move(*answer);
}

asio::awaitable<repo::Result<model::LocalPostIdeas>> PostAdvisor::ideas(std::int64_t location_id) {
    auto prepared = co_await prepare(location_id, true);
    if (!prepared) co_return std::unexpected(prepared.error());
    auto answer = co_await ask(prepared->help, domain::post_ideas_messages(prepared->input, domain::make_nonce()));
    if (!answer) co_return std::unexpected(answer.error());
    auto ideas = domain::parse_post_ideas_answer(answer->json);
    if (!ideas) co_return no_model(std::format("The model's answer could not be used: {}.", ideas.error()));
    model::LocalPostIdeas out{.ideas = std::move(*ideas),
                              .model = answer->fallback_model.value_or(prepared->help.settings.posts_model)};
    co_return out;
}

asio::awaitable<repo::Result<model::LocalPostSuggestion>> PostAdvisor::draft(std::int64_t location_id, domain::PostDraftRequest request) {
    auto prepared = co_await prepare(location_id, false);
    if (!prepared) co_return std::unexpected(prepared.error());
    auto answer = co_await ask(prepared->help, domain::post_draft_messages(prepared->input, request, domain::make_nonce()));
    if (!answer) co_return std::unexpected(answer.error());
    auto suggestion = domain::parse_post_draft_answer(answer->json, request.topic_type);
    if (!suggestion) co_return no_model(std::format("The model's answer could not be used: {}.", suggestion.error()));
    model::LocalPostSuggestion out{.summary = std::move(suggestion->summary),
                                   .title = std::move(suggestion->title),
                                   .notes_for_staff = std::move(suggestion->notes_for_staff),
                                   .model = answer->fallback_model.value_or(prepared->help.settings.posts_model)};
    co_return out;
}

}  // namespace poni::jobs
