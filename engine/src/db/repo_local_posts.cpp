#include "db/repo_local_posts.hpp"

#include "core/json.hpp"
#include "core/text.hpp"
#include "db/audit.hpp"
#include "db/database.hpp"
#include "db/repo_notifications.hpp"
#include "domain/review_text.hpp"
#include "jobs/queue.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value_from.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <set>
#include <stdexcept>

namespace poni::repo {

using db::iso;
using db::to_timestamptz;

namespace {

using OptString = std::optional<std::string>;

// A value as a jsonb parameter. For a post's content this is what its column holds: the engine's own
// writing of the struct, as approved_content is.
template <class T>
std::string json_text(const T& value) {
    return boost::json::serialize(boost::json::value_from(value));
}

domain::LocalPostContent read_content(const boost::json::value& json) {
    auto content = core::from_json<domain::LocalPostContent>(json);
    // The engine writes the column only in this file, from the struct.
    if (!content) throw std::runtime_error(std::format("a local post's content cannot be read: {}", content.error()));
    return std::move(*content);
}

// A post as the API shows it, read by column name. `base` is the placeholder of the public bucket's
// address ("" where there is none), to which an uploaded file's name is added.
std::string local_post_select(std::string_view base) {
    const auto at = [](std::string_view column) { return std::format("{} AS {}", iso(std::format("p.{}", column)), column); };
    const auto by = [](std::string_view column) { return std::format("(SELECT u.email FROM users u WHERE u.id = p.{0}) AS {0}", column); };
    // media: what Google shows once it has the post; before that, the uploaded file.
    // copies: the posts copied from this one, newest first (D-098).
    const auto post_media = std::format(R"sql(CASE WHEN jsonb_array_length(p.google_media) > 0 THEN p.google_media
                    WHEN m.id IS NOT NULL AND {0}::text <> '' THEN
                        jsonb_build_array(jsonb_build_object('format', m.format, 'url', {0}::text || m.object_name))
                    ELSE '[]'::jsonb END)sql",
                                        base);
    // edit: the open edit of a post that is at Google, as model::LocalPostEdit (D-099). Its media is what
    // the post would show with the edit applied: the edit's own upload where it changes the media (nothing
    // where it takes the upload away), and what the post shows now where it does not.
    const auto edit = std::format(
        "(SELECT jsonb_build_object('content', e.content, 'fields', e.fields, "
        "'media', CASE WHEN NOT e.fields @> '[\"media\"]'::jsonb THEN {3} "
        "WHEN em.id IS NOT NULL AND {2}::text <> '' THEN jsonb_build_array(jsonb_build_object('format', em.format, 'url', {2}::text || em.object_name)) "
        "ELSE '[]'::jsonb END, "
        "'checks', e.checks, 'status', e.status, 'status_reason', e.status_reason, "
        "'written_by', (SELECT u.email FROM users u WHERE u.id = e.written_by), 'written_at', {0}, "
        "'decided_by', (SELECT u.email FROM users u WHERE u.id = e.decided_by), 'decided_at', {1}) "
        "FROM local_post_edits e LEFT JOIN local_post_media em ON em.id = e.media_id WHERE e.local_post_id = p.id) AS edit",
        iso("e.written_at"), iso("e.decided_at"), base, post_media);
    return std::format(R"sql(
        SELECT p.id, p.location_id, l.title AS location_title, l.client_id, c.name AS client_name, p.origin, p.status,
               p.status_reason, p.google_state, p.google_name, p.search_url,
               p.content->>'topic_type' AS topic_type, p.content->>'summary' AS summary,
               p.content->>'language_code' AS language_code, p.content->'call_to_action' AS call_to_action,
               p.content->'event' AS event, p.content->'offer' AS offer, p.media_id,
               p.content->>'scheduled_time' AS scheduled_time,
               {15} AS media,
               p.checks, p.copied_from_id,
               coalesce((SELECT jsonb_agg(jsonb_build_object('id', k.id, 'status', k.status) ORDER BY k.created_at DESC, k.id DESC)
                           FROM local_posts k WHERE k.copied_from_id = p.id), '[]'::jsonb) AS copies,
               {14},
               {1}, {2}, {3}, {4}, {5}, {6}, {7}, {8}, {9}, {10}, {11}, {12}, {13}
          FROM local_posts p
          JOIN locations l ON l.id = p.location_id
          LEFT JOIN clients c ON c.id = l.client_id
          LEFT JOIN local_post_media m ON m.id = p.media_id)sql",
                       base, by("created_by"), by("edited_by"), at("edited_at"), by("decided_by"), at("decided_at"), at("sent_at"),
                       by("removal_requested_by"), at("removal_requested_at"), at("removed_at"), at("google_created_at"),
                       at("synced_at"), at("created_at"), at("updated_at"), edit, post_media);
}

// ---- writing ---------------------------------------------------------------------------

// What the engine does to a post on its own, in the audit log next to what people did.
audit::Actor engine_actor() { return audit::Actor::system("local-posts"); }

void system_audit(pqxx::transaction_base& tx, std::string action, std::int64_t id, boost::json::object after, core::TimePoint now) {
    audit::record(tx, engine_actor(),
                  {.action = std::move(action), .object_type = "local_post", .object_id = std::to_string(id), .after = std::move(after)}, now);
}

// A system-problem notification about one post. The post's text goes into the body only.
void notify(pqxx::transaction_base& tx, std::int64_t id, std::string key, std::string subject, std::string body, core::TimePoint now) {
    // Three plain values, not the post as the API shows it: this runs in the bookkeeping of a send
    // and of a removal, which must not fail over anything else the row holds.
    const auto row = tx.exec("SELECT p.location_id, l.title, p.content->>'summary' FROM local_posts p JOIN locations l ON l.id = p.location_id "
                             "WHERE p.id = $1",
                             pqxx::params{id})
                         .one_row();
    const auto summary = row[2].as<std::string>();
    emit(tx,
         {.type = std::string(kEventSystemProblem),
          .key = std::move(key),
          .client_id = std::nullopt,
          .location_id = row[0].as<std::int64_t>(),
          .review_id = std::nullopt,
          .subject = std::move(subject),
          .body = std::format("{}\n\nLocation: {}\nThe post: {}", body, row[1].as<std::string>(),
                              summary.empty() ? std::string("(no text)") : text::truncate(summary, 300)),
          .link_path = std::format("/#/posts/{}", id),
          .local_post_id = id},
         now);
}

// ---- drafts, approval and the send --------------------------------------------------------

// How long an uploaded file is used. The bucket removes it after 365 days.
constexpr auto kMediaUsableFor = std::chrono::hours(24) * 360;

// The uploaded file a post names.
struct MediaState {
    bool exists = false;
    std::string format;
    std::string url;
    // Why Google could not be given the file, if it could not.
    OptString problem;
};

struct Upload {
    std::string object_name;
    std::string format;
    bool old = false;  // uploaded longer ago than kMediaUsableFor
};
BOOST_DESCRIBE_STRUCT(Upload, (), (object_name, format, old))

MediaState media_state(pqxx::transaction_base& tx, std::int64_t media_id, core::TimePoint now, std::string_view media_base) {
    const auto file = db::row_as<Upload>(tx, "SELECT object_name, format, created_at <= $2::timestamptz AS old FROM local_post_media WHERE id = $1",
                                         pqxx::params{media_id, to_timestamptz(now - kMediaUsableFor)});
    if (!file) return {.exists = false, .format = {}, .url = {}, .problem = "the uploaded file no longer exists"};
    MediaState state{.exists = true, .format = file->format, .url = std::string(media_base) + file->object_name, .problem = std::nullopt};
    if (!media_base.starts_with("https://")) {
        // Without the bucket, or with a folder standing in for it, Google has no address to fetch the file from.
        state.problem = "the engine has no public bucket, so Google cannot fetch the file";
    } else if (file->old) {
        state.problem = "the file was uploaded more than 360 days ago and may be gone: upload it again";
    }
    return state;
}

domain::CheckReport checks_for(pqxx::transaction_base& tx, const domain::LocalPostContent& content, core::TimePoint now,
                               std::string_view media_base) {
    domain::LocalPostCheckContext context{.now = now, .media_problem = std::nullopt, .edit_at_google = false};
    if (content.media_id) context.media_problem = media_state(tx, *content.media_id, now, media_base).problem;
    return domain::run_local_post_checks(content, context);
}

// The checks of an edit of a post that is at Google (D-099). `base` is the post's content the edit was
// written against. An upload is looked at only when it is not the post's own: that one is at Google
// already and is not sent.
domain::CheckReport edit_checks_for(pqxx::transaction_base& tx, const domain::LocalPostContent& base, const domain::LocalPostContent& edit,
                                    core::TimePoint now, std::string_view media_base) {
    domain::LocalPostCheckContext context{.now = now, .media_problem = std::nullopt, .edit_at_google = true};
    if (edit.media_id && edit.media_id != base.media_id) context.media_problem = media_state(tx, *edit.media_id, now, media_base).problem;
    return domain::run_local_post_checks(edit, context);
}

// What an action needs to know about a post, with its row locked.
struct PostState {
    std::string status;
    std::int64_t location_id = 0;
    std::optional<std::int64_t> decided_by;
    // Both as the row holds them; null where nothing was approved.
    boost::json::value content;
    boost::json::value approved_content;
    OptString google_name;
    std::string origin;
    // The decision is no older than the caller allows (true when it does not ask).
    bool approval_fresh = true;
    // A person asked for the post to be deleted at Google, and the request stands.
    bool removal_requested = false;
};
BOOST_DESCRIBE_STRUCT(PostState, (),
                      (status, location_id, decided_by, content, approved_content, google_name, origin, approval_fresh, removal_requested))

std::optional<PostState> lock_post(pqxx::transaction_base& tx, std::int64_t id, std::optional<core::TimePoint> decided_since = std::nullopt) {
    return db::row_as<PostState>(tx,
                                 "SELECT status, location_id, decided_by, content, approved_content, google_name, origin, "
                                 "$2::timestamptz IS NULL OR (decided_at IS NOT NULL AND decided_at >= $2::timestamptz) AS approval_fresh, "
                                 "removal_requested_at IS NOT NULL AS removal_requested "
                                 "FROM local_posts WHERE id = $1 FOR UPDATE",
                                 pqxx::params{id, decided_since ? OptString(to_timestamptz(*decided_since)) : OptString{}});
}

// A post's open edit (D-099), with its row locked. Every caller has locked the post's row first.
struct EditState {
    std::string status;
    // As the row holds them: the edit, the post's content it was written against, and what was approved
    // (null where nothing was).
    boost::json::value content;
    boost::json::value base_content;
    boost::json::value approved_content;
    OptString status_reason;
    std::int64_t written_by = 0;
    std::optional<std::int64_t> decided_by;
    // The approval is no older than the caller allows (true when it does not ask).
    bool approval_fresh = true;
};
BOOST_DESCRIBE_STRUCT(EditState, (), (status, content, base_content, approved_content, status_reason, written_by, decided_by, approval_fresh))

