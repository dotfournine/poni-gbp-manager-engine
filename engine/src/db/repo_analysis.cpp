#include "db/repo_analysis.hpp"

#include "db/database.hpp"
#include "db/repo_auth.hpp"
#include "db/repo_posting.hpp"
#include "db/repo_settings.hpp"
#include "jobs/queue.hpp"

#include <boost/json/serialize.hpp>
#include <boost/json/value_from.hpp>

#include <format>

namespace poni::repo {

using db::iso;
using db::to_timestamptz;

namespace {

using OptString = std::optional<std::string>;

template <class T>
std::string to_json_text(const T& value) {
    return boost::json::serialize(boost::json::value_from(value));
}

// Which reviews the pipeline works on, and which of them still need work.
// A client may set the day its analysis starts from (D-055); older reviews
// are left alone unless staff ask for one by hand.
// `a` is the newest analysis of the review's current version.
constexpr std::string_view kPendingFrom = R"sql(
      FROM reviews r
      JOIN locations l ON l.id = r.location_id
      JOIN clients c ON c.id = l.client_id
      LEFT JOIN LATERAL (SELECT x.id, x.severity_flags FROM review_analyses x
                          WHERE x.review_id = r.id AND x.review_updated_time = r.updated_time
                          ORDER BY x.id DESC LIMIT 1) a ON true
     WHERE r.removed_at IS NULL AND l.access_status = 'active' AND NOT c.archived
       -- A client without an industry, or with one the knowledge base doesn't have, is not analyzed (D-078 point 5).
       AND c.industry_key IN (SELECT k.key FROM kb_industries k)
       AND (c.analyze_from IS NULL OR r.created_time >= (c.analyze_from::timestamp AT TIME ZONE 'UTC'))
       AND (a.id IS NULL
            -- No reply, nobody said "no reply needed", and no draft that settles this version.
            OR (r.reply_text IS NULL AND r.ignored_at IS NULL
                AND NOT EXISTS (SELECT 1 FROM reply_drafts d
                                 WHERE d.review_id = r.id AND d.review_updated_time = r.updated_time
                                   AND d.status IN ('draft', 'scheduled', 'approved', 'posting', 'posted', 'published', 'rejected')))
            -- Flagged, unanswered, and no escalation yet: analyzed before
            -- escalations existed, or the job died before opening one.
            OR (r.reply_text IS NULL AND jsonb_array_length(a.severity_flags) > 0
                AND NOT EXISTS (SELECT 1 FROM cases k WHERE k.review_id = r.id))))sql";

// A draft on its way: scheduled, approved or sent. It stands, and nothing replaces it.
constexpr std::string_view kDraftsInFlight = "('scheduled', 'approved', 'posting', 'posted')";

// The columns of a model::ReviewAnalysis.
std::string analysis_select() {
    return std::format(R"sql(
        SELECT a.id, a.status, a.error, a.language, a.script, a.sentiment, a.contains_complaint, a.topics, a.items,
               a.severity_flags, a.manipulation_suspected, a.manipulation_evidence, a.summary_en,
               a.industry_key, a.model, a.prompt_version, a.review_updated_time = r.updated_time AS is_current,
               {} AS created_at
          FROM review_analyses a JOIN reviews r ON r.id = a.review_id)sql",
                       iso("a.created_at"));
}

// The columns of a model::ReplyDraft, with its newest post. `inputs` is
// "d.inputs" for the detail view and "NULL::jsonb" in lists.
std::string draft_select(std::string_view inputs) {
    return std::format(R"sql(
        SELECT d.id, d.review_id, d.status, d.status_reason, d.route, d.route_reasons, d.mode, d.text,
               d.language, d.script, d.generator_manipulation_suspected, d.notes_for_staff, d.checks,
               d.model, d.prompt_version, {} AS inputs, d.generated_text, d.instructions,
               (SELECT u.email FROM users u WHERE u.id = d.created_by) AS created_by,
               (SELECT u.email FROM users u WHERE u.id = d.edited_by) AS edited_by, {} AS edited_at,
               (SELECT u.email FROM users u WHERE u.id = d.decided_by) AS decided_by, {} AS decided_at,
               CASE WHEN p.id IS NOT NULL
                    THEN jsonb_build_object('id', p.id, 'status', p.status, 'started_at', {}, 'posted_at', {},
                                            'published_at', {}, 'error', p.error) END AS post,
               {} AS post_at, d.policy_version, {} AS created_at, {} AS updated_at
          FROM reply_drafts d
          LEFT JOIN LATERAL (SELECT x.* FROM reply_posts x WHERE x.draft_id = d.id ORDER BY x.id DESC LIMIT 1) p ON true)sql",
                       inputs, iso("d.edited_at"), iso("d.decided_at"), iso("p.started_at"), iso("p.posted_at"), iso("p.published_at"),
                       iso("d.post_at"), iso("d.created_at"), iso("d.updated_at"));
}

