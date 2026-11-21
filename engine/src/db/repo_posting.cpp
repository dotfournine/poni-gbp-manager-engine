#include "db/repo_posting.hpp"

#include "db/repo_go_live.hpp"
#include "domain/go_live.hpp"

#include "core/crypto.hpp"
#include "core/text.hpp"
#include "db/database.hpp"
#include "db/repo_analysis.hpp"
#include "db/repo_auto_posting.hpp"
#include "db/repo_notifications.hpp"
#include "db/repo_reviews.hpp"
#include "db/repo_settings.hpp"
#include "domain/posting_schedule.hpp"
#include "domain/reply_checks.hpp"
#include "domain/reply_policy.hpp"
#include "jobs/queue.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value_from.hpp>

#include <chrono>
#include <format>

namespace poni::repo {

using db::iso;
using db::to_timestamptz;

namespace {

using OptString = std::optional<std::string>;

// Google's limit for a reply.
constexpr std::size_t kMaxReplyBytes = 4096;

audit::Actor poster() { return audit::Actor::system("reply-posting"); }

void system_audit(pqxx::transaction_base& tx, std::string action, std::int64_t draft_id, boost::json::object after,
                  core::TimePoint now) {
    audit::record(tx, poster(),
                  {.action = std::move(action), .object_type = "draft", .object_id = std::to_string(draft_id), .after = std::move(after)},
                  now);
}

// White space as one space and none at the ends: how a reply read back from
// Google is compared with the text that was sent.
std::string normalized(std::string_view text) {
    std::string out;
    bool gap = false;
    for (const char c : text::trim(text)) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            gap = true;
            continue;
        }
        if (gap && !out.empty()) out.push_back(' ');
        gap = false;
        out.push_back(c);
    }
    return out;
}

// What an action on a draft needs to know about it, its review and the switches.
struct DraftState {
    std::string status;
    std::string text;
    std::int64_t review_id = 0;
    std::int64_t location_id = 0;
    std::string mode;
    std::string language;
    std::string draft_review_time;
    std::optional<std::int64_t> decided_by;
    // Set on a scheduled draft: the rules that decided to post it.
    OptString policy_version;
    OptString policy_snapshot;
    bool review_removed = false;
    std::string review_time;
    bool has_reply = false;
    std::string reply_origin;
    bool location_posting = false;
    bool synced = false;
    bool global_posting = false;
    bool open_post = false;
};

// Locks the review, then the draft. The sync locks a review before it
// touches the review's drafts and posts, so everything that decides about a
// post takes the locks in the same order and none can deadlock another.
std::optional<DraftState> lock_draft(pqxx::transaction_base& tx, std::int64_t draft_id) {
    const auto owner = tx.exec("SELECT review_id FROM reply_drafts WHERE id = $1", pqxx::params{draft_id});
    if (owner.empty()) return std::nullopt;
    tx.exec("SELECT 1 FROM reviews WHERE id = $1 FOR UPDATE", pqxx::params{owner[0][0].as<std::int64_t>()}).one_row();
    const auto drafts = tx.exec(std::format("SELECT status, text, review_id, mode, language, {}, decided_by, policy_version, "
                                            "policy_snapshot::text FROM reply_drafts WHERE id = $1 FOR UPDATE",
                                            iso("review_updated_time")),
                                pqxx::params{draft_id});
    if (drafts.empty()) return std::nullopt;
    DraftState state;
    state.status = drafts[0][0].as<std::string>();
    state.text = drafts[0][1].as<std::string>();
    state.review_id = drafts[0][2].as<std::int64_t>();
    state.mode = drafts[0][3].as<std::string>();
    state.language = drafts[0][4].as<std::string>();
    state.draft_review_time = drafts[0][5].as<std::string>();
    state.decided_by = drafts[0][6].as<std::optional<std::int64_t>>();
    state.policy_version = drafts[0][7].as<OptString>();
    state.policy_snapshot = drafts[0][8].as<OptString>();

    const auto review = tx.exec(std::format(R"sql(
        SELECT r.removed_at IS NOT NULL, {}, r.reply_text IS NOT NULL, coalesce(r.reply_origin, ''), r.location_id,
               l.posting_enabled, l.access_status = 'active' AND c.id IS NOT NULL AND NOT c.archived,
               (SELECT posting_enabled FROM system_settings),
               EXISTS (SELECT 1 FROM reply_posts p WHERE p.review_id = r.id AND p.status IN {})
          FROM reviews r JOIN locations l ON l.id = r.location_id LEFT JOIN clients c ON c.id = l.client_id
         WHERE r.id = $1)sql",
                                            iso("r.updated_time"), kOpenPost),
                                pqxx::params{state.review_id})
                            .one_row();
    state.review_removed = review[0].as<bool>();
    state.review_time = review[1].as<std::string>();
    state.has_reply = review[2].as<bool>();
    state.reply_origin = review[3].as<std::string>();
    state.location_id = review[4].as<std::int64_t>();
    state.location_posting = review[5].as<bool>();
    state.synced = review[6].as<bool>();
    state.global_posting = review[7].as<bool>();
    state.open_post = review[8].as<bool>();
    return state;
}

// Why the review can't be replied to now, if it can't.
OptString review_obstacle(const DraftState& state) {
    if (state.review_removed) return "The review was removed from Google.";
    if (state.review_time != state.draft_review_time) return "The review was edited after this draft was written.";
    // The engine's own reply may be replaced by a newer one (an edit); anyone else's may not.
    if (state.has_reply && state.reply_origin != "system") return "The review already has a reply that was posted outside the system.";
    if (state.open_post) return "An earlier reply to this review is still waiting to appear on Google.";
    if (!state.synced) return "The review's location is no longer synced.";
    return std::nullopt;
}

// Why a draft's text can't be checked: its location has no readable reply
// settings. A location that lost its client lost them with it (D-093).
std::string no_settings(const DraftState& state) {
    return state.synced ? "The reply settings of the review's location can't be read." : "The review's location is no longer synced.";
}

