#include "db/repo_reviews.hpp"

#include "core/text.hpp"
#include "db/database.hpp"
#include "db/repo_analysis.hpp"
#include "db/repo_posting.hpp"
#include "domain/review_sync.hpp"
#include "domain/review_text.hpp"

#include <boost/json/serialize.hpp>

#include <algorithm>
#include <format>

namespace poni::repo {

using db::iso;
using db::to_timestamptz;

namespace {

using OptString = std::optional<std::string>;

// The locations whose reviews are synced, as SyncLocation; `more` narrows them and may order them.
std::string sync_select(std::string_view more) {
    return std::format("SELECT l.id, l.gbp_location_id, l.gbp_account_id FROM locations l JOIN clients c ON c.id = l.client_id "
                       "WHERE l.access_status = 'active' AND NOT c.archived {}",
                       more);
}

void add_event(pqxx::transaction_base& tx, std::int64_t review_id, std::string_view kind, std::string_view source,
               const boost::json::object& detail, const std::string& now) {
    tx.exec("INSERT INTO review_events (review_id, kind, source, occurred_at, detail) VALUES ($1, $2, $3, $4::timestamptz, $5::jsonb)",
            pqxx::params{review_id, std::string(kind), std::string(source), now, boost::json::serialize(detail)})
        .no_rows();
}

// The columns of a model::Review. `a` and `d` are the review's newest analysis
// and newest draft, whatever their state: a list filters and sorts by them,
// and they are in the row as sub-objects (the draft without its inputs).
std::string review_select() {
    return std::format(R"sql(
        SELECT r.id, r.location_id, l.title AS location_title, l.client_id, c.name AS client_name, r.rating,
               r.comment, r.original_text, r.translated_text, r.language, r.reviewer_name, r.reviewer_photo_url,
               r.reviewer_anonymous, {} AS created_time, {} AS updated_time, r.reply_text IS NOT NULL AS has_reply,
               CASE WHEN r.reply_text IS NOT NULL
                    THEN jsonb_build_object('text', r.reply_text, 'updated_time', {}, 'state', r.reply_state,
                                            'origin', r.reply_origin) END AS reply,
               CASE WHEN r.removed_at IS NULL THEN 'active' ELSE 'removed' END AS status, {} AS removed_at,
               {} AS first_seen_at, {} AS last_seen_at,
               CASE WHEN r.ignored_at IS NOT NULL
                    THEN jsonb_build_object('by', coalesce((SELECT u.email FROM users u WHERE u.id = r.ignored_by), ''),
                                            'at', {}, 'reason', r.ignore_reason) END AS ignored,
               {} AS analysis, {} AS draft
          FROM reviews r JOIN locations l ON l.id = r.location_id LEFT JOIN clients c ON c.id = l.client_id
          LEFT JOIN LATERAL (SELECT x.id, x.sentiment, x.severity_flags, x.manipulation_suspected FROM review_analyses x
                              WHERE x.review_id = r.id ORDER BY x.id DESC LIMIT 1) a ON true
          LEFT JOIN LATERAL (SELECT x.id, x.route, x.status, x.created_at FROM reply_drafts x
                              WHERE x.review_id = r.id ORDER BY x.id DESC LIMIT 1) d ON true)sql",
                       iso("r.created_time"), iso("r.updated_time"), iso("r.reply_updated_time"), iso("r.removed_at"),
                       iso("r.first_seen_at"), iso("r.last_seen_at"), iso("r.ignored_at"), analysis_json("a.id"),
                       draft_json("d.id"));
}

}  // namespace

// ---- sync ----------------------------------------------------------------------

std::vector<SyncLocation> locations_due_for_poll(pqxx::transaction_base& tx, core::TimePoint now) {
    // The poll job runs once a minute, a second or two after the minute. Ten
    // seconds of grace keeps a location that is due "at :04" from waiting
    // for the next minute because this run came at ":03".
    return db::rows_as<SyncLocation>(tx,
                                     sync_select("AND l.reviews_full_synced_at IS NOT NULL "
                                                 "AND (l.next_review_poll_at IS NULL OR l.next_review_poll_at <= $1::timestamptz) ORDER BY l.id"),
                                     pqxx::params{to_timestamptz(now + std::chrono::seconds(10))});
}