// The sums of one grouping, as model::LlmUsageRow; `key` and `label` are SQL
// expressions, and `order` counts the columns (7 is the cost).
std::string usage_query(std::string_view key, std::string_view label, std::string_view order) {
    return std::format(R"sql(
        SELECT {0} AS key, {1} AS label, count(*) AS calls, count(*) FILTER (WHERE k.status = 'error') AS errors,
               coalesce(sum(k.prompt_tokens), 0)::bigint AS prompt_tokens,
               coalesce(sum(k.completion_tokens), 0)::bigint AS completion_tokens,
               coalesce(sum(k.estimated_cost_usd), 0)::float8 AS cost_usd,
               count(*) FILTER (WHERE k.estimated_cost_usd IS NULL AND k.prompt_tokens + k.completion_tokens > 0) AS unpriced_calls,
               coalesce(round(avg(k.latency_ms)), 0)::bigint AS average_latency_ms
          FROM llm_calls k LEFT JOIN clients c ON c.id = k.client_id LEFT JOIN locations l ON l.id = k.location_id
         WHERE k.occurred_at >= $1::timestamptz AND k.occurred_at < $2::timestamptz
         GROUP BY 1, 2 ORDER BY {2})sql",
                       key, label, order);
}

}  // namespace

// ---- the pipeline job ----------------------------------------------------------

std::vector<PendingReview> pending_reviews(pqxx::transaction_base& tx, std::int64_t limit) {
    if (limit <= 0) return {};
    return db::rows_as<PendingReview>(
        tx,
        std::format("SELECT r.id, {} AS updated_time, a.id IS NULL AS needs_analysis {} "
                    "AND NOT EXISTS (SELECT 1 FROM jobs j WHERE j.kind = $2 AND j.status IN ('queued', 'running') "
                    "                 AND (j.payload->>'review_id')::bigint = r.id) "
                    "ORDER BY (r.reply_text IS NULL) DESC, r.created_time DESC, r.id DESC LIMIT $1",
                    iso("r.updated_time"), kPendingFrom),
        pqxx::params{limit, std::string(kAnalyzeReviewKind)});
}

std::int64_t active_analysis_jobs(pqxx::transaction_base& tx) {
    return tx.query_value<std::int64_t>("SELECT count(*) FROM jobs WHERE kind = $1 AND status IN ('queued', 'running')",
                                        pqxx::params{std::string(kAnalyzeReviewKind)});
}

std::optional<ReviewContext> review_context(pqxx::transaction_base& tx, std::int64_t review_id) {
    const auto rows = tx.exec(
        std::format(R"sql(
        SELECT r.location_id, coalesce(r.rating, 0), coalesce(r.original_text, ''), {}, r.reply_text IS NOT NULL,
               r.removed_at IS NOT NULL, l.access_status = 'active' AND c.id IS NOT NULL AND NOT c.archived, r.ignored_at IS NOT NULL
          FROM reviews r JOIN locations l ON l.id = r.location_id LEFT JOIN clients c ON c.id = l.client_id
         WHERE r.id = $1)sql",
                    iso("r.updated_time")),
        pqxx::params{review_id});
    if (rows.empty()) return std::nullopt;
    const auto row = rows[0];

    ReviewContext context;
    context.review_id = review_id;
    const auto location_id = row[0].as<std::int64_t>();
    context.stars = row[1].as<int>();
    context.text = row[2].as<std::string>();
    context.updated_time = row[3].as<std::string>();
    context.has_reply = row[4].as<bool>();
    context.removed = row[5].as<bool>();
    context.synced = row[6].as<bool>();
    context.ignored = row[7].as<bool>();
    const auto ai = get_ai_settings(tx);
    context.enabled = ai.enabled;
    context.setup = go_live_setup(tx, location_id, ai);

    context.analysis = db::row_as<model::ReviewAnalysis>(
        tx, analysis_select() + " WHERE a.review_id = $1 AND a.review_updated_time = r.updated_time ORDER BY a.id DESC LIMIT 1",
        pqxx::params{review_id});
    // By review version, not by analysis: a draft a person rejected, or one
    // that is published, settles this version as much as a waiting one does
    // (the statuses of kPendingFrom).
    const auto drafts = tx.exec(std::format("SELECT count(*) > 0, count(*) FILTER (WHERE d.status IN {}) > 0 "
                                            "FROM reply_drafts d JOIN reviews r ON r.id = d.review_id "
                                            "WHERE d.review_id = $1 AND d.review_updated_time = r.updated_time "
                                            "AND d.status IN ('draft', 'scheduled', 'approved', 'posting', 'posted', 'published', 'rejected')",
                                            kDraftsInFlight),
                                pqxx::params{review_id})
                            .one_row();
    context.has_live_draft = drafts[0].as<bool>();
    context.draft_in_flight = drafts[1].as<bool>();
    for (const auto& reply : tx.exec(
             "SELECT d.text FROM reply_drafts d JOIN reviews r ON r.id = d.review_id "
             "WHERE r.location_id = $1 AND d.mode = 'rating_only' AND d.generation_ok AND d.review_id <> $2 "
             "ORDER BY d.id DESC LIMIT 5",
             pqxx::params{location_id, review_id})) {
        context.recent_rating_only_replies.push_back(reply[0].as<std::string>());
    }
    return context;
}