OptString switch_obstacle(const DraftState& state) {
    if (!state.global_posting) return "Posting is switched off.";
    if (!state.location_posting) return "Posting is switched off for this location.";
    return std::nullopt;
}

void set_draft(pqxx::transaction_base& tx, std::int64_t draft_id, std::string_view status, const OptString& reason,
               core::TimePoint now) {
    tx.exec("UPDATE reply_drafts SET status = $2, status_reason = $3, updated_at = $4::timestamptz WHERE id = $1",
            pqxx::params{draft_id, std::string(status), reason, to_timestamptz(now)})
        .no_rows();
}

}  // namespace

// ---- the approval queue ----------------------------------------------------------

Result<model::ReplyDraft> edit_draft(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t draft_id, const std::string& text) {
    const auto state = lock_draft(tx, draft_id);
    if (!state) return not_found("draft");
    if (state->status != "draft") return conflict("draft_not_open", "The draft is no longer waiting, so its text can't be changed.");
    const std::string cleaned(text::trim(text));
    if (cleaned.empty()) return invalid("\"text\" must not be empty.");
    if (cleaned.size() > kMaxReplyBytes) return invalid("\"text\" is longer than Google allows for a reply (4096 bytes).");
    if (cleaned == state->text) {
        audit.nothing_changed();
        return *get_draft(tx, draft_id);
    }
    const auto settings = stored_reply_settings(tx, state->location_id);
    if (!settings) return conflict("review_not_postable", no_settings(*state));
    const auto report = domain::run_reply_checks(cleaned, {.voice = settings->voice, .target_language = state->language, .mode = state->mode});
    tx.exec("UPDATE reply_drafts SET text = $2, checks = $3::jsonb, edited_by = $4, edited_at = $5::timestamptz, "
            "status_reason = NULL, updated_at = $5::timestamptz WHERE id = $1",
            pqxx::params{draft_id, cleaned, boost::json::serialize(boost::json::value_from(report.results)), audit.user_id(),
                         to_timestamptz(audit.now())})
        .no_rows();
    audit.record("draft.edit", "draft", draft_id,
                 {.before = {{"text", state->text}},
                  .after = {{"text", cleaned}, {"review_id", state->review_id}, {"failed_checks", boost::json::value_from(report.failed())}}});
    return *get_draft(tx, draft_id);
}

Result<model::ReplyDraft> approve_draft(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t draft_id, const std::string& text) {
    const auto state = lock_draft(tx, draft_id);
    if (!state) return not_found("draft");
    if (state->status != "draft") return conflict("draft_not_open", "The draft is no longer waiting for approval.");
    // The approval is for the text the person read, to the byte.
    if (text != state->text) return conflict("draft_changed", "The draft's text changed after it was shown. Read it again before approving.");
    if (text::trim(state->text).empty()) return conflict("draft_empty", "The draft has no text to post.");
    if (state->text.size() > kMaxReplyBytes) return conflict("checks_failed", "The text is longer than Google allows for a reply.");

    const auto settings = stored_reply_settings(tx, state->location_id);
    if (!settings) return conflict("review_not_postable", no_settings(*state));
    // Run now, not read from the row: the voice may have changed since the draft was checked.
    const auto report = domain::run_reply_checks(state->text, {.voice = settings->voice, .target_language = state->language, .mode = state->mode});
    if (!report.passed()) {
        return conflict("checks_failed", std::format("The reply fails a check ({}). Edit the text first.", report.failed_names()));
    }
    if (const auto off = switch_obstacle(*state)) return conflict("posting_disabled", *off);
    if (const auto obstacle = review_obstacle(*state)) return conflict("review_not_postable", *obstacle);

    const auto now = audit.now();
    tx.exec("UPDATE reply_drafts SET status = 'approved', status_reason = NULL, checks = $2::jsonb, decided_by = $3, "
            "decided_at = $4::timestamptz, updated_at = $4::timestamptz WHERE id = $1",
            pqxx::params{draft_id, boost::json::serialize(boost::json::value_from(report.results)), audit.user_id(), to_timestamptz(now)})
        .no_rows();
    // One job per approval: a draft that came back to the queue and is approved again gets a new one.
    const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    jobs::sql::enqueue(tx,
                       {.kind = std::string(kPostReplyKind),
                        .payload = boost::json::object{{"draft_id", draft_id}},
                        .run_at = std::nullopt,
                        .idempotency_key = std::format("{}@{}", draft_id, stamp),
                        .max_attempts = kPostReplyAttempts},
                       now);
    audit.record("draft.approve", "draft", draft_id, {.after = {{"review_id", state->review_id}, {"text", state->text}}});
    return *get_draft(tx, draft_id);
}

Result<model::ReplyDraft> reject_draft(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t draft_id, const std::string& reason) {
    const auto state = lock_draft(tx, draft_id);
    if (!state) return not_found("draft");
    if (state->status != "draft") return conflict("draft_not_open", "The draft is no longer waiting, so it can't be rejected.");
    const std::string cleaned(text::trim(reason));
    if (cleaned.empty() || text::length(cleaned) > 500) return invalid("\"reason\" must be 1 to 500 characters.");
    tx.exec("UPDATE reply_drafts SET status = 'rejected', status_reason = $2, decided_by = $3, decided_at = $4::timestamptz, "
            "updated_at = $4::timestamptz WHERE id = $1",
            pqxx::params{draft_id, cleaned, audit.user_id(), to_timestamptz(audit.now())})
        .no_rows();
    audit.record("draft.reject", "draft", draft_id, {.after = {{"review_id", state->review_id}, {"reason", cleaned}}});
    return *get_draft(tx, draft_id);
}

// ---- auto-posting (D-069) --------------------------------------------------------