std::vector<SyncLocation> locations_due_for_full_sync(pqxx::transaction_base& tx, core::TimePoint now) {
    return db::rows_as<SyncLocation>(
        tx, sync_select("AND (l.next_full_sync_at IS NULL OR l.next_full_sync_at <= $1::timestamptz) ORDER BY l.id"),
        pqxx::params{to_timestamptz(now)});
}

std::optional<SyncLocation> syncable_location(pqxx::transaction_base& tx, std::int64_t id) {
    return db::row_as<SyncLocation>(tx, sync_select("AND l.id = $1"), pqxx::params{id});
}

std::optional<SyncLocation> syncable_location_by_gbp_id(pqxx::transaction_base& tx, std::string_view gbp_location_id) {
    return db::row_as<SyncLocation>(tx, sync_select("AND l.gbp_location_id = $1"), pqxx::params{std::string(gbp_location_id)});
}

std::optional<ApplyResult> apply_review(pqxx::transaction_base& tx, std::int64_t location_id, const gbp::Review& review,
                                        std::string_view source, core::TimePoint now) {
    const auto created_time = domain::canonical_timestamp(review.createTime);
    const auto updated_time = domain::canonical_timestamp(review.updateTime);
    if (!created_time || !updated_time || review.reviewId.empty()) return std::nullopt;

    const auto reply = review.reviewReply.value_or(gbp::ReviewReply{});
    // A reply object without text is no reply.
    const OptString reply_text = reply.comment && !reply.comment->empty() ? reply.comment : std::nullopt;
    const OptString reply_time = reply_text && reply.updateTime ? domain::canonical_timestamp(*reply.updateTime) : std::nullopt;
    const OptString reply_state = reply_text ? reply.reviewReplyState : std::nullopt;
    const OptString comment = review.comment && !review.comment->empty() ? review.comment : std::nullopt;
    const auto rating = review.starRating ? domain::parse_star_rating(*review.starRating) : std::nullopt;

    OptString original_text;
    OptString translated_text;
    OptString language;
    if (comment) {
        auto parts = domain::split_translation(*comment);
        language = domain::guess_language(parts.original, parts.translated.has_value());
        original_text = std::move(parts.original);
        translated_text = std::move(parts.translated);
    }
    const auto reviewer = review.reviewer.value_or(gbp::Reviewer{});
    const auto raw = boost::json::serialize(review.raw);
    const auto now_text = to_timestamptz(now);

    const pqxx::params fields{
        location_id,                         // $1
        review.reviewId,                     // $2
        review.name,                         // $3
        rating,                              // $4
        comment,                             // $5
        original_text,                       // $6
        translated_text,                     // $7
        language,                            // $8
        reviewer.displayName,                // $9
        reviewer.profilePhotoUrl,            // $10
        reviewer.isAnonymous.value_or(false),  // $11
        *created_time,                       // $12
        *updated_time,                       // $13
        reply_text,                          // $14
        reply_time,                          // $15
        reply_state,                         // $16
        raw,                                 // $17
        now_text,                            // $18
    };

    const auto existing = tx.exec(
        std::format("SELECT id, rating, comment, {}, reply_text, {}, removed_at IS NOT NULL, raw = $3::jsonb "
                    "FROM reviews WHERE location_id = $1 AND gbp_review_id = $2 FOR UPDATE",
                    iso("updated_time"), iso("reply_updated_time")),
        pqxx::params{location_id, review.reviewId, raw});

    if (existing.empty()) {
        const auto inserted = tx.exec(
            R"sql(
            INSERT INTO reviews (location_id, gbp_review_id, gbp_name, rating, comment, original_text, translated_text,
                                 language, reviewer_name, reviewer_photo_url, reviewer_anonymous, created_time,
                                 updated_time, reply_text, reply_updated_time, reply_state, reply_origin, raw,
                                 first_seen_at, last_seen_at, last_changed_at)
            VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12::timestamptz, $13::timestamptz, $14,
                    $15::timestamptz, $16, CASE WHEN $14::text IS NULL THEN NULL ELSE 'external' END, $17::jsonb,
                    $18::timestamptz, $18::timestamptz, $18::timestamptz)
            ON CONFLICT (location_id, gbp_review_id) DO NOTHING
            RETURNING id)sql",
            fields);
        // No row: another worker stored the same review a moment ago.
        if (inserted.empty()) return ApplyResult{};
        add_event(tx, inserted[0][0].as<std::int64_t>(), "discovered", source,
                  {{"rating", rating ? boost::json::value(*rating) : boost::json::value(nullptr)},
                   {"has_text", comment.has_value()},
                   {"has_reply", reply_text.has_value()}},
                  now_text);
        return ApplyResult{.created = true, .events = {"discovered"}};
    }

    const auto row = existing[0];
    const auto id = row[0].as<std::int64_t>();
    const domain::ReviewSnapshot stored{.rating = row[1].as<std::optional<int>>(),
                                        .comment = row[2].as<OptString>(),
                                        .updated_time = row[3].as<std::string>(),
                                        .reply_text = row[4].as<OptString>(),
                                        .reply_updated_time = row[5].as<OptString>(),
                                        .removed = row[6].as<bool>()};
    const domain::ReviewSnapshot incoming{.rating = rating,
                                          .comment = comment,
                                          .updated_time = *updated_time,
                                          .reply_text = reply_text,
                                          .reply_updated_time = reply_time,
                                          .removed = false};
    auto events = domain::diff_review(stored, incoming);
    const bool same_payload = row[7].as<bool>();
    if (events.empty() && same_payload) return ApplyResult{};

    // A reply that appeared or changed is either the engine's own post
    // showing up (Google publishes a reply minutes after accepting it) or
    // somebody's reply from outside. Settled here, in the transaction that
    // stores the reply, so the mirror and the posts never disagree.
    std::string reply_origin = "external";
    for (auto& event : events) {
        if (event.kind != "reply_added" && event.kind != "reply_changed") continue;
        reply_origin = std::string(reconcile_reply(tx, id, *reply_text, now));
        event.detail["origin"] = reply_origin;
    }

    // Either something we track changed, or only details we don't (the
    // reviewer's name or photo): the mirror follows Google in both cases.
    tx.exec(
          R"sql(
        UPDATE reviews
           SET gbp_name = $3, rating = $4, comment = $5, translated_text = $7,
               -- The analyzer's language stands until the text changes; then the guess returns.
               language = CASE WHEN original_text IS NOT DISTINCT FROM $6 THEN language ELSE $8 END,
               original_text = $6,
               reviewer_name = $9, reviewer_photo_url = $10, reviewer_anonymous = $11,
               created_time = $12::timestamptz, updated_time = $13::timestamptz,
               reply_origin = CASE WHEN $14::text IS NULL THEN NULL
                                   WHEN reply_text IS NOT DISTINCT FROM $14 THEN COALESCE(reply_origin, 'external')
                                   ELSE $20 END,
               reply_text = $14, reply_updated_time = $15::timestamptz, reply_state = $16,
               raw = $17::jsonb, removed_at = NULL, last_seen_at = $18::timestamptz,
               last_changed_at = CASE WHEN $19 THEN $18::timestamptz ELSE last_changed_at END
         WHERE location_id = $1 AND gbp_review_id = $2)sql",
          [&] {
              auto all = fields;
              all.append(!events.empty());  // $19
              all.append(reply_origin);     // $20
              return all;
          }())
        .no_rows();

    ApplyResult result;
    for (const auto& event : events) {
        add_event(tx, id, event.kind, source, event.detail, now_text);
        result.events.push_back(event.kind);
        // A draft is for one version of an unreplied review. The pipeline
        // writes a new one for an edited review on its next pass. (A reply
        // from outside cancelled the drafts in reconcile_reply above.)
        if (event.kind == "review_edited") {
            close_live_draft(tx, id, "superseded", "the review was edited", now);
            // "No reply needed" was said about the review as it was (D-068).
            tx.exec("UPDATE reviews SET ignored_at = NULL, ignored_by = NULL, ignore_reason = '' WHERE id = $1 AND ignored_at IS NOT NULL",
                    pqxx::params{id})
                .no_rows();
        }
    }
    return result;
}