bool review_is_current(pqxx::transaction_base& tx, std::int64_t review_id, const std::string& updated_time) {
    const auto rows = tx.exec(std::format("SELECT {} FROM reviews WHERE id = $1 AND removed_at IS NULL FOR UPDATE", iso("updated_time")),
                              pqxx::params{review_id});
    return !rows.empty() && rows[0][0].as<std::string>() == updated_time;
}

Result<void> request_analysis(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t review_id) {
    const auto context = review_context(tx, review_id);
    if (!context) return not_found("review");
    if (context->removed || !context->synced || context->unsupported()) {
        return conflict("not_analyzable", "The review was removed, its client has no industry profile, or its location isn't synced.");
    }
    jobs::sql::enqueue(tx,
                       {.kind = std::string(kAnalyzeReviewKind),
                        .payload = boost::json::object{{"review_id", review_id}, {"force", true}},
                        .max_attempts = kAnalyzeReviewAttempts},
                       audit.now());
    audit.record("review.analysis_requested", "review", review_id);
    return {};
}

model::ReviewAnalysis store_analysis(pqxx::transaction_base& tx, const NewAnalysis& entry, core::TimePoint now) {
    const auto& a = entry.analysis;
    const auto id = tx.query_value<std::int64_t>(
        R"sql(
        INSERT INTO review_analyses (review_id, review_updated_time, status, error, language, script, sentiment,
                                     contains_complaint, topics, items, severity_flags, manipulation_suspected,
                                     manipulation_evidence, summary_en, industry_key, model, prompt_version, raw, created_at)
        VALUES ($1, $2::timestamptz, $3, $4, $5, $6, $7, $8, $9::jsonb, $10::jsonb, $11::jsonb, $12, $13, $14,
                $15, $16, $17, $18::jsonb, $19::timestamptz)
        RETURNING id)sql",
        pqxx::params{entry.review_id,
                     entry.review_updated_time,
                     a.ok ? "ok" : "failed",
                     a.ok ? OptString{} : OptString{a.error},
                     a.language,
                     a.script,
                     a.sentiment,
                     a.contains_complaint,
                     to_json_text(a.topics),
                     to_json_text(a.items),
                     to_json_text(a.severity_flags),
                     a.manipulation_suspected,
                     a.manipulation_evidence,
                     a.summary_en,
                     entry.industry_key,
                     entry.model,
                     entry.prompt_version,
                     entry.raw.is_null() ? OptString{} : OptString{boost::json::serialize(entry.raw)},
                     to_timestamptz(now)});
    // The analyzer's language replaces the guess made from the script. A
    // failed analysis only has the mechanical guess, which is no better.
    if (a.ok && a.language != "none") {
        tx.exec("UPDATE reviews SET language = $2 WHERE id = $1 AND updated_time = $3::timestamptz AND language IS DISTINCT FROM $2",
                pqxx::params{entry.review_id, a.language, entry.review_updated_time})
            .no_rows();
    }
    return db::one_as<model::ReviewAnalysis>(tx, analysis_select() + " WHERE a.id = $1", pqxx::params{id});
}

bool close_live_draft(pqxx::transaction_base& tx, std::int64_t review_id, std::string_view status, std::string_view reason,
                      core::TimePoint now) {
    return tx.exec(std::format("UPDATE reply_drafts SET status = $2, status_reason = $3, updated_at = $4::timestamptz "
                               "WHERE review_id = $1 AND status IN {}",
                               kWaitingDrafts),
                   pqxx::params{review_id, std::string(status), std::string(reason), to_timestamptz(now)})
               .affected_rows() > 0;
}

