#include "jobs/go_live.hpp"

#include "core/log.hpp"
#include "core/text.hpp"
#include "domain/go_live.hpp"
#include "domain/prompts.hpp"
#include "domain/review_pipeline.hpp"
#include "jobs/analysis.hpp"

#include <chrono>
#include <format>
#include <stdexcept>

namespace poni::jobs {

namespace asio = boost::asio;

namespace {

// The judge thinks before it answers and rates several replies at once.
constexpr std::chrono::seconds kJudgeTimeout{240};

}  // namespace

asio::awaitable<GoLiveTester::Asked> GoLiveTester::ask(std::int64_t run_id, llm::ChatRequest request) {
    Asked out;
    const bool running = co_await db_.tx([run_id](pqxx::work& tx) { return repo::go_live_running(tx, run_id); });
    if (!running) {
        out.stopped = true;
        co_return out;
    }
    const std::string task = request.task;
    auto result = co_await llm_.chat_json(std::move(request));
    if (result) {
        out.json = std::move(result->json);
        out.fallback_model = std::move(result->fallback_model);
    } else {
        out.error = result.error().message;
        // The client already retried what is worth retrying. What is left is
        // an outage or a wrong setup, which says nothing about the case, or a
        // refusal, which the caller weighs by the kind of review.
        if (result.error().kind == llm::Error::Kind::rejected) {
            out.refused = true;
        } else {
            out.outage = std::format("the {} gave no answer ({}: {}).", task, llm::to_string(result.error().kind), result.error().message);
        }
    }
    co_return out;
}

asio::awaitable<void> GoLiveTester::finish(std::int64_t case_id, std::string verdict, std::string detail,
                                           std::vector<model::GoLiveAttempt> attempts) {
    co_await db_.tx([&, now = clock_.now()](pqxx::work& tx) {
        repo::finish_go_live_case(tx, case_id, verdict, detail, attempts, now);
    });
    // Ids and the outcome only: no review and no reply in the log.
    log::info("go-live case finished", {{"case_id", case_id}, {"verdict", verdict}});
}

asio::awaitable<void> GoLiveTester::run(const repo::GoLiveCaseWork& work) {
    const auto& setup = work.setup;
    const AskedFor ids{.review_id = work.review_id, .go_live_run_id = work.run_id};
    const bool has_text = !text::trim(work.text).empty();
    // A provider's content filter may refuse a review that describes an
    // illness or a threat; it has no reason to refuse ordinary praise. A
    // refusal there means the request itself is wrong (an unknown model, an
    // extra the provider rejects), every case would end without a reply, and
    // the run would pass having tested nothing.
    const bool ordinary = domain::may_auto_reply(work.category);
    const auto refusal = [](std::string_view task, const std::string& error) {
        return std::format("the {} refused an ordinary review ({}), which points at the model's name or the request, not at the review.",
                           task, error);
    };
    std::vector<model::GoLiveAttempt> attempts;
    // What the writer was asked for in each repeat, for the judge.
    std::vector<domain::DraftPlan> plans;

    for (int repeat = 0; repeat < work.repeats; ++repeat) {
        // ---- analysis: the pipeline's own request (jobs/analysis.hpp) ----
        std::optional<domain::AnalyzerAnswer> answer;
        std::optional<boost::json::object> analyzer_json;
        if (has_text) {
            auto request = analyzer_request(setup, work.stars, work.text, ids);
            auto asked = co_await ask(work.run_id, std::move(request));
            if (asked.stopped) co_return;
            if (asked.refused && ordinary) asked.outage = refusal("analyzer", asked.error);
            if (asked.outage) {
                co_await finish(work.case_id, std::string(domain::kVerdictInconclusive), *asked.outage, attempts);
                co_return;
            }
            analyzer_json = std::move(asked.json);
            if (analyzer_json) {
                answer = domain::AnalyzerAnswer{.json = &*analyzer_json, .error = {}};
            } else {
                answer = domain::AnalyzerAnswer{.json = nullptr, .error = asked.error};
            }
        }
        const auto analysis = domain::build_analysis(setup.industry, work.text, answer);
        auto plan = domain::plan_draft(analysis, work.stars, has_text, setup.settings.localization);

        // ---- the draft ----
        const auto input = domain::generator_input(setup.business_name, setup.settings, work.stars, work.text, plan);
        auto request = writer_request(setup, input, ids);
        auto asked = co_await ask(work.run_id, std::move(request));
        if (asked.stopped) co_return;
        if (asked.refused && ordinary) asked.outage = refusal("generator", asked.error);
        if (asked.outage) {
            co_await finish(work.case_id, std::string(domain::kVerdictInconclusive), *asked.outage, attempts);
            co_return;
        }
        std::optional<domain::GeneratorAnswer> written;
        if (asked.json) written = domain::parse_generator_answer(*asked.json);
        const auto outcome = domain::finish_draft(analysis, plan, work.stars, has_text, written, setup.settings);

        model::GoLiveAttempt attempt;
        attempt.route = outcome.route;
        attempt.reasons = outcome.route_reasons;
        attempt.reply = outcome.text;
        for (const auto& check : outcome.checks) {
            if (!check.passed) attempt.failed_checks.push_back(check.name);
        }
        attempts.push_back(std::move(attempt));
        plans.push_back(std::move(plan));
    }

    // ---- the verdict ----
    std::vector<std::size_t> posted;  // the repeats whose reply would have gone out without a person
    for (std::size_t i = 0; i < attempts.size(); ++i) {
        if (attempts[i].route == domain::kRouteAutoReply) posted.push_back(i);
    }
    if (!domain::may_auto_reply(work.category)) {
        if (posted.empty()) {
            co_await finish(work.case_id, std::string(domain::kVerdictOk), {}, std::move(attempts));
        } else {
            auto detail = std::format("Reached auto reply in {} of {} runs.", posted.size(), attempts.size());
            co_await finish(work.case_id, std::string(domain::kVerdictLeaked), std::move(detail), std::move(attempts));
        }
        co_return;
    }
    if (posted.empty()) {
        // Nothing would be posted without a person, so there is nothing to judge.
        co_await finish(work.case_id, std::string(domain::kVerdictOk), {}, std::move(attempts));
        co_return;
    }

    domain::JudgeInput judged{.business_name = setup.business_name, .stars = work.stars, .review_text = work.text, .replies = {}};
    for (const auto i : posted) {
        judged.replies.push_back({.reply = attempts[i].reply,
                                  .mode = plans[i].mode,
                                  .target_language = plans[i].target.language,
                                  .target_script = plans[i].target.script});
    }
    auto request = request_for(setup.judge, "judge", setup, ids);
    request.messages = domain::judge_messages(judged, domain::make_nonce());
    request.schema = domain::judge_schema();
    request.timeout = kJudgeTimeout;
    auto asked = co_await ask(work.run_id, std::move(request));
    if (asked.stopped) co_return;
    std::optional<std::vector<domain::JudgeRating>> ratings;
    if (asked.json) ratings = domain::parse_judge_answer(*asked.json, posted.size());
    if (!ratings) {
        // A reply that would be posted and that nobody rated can't count as fine.
        std::string detail = "the judge's answer could not be read.";
        if (asked.outage) {
            detail = *asked.outage;
        } else if (!asked.json) {
            detail = std::format("the judge declined to rate ({}).", asked.error);
        }
        co_await finish(work.case_id, std::string(domain::kVerdictInconclusive), std::move(detail), std::move(attempts));
        co_return;
    }
    std::string verdict(domain::kVerdictOk);
    std::string detail;
    for (std::size_t n = 0; n < posted.size(); ++n) {
        auto& attempt = attempts[posted[n]];
        attempt.rating = (*ratings)[n].rating;
        attempt.rating_reason = (*ratings)[n].reason;
        // Not the judge named in the settings: the result says so.
        attempt.rated_by = asked.fallback_model;
        if ((*ratings)[n].rating == domain::kRatingUnacceptable && verdict == domain::kVerdictOk) {
            verdict = std::string(domain::kVerdictUnacceptable);
            detail = (*ratings)[n].reason;
        }
    }
    co_await finish(work.case_id, std::move(verdict), std::move(detail), std::move(attempts));
}

asio::awaitable<void> GoLiveTester::run_case(std::int64_t case_id, bool last_attempt) {
    std::optional<std::string> failure;
    try {
        const auto work = co_await db_.tx([case_id, now = clock_.now()](pqxx::work& tx) { return repo::go_live_case(tx, case_id, now); });
        if (!work) co_return;
        co_await run(*work);
    } catch (const std::exception& e) {
        failure = e.what();  // no co_await in a catch block
    }
    if (!failure) co_return;
    if (!last_attempt) throw std::runtime_error(*failure);
    log::error("go-live case could not be run", {{"case_id", case_id}, {"error", *failure}});
    co_await finish(case_id, std::string(domain::kVerdictInconclusive), "the engine hit an error while running a case (see its log).", {});
}

asio::awaitable<std::size_t> GoLiveTester::sweep() {
    const auto ended = co_await db_.tx([now = clock_.now()](pqxx::work& tx) { return repo::end_stalled_go_live_runs(tx, kGoLiveStalledAfter, now); });
    if (ended > 0) log::warn("go-live tests that stopped making progress were ended", {{"count", ended}});
    co_return ended;
}

void install(Registry& registry, GoLiveTester& tester) {
    registry.every(std::string(kGoLiveSweepKind), std::chrono::minutes(5), 1,
                   [&tester](const Job&) -> asio::awaitable<void> { co_await tester.sweep(); });
    registry.add(std::string(repo::kGoLiveCaseKind), [&tester](const Job& job) -> asio::awaitable<void> {
        co_await tester.run_case(job.payload_id("case_id"), job.last_attempt());
    });
}

}  // namespace poni::jobs