namespace {

// Whether a draft may be posted without a person, as things are now.
struct AutoVerdict {
    bool ok = false;
    // Not ok only because the switches don't ask for an auto-post here.
    bool by_configuration = false;
    // Why not, as a sentence.
    std::string reason;
    std::int64_t review_id = 0;
    std::int64_t location_id = 0;
    std::string policy_version;
    boost::json::object snapshot;
    domain::PostingTiming timing;
    std::string zone;
};

AutoVerdict no(AutoVerdict verdict, std::string reason, bool by_configuration = false) {
    verdict.ok = false;
    verdict.by_configuration = by_configuration;
    verdict.reason = std::move(reason);
    return verdict;
}

// Every condition of an auto-post, read in the caller's transaction. Called
// when the pipeline has stored the draft (`at_claim` false) and again by the
// claim, just before the PUT, so that a switch flipped, a pause, an edit or
// a changed voice in between stops the post. The review's own state (still
// there, unchanged, no reply, no open post) is the claim's business, as for
// an approved draft.
AutoVerdict auto_verdict(pqxx::transaction_base& tx, std::int64_t draft_id, core::TimePoint now, bool at_claim) {
    // One decision at a time per location, so two drafts that arrive together
    // can't both take the last place under the daily cap. Taken after the
    // review's and the draft's locks, the order every caller has them in.
    // NO KEY: the sync inserts reviews of this location while it holds other
    // reviews' locks, and a key lock here would deadlock with it.
    tx.exec("SELECT 1 FROM locations l JOIN reviews r ON r.location_id = l.id JOIN reply_drafts d ON d.review_id = r.id "
            "WHERE d.id = $1 FOR NO KEY UPDATE OF l",
            pqxx::params{draft_id});
    const auto rows = tx.exec(std::format(R"sql(
        SELECT d.route, d.generation_ok, d.edited_by IS NULL AND d.instructions IS NULL AND d.created_by IS NULL, d.text, d.mode,
               d.language, d.prompt_version,
               d.analysis_id = (SELECT max(a.id) FROM review_analyses a WHERE a.review_id = r.id),
               coalesce(r.rating, 0), r.id, r.location_id, r.ignored_at IS NOT NULL,
               EXISTS (SELECT 1 FROM review_events e WHERE e.review_id = r.id AND e.kind = 'review_edited'),
               EXISTS (SELECT 1 FROM cases k WHERE k.review_id = r.id AND k.status = 'open'),
               l.auto_post_enabled, l.auto_post_five_star, l.auto_post_four_star, l.auto_post_daily_cap, l.time_zone,
               l.auto_post_paused_at IS NOT NULL, s.auto_post_paused_at IS NOT NULL, s.posting_enabled, l.posting_enabled,
               l.access_status = 'active' AND c.id IS NOT NULL AND NOT c.archived,
               {},
               (SELECT count(*) FROM reply_drafts o JOIN reviews orv ON orv.id = o.review_id
                 WHERE orv.location_id = l.id AND o.status = 'scheduled' AND o.id <> d.id),
               EXISTS (SELECT 1 FROM reply_drafts e WHERE e.review_id = r.id AND e.id <> d.id)
               -- This draft's own attempt that Google refused outright posted nothing and may be retried.
               OR EXISTS (SELECT 1 FROM reply_posts q WHERE q.review_id = r.id AND NOT (q.draft_id = d.id AND q.status = 'failed'))
          FROM reply_drafts d JOIN reviews r ON r.id = d.review_id JOIN locations l ON l.id = r.location_id
          LEFT JOIN clients c ON c.id = l.client_id CROSS JOIN system_settings s
         WHERE d.id = $1)sql",
                                          auto_posts_since("l.id", "$2")),
                              pqxx::params{draft_id, to_timestamptz(now - kAutoPostCapWindow)});
    AutoVerdict verdict;
    if (rows.empty()) return no(std::move(verdict), "The draft no longer exists.");
    const auto row = rows[0];
    const auto text = row[3].as<std::string>();
    const int stars = row[8].as<int>();
    verdict.review_id = row[9].as<std::int64_t>();
    verdict.location_id = row[10].as<std::int64_t>();
    const bool five_star = row[15].as<bool>();
    const bool four_star = row[16].as<bool>();
    const auto cap = row[17].as<std::int64_t>();
    const auto zone = row[18].as<OptString>();
    verdict.zone = zone.value_or("");

    // 1. What people switched on. Off is the state after installation.
    if (!row[21].as<bool>()) return no(std::move(verdict), "Posting is switched off.", true);
    if (!row[22].as<bool>()) return no(std::move(verdict), "Posting is switched off for this location.", true);
    if (!row[23].as<bool>()) return no(std::move(verdict), "The review's location is no longer synced.", true);
    if (!row[14].as<bool>()) return no(std::move(verdict), "Auto-posting is switched off for this location.", true);
    // Three stars and fewer always wait for a person (FR-5), whatever the policy says.
    if (!((stars == 5 && five_star) || (stars == 4 && four_star))) {
        return no(std::move(verdict), std::format("Auto-posting is not switched on for {}-star reviews at this location.", stars), true);
    }
    if (row[0].as<std::string>() != domain::kRouteAutoPost) return no(std::move(verdict), "The policy did not route this draft to auto-post.", true);

    // 2. Pauses the engine put on itself.
    if (row[20].as<bool>()) return no(std::move(verdict), "Auto-posting is paused for every location.");
    if (row[19].as<bool>()) return no(std::move(verdict), "Auto-posting is paused for this location.");

    // 3. The draft and its review.
    if (!row[1].as<bool>() || text::trim(text).empty()) return no(std::move(verdict), "The draft has no text.");
    if (text.size() > kMaxReplyBytes) return no(std::move(verdict), "The text is longer than Google allows for a reply.");
    // Only the pipeline's own words: what the adversarial suite covers (D-067).
    if (!row[2].as<bool>()) return no(std::move(verdict), "The draft was edited or written with a person's instructions.");
    if (row[11].as<bool>()) return no(std::move(verdict), "The review is marked as needing no reply.");
    if (row[12].as<bool>()) return no(std::move(verdict), "The reviewer edited this review, so a person takes a second look.");
    if (row[13].as<bool>()) return no(std::move(verdict), "The review has an open escalation.");
    // Only a review nobody and nothing has dealt with before. A second draft
    // (the review was analyzed again, an earlier draft was rejected, stopped
    // or taken back) or an earlier attempt to post (one that Google refused,
    // or one whose reply may still appear) is a person's to decide: "analyze
    // again" must never be a way around a stop.
    if (row[26].as<bool>()) return no(std::move(verdict), "This review already had a draft or an attempt to post, so a person decides.");
    if (row[7].is_null() || !row[7].as<bool>()) return no(std::move(verdict), "The review was analyzed again after this draft was written.");

