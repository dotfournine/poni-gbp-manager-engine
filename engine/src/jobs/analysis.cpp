#include "jobs/analysis.hpp"

#include "db/repo_auto_reply.hpp"
#include "db/repo_posting.hpp"

#include "core/log.hpp"
#include "core/text.hpp"
#include "db/repo_analysis.hpp"
#include "db/repo_auth.hpp"
#include "db/repo_cases.hpp"
#include "db/repo_notifications.hpp"
#include "db/repo_settings.hpp"

#include <boost/json/value_from.hpp>

#include <format>
#include <random>
#include <stdexcept>

namespace poni::jobs {

namespace asio = boost::asio;

namespace {

domain::Analysis to_domain(const model::ReviewAnalysis& stored) {
    return {.ok = stored.status == "ok",
            .error = stored.error.value_or(""),
            .language = stored.language,
            .script = stored.script,
            .sentiment = stored.sentiment,
            .contains_complaint = stored.contains_complaint,
            .topics = stored.topics,
            .items = stored.items,
            .severity_flags = stored.severity_flags,
            .manipulation_suspected = stored.manipulation_suspected,
            .manipulation_evidence = stored.manipulation_evidence,
            .summary_en = stored.summary_en};
}

// In [0, 1).
double random_roll() {
    static std::mt19937_64 engine{std::random_device{}()};
    return std::uniform_real_distribution<double>(0.0, 1.0)(engine);
}

bool review_has_text(const repo::ReviewContext& context) { return !text::trim(context.text).empty(); }

// What a failed model call means for the pipeline: stop and retry the job
// later, or go on without an answer. A wrong key or address is never a
// property of the review.
bool must_retry(const llm::Error& error, bool last_attempt) { return error.configuration() || (error.transient() && !last_attempt); }

// The review as a notification shows it. Its words are untrusted: they go in
// a body, which is sent as plain text, and never in a subject (a header).
std::string review_lines(const repo::ReviewContext& context) {
    return std::format("Rating: {}\n\nReview:\n{}", context.stars > 0 ? std::format("{} of 5 stars", context.stars) : std::string("none"),
                       context.text.empty() ? std::string("(no text)") : context.text);
}

// Reply settings that can't be read are a fault in the stored row, which the
// check of a save and the defaults written at discovery prevent: raised, so the job fails and says so, where
// a location without an industry profile is only skipped.
void raise_broken_settings(const repo::ReviewContext& context) {
    if (context.setup || !context.setup.error().settings_error) return;
    throw std::runtime_error(
        std::format("review {}: the settings can't be read: {}", context.review_id, *context.setup.error().settings_error));
}

}  // namespace

llm::ChatRequest request_for(const domain::ModelSetup& model, std::string task, const repo::GoLiveSetup& setup, const AskedFor& ids) {
    llm::ChatRequest request;
    request.task = std::move(task);
    request.provider = model.provider;
    request.model = model.model;
    request.temperature = model.temperature;
    request.max_tokens = model.max_tokens;
    request.extras = model.extras;
    request.location_id = setup.location_id;
    request.review_id = ids.review_id;
    request.go_live_run_id = ids.go_live_run_id;
    return request;
}

llm::ChatRequest analyzer_request(const repo::GoLiveSetup& setup, int stars, std::string_view review_text, const AskedFor& ids) {
    auto request = request_for(setup.analyzer, "analyzer", setup, ids);
    request.messages = domain::analyzer_messages(setup.business_name, setup.industry, stars, review_text, domain::make_nonce());
    return request;
}

llm::ChatRequest writer_request(const repo::GoLiveSetup& setup, const domain::GeneratorInput& input, const AskedFor& ids) {
    auto request = request_for(setup.writer, "generator", setup, ids);
    request.messages = domain::generator_messages(input, domain::make_nonce());
    return request;
}

asio::awaitable<std::size_t> ReviewPipeline::enqueue_pending() {
    const auto providers = llm_.providers();
    if (providers.empty()) {
        log::debug("review analysis is idle: no model provider is configured");
        co_return 0;
    }
    struct Plan {
        bool enabled = false;
        // The providers both tasks name have an address and a key.
        bool ready = false;
        std::vector<repo::PendingReview> reviews;
    };
    const auto plan = co_await db_.tx([providers](pqxx::work& tx) {
        Plan out;
        const auto settings = repo::get_ai_settings(tx);
        out.enabled = settings.enabled;
        if (!settings.enabled) return out;
        out.ready = std::ranges::contains(providers, settings.analyzer_provider) &&
                    std::ranges::contains(providers, settings.generator_provider);
        if (!out.ready) return out;
        auto room = settings.batch_size - repo::active_analysis_jobs(tx);
        // While the model is failing, one review at a time finds out when it is back.
        const auto health = repo::integration(tx, repo::kLlmStatus);
        const bool failing = health.last_error_at && (!health.last_ok_at || *health.last_ok_at < *health.last_error_at);
        if (failing && room > 1) room = 1;
        out.reviews = repo::pending_reviews(tx, room);
        return out;
    });
    if (plan.enabled && !plan.ready) log::debug("review analysis is idle: a task names a model provider that is not configured");
    if (!plan.enabled || !plan.ready || plan.reviews.empty()) co_return 0;

    // One job per review version and hour: a job that failed for good gets
    // another chance in the next hour, and no sooner.
    const auto hour = std::chrono::duration_cast<std::chrono::hours>(clock_.now().time_since_epoch()).count();
    std::size_t enqueued = 0;
    for (const auto& review : plan.reviews) {
        const auto id = co_await queue_.enqueue(
            {.kind = std::string(repo::kAnalyzeReviewKind),
             .payload = boost::json::object{{"review_id", review.id}},
             .run_at = std::nullopt,
             .idempotency_key = std::format("{}@{}@{}@{}", review.id, review.updated_time, review.needs_analysis ? "a" : "d", hour),
             .max_attempts = repo::kAnalyzeReviewAttempts});
        if (id) ++enqueued;
    }
    if (enqueued > 0) log::info("reviews queued for analysis", {{"count", enqueued}});
    co_return enqueued;
}

// Calls the model and notes on the status page whether it answered.
// (A member coroutine with named locals, not a lambda: a coroutine lambda's captures must
// outlive it.)
asio::awaitable<llm::Result<llm::ChatResult>> ReviewPipeline::ask(llm::ChatRequest request) {
    auto result = co_await llm_.chat_json(std::move(request));
    std::optional<std::string> error;
    if (!result) error = std::format("{}: {}", llm::to_string(result.error().kind), result.error().message);
    const auto now = clock_.now();
    co_await db_.tx([error = std::move(error), now](pqxx::work& tx) { repo::record_integration(tx, repo::kLlmStatus, error, now); });
    co_return result;
}

asio::awaitable<void> ReviewPipeline::process(std::int64_t review_id, bool force, bool last_attempt) {
    auto loaded = co_await db_.tx([review_id](pqxx::work& tx) { return repo::review_context(tx, review_id); });
    const auto skip = [review_id](std::string_view why) {
        log::debug("review not analyzed", {{"review_id", review_id}, {"reason", why}});
    };
    if (!loaded) {
        skip("no such review");
        co_return;
    }
    auto& context = *loaded;
    if (context.removed) {
        skip("the review was removed");
        co_return;
    }
    if (!context.synced) {
        skip("its location is not synced");
        co_return;
    }
    if (!context.enabled) {
        skip("analysis is switched off");
        co_return;
    }
    raise_broken_settings(context);
    if (!context.setup) {
        // A location without an industry profile.
        skip(context.setup.error().reason);
        co_return;
    }
    const auto& setup = *context.setup;
    const bool has_text = review_has_text(context);

    // ---- 1. analysis -------------------------------------------------------------
    domain::Analysis analysis;
    std::int64_t analysis_id = 0;
    const bool reuse = context.analysis && !force;
    if (reuse) {
        analysis = to_domain(*context.analysis);
        analysis_id = context.analysis->id;
    } else {
        std::optional<domain::AnalyzerAnswer> answer;
        std::optional<llm::ChatResult> model_answer;
        if (has_text) {
            auto request = analyzer_request(setup, context.stars, context.text, {.review_id = review_id, .go_live_run_id = {}});
            auto result = co_await ask(std::move(request));
            if (!result) {
                if (must_retry(result.error(), last_attempt)) {
                    throw std::runtime_error(std::format("review {}: analyzer: {}", review_id, result.error().message));
                }
                answer = domain::AnalyzerAnswer{.json = nullptr, .error = result.error().message};
            } else {
                model_answer = std::move(*result);
                answer = domain::AnalyzerAnswer{.json = &model_answer->json, .error = {}};
            }
        }
        analysis = domain::build_analysis(setup.industry, context.text, answer);
        if (has_text && model_answer && !analysis.ok && !last_attempt) {
            // The model answered, but not with the fields the routing needs: ask again.
            throw std::runtime_error(std::format("review {}: analyzer: {}", review_id, analysis.error));
        }
        const repo::NewAnalysis entry{.review_id = review_id,
                                      .review_updated_time = context.updated_time,
                                      .analysis = analysis,
                                      .industry_key = setup.industry.key,
                                      .model = has_text ? std::optional(setup.analyzer.model) : std::nullopt,
                                      .prompt_version = std::string(domain::prompt_version()),
                                      .raw = model_answer ? boost::json::value(model_answer->json) : boost::json::value(nullptr)};
        const auto stored = co_await db_.tx([&entry, now = clock_.now()](pqxx::work& tx) -> std::optional<std::int64_t> {
            if (!repo::review_is_current(tx, entry.review_id, entry.review_updated_time)) return std::nullopt;
            return repo::store_analysis(tx, entry, now).id;
        });
        if (!stored) {
            skip("the review changed while it was analyzed");
            co_return;
        }
        analysis_id = *stored;
    }

    // ---- 2. escalation ------------------------------------------------------------
    // A severity flag on a review nobody has answered needs a person now
    // (FR-6). Read from the data like everything else here, so a run that
    // died after storing the analysis still raises the alert on the next one.
    if (!analysis.severity_flags.empty() && !context.has_reply) co_await escalate(context, analysis, analysis_id);

    // ---- 3. draft ----------------------------------------------------------------
    // A review that already has a reply is analyzed and nothing more.
    if (context.has_reply) {
        log::info("review analyzed", {{"review_id", review_id}, {"severity_flags", analysis.severity_flags.size()}, {"draft", false}});
        co_return;
    }
    if (context.ignored) {
        // Also when staff asked for a fresh analysis: the decision stands until they take it back.
        log::info("review analyzed", {{"review_id", review_id}, {"severity_flags", analysis.severity_flags.size()}, {"draft", false}});
        co_return;
    }
    if (reuse && context.has_live_draft) {
        skip("it already has an analysis and a draft");
        co_return;
    }
    if (context.draft_in_flight) {
        // Approved or on its way to Google: that reply stands, whatever a new analysis says.
        skip("a reply to it is approved or being posted");
        co_return;
    }
    // The pipeline's own draft always comes: empty on the last attempt, if the model gave nothing.
    const auto draft = co_await compose_draft(context, analysis, analysis_id, last_attempt, nullptr);
    const bool stored = co_await store_own_draft(context, analysis, draft.value());
    if (!stored) skip("the review changed or was replied to while its draft was written");
}

asio::awaitable<void> ReviewPipeline::escalate(const repo::ReviewContext& context, const domain::Analysis& analysis,
                                               std::int64_t analysis_id) {
    std::string labels;
    std::string details;
    for (const auto& flag : analysis.severity_flags) {
        labels += (labels.empty() ? "" : ", ") + flag.label;
        details += std::format("- {}{}\n", flag.label, flag.evidence.empty() ? "" : std::format(": \"{}\"", flag.evidence));
    }
    const auto& setup = *context.setup;
    auto body = std::format("A review at {} was flagged: {}.\n\n{}\n\nFlags:\n{}\n"
                            "A holding reply is being drafted for approval. Nothing has been posted.",
                            setup.location_title, labels, review_lines(context), details);
    auto subject = std::format("Escalation: {} - {}", setup.location_title, labels);

    const auto opened = co_await db_.tx([&, now = clock_.now()](pqxx::work& tx) {
        if (!repo::review_is_current(tx, context.review_id, context.updated_time)) return repo::OpenedCase{};
        const auto opened_case = repo::open_case_for_flags(tx, context.review_id, analysis_id, analysis.severity_flags, now);
        if (opened_case.alert) {
            repo::emit(tx,
                       {.type = std::string(repo::kEventEscalation),
                        // The analysis is part of the key: a case that is opened again alerts again.
                        .key = std::format("case:{}:opened:{}", opened_case.id, analysis_id),
                        .location_id = setup.location_id,
                        .review_id = context.review_id,
                        .subject = subject,
                        .body = body,
                        .link_path = std::format("/#/escalations/{}/show", opened_case.id),
                        .escalation_id = opened_case.id},
                       now);
        }
        return opened_case;
    });
    if (opened.alert) log::info("escalation opened", {{"review_id", context.review_id}, {"escalation_id", opened.id}});
}

asio::awaitable<std::optional<repo::NewDraft>> ReviewPipeline::compose_draft(
    const repo::ReviewContext& context, const domain::Analysis& analysis, std::int64_t analysis_id, bool last_attempt,
    const Regeneration* regeneration) {
    const auto review_id = context.review_id;
    const auto& setup = *context.setup;
    const auto& settings = setup.settings;
    const bool has_text = review_has_text(context);

    const auto plan = domain::plan_draft(analysis, context.stars, has_text, settings);
    auto input = domain::generator_input(setup.business_name, settings, context.stars, context.text, plan);
    if (plan.mode == domain::kModeRatingOnly) input.recent_replies = context.recent_rating_only_replies;
    if (regeneration) input.staff_instructions = regeneration->instructions;

    std::optional<domain::GeneratorAnswer> answer;
    // The writer's temperature is a reply setting (D-074), so a location may have its own: it is in the setup.
    auto request = writer_request(setup, input, {.review_id = review_id, .go_live_run_id = {}});
    const auto result = co_await ask(std::move(request));
    if (!result) {
        // A person is waiting for this answer: say so instead of storing an empty draft.
        if (regeneration) co_return std::nullopt;
        if (must_retry(result.error(), last_attempt)) {
            throw std::runtime_error(std::format("review {}: generator: {}", review_id, result.error().message));
        }
    } else {
        answer = domain::parse_generator_answer(result->json);
        if (!answer && regeneration) co_return std::nullopt;
        if (!answer && !last_attempt) {
            throw std::runtime_error(std::format("review {}: generator: the answer has no usable reply", review_id));
        }
    }
    auto outcome = domain::finish_draft(analysis, plan, context.stars, has_text, answer, settings);
    if (regeneration && outcome.route == domain::kRouteAutoReply) {
        // Words a person added to the prompt were never part of what the
        // adversarial suite ran, so such a draft always waits for a person.
        outcome.route = std::string(domain::kRouteApproval);
        outcome.route_reasons.emplace_back("regenerated_with_instructions");
    }

    boost::json::object inputs{
        {"review", boost::json::object{{"stars", context.stars}, {"text", context.text}, {"items_mentioned", boost::json::value_from(plan.items)}}},
        {"business_name", setup.business_name},
        {"mode", plan.mode},
        {"target_language", plan.target.language},
        {"target_script", plan.target.script ? boost::json::value(*plan.target.script) : boost::json::value(nullptr)},
        {"severity_flags", boost::json::value_from(plan.severity_ids)},
        {"recent_replies", boost::json::value_from(input.recent_replies)},
        {"content", boost::json::value_from(settings.content)},
        {"reply", boost::json::value_from(settings.reply)},
        {"temperature", setup.writer.temperature ? boost::json::value(*setup.writer.temperature) : boost::json::value(nullptr)},
    };
    if (regeneration) inputs["staff_instructions"] = regeneration->instructions;
    repo::NewDraft draft{.review_id = review_id,
                         .analysis_id = analysis_id,
                         .review_updated_time = context.updated_time,
                         .mode = plan.mode,
                         .language = plan.target.language,
                         .script = plan.target.script,
                         .outcome = std::move(outcome),
                         .model = setup.writer.model,
                         .prompt_version = std::string(domain::prompt_version()),
                         .inputs = std::move(inputs),
                         .instructions = regeneration ? std::optional(regeneration->instructions) : std::nullopt,
                         .created_by = regeneration ? regeneration->actor.user_id : std::nullopt};
    co_return draft;
}

asio::awaitable<bool> ReviewPipeline::store_own_draft(const repo::ReviewContext& context, const domain::Analysis& analysis,
                                                      const repo::NewDraft& draft) {
    const auto& setup = *context.setup;
    // The words of the "approval needed" message. The review and the draft are in the body only.
    const auto& place = setup.location_title;
    std::string reasons;
    for (const auto& reason : draft.outcome.route_reasons) reasons += (reasons.empty() ? "" : ", ") + reason;
    auto notice_body = std::format(
        "A reply to a review at {} is waiting for approval.\n\n{}\n\nDraft:\n{}\n\nWhat the policy would do: {}{}\n"
        "Nothing is posted until a person approves it.",
        place, review_lines(context),
        draft.outcome.text.empty() ? std::string("(the model gave no reply; write one in the dashboard)") : draft.outcome.text,
        draft.outcome.route, reasons.empty() ? std::string{} : std::format(" ({})", reasons));
    auto notice_subject = std::format("Approval needed: {}", place);

    // Picked here, on the engine's thread: where in the policy's delay range an auto reply falls.
    const double roll = options_.delay_roll ? options_.delay_roll() : random_roll();
    const bool failed_a_check = std::ranges::any_of(draft.outcome.checks, [](const domain::CheckResult& check) { return !check.passed; });
    const bool stored = co_await db_.tx([&, now = clock_.now()](pqxx::work& tx) {
        const auto id = repo::store_draft_if_wanted(tx, draft, std::nullopt, now);
        if (!id) return false;
        // Checks that keep failing pause auto reply at the location (D-072).
        if (failed_a_check) repo::pause_if_checks_keep_failing(tx, setup.location_id, now);
        // The second way a reply gets on its way to Google (D-069): the
        // policy routed this draft to auto reply, and a person switched
        // auto reply on for this location and star rating. Everywhere
        // else, which is everywhere after installation, the draft waits
        // for a person as before.
        bool scheduled = false;
        if (draft.outcome.route == domain::kRouteAutoReply) {
            scheduled = repo::schedule_auto_reply(tx, *id, now, roll);
        }
        // A draft that waits for a person says so. An escalated review
        // already raised its own alert.
        if (!scheduled && draft.outcome.route != domain::kRouteEscalate) {
            repo::emit(tx,
                       {.type = std::string(repo::kEventApprovalNeeded),
                        .key = std::format("draft:{}:approval_needed", *id),
                        .location_id = setup.location_id,
                        .review_id = draft.review_id,
                        .subject = notice_subject,
                        .body = notice_body,
                        .link_path = std::format("/#/drafts/{}/show", draft.review_id)},
                       now);
        }
        return true;
    });
    if (!stored) co_return false;
    // No review text and no reply text in the log: ids and the outcome only.
    log::info("review analyzed", {{"review_id", draft.review_id},
                                  {"severity_flags", analysis.severity_flags.size()},
                                  {"draft", true},
                                  {"mode", draft.mode},
                                  {"route", draft.outcome.route},
                                  {"generation_ok", draft.outcome.generation_ok}});
    co_return true;
}

asio::awaitable<std::optional<std::int64_t>> ReviewPipeline::store_regenerated_draft(const repo::NewDraft& draft,
                                                                                    const Regeneration& regeneration) {
    // A person's action: audited as theirs, and only if the draft they looked at is still the waiting one.
    const auto stored = co_await db_.audited(
        regeneration.actor, clock_.now(), [&](pqxx::work& tx, audit::Writer& audit) -> std::optional<std::int64_t> {
            const auto id = repo::store_draft_if_wanted(tx, draft, regeneration.draft_id, audit.now());
            if (!id) {
                audit.nothing_changed();
                return id;
            }
            audit.record("draft.regenerate", "draft", *id,
                         {.before = {{"draft_id", regeneration.draft_id}},
                          .after = {{"review_id", draft.review_id}, {"instructions", regeneration.instructions}}});
            return id;
        });
    if (stored) {
        log::info("draft regenerated", {{"review_id", draft.review_id}, {"draft_id", *stored}, {"generation_ok", draft.outcome.generation_ok}});
    }
    co_return stored;
}

asio::awaitable<repo::Result<std::int64_t>> ReviewPipeline::regenerate(std::int64_t draft_id, std::string instructions, audit::Actor actor) {
    using Out = repo::Result<std::int64_t>;
    struct Loaded {
        std::string status;
        std::int64_t analysis_id = 0;
        std::optional<repo::ReviewContext> context;
    };
    const auto loaded = co_await db_.tx([draft_id](pqxx::work& tx) -> std::optional<Loaded> {
        const auto draft = repo::draft_basis(tx, draft_id);
        if (!draft) return std::nullopt;
        return Loaded{draft->status, draft->analysis_id, repo::review_context(tx, draft->review_id)};
    });
    // Named locals for every answer, so that no temporary is built inside co_return.
    if (!loaded || !loaded->context) {
        Out missing = repo::not_found("draft");
        co_return missing;
    }
    if (loaded->status != "draft") {
        Out closed = repo::conflict("draft_not_open", "The draft is no longer waiting, so it can't be written again.");
        co_return closed;
    }
    const auto& context = *loaded->context;
    std::string obstacle;
    if (context.removed) {
        obstacle = "The review was removed from Google.";
    } else if (context.has_reply) {
        obstacle = "The review already has a reply.";
    } else if (context.ignored) {
        obstacle = "The review is marked as needing no reply.";
    } else if (!context.synced || context.unsupported()) {
        obstacle = "The review's location isn't synced, or it has no industry profile.";
    } else if (!context.analysis || context.analysis->id != loaded->analysis_id) {
        obstacle = "The review was edited or analyzed again; a new draft replaces this one.";
    }
    if (!obstacle.empty()) {
        Out refused = repo::conflict("review_not_postable", std::move(obstacle));
        co_return refused;
    }
    raise_broken_settings(context);

    const Regeneration regeneration{.draft_id = draft_id, .instructions = std::move(instructions), .actor = std::move(actor)};
    const auto analysis = to_domain(*context.analysis);
    // No draft: the model gave no usable answer. Not stored: the draft or its review changed while the new one was written.
    const auto draft = co_await compose_draft(context, analysis, context.analysis->id, true, &regeneration);
    std::optional<std::int64_t> stored;
    if (draft) stored = co_await store_regenerated_draft(*draft, regeneration);
    if (!stored) {
        // The model's own error text can name hosts; it is in the log (llm_calls), not in the answer.
        Out failed = repo::unavailable("model_unavailable", "The model gave no usable answer. The draft is unchanged; try again.");
        co_return failed;
    }
    co_return *stored;
}

void install(Registry& registry, ReviewPipeline& pipeline) {
    registry.every(std::string(kAnalyzePendingKind), pipeline.options().tick, 1,
                   [&pipeline](const Job&) -> asio::awaitable<void> { co_await pipeline.enqueue_pending(); });
    registry.add(std::string(repo::kAnalyzeReviewKind), [&pipeline](const Job& job) -> asio::awaitable<void> {
        const auto* force = job.payload.is_object() ? job.payload.get_object().if_contains("force") : nullptr;
        co_await pipeline.process(job.payload_id("review_id"), force && force->is_bool() && force->get_bool(), job.last_attempt());
    });
}

}  // namespace poni::jobs