std::optional<EditState> lock_edit(pqxx::transaction_base& tx, std::int64_t post_id, std::optional<core::TimePoint> decided_since = std::nullopt) {
    return db::row_as<EditState>(tx,
                                 "SELECT status, content, base_content, approved_content, status_reason, written_by, decided_by, "
                                 "$2::timestamptz IS NULL OR (decided_at IS NOT NULL AND decided_at >= $2::timestamptz) AS approval_fresh "
                                 "FROM local_post_edits WHERE local_post_id = $1 FOR UPDATE",
                                 pqxx::params{post_id, decided_since ? OptString(to_timestamptz(*decided_since)) : OptString{}});
}

// A post and the request to remove it, with its row locked.
struct RemovalRequest {
    std::string status;
    OptString google_name;
    std::optional<std::int64_t> removal_requested_by;
    std::int64_t location_id = 0;
    // There is a request, and it is no older than removal_valid_for.
    bool fresh = false;
};
BOOST_DESCRIBE_STRUCT(RemovalRequest, (), (status, google_name, removal_requested_by, location_id, fresh))

// The switches and the location a send depends on, as they are now.
struct SendingState {
    bool global_posting = false;
    bool location_posting = false;
    bool synced = false;
    std::string gbp_account_id;
    std::string gbp_location_id;
};
BOOST_DESCRIBE_STRUCT(SendingState, (), (global_posting, location_posting, synced, gbp_account_id, gbp_location_id))

std::optional<SendingState> sending_state(pqxx::transaction_base& tx, std::int64_t location_id) {
    return db::row_as<SendingState>(
        tx,
        "SELECT (SELECT posting_enabled FROM system_settings) AS global_posting, l.posting_enabled AS location_posting, "
        "l.access_status = 'active' AND c.id IS NOT NULL AND NOT c.archived AS synced, l.gbp_account_id, l.gbp_location_id "
        "FROM locations l LEFT JOIN clients c ON c.id = l.client_id WHERE l.id = $1",
        pqxx::params{location_id});
}

OptString switch_obstacle(const SendingState& state) {
    if (!state.global_posting) return "Posting is switched off.";
    if (!state.location_posting) return "Posting is switched off for this location.";
    return std::nullopt;
}

constexpr std::string_view kNotSynced = "The location is not linked to an active client, or the engine has lost access to it.";

// A post returns to the drafts with the reason: nothing goes out later by itself.
void back_to_draft(pqxx::transaction_base& tx, std::int64_t id, const std::string& reason, core::TimePoint now) {
    tx.exec("UPDATE local_posts SET status = 'draft', status_reason = $2, updated_at = $3::timestamptz WHERE id = $1",
            pqxx::params{id, reason, to_timestamptz(now)})
        .no_rows();
}

// An approval that is not carried out: the post is a draft again with the reason, which the audit log
// keeps. `advice` is what the person can do next, for the post's page only.
void cancel_send(pqxx::transaction_base& tx, std::int64_t id, std::string_view why, core::TimePoint now, std::string_view advice = {}) {
    back_to_draft(tx, id, std::format("{}{}", why, advice), now);
    system_audit(tx, "local_post.send_cancelled", id, {{"reason", why}}, now);
}

// SQL: a queued or running job of the kind `kind` (a placeholder) names the post `p`.
std::string has_live_job(std::string_view kind) {
    return std::format("EXISTS (SELECT 1 FROM jobs j WHERE j.kind = {} AND j.status IN ('queued', 'running') "
                       "AND (j.payload->>'local_post_id')::bigint = p.id)",
                       kind);
}

// After a send, Google is read again 1, 5 and 15 minutes on (D-088 point 8).
void enqueue_follow_up_reads(pqxx::transaction_base& tx, std::int64_t id, std::int64_t location_id, core::TimePoint now) {
    for (const int minutes : {1, 5, 15}) {
        jobs::sql::enqueue(tx,
                           {.kind = std::string(kSyncLocalPostsKind),
                            .payload = boost::json::object{{"location_id", location_id}},
                            .run_at = now + std::chrono::minutes(minutes),
                            .idempotency_key = std::format("post:{}@{}m", id, minutes),
                            .max_attempts = kSyncLocalPostsAttempts},
                           now);
    }
}

// Whether a post at Google says what a send said: the kind, the text, an
// event's title and the time it was scheduled for.
bool same_post(const domain::LocalPostContent& sent, const domain::LocalPostContent& at_google) {
    const auto title = [](const domain::LocalPostContent& c) { return c.event ? std::string(text::trim(c.event->title)) : std::string{}; };
    return sent.topic_type == at_google.topic_type && text::trim(sent.summary) == text::trim(at_google.summary) &&
           title(sent) == title(at_google) && sent.scheduled_time == at_google.scheduled_time;
}

// A new draft by the signed-in person, with its checks: what writing a draft and copying a post share.
// `normalized` has been through domain::normalize_local_post. `copied_from`: the post a copy was made
// of, and nothing for a draft written new. The caller records the audit entry.
Result<std::int64_t> insert_draft(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t location_id,
                                  const domain::LocalPostContent& normalized, const OptString& status_reason,
                                  std::optional<std::int64_t> copied_from, std::string_view media_base) {
    const auto location = sending_state(tx, location_id);
    if (!location) return not_found("location");
    if (!location->synced) return conflict("location_not_synced", std::string(kNotSynced));
    if (normalized.media_id && !media_state(tx, *normalized.media_id, audit.now(), media_base).exists) {
        return invalid("\"media_id\" names no uploaded file.");
    }
    const auto report = checks_for(tx, normalized, audit.now(), media_base);
    return tx.query_value<std::int64_t>(
        "INSERT INTO local_posts (location_id, origin, status, status_reason, content, checks, created_by, created_at, updated_at, "
        "copied_from_id) VALUES ($1, 'system', 'draft', $2, $3::jsonb, $4::jsonb, $5, $6::timestamptz, $6::timestamptz, $7) RETURNING id",
        pqxx::params{location_id, status_reason, json_text(normalized), json_text(report.results), audit.user_id(),
                     to_timestamptz(audit.now()), copied_from});
}

// "a; b; c".
std::string joined(const std::vector<std::string>& parts) {
    std::string out;
    for (const auto& part : parts) out += (out.empty() ? "" : "; ") + part;
    return out;
}

// What a copy reads of the post it copies.
struct CopySource {
    std::int64_t location_id = 0;
    boost::json::value content;
    // The format of the first file the post shows at Google, if any.
    OptString media_at_google;
};
BOOST_DESCRIBE_STRUCT(CopySource, (), (location_id, content, media_at_google))

}  // namespace


model::LocalPostList list_local_posts(pqxx::transaction_base& tx, const LocalPostFilter& filter, std::string_view media_base) {
    db::Conditions where;
    const auto base = where.bind(std::string(media_base));
    if (filter.client_id) where.add("l.client_id = " + where.bind(*filter.client_id));
    if (filter.location_id) where.add("p.location_id = " + where.bind(*filter.location_id));
    // The column is any of the values; no values, no filter.
    const auto any_of = [&](std::string_view column, const std::vector<std::string>& values) {
        if (!values.empty()) where.add(std::format("{} IN (SELECT jsonb_array_elements_text({}::jsonb))", column, where.bind(db::json_list(values))));
    };
    any_of("p.status", filter.statuses);
    // A post without a state is `none` here; the column's CHECK holds no such value, so nothing else is.
    if (!filter.google_states.empty()) {
        any_of(std::format("coalesce(p.google_state, {})", where.bind(std::string(kNoGoogleState))), filter.google_states);
    }
    // One key, in either direction; the id breaks ties, so that paging is stable both ways.
    const std::string_view direction = filter.descending ? "DESC" : "ASC";
    return db::page<model::LocalPost>(tx, local_post_select(base), where,
                                      std::format("coalesce(p.google_created_at, p.created_at) {0}, p.id {0}", direction), filter.page);
}

std::optional<model::LocalPost> get_local_post(pqxx::transaction_base& tx, std::int64_t id, std::string_view media_base) {
    return db::row_as<model::LocalPost>(tx, local_post_select("$2") + " WHERE p.id = $1", pqxx::params{id, std::string(media_base)});
}

// ---- drafts and their approval --------------------------------------------------------------


Result<model::LocalPost> create_local_post(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t location_id,
                                           const domain::LocalPostContent& content, std::string_view media_base) {
    const auto normalized = domain::normalize_local_post(content);
    if (!normalized) return invalid(normalized.error());
    const auto id = insert_draft(tx, audit, location_id, *normalized, std::nullopt, std::nullopt, media_base);
    if (!id) return std::unexpected(id.error());
    audit.record("local_post.create", "local_post", *id, {.after = {{"location_id", location_id}, {"content", boost::json::value_from(*normalized)}}});
    return *get_local_post(tx, *id, media_base);
}

Result<model::LocalPost> copy_local_post(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id, std::string_view media_base) {
    // The copy names its source by a foreign key: the lock makes a delete of the source wait for the
    // copy (and then clear the link) or, if the delete came first, makes the copy find nothing (a 404).
    const auto source = db::row_as<CopySource>(tx,
                                               "SELECT location_id, content, google_media->0->>'format' AS media_at_google "
                                               "FROM local_posts WHERE id = $1 FOR KEY SHARE",
                                               pqxx::params{id});
    if (!source) return not_found("local post");
    auto fitted = domain::fit_for_draft(read_content(source->content), audit.now(), source->media_at_google);
    const auto normalized = domain::normalize_local_post(std::move(fitted.content));
    if (!normalized) return conflict("local_post_not_copyable", std::format("The post cannot be copied as a draft: {}", normalized.error()));
    const OptString reason = fitted.left_out.empty()
                                 ? OptString{}
                                 : OptString(std::format("Copied from post {}. Left out: {}.", id, joined(fitted.left_out)));
    const auto copy = insert_draft(tx, audit, source->location_id, *normalized, reason, id, media_base);
    if (!copy) return std::unexpected(copy.error());
    audit.record("local_post.copy", "local_post", *copy,
                 {.after = {{"location_id", source->location_id},
                            {"copied_from", id},
                            {"content", boost::json::value_from(*normalized)},
                            {"left_out", boost::json::value_from(fitted.left_out)}}});
    return *get_local_post(tx, *copy, media_base);
}

Result<void> delete_local_post(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id) {
    const auto state = lock_post(tx, id);
    if (!state) return not_found("local post");
    // Only what Google never had. A `failed` send is still adopted if its post turns up at Google.
    if (state->status != "draft" && state->status != "rejected") {
        return conflict("local_post_not_open",
                        "Only a draft or a rejected post can be deleted. A post that is at Google, or may be, is removed there instead.");
    }
    const auto gone =
        tx.exec("DELETE FROM local_posts WHERE id = $1 RETURNING status_reason, created_by, copied_from_id", pqxx::params{id}).one_row();
    audit.record("local_post.delete", "local_post", id,
                 {.before = {{"location_id", state->location_id},
                             {"status", state->status},
                             {"status_reason", boost::json::value_from(gone[0].as<OptString>())},
                             {"created_by_user_id", boost::json::value_from(gone[1].as<std::optional<std::int64_t>>())},
                             // Which post a deleted copy was a correction of (D-098).
                             {"copied_from_id", boost::json::value_from(gone[2].as<std::optional<std::int64_t>>())},
                             {"content", state->content}}});
    return {};
}

