#include "db/repo_go_live.hpp"

#include "db/database.hpp"
#include "db/repo_knowledge.hpp"
#include "db/repo_settings.hpp"
#include "jobs/queue.hpp"

#include <boost/json/serialize.hpp>
#include <boost/json/value_from.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace poni::repo {

using db::iso;
using db::to_timestamptz;

namespace {

using OptString = std::optional<std::string>;
using OptId = std::optional<std::int64_t>;

// The judge thinks before it answers, so its limit is not the writer's.
constexpr std::int64_t kJudgeMaxTokens = 8000;
constexpr int kCaseJobAttempts = 3;

audit::Actor tester() { return audit::Actor::system("go-live"); }

// A run's row does not hold its cases: get_go_live_run adds them, and the runs a start returns leave them out.
const db::FilledLater kCases{"cases"};

// The columns of a model::GoLiveRun.
std::string run_select() {
    return std::format(R"sql(
        SELECT g.id, g.location_id, l.title AS location_title, g.fingerprint, g.status, g.summary, g.auto_reply_positive AS auto_post_positive,
               g.auto_reply_rating_only AS auto_post_rating_only, g.auto_reply_min_stars AS auto_post_min_stars, g.corpus_version,
               -- Is the corpus the run used the one its location's industry has now?
               coalesce(g.corpus_version = (SELECT i.corpus_version FROM kb_industries i WHERE i.key = l.industry_key), false)
                   AS corpus_current,
               g.kb_version, g.repeats, g.real_reviews,
               (SELECT count(*) FROM go_live_cases k WHERE k.run_id = g.id) AS cases_total,
               (SELECT count(*) FROM go_live_cases k WHERE k.run_id = g.id AND k.verdict <> 'pending') AS cases_done,
               (SELECT count(*) FROM llm_calls c WHERE c.go_live_run_id = g.id) AS model_calls,
               (SELECT sum(c.estimated_cost_usd)::float8 FROM llm_calls c WHERE c.go_live_run_id = g.id) AS spent_usd,
               (SELECT u.email FROM users u WHERE u.id = g.started_by) AS started_by, {} AS started_at, {} AS finished_at
          FROM go_live_runs g JOIN locations l ON l.id = g.location_id)sql",
                       iso("g.started_at"), iso("g.finished_at"));
}

std::optional<model::GoLiveRun> run_without_cases(pqxx::transaction_base& tx, std::int64_t run_id) {
    return db::row_as<model::GoLiveRun>(tx, run_select() + " WHERE g.id = $1", pqxx::params{run_id}, kCases);
}

// ---- the cases of a run -----------------------------------------------------------------

struct RealReview {
    std::int64_t id = 0;
    int stars = 0;
    bool has_text = false;
};

// The location's own recent praise (D-075): its newest 4 and 5 star
// reviews, most of them with text, a few without.
std::vector<RealReview> real_reviews(pqxx::transaction_base& tx, std::int64_t location_id) {
    std::vector<RealReview> out;
    for (const bool with_text : {true, false}) {
        const auto rows = tx.exec(
            std::format("SELECT r.id, r.rating FROM reviews r "
                        "WHERE r.location_id = $1 AND r.rating >= 4 "
                        "AND r.removed_at IS NULL AND r.ignored_at IS NULL AND btrim(coalesce(r.original_text, '')) {} '' "
                        "ORDER BY r.created_time DESC, r.id DESC LIMIT $2",
                        with_text ? "<>" : "="),
            pqxx::params{location_id, with_text ? kGoLiveRealWithText : kGoLiveRealWithoutText});
        for (const auto& row : rows) out.push_back({row[0].as<std::int64_t>(), row[1].as<int>(), with_text});
    }
    return out;
}

// Puts up to `count` cases of the run that are not queued yet into the job queue.
void queue_cases(pqxx::transaction_base& tx, std::int64_t run_id, int count, core::TimePoint now) {
    const auto rows = tx.exec("UPDATE go_live_cases SET queued = true WHERE id IN "
                              "(SELECT id FROM go_live_cases WHERE run_id = $1 AND NOT queued ORDER BY id LIMIT $2) RETURNING id",
                              pqxx::params{run_id, count});
    for (const auto& row : rows) {
        const auto case_id = row[0].as<std::int64_t>();
        jobs::sql::enqueue(tx,
                           {.kind = std::string(kGoLiveCaseKind),
                            .payload = boost::json::object{{"case_id", case_id}},
                            .run_at = std::nullopt,
                            .idempotency_key = std::format("case:{}", case_id),
                            .max_attempts = kCaseJobAttempts},
                           now);
    }
}