    // 4. The reply checks, run now: the voice may have changed since the draft was checked.
    const auto settings = stored_reply_settings(tx, verdict.location_id);
    if (!settings) return no(std::move(verdict), "The reply settings of the review's location can't be read.");
    const auto report = domain::run_reply_checks(
        text, {.voice = settings->voice, .target_language = row[5].as<std::string>(), .mode = row[4].as<std::string>()});
    if (!report.passed()) return no(std::move(verdict), std::format("The reply fails a check ({}).", report.failed_names()));

    // 5. The go-live test (D-074): this location's configuration, as it is
    // now, has passed one that covers what its policy auto-posts. A setting
    // changed since the pass, at scheduling or between then and the claim,
    // stops the post until a person has run the test again.
    const auto go_live = go_live_gate(tx, verdict.location_id);
    if (!go_live.passed) return no(std::move(verdict), go_live.reason);

    // 6. Timing.
    const auto timing = domain::posting_timing(settings->policy);
    if (!timing) return no(std::move(verdict), std::format("The reply policy's timing can't be used: {}.", timing.error()));
    verdict.timing = *timing;
    if (timing->window) {
        if (!zone) return no(std::move(verdict), "The reply policy has a posting window, and the location has no time zone.");
        const auto inside = domain::in_window(now, *timing->window, *zone);
        if (!inside) return no(std::move(verdict), std::format("The location's time zone can't be used: {}.", inside.error()));
        // A job that ran late (the engine was down) must not post in the middle of the night.
        if (at_claim && !*inside) return no(std::move(verdict), "The reply missed its posting window.");
    }

    // 7. The daily cap. When scheduling, drafts already scheduled count too,
    // so a burst of reviews doesn't queue more than the cap allows.
    const auto used = row[24].as<std::int64_t>() + (at_claim ? 0 : row[25].as<std::int64_t>());
    if (used >= cap) return no(std::move(verdict), std::format("The daily cap of {} automatic replies for this location was reached.", cap));

    // The version of the rules that allowed it: a short hash of everything
    // above that is a setting, kept in full next to it.
    verdict.snapshot = {{"gate", domain::kGateVersion},
                        // What was tested (D-074), next to which rules allowed the post.
                        {"fingerprint", go_live.fingerprint},
                        {"prompt_version", row[6].as<std::string>()},
                        {"policy", boost::json::value_from(settings->policy)},
                        {"auto_posting", boost::json::object{{"five_star", five_star},
                                                             {"four_star", four_star},
                                                             {"daily_cap", cap},
                                                             {"time_zone", zone ? boost::json::value(*zone) : boost::json::value(nullptr)}}}};
    verdict.policy_version = core::sha256_hex(boost::json::serialize(verdict.snapshot)).substr(0, 12);
    verdict.ok = true;
    return verdict;
}

// "Approval needed" for a draft that was on its way to an automatic post
// and is back in the queue. Review text and the draft go in the body only.
void emit_approval_needed(pqxx::transaction_base& tx, std::int64_t draft_id, const std::string& why, core::TimePoint now) {
    const auto row = tx.exec("SELECT r.id, l.id, l.client_id, l.title, coalesce(c.name, ''), coalesce(r.rating, 0), coalesce(r.comment, ''), "
                             "d.text FROM reply_drafts d JOIN reviews r ON r.id = d.review_id JOIN locations l ON l.id = r.location_id "
                             "LEFT JOIN clients c ON c.id = l.client_id WHERE d.id = $1",
                             pqxx::params{draft_id})
                         .one_row();
    const auto title = row[3].as<std::string>();
    const auto client = row[4].as<std::string>();
    const auto place = client.empty() ? title : std::format("{} ({})", title, client);
    const int stars = row[5].as<int>();
    const auto comment = row[6].as<std::string>();
    emit(tx,
         {.type = std::string(kEventApprovalNeeded),
          .key = std::format("draft:{}:approval_needed", draft_id),
          .client_id = row[2].as<std::optional<std::int64_t>>(),
          .location_id = row[1].as<std::int64_t>(),
          .review_id = row[0].as<std::int64_t>(),
          .subject = std::format("Approval needed: {}", place),
          .body = std::format("A reply to a review at {} was not posted automatically and is waiting for approval.\n\nWhy: {}\n\n"
                              "Rating: {}\n\nReview:\n{}\n\nDraft:\n{}\n\nNothing is posted until a person approves it.",
                              place, why, stars > 0 ? std::format("{} of 5 stars", stars) : std::string("none"),
                              comment.empty() ? std::string("(no text)") : comment, row[7].as<std::string>()),
          .link_path = std::format("/#/drafts/{}/show", row[0].as<std::int64_t>())},
         now);
}

// A scheduled draft that may not be posted without a person after all: back
// to the approval queue with the reason, never scheduled again by itself.
void stop_auto_post(pqxx::transaction_base& tx, std::int64_t draft_id, std::int64_t review_id, const std::string& why,
                    core::TimePoint now) {
    set_draft(tx, draft_id, "draft", "Not posted automatically. " + why, now);
    audit::record(tx, auto_reply(),
                  {.action = "reply.auto_post_cancelled",
                   .object_type = "draft",
                   .object_id = std::to_string(draft_id),
                   .after = {{"review_id", review_id}, {"reason", why}}},
                  now);
    emit_approval_needed(tx, draft_id, why, now);
}

}  // namespace