Result<model::LocalPost> edit_local_post(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id,
                                         const domain::LocalPostContent& content, std::string_view media_base) {
    const auto state = lock_post(tx, id);
    if (!state) return not_found("local post");
    if (state->status != "draft") {
        return conflict("local_post_not_open", "Only a draft can be edited here. A post that is at Google gets an edit of its own.");
    }
    const auto normalized = domain::normalize_local_post(content);
    if (!normalized) return invalid(normalized.error());
    if (normalized->media_id && !media_state(tx, *normalized->media_id, audit.now(), media_base).exists) {
        return invalid("\"media_id\" names no uploaded file.");
    }
    const auto before = read_content(state->content);
    if (*normalized == before) {
        audit.nothing_changed();
        return *get_local_post(tx, id, media_base);
    }
    const auto report = checks_for(tx, *normalized, audit.now(), media_base);
    // What was approved before, if anything, was other content: the approval does not outlive the edit.
    tx.exec("UPDATE local_posts SET content = $2::jsonb, checks = $3::jsonb, edited_by = $4, edited_at = $5::timestamptz, "
            "status_reason = NULL, approved_content = NULL, decided_by = NULL, decided_at = NULL, "
            "updated_at = $5::timestamptz WHERE id = $1",
            pqxx::params{id, json_text(*normalized), json_text(report.results), audit.user_id(),
                         to_timestamptz(audit.now())})
        .no_rows();
    audit.record("local_post.edit", "local_post", id,
                 {.before = {{"content", boost::json::value_from(before)}},
                  .after = {{"content", boost::json::value_from(*normalized)}, {"failed_checks", boost::json::value_from(report.failed())}}});
    return *get_local_post(tx, id, media_base);
}

Result<model::LocalPost> reject_local_post(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id,
                                           const std::string& reason, std::string_view media_base) {
    const auto state = lock_post(tx, id);
    if (!state) return not_found("local post");
    if (state->status != "draft") return conflict("local_post_not_open", "Only a draft can be rejected.");
    const std::string cleaned(text::trim(reason));
    if (text::length(cleaned) > 500) return invalid("\"reason\" may be up to 500 characters.");
    tx.exec("UPDATE local_posts SET status = 'rejected', status_reason = $2, decided_by = $3, decided_at = $4::timestamptz, "
            "updated_at = $4::timestamptz WHERE id = $1",
            pqxx::params{id, cleaned.empty() ? OptString{} : OptString(cleaned), audit.user_id(), to_timestamptz(audit.now())})
        .no_rows();
    audit.record("local_post.reject", "local_post", id, {.after = {{"location_id", state->location_id}, {"reason", cleaned}}});
    return *get_local_post(tx, id, media_base);
}

Result<model::LocalPost> approve_local_post(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id,
                                            const domain::LocalPostContent& content, std::string_view media_base) {
    const auto state = lock_post(tx, id);
    if (!state) return not_found("local post");
    if (state->status != "draft") return conflict("local_post_not_open", "The post is no longer a draft waiting for approval.");
    const auto read = domain::normalize_local_post(content);
    if (!read) return invalid(read.error());
    // The approval is for the content the person read: every field of it.
    const auto stored = read_content(state->content);
    if (*read != stored) {
        return conflict("local_post_changed", "The post changed after it was shown. Read it again before approving.");
    }
    const auto now = audit.now();
    // Run now, not read from the row: time has passed since the draft was checked.
    const auto report = checks_for(tx, stored, now, media_base);
    if (!report.passed()) {
        return conflict("checks_failed", std::format("The post fails a check ({}). Edit it first.", report.failed_names()));
    }
    const auto location = sending_state(tx, state->location_id);
    if (!location) return not_found("local post");
    if (const auto off = switch_obstacle(*location)) return conflict("posting_disabled", *off);
    if (!location->synced) return conflict("location_not_synced", std::string(kNotSynced));

    tx.exec("UPDATE local_posts SET status = 'approved', status_reason = NULL, checks = $2::jsonb, decided_by = $3, "
            "decided_at = $4::timestamptz, approved_content = $5::jsonb, updated_at = $4::timestamptz WHERE id = $1",
            pqxx::params{id, json_text(report.results), audit.user_id(), to_timestamptz(now),
                         json_text(stored)})
        .no_rows();
    // One job per approval: a post that came back to the drafts and is approved again gets a new one.
    const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    jobs::sql::enqueue(tx,
                       {.kind = std::string(kPublishLocalPostKind),
                        .payload = boost::json::object{{"local_post_id", id}},
                        .run_at = std::nullopt,
                        .idempotency_key = std::format("{}@{}", id, stamp),
                        .max_attempts = kPublishLocalPostAttempts},
                       now);
    audit.record("local_post.approve", "local_post", id, {.after = {{"location_id", state->location_id}, {"content", boost::json::value_from(stored)}}});
    return *get_local_post(tx, id, media_base);
}

// ---- the publish job -------------------------------------------------------------------------

LocalPostSendClaim claim_local_post_send(pqxx::transaction_base& tx, std::int64_t id, core::TimePoint now, std::string_view media_base) {
    using Outcome = LocalPostSendClaim::Outcome;
    const auto state = lock_post(tx, id, now - approval_valid_for);
    if (!state) return {.outcome = Outcome::not_approved, .detail = "no such post"};
    // `sending`, `sent` and `unknown` belong to an earlier run of the job: nothing is sent again from here.
    if (state->status != "approved") return {.outcome = Outcome::not_approved, .detail = "the post is " + state->status};
    // Only a post written here that Google has never had is sent. The states and the approval
    // rules already say so; this is the same rule where the create starts.
    if (state->google_name || state->origin != "system") {
        return {.outcome = Outcome::not_approved, .detail = "the post has a name at Google or was not written here"};
    }
    // An approval is carried out promptly or not at all: a job that ran late (the engine was down, the
    // queue was behind, the database was restored from a backup) does not send what a person approved long ago.
    if (!state->approval_fresh) {
        const auto why = std::format("The approval was not carried out within {} minutes, so nothing was sent. Approve again.",
                                     approval_valid_for.count() / 60);
        cancel_send(tx, id, why, now);
        return {.outcome = Outcome::not_approved, .detail = "the approval is too old"};
    }
    // A person's approval of exactly this content, or nothing goes out: the two values as the row holds them.
    if (!state->decided_by || state->approved_content.is_null() || state->approved_content != state->content) {
        cancel_send(tx, id, "The post's content is not the content that was approved. Read it again and approve it.", now);
        return {.outcome = Outcome::not_approved, .detail = "the content is not the approved content"};
    }
    const auto content = read_content(state->content);
    const auto location = sending_state(tx, state->location_id);
    // The kill switches, read in the transaction that claims the send.
    if (!location || switch_obstacle(*location)) {
        const std::string off = location ? *switch_obstacle(*location) : std::string("The location no longer exists.");
        // Back to the drafts: a post never goes out later on its own because a switch was flipped back.
        cancel_send(tx, id, off, now, " The approval was not carried out; approve again when posting is on.");
        return {.outcome = Outcome::not_approved, .detail = off};
    }
    if (!location->synced) {
        cancel_send(tx, id, kNotSynced, now, " Nothing was sent.");
        return {.outcome = Outcome::not_approved, .detail = std::string(kNotSynced)};
    }
    // As things are now: the time to publish at may have passed since the approval.
    const auto report = checks_for(tx, content, now, media_base);
    if (!report.passed()) {
        const auto why = std::format("The post no longer passes a check ({}). Nothing was sent; edit it and approve again.", report.failed_names());
        tx.exec("UPDATE local_posts SET checks = $2::jsonb WHERE id = $1",
                pqxx::params{id, json_text(report.results)})
            .no_rows();
        cancel_send(tx, id, why, now);
        return {.outcome = Outcome::not_approved, .detail = why};
    }

    std::optional<gbp::MediaSource> media;
    if (content.media_id) {
        const auto file = media_state(tx, *content.media_id, now, media_base);
        media = gbp::MediaSource{.format = file.format, .url = file.url};
    }
    tx.exec("UPDATE local_posts SET status = 'sending', status_reason = NULL, send_started_at = $2::timestamptz, "
            "updated_at = $2::timestamptz WHERE id = $1",
            pqxx::params{id, to_timestamptz(now)})
        .no_rows();
    system_audit(tx, "local_post.send_started", id, {{"location_id", state->location_id}, {"approved_by_user_id", *state->decided_by}}, now);
    return {.outcome = Outcome::claimed,
            .gbp_account_id = location->gbp_account_id,
            .gbp_location_id = location->gbp_location_id,
            .body = gbp::create_body(content, media),
            .detail = {}};
}

void local_post_send_accepted(pqxx::transaction_base& tx, std::int64_t id, const gbp::LocalPost& created, core::TimePoint now) {
    const auto state = lock_post(tx, id);
    if (!state) return;
    // The answer came late: a read had already taken the send for one of unknown outcome, or given it
    // up. Google's own answer settles it, and names the post.
    const bool late = (state->status == "unknown" || state->status == "failed") && !state->google_name;
    // Anything else was settled meanwhile: a read already found the post and adopted it.
    if (state->status != "sending" && !late) return;
    // A read that ran between Google's answer and this moment may have stored
    // the post as one made at Google. It is this post: the other row goes.
    // If a person has already asked for it to be removed there, the request moves to this row; its job
    // names the other row and finds nothing, and the request is given back for the person to make again.
    const auto twin = tx.exec("DELETE FROM local_posts WHERE google_name = $1 AND id <> $2 AND origin = 'google' "
                              "RETURNING removal_requested_by, removal_requested_at",
                              pqxx::params{created.name, id});
    tx.exec("UPDATE local_posts SET status = 'sent', status_reason = NULL, google_name = $2, sent_at = $3::timestamptz, "
            "updated_at = $3::timestamptz WHERE id = $1",
            pqxx::params{id, created.name, to_timestamptz(now)})
        .no_rows();
    if (!twin.empty() && !twin[0][1].is_null()) {
        tx.exec("UPDATE local_posts SET removal_requested_by = $2, removal_requested_at = $3::timestamptz WHERE id = $1",
                pqxx::params{id, twin[0][0].as<std::optional<std::int64_t>>(), twin[0][1].as<std::string>()})
            .no_rows();
    }
    apply_local_post(tx, state->location_id, created, now);
    system_audit(tx, "local_post.sent", id, {{"location_id", state->location_id}, {"google_name", created.name}}, now);
    enqueue_follow_up_reads(tx, id, state->location_id, now);
}