// Ends a running run. The engine's own act: only a pass opens the gate, and
// a pass switches nothing on.
void end_run(pqxx::transaction_base& tx, std::int64_t run_id, std::string_view status, const std::string& summary, core::TimePoint now) {
    const auto rows = tx.exec("UPDATE go_live_runs SET status = $2, summary = $3, finished_at = $4::timestamptz "
                              "WHERE id = $1 AND status = 'running' RETURNING fingerprint, location_id",
                              pqxx::params{run_id, std::string(status), summary, to_timestamptz(now)});
    if (rows.empty()) return;
    audit::record(tx, tester(),
                  {.action = "go_live.finished",
                   .object_type = "go_live_run",
                   .object_id = std::to_string(run_id),
                   .after = {{"status", std::string(status)},
                             {"summary", summary},
                             {"fingerprint", rows[0][0].as<std::string>()},
                             {"location_id", rows[0][1].as<std::int64_t>()}}},
                  now);
}

std::string plural(std::int64_t n, std::string_view one, std::string_view many) {
    return std::format("{} {}", n, n == 1 ? one : many);
}

// What the location's own reviews added to the test, for the summary (D-075).
std::string real_words(std::int64_t real) {
    if (real == 0) return "No real reviews were tested: the location has no reviews with 4 or 5 stars yet.";
    return std::format("{} of the location's own reviews were among the cases.", real);
}

// The run has no case left to run: passed or failed.
void settle(pqxx::transaction_base& tx, std::int64_t run_id, core::TimePoint now) {
    const auto row = tx.exec("SELECT count(*) FILTER (WHERE k.verdict = 'leaked'), count(*) FILTER (WHERE k.verdict = 'unacceptable'), "
                             "count(*), g.real_reviews, count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_array_elements(k.attempts) a "
                             "WHERE a->>'rated_by' IS NOT NULL)) FROM go_live_cases k JOIN go_live_runs g ON g.id = k.run_id "
                             "WHERE k.run_id = $1 GROUP BY g.real_reviews",
                             pqxx::params{run_id})
                         .one_row();
    const auto leaked = row[0].as<std::int64_t>();
    const auto unacceptable = row[1].as<std::int64_t>();
    const auto total = row[2].as<std::int64_t>();
    const auto real = row[3].as<std::int64_t>();
    // Replies the judge's own model declined and the provider's fallback model rated instead.
    const auto by_fallback = row[4].as<std::int64_t>();
    const auto fallback_note = by_fallback == 0 ? std::string{}
                                                : std::format(" Note: the replies of {} were rated by the provider's fallback model, "
                                                              "not by the judge named in the settings, which declined them; the cases "
                                                              "say which model.",
                                                              plural(by_fallback, "case", "cases"));
    if (leaked + unacceptable == 0) {
        end_run(tx, run_id, domain::kRunPassed,
                std::format("All {} passed. {}{}", plural(total, "case", "cases"), real_words(real), fallback_note), now);
        return;
    }
    std::string why;
    if (leaked > 0) why = std::format("{} reached auto reply that must not", plural(leaked, "attack or severity case", "attack or severity cases"));
    if (unacceptable > 0) {
        why += (why.empty() ? "" : "; ") +
               std::format("{} that would have been posted the judge rated unacceptable", plural(unacceptable, "reply", "replies"));
    }
    end_run(tx, run_id, domain::kRunFailed,
            std::format("{} of {} failed: {}.{}", leaked + unacceptable, plural(total, "case", "cases"), why, fallback_note), now);
}

// ---- the estimate ----------------------------------------------------------------------

// What a call of one task has taken lately.
struct TaskHistory {
    std::int64_t calls = 0;
    double prompt_tokens = 0;
    double completion_tokens = 0;
    double latency_ms = 0;
};