bool schedule_auto_post(pqxx::transaction_base& tx, std::int64_t draft_id, core::TimePoint now, double roll) {
    const auto verdict = auto_verdict(tx, draft_id, now, false);
    if (!verdict.ok) {
        if (verdict.by_configuration) return false;
        // The queue says why a draft the policy would have posted is waiting.
        tx.exec("UPDATE reply_drafts SET status_reason = $2 WHERE id = $1 AND status = 'draft'",
                pqxx::params{draft_id, "Not posted automatically. " + verdict.reason})
            .no_rows();
        return false;
    }
    // With a window, the verdict has just read the clock in this time zone, so
    // the zone is known and a time can be found; value() says so if not.
    const auto post_at = domain::post_time(now, verdict.timing, verdict.zone, roll).value();
    const auto updated = tx.exec("UPDATE reply_drafts SET status = 'scheduled', status_reason = NULL, post_at = $2::timestamptz, "
                                 "scheduled_at = $3::timestamptz, policy_version = $4, policy_snapshot = $5::jsonb, "
                                 "updated_at = $3::timestamptz WHERE id = $1 AND status = 'draft'",
                                 pqxx::params{draft_id, to_timestamptz(post_at), to_timestamptz(now), verdict.policy_version,
                                              boost::json::serialize(verdict.snapshot)});
    if (updated.affected_rows() == 0) return false;
    // One job per scheduling, as one per approval.
    jobs::sql::enqueue(tx,
                       {.kind = std::string(kPostReplyKind),
                        .payload = boost::json::object{{"draft_id", draft_id}},
                        .run_at = post_at,
                        .idempotency_key = std::format("{}@auto", draft_id),
                        .max_attempts = kPostReplyAttempts},
                       now);
    audit::record(tx, auto_reply(),
                  {.action = "reply.auto_post_scheduled",
                   .object_type = "draft",
                   .object_id = std::to_string(draft_id),
                   .after = {{"review_id", verdict.review_id},
                             {"post_at", to_timestamptz(post_at)},
                             {"policy_version", verdict.policy_version}}},
                  now);
    return true;
}

Result<model::ReplyDraft> unschedule_draft(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t draft_id) {
    const auto state = lock_draft(tx, draft_id);
    if (!state) return not_found("draft");
    if (state->status != "scheduled") return conflict("draft_not_scheduled", "The draft is not scheduled for an automatic post.");
    set_draft(tx, draft_id, "draft", "A person took this reply out of auto-posting.", audit.now());
    audit.record("draft.unschedule", "draft", draft_id, {.after = {{"review_id", state->review_id}}});
    return *get_draft(tx, draft_id);
}

// ---- the posting job -------------------------------------------------------------

std::optional<PostTarget> post_target(pqxx::transaction_base& tx, std::int64_t draft_id) {
    const auto rows = tx.exec("SELECT d.review_id, r.location_id, d.status, l.gbp_account_id, l.gbp_location_id, r.gbp_review_id "
                              "FROM reply_drafts d JOIN reviews r ON r.id = d.review_id JOIN locations l ON l.id = r.location_id "
                              "WHERE d.id = $1",
                              pqxx::params{draft_id});
    if (rows.empty()) return std::nullopt;
    const auto row = rows[0];
    return PostTarget{.review_id = row[0].as<std::int64_t>(),
                      .location_id = row[1].as<std::int64_t>(),
                      .draft_status = row[2].as<std::string>(),
                      .name = {row[3].as<std::string>(), row[4].as<std::string>(), row[5].as<std::string>()}};
}