void local_post_send_refused(pqxx::transaction_base& tx, std::int64_t id, const std::string& error, bool retry, core::TimePoint now) {
    const auto state = lock_post(tx, id);
    if (!state || state->status != "sending") return;
    if (retry) {
        // Nothing was created, so the job's next attempt may claim it again.
        tx.exec("UPDATE local_posts SET status = 'approved', updated_at = $2::timestamptz WHERE id = $1", pqxx::params{id, to_timestamptz(now)})
            .no_rows();
    } else {
        back_to_draft(tx, id, std::format("Google refused the post ({}). Nothing was published.", error), now);
        notify(tx, id, std::format("local_post:{}:refused:{}", id, to_timestamptz(now)), "Google refused a post",
               std::format("Google refused a post ({}). Nothing was published. The post is back among the drafts.", error), now);
    }
    system_audit(tx, "local_post.send_refused", id, {{"location_id", state->location_id}, {"error", error}, {"will_retry", retry}}, now);
}

// The messages to staff, here and in store_local_posts, say how long a send is looked for.
static_assert(kLocalPostFailAfter == std::chrono::minutes(15));

void local_post_send_unknown(pqxx::transaction_base& tx, std::int64_t id, const std::string& error, core::TimePoint now) {
    const auto state = lock_post(tx, id);
    if (!state || state->status != "sending") return;
    tx.exec("UPDATE local_posts SET status = 'unknown', status_reason = $2, updated_at = $3::timestamptz WHERE id = $1",
            pqxx::params{id,
                         std::format("Google's answer to the send never came ({}), so it is not known whether Google has the post. It is "
                                     "not sent again; the engine looks for it at Google for 15 minutes.",
                                     error),
                         to_timestamptz(now)})
        .no_rows();
    system_audit(tx, "local_post.send_outcome_unknown", id, {{"location_id", state->location_id}, {"error", error}}, now);
    notify(tx, id, std::format("local_post:{}:unknown", id), "A post may or may not have reached Google",
           "A post was sent to Google and the answer never came. It is not sent again. The engine looks for it at Google for 15 "
           "minutes and reports what it finds.",
           now);
    enqueue_follow_up_reads(tx, id, state->location_id, now);
}

std::vector<std::int64_t> release_orphaned_local_post_approvals(pqxx::transaction_base& tx, core::TimePoint approved_before,
                                                                core::TimePoint now) {
    std::vector<std::int64_t> released;
    for (const auto& row : tx.exec(std::format("SELECT p.id FROM local_posts p WHERE p.status = 'approved' AND p.decided_at <= $1::timestamptz "
                                               "AND NOT {} FOR UPDATE OF p",
                                               has_live_job("$2")),
                                   pqxx::params{to_timestamptz(approved_before), std::string(kPublishLocalPostKind)})) {
        const auto id = row[0].as<std::int64_t>();
        cancel_send(tx, id, "The job that sends the post did not finish, so nothing was sent. Approve again to retry.", now);
        released.push_back(id);
    }
    return released;
}

// ---- an edit of a post that is at Google (D-099) ------------------------------------------------

namespace {

// Why the post cannot have an edit as it stands, if it cannot: an edit is for a post Google has and
// that nobody asked to remove. (Whether its location is synced, and whether its content is something
// a draft can hold, are asked separately.)
OptString edit_refusal(const PostState& post) {
    if (post.status == "removed") return "The post is no longer at Google.";
    if (post.status != "sent" || !post.google_name) return "Only a post that is at Google has an edit. A draft is edited itself.";
    if (post.removal_requested) return "A person asked for this post to be removed at Google.";
    return std::nullopt;
}

// The post changed as the API shows it: its edit did.
void touch_post(pqxx::transaction_base& tx, std::int64_t id, core::TimePoint now) {
    tx.exec("UPDATE local_posts SET updated_at = $2::timestamptz WHERE id = $1", pqxx::params{id, to_timestamptz(now)}).no_rows();
}

// A mask's fields by the API's names, as LocalPostEdit.fields lists them.
boost::json::array members_of(const std::vector<std::string_view>& mask) {
    boost::json::array out;
    for (const auto field : mask) out.emplace_back(domain::edit_member(field));
    return out;
}

// An edit returns to the drafts with the reason: nothing goes out later by itself.
void edit_back_to_draft(pqxx::transaction_base& tx, std::int64_t id, const std::string& reason, core::TimePoint now) {
    tx.exec("UPDATE local_post_edits SET status = 'draft', status_reason = $2 WHERE local_post_id = $1", pqxx::params{id, reason}).no_rows();
    touch_post(tx, id, now);
}

// An approval of an edit that is not carried out: nothing was sent, the edit is a draft again with the
// reason, and the audit log keeps it. `advice` is what the person can do next, for the post's page only.
void cancel_edit(pqxx::transaction_base& tx, std::int64_t id, std::string_view why, core::TimePoint now, std::string_view advice = {}) {
    edit_back_to_draft(tx, id, std::format("{}{}", why, advice), now);
    system_audit(tx, "local_post.edit_cancelled", id, {{"reason", why}}, now);
}

// After an edit that reached Google, or may have, Google is read again 1, 5 and 15 minutes on, as after
// a send: to see the state Google gives the changed post, and because a list that was read a moment
// before the edit may be stored a moment after it. Under keys of this edit's own.
void enqueue_reads_after_edit(pqxx::transaction_base& tx, std::int64_t id, std::int64_t location_id, core::TimePoint now) {
    const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    for (const int minutes : {1, 5, 15}) {
        jobs::sql::enqueue(tx,
                           {.kind = std::string(kSyncLocalPostsKind),
                            .payload = boost::json::object{{"location_id", location_id}},
                            .run_at = now + std::chrono::minutes(minutes),
                            .idempotency_key = std::format("post:{}:edit@{}:{}m", id, stamp, minutes),
                            .max_attempts = kSyncLocalPostsAttempts},
                           now);
    }
}

// The edit's one request went out, or may have, and nothing says what became of it. It is not sent
// again: the edit is a draft again and says so, and staff are told. The post's row is not touched:
// what Google has is read, not guessed.
void edit_outcome_unknown(pqxx::transaction_base& tx, std::int64_t id, const PostState& post, const EditState& edit, const std::string& error,
                          core::TimePoint now) {
    edit_back_to_draft(tx, id,
                       std::format("Google did not confirm the edit ({}), so the post may or may not have changed. The edit is not sent again by "
                                   "itself. \"Refresh from Google\" shows what the post says now; approving the edit again is safe.",
                                   error),
                       now);
    system_audit(tx, "local_post.edit_outcome_unknown", id,
                 {{"location_id", post.location_id},
                  {"error", error},
                  {"edit", edit.content},
                  {"approved_by_user_id", boost::json::value_from(edit.decided_by)}},
                 now);
    notify(tx, id, std::format("local_post:{}:edit_unknown:{}", id, to_timestamptz(now)), "An edit of a post may or may not have reached Google",
           std::format("An approved edit of a post was sent to Google, and Google did not confirm it ({}). It is not sent again. The post may or "
                       "may not have changed: refresh the location's posts to see what it says now. The edit is a draft again, and approving it "
                       "again is safe.",
                       error),
           now);
    enqueue_reads_after_edit(tx, id, post.location_id, now);
}

// The post and its edit, if the edit is being sent: what the job's bookkeeping may settle.
struct EditBeingSent {
    PostState post;
    EditState edit;
};
std::optional<EditBeingSent> lock_edit_being_sent(pqxx::transaction_base& tx, std::int64_t id) {
    auto post = lock_post(tx, id);
    if (!post) return std::nullopt;
    auto edit = lock_edit(tx, id);
    if (!edit || edit->status != "sending") return std::nullopt;
    return EditBeingSent{std::move(*post), std::move(*edit)};
}

// Which upload the post carries is the engine's to know (apply_local_post keeps it): after an edit
// that changed the media, it is the edit's.
void set_post_upload(pqxx::transaction_base& tx, std::int64_t id, const std::optional<std::int64_t>& media_id) {
    tx.exec("UPDATE local_posts SET content = jsonb_set(content, '{media_id}', $2::jsonb) WHERE id = $1 AND status <> 'removed'",
            pqxx::params{id, json_text(media_id)})
        .no_rows();
}

// The edit is at Google: its row goes, and the post names its author and the moment.
void finish_edit(pqxx::transaction_base& tx, std::int64_t id, const EditState& edit, core::TimePoint now) {
    tx.exec("DELETE FROM local_post_edits WHERE local_post_id = $1", pqxx::params{id}).no_rows();
    tx.exec("UPDATE local_posts SET edited_by = $2, edited_at = $3::timestamptz, updated_at = $3::timestamptz WHERE id = $1",
            pqxx::params{id, edit.written_by, to_timestamptz(now)})
        .no_rows();
}

boost::json::value content_of_post(pqxx::transaction_base& tx, std::int64_t id) {
    return boost::json::parse(tx.query_value<std::string>("SELECT content::text FROM local_posts WHERE id = $1", pqxx::params{id}));
}

}  // namespace

Result<model::LocalPost> write_local_post_edit(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id,
                                               const domain::LocalPostContent& content, std::string_view media_base) {
    const auto post = lock_post(tx, id);
    if (!post) return not_found("local post");
    if (const auto why = edit_refusal(*post)) return conflict("local_post_not_editable", *why);
    const auto location = sending_state(tx, post->location_id);
    if (!location || !location->synced) return conflict("location_not_synced", std::string(kNotSynced));
    const auto open = lock_edit(tx, id);
    if (open && open->status != "draft") {
        return conflict("local_post_edit_not_open",
                        "The post's edit was approved and is on its way to Google. It can be replaced once it has been applied or has come back.");
    }
    const auto normalized = domain::normalize_local_post(content);
    if (!normalized) return invalid(normalized.error());
    // The edit is written against the post as it is now: what it changes is where it differs from this.
    const auto base = read_content(post->content);
    const auto changes = domain::local_post_edit_mask(base, *normalized);
    if (!changes) {
        if (changes.error().kind == domain::EditObstacle::Kind::fixed_part) return invalid(changes.error().message);
        return conflict("local_post_not_editable", changes.error().message);
    }
    // The post's own upload is on its row; another one must be a file that was uploaded.
    if (normalized->media_id && normalized->media_id != base.media_id && !media_state(tx, *normalized->media_id, audit.now(), media_base).exists) {
        return invalid("\"media_id\" names no uploaded file.");
    }
    const auto report = edit_checks_for(tx, base, *normalized, audit.now(), media_base);
    // Every save rewrites the edit whole, also one that says what the edit already said: its writer, its
    // time and the post it was written against. What was approved before does not outlive it.
    tx.exec("INSERT INTO local_post_edits (local_post_id, status, content, base_content, fields, checks, written_by, written_at) "
            "VALUES ($1, 'draft', $2::jsonb, $3::jsonb, $4::jsonb, $5::jsonb, $6, $7::timestamptz) "
            "ON CONFLICT (local_post_id) DO UPDATE SET status = 'draft', status_reason = NULL, content = EXCLUDED.content, "
            "base_content = EXCLUDED.base_content, fields = EXCLUDED.fields, checks = EXCLUDED.checks, written_by = EXCLUDED.written_by, "
            "written_at = EXCLUDED.written_at, decided_by = NULL, decided_at = NULL, approved_content = NULL, send_started_at = NULL",
            pqxx::params{id, json_text(*normalized), boost::json::serialize(post->content), boost::json::serialize(members_of(*changes)),
                         json_text(report.results), audit.user_id(), to_timestamptz(audit.now())})
        .no_rows();
    touch_post(tx, id, audit.now());
    audit.record("local_post.edit_write", "local_post", id,
                 {.before = open ? boost::json::value(boost::json::object{{"edit", open->content}, {"written_by_user_id", open->written_by}})
                                 : boost::json::value(nullptr),
                  .after = {{"location_id", post->location_id},
                            {"edit", boost::json::value_from(*normalized)},
                            {"fields", members_of(*changes)},
                            {"failed_checks", boost::json::value_from(report.failed())}}});
    return *get_local_post(tx, id, media_base);
}