std::optional<std::int64_t> store_draft_if_wanted(pqxx::transaction_base& tx, const NewDraft& draft, std::optional<std::int64_t> replaces,
                                                  core::TimePoint now) {
    if (!review_is_current(tx, draft.review_id, draft.review_updated_time)) return std::nullopt;
    if (replaces &&
        tx.exec("SELECT 1 FROM reply_drafts WHERE id = $1 AND status = 'draft' FOR UPDATE", pqxx::params{*replaces}).empty()) {
        return std::nullopt;
    }
    // A reply appeared while the draft was written: there is nothing to draft for any more.
    if (!tx.exec("SELECT 1 FROM reviews WHERE id = $1 AND reply_text IS NOT NULL", pqxx::params{draft.review_id}).empty()) {
        return std::nullopt;
    }
    // Approved or sent meanwhile: that reply stands.
    if (!replaces && !tx.exec(std::format("SELECT 1 FROM reply_drafts WHERE review_id = $1 AND status IN {}", kDraftsInFlight),
                              pqxx::params{draft.review_id})
                          .empty()) {
        return std::nullopt;
    }
    close_live_draft(tx, draft.review_id, "superseded", "replaced by a newer draft", now);
    const auto& o = draft.outcome;
    return tx.query_value<std::int64_t>(
        R"sql(
        INSERT INTO reply_drafts (review_id, analysis_id, review_updated_time, route, route_reasons, mode, text,
                                  language, script, generation_ok, generator_manipulation_suspected,
                                  notes_for_staff, checks, model, prompt_version, inputs, created_at, updated_at,
                                  generated_text, instructions, created_by)
        VALUES ($1, $2, $3::timestamptz, $4, $5::jsonb, $6, $7, $8, $9, $10, $11, $12, $13::jsonb, $14, $15,
                $16::jsonb, $17::timestamptz, $17::timestamptz, $7, $18, $19)
        RETURNING id)sql",
        pqxx::params{draft.review_id,
                     draft.analysis_id,
                     draft.review_updated_time,
                     o.route,
                     to_json_text(o.route_reasons),
                     draft.mode,
                     o.text,
                     draft.language,
                     draft.script,
                     o.generation_ok,
                     o.generator_manipulation_suspected,
                     o.notes_for_staff,
                     to_json_text(o.checks),
                     draft.model,
                     draft.prompt_version,
                     boost::json::serialize(draft.inputs),
                     to_timestamptz(now),
                     draft.instructions,
                     draft.created_by});
}

std::optional<model::ReplyDraft> get_draft(pqxx::transaction_base& tx, std::int64_t id) {
    return db::row_as<model::ReplyDraft>(tx, draft_select("d.inputs") + " WHERE d.id = $1", pqxx::params{id});
}

std::optional<DraftBasis> draft_basis(pqxx::transaction_base& tx, std::int64_t draft_id) {
    return db::row_as<DraftBasis>(tx, "SELECT status, review_id, analysis_id FROM reply_drafts WHERE id = $1", pqxx::params{draft_id});
}

// ---- LLM calls -----------------------------------------------------------------

void record_llm_call(pqxx::transaction_base& tx, const llm::CallRecord& call) {
    tx.exec(R"sql(
        INSERT INTO llm_calls (occurred_at, task, model, client_id, location_id, review_id, attempt, status, error,
                               http_status, prompt_tokens, completion_tokens, latency_ms, finish_reason, provider,
                               go_live_run_id, estimated_cost_usd)
        SELECT $1::timestamptz, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11::integer, $12::integer, $13, $14, $15, $16,
               round(($11::integer * (p.price->>'input_per_mtok')::numeric
                      + $12::integer * (p.price->>'output_per_mtok')::numeric) / 1000000, 6)
          FROM (SELECT (SELECT jsonb_build_object('input_per_mtok', m.input_per_mtok, 'output_per_mtok', m.output_per_mtok)
                          FROM kb_model_prices m WHERE m.model = $3) AS price) p)sql",
            pqxx::params{to_timestamptz(call.at),
                         call.task,
                         call.model,
                         call.client_id,
                         call.location_id,
                         call.review_id,
                         call.attempt,
                         call.ok ? "ok" : "error",
                         call.ok ? OptString{} : OptString{call.error},
                         call.http_status ? std::optional<int>(static_cast<int>(call.http_status)) : std::nullopt,
                         call.prompt_tokens,
                         call.completion_tokens,
                         call.latency_ms,
                         call.finish_reason.empty() ? OptString{} : OptString{call.finish_reason},
                         call.provider.empty() ? OptString{} : OptString{call.provider},
                         call.go_live_run_id})
        .no_rows();
}