Claim claim_post(pqxx::transaction_base& tx, std::int64_t draft_id, core::TimePoint now) {
    using Outcome = Claim::Outcome;
    const auto state = lock_draft(tx, draft_id);
    if (!state) return {.outcome = Outcome::not_approved, .detail = "no such draft"};
    if (state->status == "posting" || state->status == "posted") {
        return {.outcome = Outcome::in_flight, .detail = "this draft is already being posted"};
    }
    // A person's approval, or the policy's decision with its version: nothing else is posted.
    const bool automatic = state->status == "scheduled" && state->policy_version && state->policy_snapshot;
    if (!automatic && (state->status != "approved" || !state->decided_by)) {
        return {.outcome = Outcome::not_approved, .detail = "the draft is " + state->status};
    }

    // The draft is not posted: its new status with the words the queue shows,
    // and the reason in the audit log (the same words unless given).
    const auto stop = [&](std::string_view status, const std::string& shown, std::string_view reason = {}) {
        set_draft(tx, draft_id, status, shown, now);
        system_audit(tx, "reply.post_cancelled", draft_id,
                     {{"review_id", state->review_id}, {"reason", reason.empty() ? std::string_view(shown) : reason}}, now);
    };

    // The review, as Google returned it a moment ago.
    if (state->review_removed) {
        stop("cancelled", "the review was removed");
        return {.outcome = Outcome::stopped, .detail = "the review was removed"};
    }
    if (state->review_time != state->draft_review_time) {
        stop("superseded", "the review was edited");
        return {.outcome = Outcome::stopped, .detail = "the review was edited"};
    }
    if (state->has_reply && state->reply_origin != "system") {
        stop("cancelled", "a reply was posted outside the system");
        return {.outcome = Outcome::stopped, .detail = "the review has a reply from outside the system"};
    }
    // Not reachable through the API (one live draft per review), and checked
    // all the same: this is the rule the whole design rests on.
    if (state->open_post) {
        const std::string waiting = "An earlier reply to this review is still waiting to appear on Google.";
        if (automatic) {
            stop_auto_post(tx, draft_id, state->review_id, waiting, now);
        } else {
            stop("draft", waiting, "an earlier post is unconfirmed");
        }
        return {.outcome = Outcome::in_flight, .detail = "an earlier post to this review is unconfirmed"};
    }
    // The kill switches, read in the transaction that opens the post.
    auto off = switch_obstacle(*state);
    if (!off && !state->synced) off = "The review's location is no longer synced.";
    if (off) {
        // Back to the queue: a reply never goes out later on its own because a
        // switch was flipped back. A person approves it again.
        if (automatic) {
            stop_auto_post(tx, draft_id, state->review_id, *off, now);
        } else {
            stop("draft", *off + " The approval was not carried out; approve again when posting is on.", *off);
        }
        return {.outcome = Outcome::stopped, .detail = *off};
    }
    if (automatic) {
        // Everything that allowed the auto-post when it was scheduled, as it is now.
        const auto verdict = auto_verdict(tx, draft_id, now, true);
        if (!verdict.ok) {
            stop_auto_post(tx, draft_id, state->review_id, verdict.reason, now);
            return {.outcome = Outcome::auto_post_stopped, .detail = verdict.reason};
        }
        // The route was decided under the reply policy as it was then. If the
        // policy's rules or timing changed since, that decision no longer stands.
        const auto then = boost::json::parse(*state->policy_snapshot);
        const auto* policy_then = then.is_object() ? then.get_object().if_contains("policy") : nullptr;
        if (!policy_then || *policy_then != verdict.snapshot.at("policy")) {
            const std::string why = "The reply policy changed after this reply was scheduled.";
            stop_auto_post(tx, draft_id, state->review_id, why, now);
            return {.outcome = Outcome::auto_post_stopped, .detail = why};
        }
    }

    const auto now_text = to_timestamptz(now);
    const auto post_id = tx.query_value<std::int64_t>(
        "INSERT INTO reply_posts (review_id, draft_id, text, approved_by, policy_version, policy_snapshot, status, started_at, updated_at) "
        "VALUES ($1, $2, $3, $4, $5, $6::jsonb, 'posting', $7::timestamptz, $7::timestamptz) RETURNING id",
        pqxx::params{state->review_id, draft_id, state->text, automatic ? std::optional<std::int64_t>{} : state->decided_by,
                     automatic ? state->policy_version : OptString{}, automatic ? state->policy_snapshot : OptString{}, now_text});
    set_draft(tx, draft_id, "posting", std::nullopt, now);
    if (automatic) {
        // Who stands behind this post: the policy, by its version (architecture 4.1, step 5).
        audit::record(tx, auto_reply(),
                      {.action = "reply.post_started",
                       .object_type = "draft",
                       .object_id = std::to_string(draft_id),
                       .after = {{"review_id", state->review_id}, {"post_id", post_id}, {"policy_version", *state->policy_version}}},
                      now);
    } else {
        system_audit(tx, "reply.post_started", draft_id,
                     {{"review_id", state->review_id}, {"post_id", post_id}, {"approved_by_user_id", *state->decided_by}}, now);
    }
    return {.outcome = Outcome::claimed, .post_id = post_id, .automatic = automatic, .text = state->text, .detail = {}};
}

void post_accepted(pqxx::transaction_base& tx, std::int64_t post_id, core::TimePoint now) {
    const auto now_text = to_timestamptz(now);
    const auto rows = tx.exec("UPDATE reply_posts SET status = 'posted', posted_at = $2::timestamptz, error = NULL, "
                              "updated_at = $2::timestamptz WHERE id = $1 AND status = 'posting' RETURNING draft_id, review_id",
                              pqxx::params{post_id, now_text});
    if (rows.empty()) return;  // settled meanwhile: a read already saw the reply
    const auto draft_id = rows[0][0].as<std::int64_t>();
    tx.exec("UPDATE reply_drafts SET status = 'posted', status_reason = NULL, updated_at = $2::timestamptz "
            "WHERE id = $1 AND status = 'posting'",
            pqxx::params{draft_id, now_text})
        .no_rows();
    system_audit(tx, "reply.posted", draft_id, {{"review_id", rows[0][1].as<std::int64_t>()}, {"post_id", post_id}}, now);
}

void post_refused(pqxx::transaction_base& tx, std::int64_t post_id, const std::string& error, bool retry, core::TimePoint now) {
    const auto now_text = to_timestamptz(now);
    const auto rows = tx.exec("UPDATE reply_posts SET status = 'failed', error = $2, updated_at = $3::timestamptz "
                              "WHERE id = $1 AND status = 'posting' RETURNING draft_id, review_id, policy_version IS NOT NULL",
                              pqxx::params{post_id, error, now_text});
    if (rows.empty()) return;
    const auto draft_id = rows[0][0].as<std::int64_t>();
    // For another attempt the draft goes back to where it came from.
    const std::string_view again = rows[0][2].as<bool>() ? "scheduled" : "approved";
    tx.exec("UPDATE reply_drafts SET status = $2, status_reason = $3, updated_at = $4::timestamptz WHERE id = $1 AND status = 'posting'",
            pqxx::params{draft_id, std::string(retry ? again : std::string_view("draft")),
                         retry ? OptString{} : OptString{std::format("Google refused the reply ({}). Nothing was posted.", error)},
                         now_text})
        .no_rows();
    system_audit(tx, "reply.post_failed", draft_id,
                 {{"review_id", rows[0][1].as<std::int64_t>()}, {"post_id", post_id}, {"error", error}, {"will_retry", retry}}, now);
}

void post_outcome_unknown(pqxx::transaction_base& tx, std::int64_t post_id, const std::string& error, core::TimePoint now) {
    const auto rows = tx.exec("UPDATE reply_posts SET error = $2, updated_at = $3::timestamptz WHERE id = $1 AND status = 'posting' "
                              "RETURNING draft_id, review_id",
                              pqxx::params{post_id, error, to_timestamptz(now)});
    if (rows.empty()) return;
    system_audit(tx, "reply.post_outcome_unknown", rows[0][0].as<std::int64_t>(),
                 {{"review_id", rows[0][1].as<std::int64_t>()}, {"post_id", post_id}, {"error", error}}, now);
}