Result<void> discard_local_post_edit(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id) {
    const auto post = lock_post(tx, id);
    if (!post) return not_found("local post");
    const auto edit = lock_edit(tx, id);
    if (!edit) return conflict("local_post_edit_not_open", "The post has no edit.");
    if (edit->status != "draft") {
        return conflict("local_post_edit_not_open", "The post's edit was approved and is on its way to Google. It cannot be discarded now.");
    }
    tx.exec("DELETE FROM local_post_edits WHERE local_post_id = $1", pqxx::params{id}).no_rows();
    touch_post(tx, id, audit.now());
    audit.record("local_post.edit_discard", "local_post", id,
                 {.before = {{"location_id", post->location_id},
                             {"edit", edit->content},
                             {"status_reason", boost::json::value_from(edit->status_reason)},
                             {"written_by_user_id", edit->written_by}}});
    return {};
}

Result<model::LocalPost> approve_local_post_edit(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id,
                                                 const domain::LocalPostContent& content, std::string_view media_base) {
    const auto post = lock_post(tx, id);
    if (!post) return not_found("local post");
    const auto edit = lock_edit(tx, id);
    if (!edit) return conflict("local_post_edit_not_open", "The post has no edit to approve.");
    if (edit->status != "draft") return conflict("local_post_edit_not_open", "The post's edit is no longer a draft waiting for approval.");
    const auto read = domain::normalize_local_post(content);
    if (!read) return invalid(read.error());
    // The approval is for the content the person read: every field of it.
    const auto stored = read_content(edit->content);
    if (*read != stored) return conflict("local_post_changed", "The edit changed after it was shown. Read it again before approving.");
    if (const auto why = edit_refusal(*post)) return conflict("local_post_not_editable", *why);

    // What the edit changes, against the post it was written for; and of that, what there is to send
    // against the post as the engine holds it now (the sync may have brought a change made at Google).
    const auto base = read_content(edit->base_content);
    const auto changes = domain::local_post_edit_mask(base, stored);
    if (!changes) return conflict("local_post_not_editable", changes.error().message);
    if (changes->empty()) {
        return conflict("local_post_edit_unchanged", "The edit says what the post already says, so there is nothing to send. Discard it.");
    }
    const auto to_send = domain::local_post_edit_request(base, stored, read_content(post->content));
    if (!to_send) {
        if (to_send.error().kind == domain::EditObstacle::Kind::changed_at_google) return conflict("local_post_changed", to_send.error().message);
        return conflict("local_post_not_editable", to_send.error().message);
    }
    if (to_send->empty()) {
        return conflict("local_post_edit_unchanged", "The post already says what the edit changes, so there is nothing to send. Discard it.");
    }
    const auto now = audit.now();
    // Run now, not read from the row: time has passed since the edit was checked.
    const auto report = edit_checks_for(tx, base, stored, now, media_base);
    if (!report.passed()) {
        return conflict("checks_failed", std::format("The edit fails a check ({}). Change it first.", report.failed_names()));
    }
    const auto location = sending_state(tx, post->location_id);
    if (!location) return not_found("local post");
    if (const auto off = switch_obstacle(*location)) return conflict("posting_disabled", *off);
    if (!location->synced) return conflict("location_not_synced", std::string(kNotSynced));

    tx.exec("UPDATE local_post_edits SET status = 'approved', status_reason = NULL, checks = $2::jsonb, decided_by = $3, "
            "decided_at = $4::timestamptz, approved_content = $5::jsonb WHERE local_post_id = $1",
            pqxx::params{id, json_text(report.results), audit.user_id(), to_timestamptz(now), json_text(stored)})
        .no_rows();
    touch_post(tx, id, now);
    // One job per approval: an edit that came back to the drafts and is approved again gets a new one.
    const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    jobs::sql::enqueue(tx,
                       {.kind = std::string(kEditLocalPostKind),
                        .payload = boost::json::object{{"local_post_id", id}},
                        .run_at = std::nullopt,
                        .idempotency_key = std::format("{}@{}", id, stamp),
                        .max_attempts = kEditLocalPostAttempts},
                       now);
    audit.record("local_post.edit_approve", "local_post", id,
                 {.after = {{"location_id", post->location_id}, {"edit", boost::json::value_from(stored)}, {"fields", members_of(*changes)}}});
    return *get_local_post(tx, id, media_base);
}

LocalPostEditClaim claim_local_post_edit(pqxx::transaction_base& tx, std::int64_t id, core::TimePoint now, std::string_view media_base) {
    using Outcome = LocalPostEditClaim::Outcome;
    const auto refused = [](std::string detail) { return LocalPostEditClaim{.outcome = Outcome::not_approved, .name = {}, .detail = std::move(detail)}; };
    const auto post = lock_post(tx, id);
    if (!post) return refused("no such post");
    const auto edit = lock_edit(tx, id, now - approval_valid_for);
    if (!edit) return refused("the post has no edit");
    // `sending` belongs to an earlier run of the job, which settles it, or to an engine that stopped: the
    // sweep gives that one back once no job is left for it. Nothing is sent from here either way.
    if (edit->status != "approved") return refused(edit->status == "sending" ? "the edit is being sent already" : "the edit is a " + edit->status);
    // An approval is carried out promptly or not at all, as for a first send.
    if (!edit->approval_fresh) {
        cancel_edit(tx, id,
                    std::format("The approval was not carried out within {} minutes, so nothing was sent. Approve again.",
                                approval_valid_for.count() / 60),
                    now);
        return refused("the approval is too old");
    }
    // A person's approval of exactly this content, or nothing goes out: the two values as the row holds them.
    if (!edit->decided_by || edit->approved_content.is_null() || edit->approved_content != edit->content) {
        cancel_edit(tx, id, "The edit's content is not the content that was approved. Read it again and approve it.", now);
        return refused("the content is not the approved content");
    }
    // The post is still one that can have an edit. A removal that was asked for meanwhile wins.
    if (const auto why = edit_refusal(*post)) {
        cancel_edit(tx, id, *why, now, " The edit was not sent.");
        return refused(*why);
    }
    const auto location = sending_state(tx, post->location_id);
    // The kill switches, read in the transaction that claims the edit.
    if (!location || switch_obstacle(*location)) {
        const std::string off = location ? *switch_obstacle(*location) : std::string("The location no longer exists.");
        cancel_edit(tx, id, off, now, " The approval was not carried out; approve again when posting is on.");
        return refused(off);
    }
    if (!location->synced) {
        cancel_edit(tx, id, kNotSynced, now, " Nothing was sent.");
        return refused(std::string(kNotSynced));
    }
    // The post is edited as a post of its own location, under that location's ids as they are now, or not at all.
    const auto name = gbp::parse_local_post_name(*post->google_name);
    if (!name || name->location_id != location->gbp_location_id) {
        cancel_edit(tx, id, "The post's name at Google is not one of its location's, so the edit was not sent.", now);
        return refused("the post's name at Google is not one of its location's");
    }
    // As things are now: time has passed since the approval.
    const auto report = edit_checks_for(tx, read_content(edit->base_content), read_content(edit->content), now, media_base);
    if (!report.passed()) {
        const auto why = std::format("The edit no longer passes a check ({}). Nothing was sent; change it and approve again.", report.failed_names());
        tx.exec("UPDATE local_post_edits SET checks = $2::jsonb WHERE local_post_id = $1", pqxx::params{id, json_text(report.results)}).no_rows();
        cancel_edit(tx, id, why, now);
        return refused(why);
    }

    tx.exec("UPDATE local_post_edits SET status = 'sending', status_reason = NULL, send_started_at = $2::timestamptz WHERE local_post_id = $1",
            pqxx::params{id, to_timestamptz(now)})
        .no_rows();
    touch_post(tx, id, now);
    system_audit(tx, "local_post.edit_started", id, {{"location_id", post->location_id}, {"approved_by_user_id", *edit->decided_by}}, now);
    return {.outcome = Outcome::claimed,
            .name = {.account_id = location->gbp_account_id, .location_id = location->gbp_location_id, .post_id = name->post_id},
            .detail = {}};
}

LocalPostEditRequest prepare_local_post_edit(pqxx::transaction_base& tx, std::int64_t id, const gbp::LocalPost& read, core::TimePoint now,
                                             std::string_view media_base) {
    using Outcome = LocalPostEditRequest::Outcome;
    const auto settled = [](std::string detail) {
        return LocalPostEditRequest{.outcome = Outcome::settled, .mask = {}, .body = {}, .content = {}, .detail = std::move(detail)};
    };
    const auto being_sent = lock_edit_being_sent(tx, id);
    if (!being_sent) return settled("the edit is no longer being sent");
    const auto& [post, edit] = *being_sent;
    // Asked again where the request is about to start: a removal that was requested since the claim wins,
    // and a post that was marked removed is not written to.
    if (const auto why = edit_refusal(post)) {
        cancel_edit(tx, id, *why, now, " The edit was not sent.");
        return settled(*why);
    }
    // What was read is this post: the one of its location with its id (the account in a stored name may be an old one).
    const auto held = gbp::parse_local_post_name(*post.google_name);
    const auto got = gbp::parse_local_post_name(read.name);
    if (!held || !got || got->location_id != held->location_id || got->post_id != held->post_id) {
        cancel_edit(tx, id, "Google answered with another post than the one asked for, so the edit was not sent.", now);
        return settled("the post that was read is not this post");
    }
    // The post as Google has it now, on its row: what the sync would store, by the same code. From here
    // the row says what Google has.
    apply_local_post(tx, post.location_id, read, now);
    const auto content = read_content(edit.content);
    const auto current = read_content(content_of_post(tx, id));
    // Of the fields the edit changes, those Google does not have yet; and no field the edit does not change.
    const auto mask = domain::local_post_edit_request(read_content(edit.base_content), content, current);
    if (!mask) {
        const auto why = mask.error().kind == domain::EditObstacle::Kind::changed_at_google ? mask.error().message
                                                                                             : std::format("{} Nothing was sent.", mask.error().message);
        cancel_edit(tx, id, why, now);
        return settled(why);
    }
    if (mask->empty()) {
        // Google has what the edit changes already (an earlier request that was never confirmed arrived
        // after all, or someone made the same change there). Nothing is sent, and the edit is done.
        finish_edit(tx, id, edit, now);
        audit::record(tx, engine_actor(),
                      {.action = "local_post.edit_applied",
                       .object_type = "local_post",
                       .object_id = std::to_string(id),
                       .before = nullptr,
                       .after = boost::json::object{{"location_id", post.location_id},
                                                    {"content", boost::json::value_from(current)},
                                                    {"mask", boost::json::array{}},
                                                    {"sent", false},
                                                    {"written_by_user_id", edit.written_by},
                                                    {"approved_by_user_id", boost::json::value_from(edit.decided_by)}}},
                      now);
        return settled("Google has this content already");
    }
    std::optional<gbp::MediaSource> media;
    if (content.media_id && std::ranges::contains(*mask, std::string_view("media"))) {
        const auto file = media_state(tx, *content.media_id, now, media_base);
        media = gbp::MediaSource{.format = file.format, .url = file.url};
    }
    return {.outcome = Outcome::send, .mask = *mask, .body = gbp::update_body(read, content, *mask, media), .content = content, .detail = {}};
}