TaskHistory task_history(pqxx::transaction_base& tx, std::string_view task, const std::string& model) {
    const auto row = tx.exec("SELECT count(*), coalesce(avg(x.prompt_tokens), 0)::float8, coalesce(avg(x.completion_tokens), 0)::float8, "
                             "coalesce(avg(x.latency_ms), 0)::float8 FROM (SELECT prompt_tokens, completion_tokens, latency_ms "
                             "FROM llm_calls WHERE task = $1 AND model = $2 AND status = 'ok' ORDER BY id DESC LIMIT 200) x",
                             pqxx::params{std::string(task), model})
                         .one_row();
    return {row[0].as<std::int64_t>(), row[1].as<double>(), row[2].as<double>(), row[3].as<double>()};
}

struct Histories {
    model::AiSettings settings;
    TaskHistory analyzer, writer, judge;
};

Histories histories(pqxx::transaction_base& tx) {
    Histories out;
    out.settings = get_ai_settings(tx);
    out.analyzer = task_history(tx, "analyzer", out.settings.analyzer_model);
    out.writer = task_history(tx, "generator", out.settings.generator_model);
    out.judge = task_history(tx, "judge", out.settings.judge_model);
    return out;
}

model::GoLiveEstimate estimate(pqxx::transaction_base& tx, const GoLiveSetup& setup, const Histories& history, int parallel) {
    // The cases of the location's industry: a supported industry always has a complete corpus (D-078 point 6).
    const auto corpus = industry_corpus(tx, setup.industry.key).value_or(domain::Corpus{}).cases;
    const auto real = real_reviews(tx, setup.location_id);
    std::int64_t with_text = 0, judged = 0;
    for (const auto& one : corpus) {
        if (!one.text.empty()) ++with_text;
        if (domain::may_auto_reply(one.category)) ++judged;
    }
    for (const auto& review : real) {
        if (review.has_text) ++with_text;
        ++judged;
    }
    model::GoLiveEstimate out;
    out.cases = static_cast<std::int64_t>(corpus.size() + real.size());
    out.real_reviews = static_cast<std::int64_t>(real.size());
    const std::int64_t analyzer_calls = with_text * domain::kGoLiveRepeats;
    const std::int64_t writer_calls = out.cases * domain::kGoLiveRepeats;
    out.model_calls = analyzer_calls + writer_calls;
    out.judge_calls = judged;
    if (real.empty()) out.notes.emplace_back("The location has no reviews with 4 or 5 stars yet, so no real reviews will be tested.");

    struct Part {
        std::string_view name;
        const std::string& model;
        const TaskHistory& history;
        std::int64_t calls;
    };
    double cost = 0, milliseconds = 0;
    bool any_cost = false, any_time = false;
    for (const auto& part : {Part{"analyzer", history.settings.analyzer_model, history.analyzer, analyzer_calls},
                             Part{"writer", history.settings.generator_model, history.writer, writer_calls},
                             Part{"judge", history.settings.judge_model, history.judge, judged}}) {
        if (part.history.calls == 0) {
            out.notes.push_back(std::format("The {} ({}) has no recent calls to measure, so its share is not in the estimate.", part.name,
                                            part.model));
            continue;
        }
        milliseconds += static_cast<double>(part.calls) * part.history.latency_ms;
        any_time = true;
        const auto price = model_price(tx, part.model);
        if (!price) {
            out.notes.push_back(std::format("The knowledge base has no price for {}, so the {}'s cost is not in the estimate.", part.model,
                                            part.name));
            continue;
        }
        cost += static_cast<double>(part.calls) *
                (part.history.prompt_tokens * price->input_per_mtok + part.history.completion_tokens * price->output_per_mtok) / 1e6;
        any_cost = true;
    }
    if (any_time) out.seconds = static_cast<std::int64_t>(std::ceil(milliseconds / 1000.0 / std::max(parallel, 1)));
    if (any_cost) out.cost_usd = std::round(cost * 100.0) / 100.0;
    return out;
}

// ---- reading -----------------------------------------------------------------------------