model::LlmUsage llm_usage(pqxx::transaction_base& tx, core::TimePoint from, core::TimePoint to) {
    const pqxx::params range{to_timestamptz(from), to_timestamptz(to)};
    const auto rows = [&](std::string_view key, std::string_view label, std::string_view order) {
        return db::rows_as<model::LlmUsageRow>(tx, usage_query(key, label, order), range);
    };
    model::LlmUsage usage;
    usage.from = std::format("{:%FT%T}Z", std::chrono::floor<std::chrono::seconds>(from));
    usage.to = std::format("{:%FT%T}Z", std::chrono::floor<std::chrono::seconds>(to));
    usage.total = db::row_as<model::LlmUsageRow>(tx, usage_query("'total'::text", "'Total'::text", "1"), range)
                      .value_or(model::LlmUsageRow{.key = "total", .label = "Total"});
    constexpr std::string_view day = "to_char(k.occurred_at AT TIME ZONE 'UTC', 'YYYY-MM-DD')";
    usage.by_day = rows(day, day, "1");
    usage.by_model = rows("k.model", "k.model", "7 DESC, 1");
    usage.by_task = rows("k.task", "k.task", "1");
    usage.by_client = rows("coalesce(k.client_id::text, '')", "coalesce(c.name, 'No client')", "7 DESC, 2");
    usage.by_provider = rows("coalesce(k.provider, '')", "coalesce(k.provider, 'Not recorded')", "7 DESC, 1");
    // One row per location, under its client's name, so the cost of a client can be read per profile.
    usage.by_location = rows("coalesce(k.location_id::text, '')", "coalesce(c.name || ' · ' || l.title, l.title, 'No location')", "7 DESC, 2");
    return usage;
}

model::LlmCallList list_llm_calls(pqxx::transaction_base& tx, const db::Page& page) {
    return db::page<model::LlmCall>(
        tx,
        std::format("SELECT k.id, {} AS occurred_at, k.task, k.provider, k.model, k.client_id, c.name AS client_name, k.location_id, "
                    "k.review_id, k.attempt, k.status, k.error, k.http_status, k.prompt_tokens, k.completion_tokens, k.latency_ms, "
                    "k.estimated_cost_usd::float8 AS estimated_cost_usd "
                    "FROM llm_calls k LEFT JOIN clients c ON c.id = k.client_id",
                    iso("k.occurred_at")),
        {}, "k.occurred_at DESC, k.id DESC", page);
}

// ---- what the dashboard reads --------------------------------------------------

std::string analysis_json(std::string_view analysis_id) {
    return std::format("(SELECT to_jsonb(q) FROM ({}) q WHERE q.id = {})", analysis_select(), analysis_id);
}

std::string draft_json(std::string_view draft_id) {
    return std::format("(SELECT to_jsonb(q) FROM ({}) q WHERE q.id = {})", draft_select("NULL::jsonb"), draft_id);
}

model::ReplyDraftList list_review_drafts(pqxx::transaction_base& tx, std::int64_t review_id) {
    return {.items = db::rows_as<model::ReplyDraft>(tx, draft_select("d.inputs") + " WHERE d.review_id = $1 ORDER BY d.id DESC",
                                                    pqxx::params{review_id})};
}

model::AiStatus ai_status(pqxx::transaction_base& tx) {
    // What the model last answered is the status page's own row; llm_configured and providers are the caller's to say.
    auto status = db::one_as<model::AiStatus>(
        tx,
        std::format(R"sql(
        SELECT s.enabled, s.analyzer_provider, s.analyzer_model, s.generator_provider, s.generator_model,
               (SELECT count(*) {}) AS waiting_reviews,
               (SELECT count(*) FROM reviews r WHERE r.removed_at IS NULL AND EXISTS
                 (SELECT 1 FROM review_analyses a WHERE a.review_id = r.id AND a.review_updated_time = r.updated_time)) AS analyzed_reviews
          FROM ai_settings s)sql",
                    kPendingFrom),
        pqxx::params{}, db::FilledLater{"llm_configured", "providers", "last_ok_at", "last_error", "last_error_at"});
    const auto health = integration(tx, kLlmStatus);
    status.last_ok_at = health.last_ok_at;
    status.last_error = health.last_error;
    status.last_error_at = health.last_error_at;
    return status;
}

}  // namespace poni::repo