void local_post_edit_accepted(pqxx::transaction_base& tx, std::int64_t id, const domain::LocalPostContent& sent,
                              const std::vector<std::string_view>& mask, const gbp::LocalPost& updated, core::TimePoint now) {
    const auto being_sent = lock_edit_being_sent(tx, id);
    if (!being_sent) return;
    const auto& [post, edit] = *being_sent;
    const auto before = content_of_post(tx, id);
    // Google's answer is the post now: content, state and times, as the sync stores them.
    apply_local_post(tx, post.location_id, updated, now);
    if (std::ranges::contains(mask, std::string_view("media"))) set_post_upload(tx, id, sent.media_id);
    finish_edit(tx, id, edit, now);
    const auto after = content_of_post(tx, id);

    // Nobody can rule out that Google accepts a field in the mask and leaves it as it was. What the
    // answer says is on the row either way; where it does not say what was sent, staff are told. (The
    // media cannot be compared: Google answers with addresses of its own.)
    boost::json::array not_shown;
    if (const auto still = domain::local_post_edit_mask(read_content(after), sent); !still) {
        // Not a post the edit can be compared with any more: its kind, time or repeat, or what a draft cannot hold.
        not_shown.emplace_back(still.error().member.empty() ? std::string("the post") : still.error().member);
    } else {
        for (const auto field : *still) {
            if (field != "media" && std::ranges::contains(mask, field)) not_shown.emplace_back(domain::edit_member(field));
        }
    }
    audit::record(tx, engine_actor(),
                  {.action = "local_post.edit_applied",
                   .object_type = "local_post",
                   .object_id = std::to_string(id),
                   .before = boost::json::object{{"content", before}},
                   .after = boost::json::object{{"location_id", post.location_id},
                                                {"content", after},
                                                {"approved_content", boost::json::value_from(sent)},
                                                {"mask", boost::json::value_from(mask)},
                                                {"sent", true},
                                                {"not_shown", not_shown},
                                                {"google_state", boost::json::value_from(gbp::to_google_state(updated))},
                                                {"written_by_user_id", edit.written_by},
                                                {"approved_by_user_id", boost::json::value_from(edit.decided_by)}}},
                  now);
    if (!not_shown.empty()) {
        notify(tx, id, std::format("local_post:{}:edit_not_shown:{}", id, to_timestamptz(now)), "Google accepted an edit, but the post does not show it",
               std::format("Google accepted an approved edit of a post, but the post as Google answered with it does not say what the edit "
                           "says ({}). Google may need a moment, or may not change that part of a post. The engine reads the post again in a "
                           "minute; look at the post and at the profile.",
                           boost::json::serialize(not_shown)),
               now);
    }
    enqueue_reads_after_edit(tx, id, post.location_id, now);
}

void local_post_edit_not_made(pqxx::transaction_base& tx, std::int64_t id, LocalPostEditFailure failure, const std::string& error, bool retry,
                              core::TimePoint now) {
    const auto being_sent = lock_edit_being_sent(tx, id);
    if (!being_sent) return;
    const std::string_view action = failure == LocalPostEditFailure::refused ? "local_post.edit_refused" : "local_post.edit_not_sent";
    if (retry) {
        // Nothing changed at Google, so the job's next attempt may claim it again.
        tx.exec("UPDATE local_post_edits SET status = 'approved' WHERE local_post_id = $1", pqxx::params{id}).no_rows();
        touch_post(tx, id, now);
    } else {
        std::string reason;
        switch (failure) {
            case LocalPostEditFailure::refused: reason = std::format("Google refused the edit ({}). The post is as it was.", error); break;
            case LocalPostEditFailure::unreadable:
                reason = std::format("The post could not be read from Google before the edit ({}), so nothing was sent. The post is as it was.", error);
                break;
            case LocalPostEditFailure::gone:
                reason = std::format("Google does not find the post ({}): it may no longer be there. The edit was not sent; \"Refresh from "
                                     "Google\" shows what is there.",
                                     error);
                break;
            case LocalPostEditFailure::internal:
                reason = std::format("The engine could not prepare the edit ({}), so nothing was sent. The post is as it was. Approve again; if "
                                     "it happens again, the engine's log says why.",
                                     error);
                break;
        }
        edit_back_to_draft(tx, id, reason, now);
        notify(tx, id, std::format("local_post:{}:edit_not_made:{}", id, to_timestamptz(now)), "A post could not be edited",
               std::format("An approved edit of a post was not made. {} The edit is a draft again.", reason), now);
    }
    system_audit(tx, std::string(action), id, {{"location_id", being_sent->post.location_id}, {"error", error}, {"will_retry", retry}}, now);
}

void local_post_edit_unknown(pqxx::transaction_base& tx, std::int64_t id, const std::string& error, core::TimePoint now) {
    const auto being_sent = lock_edit_being_sent(tx, id);
    if (!being_sent) return;
    edit_outcome_unknown(tx, id, being_sent->post, being_sent->edit, error, now);
}

std::vector<std::int64_t> release_orphaned_local_post_edits(pqxx::transaction_base& tx, core::TimePoint approved_before, core::TimePoint now) {
    std::vector<std::int64_t> released;
    // The post's row first, as everywhere; its edit is locked below. Never by an edit's age alone: while a
    // job for it is queued or running, that job may still be on its way to the request.
    for (const auto& row : tx.exec(std::format("SELECT p.id FROM local_posts p JOIN local_post_edits e ON e.local_post_id = p.id "
                                               "WHERE ((e.status = 'approved' AND e.decided_at <= $1::timestamptz) OR e.status = 'sending') "
                                               "AND NOT {} ORDER BY p.id FOR UPDATE OF p",
                                               has_live_job("$2")),
                                   pqxx::params{to_timestamptz(approved_before), std::string(kEditLocalPostKind)})) {
        const auto id = row[0].as<std::int64_t>();
        const auto post = lock_post(tx, id);
        const auto edit = lock_edit(tx, id);
        if (!post || !edit) continue;
        if (edit->status == "approved") {
            cancel_edit(tx, id, "The job that sends the edit did not finish, so nothing was sent. Approve again to retry.", now);
        } else if (edit->status == "sending") {
            // Claimed, and its job ended without writing down what came of it (the engine stopped, or the
            // bookkeeping failed): whether the request went out is not known.
            edit_outcome_unknown(tx, id, *post, *edit, "the job that sends the edit ended without an answer", now);
        } else {
            continue;
        }
        released.push_back(id);
    }
    return released;
}

// ---- media -------------------------------------------------------------------------------------

model::LocalPostMedia create_local_post_media(pqxx::transaction_base& tx, audit::Writer& audit,
                                              const NewLocalPostMedia& media, std::string_view media_base) {
    const auto row = tx.exec(std::format("INSERT INTO local_post_media (object_name, format, content_type, bytes, width, height, created_by, "
                                         "created_at, duration_ms) VALUES ($1, $2, $3, $4, $5, $6, $7, $8::timestamptz, $9) "
                                         "RETURNING id, (SELECT u.email FROM users u WHERE u.id = $7), {}",
                                         iso("created_at")),
                             pqxx::params{media.object_name, media.format, media.content_type, media.bytes, media.width, media.height, audit.user_id(),
                                          to_timestamptz(audit.now()), media.duration_ms})
                         .one_row();
    const auto id = row[0].as<std::int64_t>();
    audit.record("local_post_media.upload", "local_post_media", id,
                 {.after = {{"format", media.format},
                            {"content_type", media.content_type},
                            {"bytes", media.bytes},
                            {"width", media.width ? boost::json::value(*media.width) : boost::json::value(nullptr)},
                            {"height", media.height ? boost::json::value(*media.height) : boost::json::value(nullptr)},
                            {"duration_ms", media.duration_ms ? boost::json::value(*media.duration_ms) : boost::json::value(nullptr)}}});
    return {.id = id,
            .format = media.format,
            .bytes = media.bytes,
            .width = media.width,
            .height = media.height,
            .url = std::string(media_base) + media.object_name,
            .created_by = row[1].as<OptString>(),
            .created_at = row[2].as<std::string>()};
}

// ---- removal ---------------------------------------------------------------------------------

Result<model::LocalPost> request_local_post_removal(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id,
                                                    std::string_view media_base) {
    const auto state = lock_post(tx, id);
    if (!state) return not_found("local post");
    if (state->status == "removed") {
        audit.nothing_changed();
        return *get_local_post(tx, id, media_base);
    }
    if (!state->google_name) {
        const bool open = state->status == "sending" || state->status == "unknown";
        return conflict("local_post_not_at_google",
                        open ? "It is not known yet whether Google has this post, so there is nothing to delete. Try again in a few minutes."
                             : "Google does not have this post, so there is nothing to delete there.");
    }
    const auto location = sending_state(tx, state->location_id);
    if (!location || !location->synced) return conflict("location_not_synced", std::string(kNotSynced));

    // Asked already, and the job that carries it out is still there: nothing new.
    const bool under_way = !tx.exec("SELECT 1 FROM local_posts p WHERE p.id = $1 AND p.removal_requested_at IS NOT NULL AND " + has_live_job("$2"),
                                    pqxx::params{id, std::string(kRemoveLocalPostKind)})
                                .empty();
    if (under_way) {
        audit.nothing_changed();
        return *get_local_post(tx, id, media_base);
    }

    const auto now = audit.now();
    // An edit never stands in the way of a removal (D-099). A draft edit goes with the request, and the
    // entry below keeps what it said. An approved one is stopped when its job claims it; one that is
    // being sent cannot be called back, and its own bookkeeping settles it before the delete or after.
    boost::json::value discarded_edit = nullptr;
    if (const auto edit = lock_edit(tx, id); edit && edit->status == "draft") {
        tx.exec("DELETE FROM local_post_edits WHERE local_post_id = $1", pqxx::params{id}).no_rows();
        discarded_edit = boost::json::object{{"content", edit->content}, {"written_by_user_id", edit->written_by}};
    }
    tx.exec("UPDATE local_posts SET removal_requested_by = $2, removal_requested_at = $3::timestamptz, updated_at = $3::timestamptz WHERE id = $1",
            pqxx::params{id, audit.user_id(), to_timestamptz(now)})
        .no_rows();
    const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    jobs::sql::enqueue(tx,
                       {.kind = std::string(kRemoveLocalPostKind),
                        .payload = boost::json::object{{"local_post_id", id}},
                        .run_at = std::nullopt,
                        .idempotency_key = std::format("{}@{}", id, stamp),
                        .max_attempts = kRemoveLocalPostAttempts},
                       now);
    // What is about to be deleted, kept: Google keeps nothing of a deleted post.
    auto post = *get_local_post(tx, id, media_base);
    audit.record("local_post.remove_requested", "local_post", id,
                 {.after = {{"location_id", state->location_id},
                            {"google_name", *state->google_name},
                            {"origin", post.origin},
                            {"google_state", post.google_state ? boost::json::value(*post.google_state) : boost::json::value(nullptr)},
                            {"content", boost::json::value_from(post.content())},
                            {"media", boost::json::value_from(post.media)},
                            {"discarded_edit", discarded_edit}}});
    return post;
}