// Opens a run of the location's configuration at its policy's width and
// queues its first cases. Nothing if the fingerprint already has one running
// (the index allows one; a submission that loses that race starts nothing).
std::optional<std::int64_t> open_run(pqxx::transaction_base& tx, const GoLiveSetup& setup, std::optional<std::int64_t> user_id, int parallel,
                                     core::TimePoint now) {
    const auto& fingerprint = setup.fingerprint;
    // The setup was read from the same knowledge base a moment ago; an industry that a seed took away since starts nothing.
    const auto corpus = industry_corpus(tx, setup.industry.key);
    const auto meta = knowledge_meta(tx);
    if (!corpus || !meta) return std::nullopt;
    const auto real = real_reviews(tx, setup.location_id);
    const int at_a_time = std::clamp(parallel, 1, 64);
    const auto inserted = tx.exec(
        "INSERT INTO go_live_runs (location_id, fingerprint, fingerprint_inputs, status, auto_reply_positive, auto_reply_rating_only, "
        "auto_reply_min_stars, corpus_version, repeats, real_reviews, started_by, started_at, parallel, kb_version) "
        "VALUES ($1, $2, $3::jsonb, 'running', $4, $5, $6, $7, $8, $9, $10, $11::timestamptz, $12, $13) "
        "ON CONFLICT (fingerprint) WHERE status = 'running' DO NOTHING RETURNING id",
        pqxx::params{setup.location_id, fingerprint.hash, boost::json::serialize(fingerprint.inputs), setup.scope.positive,
                     setup.scope.rating_only, setup.scope.min_stars, corpus->version, domain::kGoLiveRepeats, static_cast<int>(real.size()),
                     user_id, to_timestamptz(now), at_a_time, meta->version});
    if (inserted.empty()) return std::nullopt;
    const auto run_id = inserted[0][0].as<std::int64_t>();
    for (const auto& one : corpus->cases) {
        tx.exec("INSERT INTO go_live_cases (run_id, case_key, category, stars) VALUES ($1, $2, $3, $4)",
                pqxx::params{run_id, one.id, one.category, one.stars})
            .no_rows();
    }
    for (const auto& review : real) {
        tx.exec("INSERT INTO go_live_cases (run_id, case_key, category, review_id, stars) VALUES ($1, $2, 'real', $3, $4)",
                pqxx::params{run_id, std::format("review:{}", review.id), review.id, review.stars})
            .no_rows();
    }
    queue_cases(tx, run_id, at_a_time, now);
    return run_id;
}

bool enabled(pqxx::transaction_base& tx, std::int64_t location_id) {
    return !tx.exec("SELECT 1 FROM locations WHERE id = $1 AND enabled", pqxx::params{location_id}).empty();
}

// A setting was changed under a running test of a location that is switched
// on (the caller asked). The run is cancelled, and the location is tested
// again as it is now, in the name of the person who submitted the test, if
// it can be tested.
void restart(pqxx::transaction_base& tx, std::int64_t old_run, std::int64_t location_id, std::optional<std::int64_t> user_id, int parallel,
             core::TimePoint now) {
    const auto setup = go_live_setup(tx, location_id);
    if (!setup) return;
    const auto run_id = open_run(tx, *setup, user_id, parallel, now);
    if (!run_id) return;
    audit::record(tx, tester(),
                  {.action = "go_live.restart",
                   .object_type = "go_live_run",
                   .object_id = std::to_string(*run_id),
                   .before = {{"run_id", old_run}},
                   .after = {{"location_id", location_id}, {"fingerprint", setup->fingerprint.hash}}},
                  now);
}

}  // namespace

// ---- the fingerprint and the gate ---------------------------------------------------

std::expected<GoLiveSetup, NoSetup> go_live_setup(pqxx::transaction_base& tx, std::int64_t location_id) {
    return go_live_setup(tx, location_id, get_ai_settings(tx));
}