PageResult apply_reviews(pqxx::transaction_base& tx, const std::vector<SyncLocation>& locations,
                         const std::vector<gbp::Review>& reviews, std::string_view source, core::TimePoint now) {
    PageResult page;
    for (const auto& review : reviews) {
        const auto name = gbp::parse_review_name(review.name);
        const auto location = name ? std::ranges::find(locations, name->location_id, &SyncLocation::gbp_location_id)
                                   : locations.end();
        if (location == locations.end()) {
            ++page.skipped;
            continue;
        }
        const auto result = apply_review(tx, location->id, review, source, now);
        if (!result) {
            ++page.skipped;
        } else if (result->created) {
            ++page.created;
        } else if (!result->events.empty()) {
            ++page.changed;
        } else {
            ++page.unchanged;
        }
    }
    return page;
}

bool mark_review_removed(pqxx::transaction_base& tx, std::int64_t location_id, std::string_view gbp_review_id,
                         std::string_view source, core::TimePoint now) {
    const auto now_text = to_timestamptz(now);
    const auto rows = tx.exec(
        "UPDATE reviews SET removed_at = $3::timestamptz, last_changed_at = $3::timestamptz "
        "WHERE location_id = $1 AND gbp_review_id = $2 AND removed_at IS NULL RETURNING id",
        pqxx::params{location_id, std::string(gbp_review_id), now_text});
    if (rows.empty()) return false;
    add_event(tx, rows[0][0].as<std::int64_t>(), "removed", source, {}, now_text);
    close_posts_of_removed_review(tx, rows[0][0].as<std::int64_t>(), now);
    return true;
}