std::expected<gbp::LocalPostName, std::string> local_post_removal_target(pqxx::transaction_base& tx, std::int64_t id, core::TimePoint now) {
    const auto post = db::row_as<RemovalRequest>(tx,
                                                 "SELECT status, google_name, removal_requested_by, location_id, "
                                                 "coalesce(removal_requested_at >= $2::timestamptz, false) AS fresh FROM local_posts WHERE id = $1 FOR UPDATE",
                                                 pqxx::params{id, to_timestamptz(now - removal_valid_for)});
    if (!post) return std::unexpected("no such post");
    if (post->status == "removed") return std::unexpected("the post is removed already");
    // Only a person's request deletes anything.
    if (!post->removal_requested_by) return std::unexpected("nobody asked for this post to be removed");

    const auto cancel = [&](std::string why) {
        local_post_removal_failed(tx, id, why, false, now);
        return std::unexpected(std::move(why));
    };
    // A request is carried out promptly or not at all: a job that ran late deletes nothing a person asked for long ago.
    if (!post->fresh) {
        return cancel(std::format("the request was not carried out within {} minutes; ask again", removal_valid_for.count() / 60));
    }
    const auto name = post->google_name ? gbp::parse_local_post_name(*post->google_name) : std::nullopt;
    if (!name) return cancel("the post has no name at Google that the engine can use");
    const auto location = sending_state(tx, post->location_id);
    if (!location || !location->synced) return cancel(std::string(kNotSynced));
    // The post is deleted as a post of its own location, or not at all.
    if (name->location_id != location->gbp_location_id) return cancel("the post's name at Google is not one of its location's");
    // The location's ids as they are now: an account may have changed since the post was read.
    return gbp::LocalPostName{.account_id = location->gbp_account_id, .location_id = location->gbp_location_id, .post_id = name->post_id};
}

void apply_local_post_before_removal(pqxx::transaction_base& tx, std::int64_t id, const gbp::LocalPost& post, core::TimePoint now) {
    const auto rows = tx.exec("SELECT location_id FROM local_posts WHERE id = $1 AND status <> 'removed' FOR UPDATE", pqxx::params{id});
    if (rows.empty()) return;
    apply_local_post(tx, rows[0][0].as<std::int64_t>(), post, now);
}

void local_post_removed(pqxx::transaction_base& tx, std::int64_t id, core::TimePoint now) {
    // Whatever an earlier attempt said about not being able to delete it no longer holds.
    const auto rows = tx.exec("UPDATE local_posts SET status = 'removed', status_reason = NULL, removed_at = $2::timestamptz, "
                              "updated_at = $2::timestamptz WHERE id = $1 AND status <> 'removed' RETURNING google_name, removal_requested_by",
                              pqxx::params{id, to_timestamptz(now)});
    if (rows.empty()) return;
    // The request may have been given back by another run of the job a moment ago; the post is gone all the same.
    const auto requested_by = rows[0][1].as<std::optional<std::int64_t>>();
    const auto post = *get_local_post(tx, id, {});
    system_audit(tx, "local_post.removed", id,
                 {{"google_name", rows[0][0].as<std::string>()},
                  {"requested_by_user_id", requested_by ? boost::json::value(*requested_by) : boost::json::value(nullptr)},
                  {"content", boost::json::value_from(post.content())},
                  {"media", boost::json::value_from(post.media)}},
                 now);
}

void local_post_removal_failed(pqxx::transaction_base& tx, std::int64_t id, const std::string& error, bool uncertain, core::TimePoint now) {
    // What is known: after a refusal the post is there; after an answer that never came it may or may not be.
    const std::string_view where = uncertain ? "It may still be there: the next read shows." : "It is still there.";
    const auto rows = tx.exec("UPDATE local_posts SET removal_requested_by = NULL, removal_requested_at = NULL, status_reason = $2, "
                              "updated_at = $3::timestamptz WHERE id = $1 AND removal_requested_at IS NOT NULL AND status <> 'removed' RETURNING id",
                              pqxx::params{id, std::format("The post could not be deleted at Google ({}). {}", error, where), to_timestamptz(now)});
    if (rows.empty()) return;
    system_audit(tx, "local_post.remove_failed", id, {{"error", error}, {"uncertain", uncertain}}, now);
    notify(tx, id, std::format("local_post:{}:remove_failed:{}", id, to_timestamptz(now)), "A post could not be removed",
           std::format("A post could not be deleted at Google ({}). {} Look at the profile, then try again or remove it in Google's own "
                       "interface.",
                       error, where),
           now);
}

std::vector<std::int64_t> release_orphaned_local_post_removals(pqxx::transaction_base& tx, core::TimePoint requested_before,
                                                               core::TimePoint now) {
    std::vector<std::int64_t> released;
    for (const auto& row : tx.exec(std::format("SELECT p.id FROM local_posts p WHERE p.removal_requested_at <= $1::timestamptz "
                                               "AND p.status <> 'removed' AND NOT {} FOR UPDATE OF p",
                                               has_live_job("$2")),
                                   pqxx::params{to_timestamptz(requested_before), std::string(kRemoveLocalPostKind)})) {
        const auto id = row[0].as<std::int64_t>();
        local_post_removal_failed(tx, id, "the job that deletes the post did not finish", true, now);
        released.push_back(id);
    }
    return released;
}

// ---- the sync ----------------------------------------------------------------------------

Result<void> request_local_posts_sync(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t location_id) {
    if (tx.exec("SELECT 1 FROM locations WHERE id = $1", pqxx::params{location_id}).empty()) return not_found("location");
    if (!syncable_location(tx, location_id)) {
        return conflict("location_not_synced", std::string(kNotSynced));
    }
    jobs::sql::enqueue(tx,
                       {.kind = std::string(kSyncLocalPostsKind),
                        .payload = boost::json::object{{"location_id", location_id}},
                        .run_at = std::nullopt,
                        .idempotency_key = std::nullopt,
                        .max_attempts = kSyncLocalPostsAttempts},
                       audit.now());
    audit.record("local_posts.refresh", "location", location_id);
    return {};
}

std::vector<SyncLocation> locations_due_for_local_posts_sync(pqxx::transaction_base& tx, core::TimePoint read_before) {
    return db::rows_as<SyncLocation>(
        tx,
        "SELECT l.id, l.gbp_location_id, l.gbp_account_id FROM locations l JOIN clients c ON c.id = l.client_id "
        "WHERE l.access_status = 'active' AND NOT c.archived "
        "AND (l.local_posts_synced_at IS NULL OR l.local_posts_synced_at <= $1::timestamptz "
        "     OR EXISTS (SELECT 1 FROM local_posts p WHERE p.location_id = l.id AND p.status IN ('sending', 'unknown'))) "
        "ORDER BY l.id",
        pqxx::params{to_timestamptz(read_before)});
}

AppliedLocalPost apply_local_post(pqxx::transaction_base& tx, std::int64_t location_id, const gbp::LocalPost& post, core::TimePoint now) {
    auto content = gbp::to_content(post);
    // The payload keeps what Google called it; the column takes the kinds the engine knows.
    if (!std::ranges::contains(kLocalPostTopicTypes, std::string_view(content.topic_type))) content.topic_type = "standard";
    const auto state = gbp::to_google_state(post);
    boost::json::array media;
    if (post.media) {
        for (const auto& item : *post.media) {
            if (!item.googleUrl) continue;
            media.push_back(boost::json::object{{"format", item.mediaFormat == "VIDEO" ? "video" : "photo"}, {"url", *item.googleUrl}});
        }
    }
    const auto created = post.createTime ? domain::canonical_timestamp(*post.createTime) : OptString{};
    const auto updated = post.updateTime ? domain::canonical_timestamp(*post.updateTime) : OptString{};
    const auto raw = boost::json::serialize(post.raw);
    const auto now_text = to_timestamptz(now);

    // By its name, or as the post of this location with that id: a location that moved to another
    // account keeps its posts, and Google then names them under the new account.
    const auto parsed = gbp::parse_local_post_name(post.name);
    const auto existing = tx.exec("SELECT id, status, google_state, media_id FROM local_posts WHERE google_name = $1 "
                                  "OR (location_id = $2 AND google_name IS NOT NULL AND split_part(google_name, '/', 6) = $3) "
                                  "ORDER BY (google_name = $1) DESC, id LIMIT 1 FOR UPDATE",
                                  pqxx::params{post.name, location_id, parsed ? parsed->post_id : std::string{}});
    if (existing.empty()) {
        tx.exec("INSERT INTO local_posts (location_id, origin, status, google_name, google_state, search_url, google_media, "
                "google_created_at, google_updated_at, raw, synced_at, created_at, updated_at, content) "
                "VALUES ($1, 'google', 'sent', $2, $3, $4, $5::jsonb, $6::timestamptz, $7::timestamptz, $8::jsonb, "
                "$9::timestamptz, $9::timestamptz, $9::timestamptz, $10::jsonb)",
                pqxx::params{location_id, post.name, state, post.searchUrl, boost::json::serialize(media), created, updated, raw, now_text,
                             json_text(content)})
            .no_rows();
        return {.inserted = true, .rejected_now = false};
    }
    const auto id = existing[0][0].as<std::int64_t>();
    if (existing[0][1].as<std::string>() == "removed") return {};
    const bool rejected_now = state == "rejected" && existing[0][2].as<OptString>() != "rejected";

    // The content is Google's from now on, so a change made at Google shows.
    // What was approved stays in approved_content and in the audit log. Which
    // upload the post carries stays too: that is ours to know, Google's post does not say.
    content.media_id = existing[0][3].as<std::optional<std::int64_t>>();
    tx.exec("UPDATE local_posts SET google_name = $9, google_state = $2, search_url = $3, google_media = $4::jsonb, "
            "google_created_at = $5::timestamptz, google_updated_at = $6::timestamptz, "
            "updated_at = CASE WHEN raw IS DISTINCT FROM $7::jsonb THEN $8::timestamptz ELSE updated_at END, "
            "raw = $7::jsonb, synced_at = $8::timestamptz, content = $10::jsonb WHERE id = $1",
            pqxx::params{id, state, post.searchUrl, boost::json::serialize(media), created, updated, raw, now_text, post.name,
                         json_text(content)})
        .no_rows();
    if (rejected_now) {
        system_audit(tx, "local_post.rejected_by_google", id, {{"google_name", post.name}}, now);
        // Each time it happens: a post that an edit brought back can be rejected again (D-099).
        notify(tx, id, std::format("local_post:{}:rejected:{}", id, now_text), "Google rejected a post",
               "Google rejected a post for its content and does not show it. Google does not say why. Edit the post, or remove it and write "
               "a new one.",
               now);
    }
    return {.inserted = false, .rejected_now = rejected_now};
}