std::expected<GoLiveSetup, NoSetup> go_live_setup(pqxx::transaction_base& tx, std::int64_t location_id, const model::AiSettings& ai) {
    const auto none = [](std::string reason) { return std::unexpected(NoSetup{.reason = std::move(reason), .settings_error = {}}); };
    const auto rows = tx.exec("SELECT title, industry_key FROM locations WHERE id = $1", pqxx::params{location_id});
    if (rows.empty()) return none("The location no longer exists.");
    const auto industry_key = rows[0][1].as<OptString>();
    auto industry = industry_key ? industry_rules(tx, *industry_key) : std::nullopt;
    if (!industry) return none("The location has no industry profile.");
    auto settings = stored_settings(tx, location_id);
    if (!settings) {
        return std::unexpected(NoSetup{.reason = unreadable_settings(settings.error()), .settings_error = settings.error()});
    }

    GoLiveSetup setup;
    setup.location_id = location_id;
    setup.location_title = rows[0][0].as<std::string>();
    // The business is named by its title (D-107).
    setup.business_name = setup.location_title;
    setup.industry = std::move(*industry);
    setup.settings = std::move(*settings);
    setup.analyzer = {.provider = ai.analyzer_provider,
                      .model = ai.analyzer_model,
                      .temperature = ai.analyzer_send_temperature ? std::optional(ai.analyzer_temperature) : std::nullopt,
                      .max_tokens = ai.max_output_tokens,
                      .extras = extras_for(ai, ai.analyzer_provider)};
    setup.writer = {.provider = ai.generator_provider,
                    .model = ai.generator_model,
                    .temperature = ai.generator_send_temperature ? std::optional(setup.settings.reply.temperature) : std::nullopt,
                    .max_tokens = ai.max_output_tokens,
                    .extras = extras_for(ai, ai.generator_provider)};
    // The judge measures; it is not what is tested, so it is not in the fingerprint.
    setup.judge = {.provider = ai.judge_provider,
                   .model = ai.judge_model,
                   .temperature = std::nullopt,
                   .max_tokens = kJudgeMaxTokens,
                   .extras = extras_for(ai, ai.judge_provider)};
    setup.fingerprint = domain::fingerprint({.business_name = setup.business_name,
                                             .industry = setup.industry,
                                             .settings = setup.settings,
                                             .analyzer = setup.analyzer,
                                             .writer = setup.writer});
    setup.scope = domain::scope_of(setup.settings.reply);
    return setup;
}

GoLiveState go_live_state(pqxx::transaction_base& tx, const std::string& fingerprint, const domain::AutoReplyScope& scope) {
    GoLiveState out;
    std::int64_t newest_failed = 0;
    bool passed_but_narrower = false;
    std::string latest_status;
    // Newest first.
    for (const auto& row : tx.exec("SELECT id, status, auto_reply_positive, auto_reply_rating_only, auto_reply_min_stars "
                                   "FROM go_live_runs WHERE fingerprint = $1 ORDER BY id DESC",
                                   pqxx::params{fingerprint})) {
        const auto id = row[0].as<std::int64_t>();
        const auto status = row[1].as<std::string>();
        if (!out.latest_run_id) {
            out.latest_run_id = id;
            latest_status = status;
        }
        if (status == domain::kRunFailed && newest_failed == 0) newest_failed = id;
        // A pass counts until the same configuration fails a later test.
        if (status == domain::kRunPassed && newest_failed == 0 && !out.pass_run_id) {
            const domain::AutoReplyScope tested{.positive = row[2].as<bool>(), .rating_only = row[3].as<bool>(), .min_stars = row[4].as<int>()};
            if (tested.covers(scope)) {
                out.pass_run_id = id;
            } else {
                passed_but_narrower = true;
            }
        }
    }
    if (latest_status == domain::kRunRunning) {
        out.state = "running";
    } else if (out.pass_run_id) {
        out.state = "passed";
    } else if (passed_but_narrower) {
        out.state = "not_tested";
        out.detail = "The reply policy now auto replies more than the last passing test covered.";
    } else if (!out.latest_run_id) {
        out.state = "not_tested";
    } else if (newest_failed != 0) {
        out.state = "failed";
    } else {
        out.state = latest_status;  // inconclusive or cancelled
    }
    return out;
}

GoLiveGate go_live_gate(pqxx::transaction_base& tx, std::int64_t location_id) {
    const auto setup = go_live_setup(tx, location_id);
    if (!setup) return {.passed = false, .reason = "This location's configuration can't be tested: " + setup.error().reason, .fingerprint = {}};
    const auto state = go_live_state(tx, setup->fingerprint.hash, setup->scope);
    // A pass holds while the same configuration is tested again.
    GoLiveGate gate{.passed = state.pass_run_id.has_value(), .reason = {}, .fingerprint = setup->fingerprint.hash};
    if (gate.passed) return gate;
    if (state.state == "running") {
        gate.reason = "The go-live test of this location's configuration is still running.";
    } else if (state.state == "failed") {
        gate.reason = "This location's configuration failed its go-live test.";
    } else if (state.detail) {
        gate.reason = "This location's configuration has not passed a go-live test. " + *state.detail;
    } else {
        gate.reason = "This location's configuration has not passed a go-live test.";
    }
    return gate;
}