std::vector<std::string> active_review_ids(pqxx::transaction_base& tx, std::int64_t location_id) {
    std::vector<std::string> ids;
    for (const auto& row : tx.exec("SELECT gbp_review_id FROM reviews WHERE location_id = $1 AND removed_at IS NULL",
                                   pqxx::params{location_id})) {
        ids.push_back(row[0].as<std::string>());
    }
    return ids;
}

void mark_reviews_seen(pqxx::transaction_base& tx, std::int64_t location_id, const std::vector<std::string>& gbp_review_ids,
                       core::TimePoint now) {
    if (gbp_review_ids.empty()) return;
    tx.exec("UPDATE reviews SET last_seen_at = $3::timestamptz WHERE location_id = $1 "
            "AND gbp_review_id IN (SELECT jsonb_array_elements_text($2::jsonb))",
            pqxx::params{location_id, db::json_list(gbp_review_ids), to_timestamptz(now)})
        .no_rows();
}

void finish_poll(pqxx::transaction_base& tx, const std::vector<std::int64_t>& location_ids, core::TimePoint now) {
    if (location_ids.empty()) return;
    tx.exec(R"sql(
        UPDATE locations
           SET reviews_polled_at = $2::timestamptz,
               next_review_poll_at = $2::timestamptz + make_interval(secs => review_poll_interval_s),
               last_sync_error = NULL, last_sync_error_at = NULL
         WHERE id IN (SELECT jsonb_array_elements_text($1::jsonb)::bigint))sql",
            pqxx::params{db::json_list(location_ids), to_timestamptz(now)})
        .no_rows();
}