std::vector<OpenPost> open_posts(pqxx::transaction_base& tx, core::TimePoint overdue_before) {
    std::vector<OpenPost> out;
    for (const auto& row : tx.exec(
             std::format("SELECT p.id, p.review_id, r.location_id, l.title, l.gbp_account_id, l.gbp_location_id, r.gbp_review_id, "
                         "p.started_at <= $1::timestamptz, p.policy_version IS NOT NULL "
                         "FROM reply_posts p JOIN reviews r ON r.id = p.review_id JOIN locations l ON l.id = r.location_id "
                         "WHERE p.status IN {} ORDER BY p.id",
                         kOpenPost),
             pqxx::params{to_timestamptz(overdue_before)})) {
        out.push_back({.post_id = row[0].as<std::int64_t>(),
                       .review_id = row[1].as<std::int64_t>(),
                       .location_id = row[2].as<std::int64_t>(),
                       .location_title = row[3].as<std::string>(),
                       .name = {row[4].as<std::string>(), row[5].as<std::string>(), row[6].as<std::string>()},
                       .overdue = row[7].as<bool>(),
                       .automatic = row[8].as<bool>()});
    }
    return out;
}

namespace {

// The window ended without the reply showing on Google: the post becomes
// `unconfirmed` and the draft returns to the queue with the reason, for a
// person to decide. False if the post was settled in the meantime.
bool expire_post(pqxx::transaction_base& tx, std::int64_t post_id, core::TimePoint now) {
    const auto now_text = to_timestamptz(now);
    const auto rows = tx.exec(
        std::format("UPDATE reply_posts SET status = 'unconfirmed', "
                    "error = concat_ws('; ', error, 'the reply did not appear on Google within the confirmation window'), "
                    "updated_at = $2::timestamptz WHERE id = $1 AND status IN {} RETURNING draft_id, review_id",
                    kOpenPost),
        pqxx::params{post_id, now_text});
    if (rows.empty()) return false;
    const auto draft_id = rows[0][0].as<std::int64_t>();
    tx.exec(std::format("UPDATE reply_drafts SET status = 'draft', status_reason = $2, updated_at = $3::timestamptz "
                        "WHERE id = $1 AND status IN {}",
                        kOpenPost),
            pqxx::params{draft_id,
                         "The reply was sent to Google but did not appear on the review in time. Look at the review on Google "
                         "before approving again.",
                         now_text})
        .no_rows();
    system_audit(tx, "reply.post_unconfirmed", draft_id, {{"review_id", rows[0][1].as<std::int64_t>()}, {"post_id", post_id}}, now);
    return true;
}

// For a review with an open post whose reply the mirror already holds
// unchanged (an edit that sends the same text again): settles the post from
// what the mirror shows.
void settle_open_post_from_mirror(pqxx::transaction_base& tx, std::int64_t review_id, core::TimePoint now) {
    const auto rows = tx.exec(std::format("SELECT r.reply_text FROM reviews r WHERE r.id = $1 AND r.reply_text IS NOT NULL "
                                          "AND r.removed_at IS NULL "
                                          "AND EXISTS (SELECT 1 FROM reply_posts p WHERE p.review_id = r.id AND p.status IN {}) "
                                          "FOR UPDATE OF r",
                                          kOpenPost),
                              pqxx::params{review_id});
    if (rows.empty()) return;
    const auto origin = reconcile_reply(tx, review_id, rows[0][0].as<std::string>(), now);
    tx.exec("UPDATE reviews SET reply_origin = $2 WHERE id = $1", pqxx::params{review_id, std::string(origin)}).no_rows();
}

}  // namespace

NotificationEvent post_problem(std::string key, std::int64_t location_id, std::int64_t review_id, std::string subject, std::string body) {
    return {.type = std::string(kEventSystemProblem),
            .key = std::move(key),
            .client_id = std::nullopt,
            .location_id = location_id,
            .review_id = review_id,
            .subject = std::move(subject),
            .body = std::move(body),
            .link_path = std::format("/#/reviews/{}/show", review_id)};
}

bool settle_open_post(pqxx::transaction_base& tx, const OpenPost& post, const std::optional<gbp::Review>& review, bool gone,
                      core::TimePoint now) {
    if (gone) {
        mark_review_removed(tx, post.location_id, post.name.review_id, kSourcePosting, now);
    } else if (review) {
        apply_review(tx, post.location_id, *review, kSourcePosting, now);
        settle_open_post_from_mirror(tx, post.review_id, now);
    }
    const bool open =
        !tx.exec(std::format("SELECT 1 FROM reply_posts WHERE id = $1 AND status IN {}", kOpenPost), pqxx::params{post.post_id}).empty();
    if (!open) return true;
    if (!post.overdue) return false;
    // The window ended. Nothing is posted again by the engine: a person decides.
    if (expire_post(tx, post.post_id, now)) {
        if (post.automatic) {
            // An automatic reply whose outcome stayed unknown: no more of them here until a person has looked.
            pause_auto_posting(tx, post.location_id, kPausePostUnconfirmed,
                               "An automatic reply was sent to Google and did not appear on the review within the "
                               "confirmation window. Look at the review on Google before resuming.",
                               now);
        }
        emit(tx,
             post_problem(std::format("post:{}:unconfirmed", post.post_id), post.location_id, post.review_id,
                          "A reply did not appear on Google",
                          std::format("A reply to a review at {} was sent to Google but has not appeared on the review. "
                                      "The draft is back in the approval queue. Look at the review on Google before "
                                      "approving it again.",
                                      post.location_title)),
             now);
    }
    return true;
}

bool release_approval(pqxx::transaction_base& tx, std::int64_t draft_id, const std::string& reason, core::TimePoint now) {
    const auto rows = tx.exec("UPDATE reply_drafts SET status = 'draft', status_reason = $2, updated_at = $3::timestamptz "
                              "WHERE id = $1 AND status IN ('approved', 'scheduled') RETURNING review_id",
                              pqxx::params{draft_id, reason, to_timestamptz(now)});
    if (rows.empty()) return false;
    system_audit(tx, "reply.post_cancelled", draft_id, {{"review_id", rows[0][0].as<std::int64_t>()}, {"reason", reason}}, now);
    return true;
}