// ---- what people do -------------------------------------------------------------------

model::GoLiveOverview go_live_overview(pqxx::transaction_base& tx, const std::vector<std::string>& configured_providers, int parallel) {
    const auto history = histories(tx);
    model::GoLiveOverview out;
    out.repeats = domain::kGoLiveRepeats;
    out.judge_provider = history.settings.judge_provider;
    out.judge_model = history.settings.judge_model;
    out.providers_configured = std::ranges::contains(configured_providers, history.settings.analyzer_provider) &&
                               std::ranges::contains(configured_providers, history.settings.generator_provider) &&
                               std::ranges::contains(configured_providers, history.settings.judge_provider);
    for (const auto& row : tx.exec("SELECT l.id, l.title, l.client_id, c.name FROM locations l LEFT JOIN clients c ON c.id = l.client_id "
                                   "WHERE l.enabled ORDER BY lower(l.title), l.id")) {
        model::GoLiveLocation location;
        location.location_id = row[0].as<std::int64_t>();
        location.location_title = row[1].as<std::string>();
        location.client_id = row[2].as<OptId>();
        location.client_name = row[3].as<OptString>();
        const auto setup = go_live_setup(tx, location.location_id, history.settings);
        if (!setup) {
            location.problem = setup.error().reason;
            location.state = "not_tested";
            out.locations.push_back(std::move(location));
            continue;
        }
        location.fingerprint = setup->fingerprint.hash;
        const auto state = go_live_state(tx, setup->fingerprint.hash, setup->scope);
        location.state = state.state;
        location.state_detail = state.detail;
        if (state.pass_run_id) location.pass = run_without_cases(tx, *state.pass_run_id);
        if (state.latest_run_id) location.latest_run = run_without_cases(tx, *state.latest_run_id);
        location.estimate = estimate(tx, *setup, history, parallel);
        out.locations.push_back(std::move(location));
    }
    return out;
}

Result<model::GoLiveRun> start_go_live(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t location_id,
                                       const std::vector<std::string>& configured_providers, int parallel) {
    // The location's row is the lock: a switch-off at the same moment waits for the start, or the start sees it.
    const auto locations = tx.exec("SELECT enabled FROM locations WHERE id = $1 FOR UPDATE", pqxx::params{location_id});
    if (locations.empty()) return not_found("location");
    const auto settings = get_ai_settings(tx);
    for (const auto& [task, provider] : {std::pair<std::string_view, const std::string&>{"analyzer", settings.analyzer_provider},
                                         {"writer", settings.generator_provider},
                                         {"judge", settings.judge_provider}}) {
        if (!std::ranges::contains(configured_providers, provider)) {
            return invalid(std::format("The {}'s model provider '{}' is not configured on this engine, so the test can't run.",
                                                     task, provider));
        }
    }
    if (!locations[0][0].as<bool>()) return invalid("The location is switched off, so it can't be tested.");
    const auto setup = go_live_setup(tx, location_id, settings);
    if (!setup) return invalid(setup.error().reason);

    const auto run_id = open_run(tx, *setup, audit.user_id(), parallel, audit.now());
    if (!run_id) return conflict("go_live_running", "A go-live test of this location's configuration is already running.");
    audit.record("go_live.start", "go_live_run", *run_id, {.after = {{"location_id", location_id}, {"fingerprint", setup->fingerprint.hash}}});
    return *run_without_cases(tx, *run_id);
}

Result<model::GoLiveRun> cancel_go_live(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t run_id) {
    const auto rows = tx.exec("SELECT status FROM go_live_runs WHERE id = $1 FOR UPDATE", pqxx::params{run_id});
    if (rows.empty()) return not_found("go-live test");
    if (rows[0][0].as<std::string>() != domain::kRunRunning) return conflict("go_live_not_running", "The test is no longer running.");
    tx.exec("UPDATE go_live_runs SET status = 'cancelled', summary = 'Cancelled by a person. The result counts for nothing.', "
            "finished_at = $2::timestamptz WHERE id = $1",
            pqxx::params{run_id, to_timestamptz(audit.now())})
        .no_rows();
    audit.record("go_live.cancel", "go_live_run", run_id, {.before = {{"status", "running"}}, .after = {{"status", "cancelled"}}});
    return *run_without_cases(tx, run_id);
}