void finish_full_sync(pqxx::transaction_base& tx, std::int64_t location_id, const boost::json::object& detail,
                      core::TimePoint now, std::chrono::seconds next_in) {
    tx.exec(R"sql(
        UPDATE locations
           SET reviews_full_synced_at = $2::timestamptz, reviews_polled_at = $2::timestamptz,
               next_full_sync_at = $3::timestamptz, last_full_sync = $4::jsonb,
               last_sync_error = NULL, last_sync_error_at = NULL
         WHERE id = $1)sql",
            pqxx::params{location_id, to_timestamptz(now), to_timestamptz(now + next_in), boost::json::serialize(detail)})
        .no_rows();
}

void record_sync_error(pqxx::transaction_base& tx, const std::vector<std::int64_t>& location_ids, std::string_view error,
                       core::TimePoint now, bool reschedule_poll) {
    if (location_ids.empty()) return;
    tx.exec(R"sql(
        UPDATE locations
           SET last_sync_error = $2, last_sync_error_at = $3::timestamptz,
               next_review_poll_at = CASE WHEN $4 THEN $3::timestamptz + make_interval(secs => review_poll_interval_s)
                                          ELSE next_review_poll_at END
         WHERE id IN (SELECT jsonb_array_elements_text($1::jsonb)::bigint))sql",
            pqxx::params{db::json_list(location_ids), std::string(error), to_timestamptz(now), reschedule_poll})
        .no_rows();
}

// ---- inbox ---------------------------------------------------------------------

model::ReviewList list_reviews(pqxx::transaction_base& tx, const ReviewFilter& filter) {
    db::Conditions where;
    if (filter.client_id) where.add(std::format("l.client_id = {}", where.bind(*filter.client_id)));
    if (filter.location_id) where.add(std::format("r.location_id = {}", where.bind(*filter.location_id)));
    if (!filter.ratings.empty()) {
        where.add(std::format("r.rating IN (SELECT jsonb_array_elements_text({}::jsonb)::smallint)",
                              where.bind(db::json_list(filter.ratings))));
    }
    if (filter.status == "active") where.add("r.removed_at IS NULL");
    if (filter.status == "removed") where.add("r.removed_at IS NOT NULL");
    if (filter.language == "none") {
        where.add("r.language IS NULL");
    } else if (!filter.language.empty()) {
        where.add(std::format("r.language = {}", where.bind(filter.language)));
    }
    if (filter.has_reply) where.add(*filter.has_reply ? "r.reply_text IS NOT NULL" : "r.reply_text IS NULL");
    if (!filter.q.empty()) {
        const auto q = where.bind(filter.q);
        where.add(std::format("(position(lower({0}) in lower(coalesce(r.comment, ''))) > 0 "
                              "OR position(lower({0}) in lower(coalesce(r.reviewer_name, ''))) > 0)",
                              q));
    }
    if (filter.analyzed) where.add(*filter.analyzed ? "a.id IS NOT NULL" : "a.id IS NULL");
    if (!filter.sentiment.empty()) where.add(std::format("a.sentiment = {}", where.bind(filter.sentiment)));
    if (filter.flagged) {
        where.add(*filter.flagged ? "jsonb_array_length(a.severity_flags) > 0"
                                  : "(a.id IS NULL OR jsonb_array_length(a.severity_flags) = 0)");
    }
    if (filter.manipulation) {
        where.add(*filter.manipulation ? "a.manipulation_suspected" : "(a.id IS NULL OR NOT a.manipulation_suspected)");
    }
    if (filter.ignored) where.add(*filter.ignored ? "r.ignored_at IS NOT NULL" : "r.ignored_at IS NULL");
    if (filter.has_draft) where.add(*filter.has_draft ? "d.id IS NOT NULL" : "d.id IS NULL");
    if (!filter.route.empty()) where.add(std::format("d.route = {}", where.bind(filter.route)));
    if (!filter.draft_status.empty()) where.add(std::format("d.status = {}", where.bind(filter.draft_status)));

    // The sort column comes from a fixed list, never from the request text.
    const std::string_view column = filter.sort == "rating"         ? "r.rating"
                                    : filter.sort == "updated_time" ? "r.updated_time"
                                    : filter.sort == "draft_time"   ? "d.created_at"
                                                                    : "r.created_time";
    const std::string_view direction = filter.descending ? "DESC" : "ASC";

    return db::page<model::Review>(tx, review_select(), where, std::format("{} {} NULLS LAST, r.id {}", column, direction, direction),
                                   filter.page);
}