LocalPostsSyncOutcome store_local_posts(pqxx::transaction_base& tx, std::int64_t location_id,
                                        const std::vector<gbp::LocalPost>& posts, core::TimePoint now) {
    LocalPostsSyncOutcome out;
    out.listed = static_cast<int>(posts.size());
    const auto now_text = to_timestamptz(now);
    // One sync of a location at a time: two would both adopt the same post.
    tx.exec("SELECT 1 FROM locations WHERE id = $1 FOR UPDATE", pqxx::params{location_id}).one_row();

    // ---- 1. sends whose outcome is not settled --------------------------------------------
    std::set<std::string> taken;  // names that a row already has
    for (const auto& row : tx.exec("SELECT google_name FROM local_posts WHERE location_id = $1 AND google_name IS NOT NULL", pqxx::params{location_id})) {
        taken.insert(row[0].as<std::string>());
    }
    // A send, and the span in which Google can have created its post: from a
    // little before the engine's clock says it started (the clocks differ) to
    // the moment the engine gives it up.
    struct Send {
        std::int64_t id = 0;
        std::string status;
        bool overdue = false;
        domain::LocalPostContent content;
        std::string started;   // canonical timestamps, which compare as text
        std::string earliest;
        std::string latest;
        bool settled = false;
    };
    std::vector<Send> open;       // unknown, failed, or left `sending` by an engine that stopped
    std::vector<Send> in_flight;  // the job is at work on it right now
    // A `failed` send is still looked for: the post may show at Google after all.
    const auto unsettled = tx.exec(
        std::format("SELECT p.id, p.status, p.send_started_at <= $2::timestamptz, p.send_started_at <= $3::timestamptz, {}, {}, {}, p.content "
                    "FROM local_posts p WHERE p.location_id = $1 AND p.google_name IS NULL AND p.send_started_at IS NOT NULL "
                    "AND p.status IN ('sending', 'unknown', 'failed') ORDER BY p.send_started_at, p.id FOR UPDATE",
                    iso("p.send_started_at"), iso("(p.send_started_at - make_interval(secs => $4))"),
                    iso("(p.send_started_at + make_interval(secs => $5))")),
        pqxx::params{location_id, to_timestamptz(now - kLocalPostStaleSendingAfter), to_timestamptz(now - kLocalPostFailAfter),
                     static_cast<double>(kLocalPostAdoptTolerance.count()),
                     static_cast<double>((kLocalPostFailAfter + kLocalPostAdoptTolerance).count())});
    for (const auto& row : unsettled) {
        Send send{.id = row[0].as<std::int64_t>(),
                  .status = row[1].as<std::string>(),
                  .overdue = row[3].as<bool>(),
                  .content = read_content(boost::json::parse(row[7].view())),
                  .started = row[4].as<std::string>(),
                  .earliest = row[5].as<std::string>(),
                  .latest = row[6].as<std::string>(),
                  .settled = false};
        const bool stale = row[2].as<bool>();
        // The job is at work on it, or was a moment ago: its bookkeeping names the post, not this read.
        (send.status == "sending" && !stale ? in_flight : open).push_back(std::move(send));
    }
    const auto could_be = [](const Send& send, const domain::LocalPostContent& content, const std::string& created) {
        return created >= send.earliest && created <= send.latest && same_post(send.content, content);
    };

    // Each post that no row has, oldest first, goes to the send it most
    // plausibly came from: the one that started last before the post was
    // created. So the copy of a failed post, sent again, gets its own post,
    // and a post is never given to a send that had long been given up.
    std::vector<const gbp::LocalPost*> unowned;
    std::set<std::string> held_back;  // posts a running send may have made: left for its job
    for (const auto& post : posts) {
        if (taken.contains(post.name) || !post.createTime || !domain::canonical_timestamp(*post.createTime)) continue;
        unowned.push_back(&post);
    }
    std::ranges::stable_sort(unowned, {}, [](const gbp::LocalPost* post) { return *domain::canonical_timestamp(*post->createTime); });
    for (const auto* post : unowned) {
        const auto created = *domain::canonical_timestamp(*post->createTime);
        const auto content = gbp::to_content(*post);
        if (std::ranges::any_of(in_flight, [&](const Send& send) { return could_be(send, content, created); })) {
            held_back.insert(post->name);
            continue;
        }
        Send* owner = nullptr;
        for (auto& send : open) {
            if (send.settled || !could_be(send, content, created)) continue;
            if (!owner || send.started > owner->started) owner = &send;
        }
        if (!owner) continue;
        tx.exec("UPDATE local_posts SET google_name = $2, status = 'sent', sent_at = $3::timestamptz, status_reason = $4, "
                "updated_at = $5::timestamptz WHERE id = $1",
                pqxx::params{owner->id, post->name, created,
                             owner->status == "failed" ? OptString("The post was found at Google after its send had been reported as failed.")
                                                       : OptString{},
                             now_text})
            .no_rows();
        system_audit(tx, "local_post.adopted", owner->id, {{"google_name", post->name}, {"was", owner->status}}, now);
        owner->settled = true;
        taken.insert(post->name);
        ++out.adopted;
    }

    for (auto& send : open) {
        if (send.settled) continue;
        if (send.status == "sending") {
            // The engine stopped between the claim and the bookkeeping. Whether the create went out is not known.
            tx.exec("UPDATE local_posts SET status = 'unknown', status_reason = $2, updated_at = $3::timestamptz WHERE id = $1",
                    pqxx::params{send.id,
                                 "The engine stopped while this post was being sent, so it is not known whether Google got it. It is not sent again.",
                                 now_text})
                .no_rows();
            system_audit(tx, "local_post.send_outcome_unknown", send.id, {{"reason", "the engine stopped while sending"}}, now);
            notify(tx, send.id, std::format("local_post:{}:unknown", send.id), "A post may or may not have reached Google",
                   "The engine stopped while a post was being sent. It is not sent again. The engine looks for it at Google for 15 minutes.",
                   now);
            send.status = "unknown";
        }
        if (send.status == "unknown" && send.overdue) {
            tx.exec("UPDATE local_posts SET status = 'failed', status_reason = $2, updated_at = $3::timestamptz WHERE id = $1",
                    pqxx::params{send.id,
                                 "The post was sent to Google, the answer never came, and the post was not at Google 15 minutes later. "
                                 "Look at the profile before sending it again as a new post.",
                                 now_text})
                .no_rows();
            system_audit(tx, "local_post.send_failed", send.id, {{"reason", "not at Google 15 minutes after the send"}}, now);
            notify(tx, send.id, std::format("local_post:{}:failed", send.id), "A post did not reach Google",
                   "A post was sent to Google, the answer never came, and the post was not at Google 15 minutes later. It was not sent "
                   "again. Look at the profile, then copy the post as a new draft if it is not there.",
                   now);
            ++out.failed;
        }
    }

    // ---- 2. every listed post, by its name --------------------------------------------------
    std::vector<std::string> listed;
    for (const auto& post : posts) {
        listed.push_back(post.name);
        // A post that a running send may have just made: the job stores it under its own row in a
        // moment. Stored here, it would be taken for a post made at Google.
        if (held_back.contains(post.name)) continue;
        const auto applied = apply_local_post(tx, location_id, post, now);
        if (applied.inserted) ++out.added;
        if (applied.rejected_now) ++out.rejected;
    }

    // ---- 3. posts the engine holds that the list lacks: asked for one by one by the caller ---
    // Each is asked for under the location's own ids as they are now, the ones the list was just read
    // with: then NOT_FOUND is about the post, not about an account the location has left.
    const auto ids = tx.exec("SELECT gbp_account_id, gbp_location_id FROM locations WHERE id = $1", pqxx::params{location_id}).one_row();
    for (const auto& row : tx.exec("SELECT id, google_name FROM local_posts WHERE location_id = $1 AND google_name IS NOT NULL "
                                   "AND status <> 'removed' AND google_name NOT IN (SELECT jsonb_array_elements_text($2::jsonb)) "
                                   "ORDER BY synced_at NULLS FIRST, id LIMIT $3",
                                   pqxx::params{location_id, db::json_list(listed), kLocalPostsMaxMissing})) {
        const auto name = gbp::parse_local_post_name(row[1].as<std::string>());
        if (!name || name->location_id != ids[1].as<std::string>()) continue;
        out.missing.push_back({.id = row[0].as<std::int64_t>(),
                               .name = {.account_id = ids[0].as<std::string>(), .location_id = ids[1].as<std::string>(), .post_id = name->post_id}});
    }

    tx.exec("UPDATE locations SET local_posts_synced_at = $2::timestamptz, local_posts_sync_error = NULL, local_posts_sync_error_at = NULL "
            "WHERE id = $1",
            pqxx::params{location_id, now_text})
        .no_rows();
    return out;
}

bool mark_local_post_removed(pqxx::transaction_base& tx, std::int64_t id, core::TimePoint now) {
    const auto rows = tx.exec("UPDATE local_posts SET status = 'removed', removed_at = $2::timestamptz, synced_at = $2::timestamptz, "
                              "updated_at = $2::timestamptz WHERE id = $1 AND status <> 'removed' AND google_name IS NOT NULL "
                              "RETURNING google_name",
                              pqxx::params{id, to_timestamptz(now)});
    if (rows.empty()) return false;
    system_audit(tx, "local_post.gone_at_google", id, {{"google_name", rows[0][0].as<std::string>()}}, now);
    return true;
}

void record_local_posts_sync_error(pqxx::transaction_base& tx, std::int64_t location_id, const std::string& error, core::TimePoint now) {
    tx.exec("UPDATE locations SET local_posts_sync_error = $2, local_posts_sync_error_at = $3::timestamptz WHERE id = $1",
            pqxx::params{location_id, error, to_timestamptz(now)})
        .no_rows();
}

}  // namespace poni::repo