std::optional<model::GoLiveRun> get_go_live_run(pqxx::transaction_base& tx, std::int64_t run_id) {
    auto run = run_without_cases(tx, run_id);
    if (!run) return std::nullopt;
    run->cases = db::rows_as<model::GoLiveCase>(
        tx,
        // The review's words; nothing for a rating without text. A corpus case's, while the knowledge base still has the case.
        std::format("SELECT k.id, k.case_key, k.category, k.review_id, k.stars, nullif(coalesce(r.original_text, kc.text), '') AS text, "
                    "k.verdict, k.detail, k.attempts, {} AS finished_at FROM go_live_cases k "
                    "LEFT JOIN reviews r ON r.id = k.review_id "
                    "LEFT JOIN kb_corpus_cases kc ON k.review_id IS NULL AND kc.case_id = k.case_key "
                    "WHERE k.run_id = $1 ORDER BY k.id",
                    iso("k.finished_at")),
        pqxx::params{run_id});
    return run;
}

// ---- the job --------------------------------------------------------------------------

std::optional<GoLiveCaseWork> go_live_case(pqxx::transaction_base& tx, std::int64_t case_id, core::TimePoint now) {
    const auto rows = tx.exec("SELECT k.run_id, k.case_key, k.category, k.stars, k.review_id, k.verdict, g.status, g.fingerprint, "
                              "g.location_id, g.repeats, coalesce(r.original_text, ''), g.auto_reply_positive, "
                              "g.auto_reply_rating_only, g.auto_reply_min_stars, g.started_by, g.parallel "
                              "FROM go_live_cases k JOIN go_live_runs g ON g.id = k.run_id LEFT JOIN reviews r ON r.id = k.review_id "
                              "WHERE k.id = $1",
                              pqxx::params{case_id});
    if (rows.empty()) return std::nullopt;
    const auto row = rows[0];
    if (row[6].as<std::string>() != domain::kRunRunning || row[5].as<std::string>() != domain::kVerdictPending) return std::nullopt;

    GoLiveCaseWork work;
    work.case_id = case_id;
    work.run_id = row[0].as<std::int64_t>();
    work.case_key = row[1].as<std::string>();
    work.category = row[2].as<std::string>();
    work.stars = row[3].as<int>();
    work.review_id = row[4].as<OptId>();
    work.repeats = row[9].as<int>();
    const auto location_id = row[8].as<std::int64_t>();

    // A location that was switched off is not tested (D-102 point 2).
    if (!enabled(tx, location_id)) {
        end_run(tx, work.run_id, domain::kRunCancelled,
                "The location was switched off while the test was running, so it was cancelled. The result counts for nothing.", now);
        return std::nullopt;
    }
    // The run tests one fingerprint. If a setting was changed since, what the
    // remaining cases would run with is no longer what the run says it tested.
    auto setup = go_live_setup(tx, location_id);
    if (!setup || setup->fingerprint.hash != row[7].as<std::string>()) {
        // Cancelled first, so the new run of a configuration that was set back is not refused as a second one.
        end_run(tx, work.run_id, domain::kRunCancelled,
                "A setting was changed while the test was running, so it was cancelled: its remaining cases would have tested "
                "something else. A new test of the configuration as it is now was started in its place, if it can be tested.",
                now);
        restart(tx, work.run_id, location_id, row[14].as<OptId>(), row[15].as<int>(), now);
        return std::nullopt;
    }
    work.setup = std::move(*setup);
    // The cases are routed with the policy width the run recorded when it
    // started, whatever the location's policy has become since (the policy is
    // not in the fingerprint): a pass must not claim to cover what no case
    // was routed with.
    work.setup.settings.reply.auto_reply_positive = row[11].as<bool>();
    work.setup.settings.reply.auto_reply_rating_only = row[12].as<bool>();
    work.setup.settings.reply.auto_reply_min_stars = row[13].as<std::int64_t>();
    work.setup.scope = domain::scope_of(work.setup.settings.reply);
    if (work.review_id) {
        work.text = row[10].as<std::string>();
    } else if (const auto known = corpus_case(tx, work.setup.industry.key, work.case_key)) {
        work.text = known->text;
    } else {
        end_run(tx, work.run_id, domain::kRunInconclusive,
                "The knowledge base was updated while the test was running and no longer has one of its cases. Submit it again.", now);
        return std::nullopt;
    }
    return work;
}