std::optional<model::Review> get_review(pqxx::transaction_base& tx, std::int64_t id) {
    return db::row_as<model::Review>(tx, review_select() + " WHERE r.id = $1", pqxx::params{id});
}

std::vector<model::Review> get_reviews(pqxx::transaction_base& tx, const std::vector<std::int64_t>& ids) {
    if (ids.empty()) return {};
    // An array, not a subquery: the planner then looks the reviews up by their key.
    return db::rows_as<model::Review>(tx, review_select() + " WHERE r.id = ANY(ARRAY(SELECT jsonb_array_elements_text($1::jsonb)::bigint))",
                                      pqxx::params{db::json_list(ids)});
}

Result<model::Review> ignore_review(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id, const std::string& reason) {
    const auto rows = tx.exec(
        std::format("SELECT r.removed_at IS NOT NULL, r.reply_text IS NOT NULL, r.ignored_at IS NOT NULL, "
                    "EXISTS (SELECT 1 FROM reply_drafts d WHERE d.review_id = r.id AND d.status IN ('approved', 'posting', 'posted')) "
                    "OR EXISTS (SELECT 1 FROM reply_posts p WHERE p.review_id = r.id AND p.status IN {}) "
                    "FROM reviews r WHERE r.id = $1 FOR UPDATE OF r",
                    kOpenPost),
        pqxx::params{id});
    if (rows.empty()) return not_found("review");
    const std::string cleaned(text::trim(reason));
    if (text::length(cleaned) > 500) return invalid("\"reason\" must be at most 500 characters.");
    // Removed or answered: there is nothing left to decide. Approved or sent: a reply is on its way.
    if (rows[0][0].as<bool>() || rows[0][1].as<bool>() || rows[0][3].as<bool>()) {
        return conflict("review_not_ignorable", "The review was removed, already has a reply, or a reply to it is approved or on its way.");
    }
    if (rows[0][2].as<bool>()) {
        audit.nothing_changed();
        return *get_review(tx, id);
    }
    tx.exec("UPDATE reviews SET ignored_at = $2::timestamptz, ignored_by = $3, ignore_reason = $4 WHERE id = $1",
            pqxx::params{id, to_timestamptz(audit.now()), audit.user_id(), cleaned})
        .no_rows();
    // The waiting draft is kept, as cancelled, so what the engine would have said stays on record.
    close_live_draft(tx, id, "cancelled", "a person decided that this review needs no reply", audit.now());
    audit.record("review.ignore", "review", id, {.after = {{"reason", cleaned}}});
    return *get_review(tx, id);
}

Result<model::Review> unignore_review(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id) {
    const auto rows = tx.exec("SELECT ignored_at IS NOT NULL, ignore_reason FROM reviews WHERE id = $1 FOR UPDATE", pqxx::params{id});
    if (rows.empty()) return not_found("review");
    if (!rows[0][0].as<bool>()) {
        audit.nothing_changed();
        return *get_review(tx, id);
    }
    tx.exec("UPDATE reviews SET ignored_at = NULL, ignored_by = NULL, ignore_reason = '' WHERE id = $1", pqxx::params{id}).no_rows();
    audit.record("review.unignore", "review", id, {.before = {{"reason", rows[0][1].as<std::string>()}}});
    return *get_review(tx, id);
}

model::ReviewEventList list_review_events(pqxx::transaction_base& tx, std::int64_t review_id) {
    return {.items = db::rows_as<model::ReviewEvent>(
                tx, std::format("SELECT id, kind, source, {} AS occurred_at, detail FROM review_events WHERE review_id = $1 ORDER BY id",
                                iso("occurred_at")),
                pqxx::params{review_id})};
}

}  // namespace poni::repo