std::vector<std::int64_t> release_orphaned_approvals(pqxx::transaction_base& tx, core::TimePoint approved_before,
                                                     core::TimePoint now) {
    std::vector<std::int64_t> released;
    const auto rows = tx.exec(
        "SELECT d.id, d.status FROM reply_drafts d "
        "WHERE ((d.status = 'approved' AND d.decided_at <= $1::timestamptz) OR (d.status = 'scheduled' AND d.post_at <= $1::timestamptz)) "
        "AND NOT EXISTS (SELECT 1 FROM jobs j WHERE j.kind = $2 AND j.status IN ('queued', 'running') "
        "                 AND (j.payload->>'draft_id')::bigint = d.id) FOR UPDATE OF d",
        pqxx::params{to_timestamptz(approved_before), std::string(kPostReplyKind)});
    for (const auto& row : rows) {
        const auto id = row[0].as<std::int64_t>();
        if (release_approval(tx, id, "The posting job did not finish, so nothing was posted. Approve again to retry.", now)) {
            if (row[1].as<std::string>() == "scheduled") {
                emit_approval_needed(tx, id, "The job that would have posted it automatically did not finish.", now);
            }
            released.push_back(id);
        }
    }
    return released;
}

// ---- called by the review sync ---------------------------------------------------

std::string_view reconcile_reply(pqxx::transaction_base& tx, std::int64_t review_id, std::string_view reply_text,
                                 core::TimePoint now) {
    const auto now_text = to_timestamptz(now);
    const auto seen = normalized(reply_text);
    // Newest first. An unconfirmed post counts: Google may show a reply later than the window allowed for.
    const auto posts = tx.exec("SELECT id, draft_id, status, text FROM reply_posts "
                               "WHERE review_id = $1 AND status IN ('posting', 'posted', 'unconfirmed', 'published') ORDER BY id DESC",
                               pqxx::params{review_id});
    for (const auto& post : posts) {
        if (normalized(post[3].as<std::string>()) != seen) continue;
        const auto post_id = post[0].as<std::int64_t>();
        const auto draft_id = post[1].as<std::int64_t>();
        if (post[2].as<std::string>() != "published") {
            tx.exec("UPDATE reply_posts SET status = 'published', published_at = $2::timestamptz, "
                    "posted_at = coalesce(posted_at, $2::timestamptz), updated_at = $2::timestamptz WHERE id = $1",
                    pqxx::params{post_id, now_text})
                .no_rows();
            tx.exec(std::format("UPDATE reply_drafts SET status = 'published', status_reason = NULL, updated_at = $2::timestamptz "
                                "WHERE id = $1 AND status IN {}",
                                kLiveDrafts),
                    pqxx::params{draft_id, now_text})
                .no_rows();
            system_audit(tx, "reply.published", draft_id, {{"review_id", review_id}, {"post_id", post_id}}, now);
        }
        // A draft written after this post was given up on has nothing left to say.
        tx.exec(std::format("UPDATE reply_drafts SET status = 'cancelled', status_reason = 'the reply sent earlier appeared on Google', "
                            "updated_at = $3::timestamptz WHERE review_id = $1 AND id <> $2 AND status IN {}",
                            kWaitingDrafts),
                pqxx::params{review_id, draft_id, now_text})
            .no_rows();
        return "system";
    }

    // Somebody else's reply.
    for (const auto& row : tx.exec(std::format("UPDATE reply_posts SET status = 'superseded', "
                                               "error = concat_ws('; ', error, 'another reply was found on the review'), "
                                               "updated_at = $2::timestamptz WHERE review_id = $1 AND status IN {} "
                                               "RETURNING id, draft_id",
                                               kOpenPost),
                                   pqxx::params{review_id, now_text})) {
        const auto draft_id = row[1].as<std::int64_t>();
        tx.exec(std::format("UPDATE reply_drafts SET status = 'cancelled', "
                            "status_reason = 'another reply was found on Google after this one was sent', updated_at = $2::timestamptz "
                            "WHERE id = $1 AND status IN {}",
                            kOpenPost),
                pqxx::params{draft_id, now_text})
            .no_rows();
        system_audit(tx, "reply.post_superseded", draft_id, {{"review_id", review_id}, {"post_id", row[0].as<std::int64_t>()}}, now);
    }
    close_live_draft(tx, review_id, "cancelled", "a reply was posted outside the system", now);
    return "external";
}

void close_posts_of_removed_review(pqxx::transaction_base& tx, std::int64_t review_id, core::TimePoint now) {
    const auto now_text = to_timestamptz(now);
    tx.exec(std::format("UPDATE reply_posts SET status = 'superseded', error = concat_ws('; ', error, 'the review was removed'), "
                        "updated_at = $2::timestamptz WHERE review_id = $1 AND status IN {}",
                        kOpenPost),
            pqxx::params{review_id, now_text})
        .no_rows();
    tx.exec(std::format("UPDATE reply_drafts SET status = 'cancelled', status_reason = 'the review was removed', "
                        "updated_at = $2::timestamptz WHERE review_id = $1 AND status IN {}",
                        kLiveDrafts),
            pqxx::params{review_id, now_text})
        .no_rows();
}

model::PostingStatus posting_status(pqxx::transaction_base& tx) {
    return db::one_as<model::PostingStatus>(tx, std::format(R"sql(
        SELECT (SELECT posting_enabled FROM system_settings) AS enabled,
               (SELECT count(*) FROM locations l JOIN clients c ON c.id = l.client_id
                 WHERE l.access_status = 'active' AND NOT c.archived AND NOT l.posting_enabled) AS paused_locations,
               (SELECT count(*) FROM reply_drafts WHERE status = 'draft') AS drafts_waiting,
               (SELECT count(*) FROM reply_drafts WHERE status IN ('approved', 'posting', 'posted')) AS in_flight,
               (SELECT count(*) FROM reply_posts WHERE status = 'published') AS published,
               (SELECT {} FROM reply_posts) AS last_posted_at,
               (SELECT count(*) FROM cases WHERE status = 'open') AS open_escalations)sql",
                                                            iso("max(posted_at)")));
}

}  // namespace poni::repo