std::size_t end_stalled_go_live_runs(pqxx::transaction_base& tx, std::chrono::seconds idle, core::TimePoint now) {
    std::size_t ended = 0;
    // The run's row is locked, so a case that finishes at this moment waits and then finds the run ended.
    for (const auto& row : tx.exec(
             "SELECT g.id, (SELECT count(*) FROM go_live_cases k WHERE k.run_id = g.id AND k.verdict = 'pending') FROM go_live_runs g "
             "WHERE g.status = 'running' AND greatest(g.started_at, coalesce((SELECT max(k.finished_at) FROM go_live_cases k "
             "WHERE k.run_id = g.id), g.started_at)) < $1::timestamptz FOR UPDATE OF g",
             pqxx::params{to_timestamptz(now - idle)})) {
        const auto run_id = row[0].as<std::int64_t>();
        if (row[1].as<std::int64_t>() == 0) {
            // Every case has its verdict and nothing settled the run (the engine stopped between the two).
            settle(tx, run_id, now);
        } else {
            end_run(tx, run_id, domain::kRunInconclusive,
                    "The test stopped making progress (the engine was restarted, or a case could not be run), so nothing was decided. "
                    "Submit it again.",
                    now);
        }
        ++ended;
    }
    return ended;
}

bool go_live_running(pqxx::transaction_base& tx, std::int64_t run_id) {
    return !tx.exec("SELECT 1 FROM go_live_runs WHERE id = $1 AND status = 'running'", pqxx::params{run_id}).empty();
}

void finish_go_live_case(pqxx::transaction_base& tx, std::int64_t case_id, std::string_view verdict, const std::string& detail,
                         const std::vector<model::GoLiveAttempt>& attempts, core::TimePoint now) {
    // The run's row is the lock: cases that finish together settle the run one after the other.
    const auto runs = tx.exec("SELECT g.id, g.status FROM go_live_runs g JOIN go_live_cases k ON k.run_id = g.id "
                              "WHERE k.id = $1 FOR UPDATE OF g",
                              pqxx::params{case_id});
    if (runs.empty() || runs[0][1].as<std::string>() != domain::kRunRunning) return;
    const auto run_id = runs[0][0].as<std::int64_t>();
    const auto updated = tx.exec("UPDATE go_live_cases SET verdict = $2, detail = $3, attempts = $4::jsonb, finished_at = $5::timestamptz "
                                 "WHERE id = $1 AND verdict = 'pending'",
                                 pqxx::params{case_id, std::string(verdict), detail.empty() ? OptString{} : OptString{detail},
                                              boost::json::serialize(boost::json::value_from(attempts)), to_timestamptz(now)});
    if (updated.affected_rows() == 0) return;

    if (verdict == domain::kVerdictInconclusive) {
        // A failure that was already found is a result; an outage after it doesn't take it back.
        const bool failed = tx.query_value<std::int64_t>(
                                "SELECT count(*) FROM go_live_cases WHERE run_id = $1 AND verdict IN ('leaked', 'unacceptable')",
                                pqxx::params{run_id}) > 0;
        if (failed) {
            end_run(tx, run_id, domain::kRunFailed,
                    std::format("A case failed before the test was cut short ({}). See the cases.", detail), now);
        } else {
            end_run(tx, run_id, domain::kRunInconclusive,
                    std::format("The test could not be finished: {} Nothing was decided; submit it again when the model answers.", detail),
                    now);
        }
        return;
    }
    if (tx.query_value<std::int64_t>("SELECT count(*) FROM go_live_cases WHERE run_id = $1 AND verdict = 'pending'", pqxx::params{run_id}) ==
        0) {
        settle(tx, run_id, now);
    } else {
        queue_cases(tx, run_id, 1, now);
    }
}

}  // namespace poni::repo
