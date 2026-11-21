// Posting a reply, approved by a person or decided by the policy: what the approval queue does to
// a draft, the posting job's bookkeeping, and how a reply seen on Google is
// matched with the engine's own posts. Blocking calls on a transaction the
// caller owns: run them through Database::run.
//
// A draft moves   draft -> approved  -> posting -> posted -> published
//            or   draft -> scheduled -> posting -> posted -> published.
//
//   approved   a signed-in person approved exactly this text
//   scheduled  the policy decided to post it without a person, at post_at
//              (auto-posting, D-069)
//   posting    the job owns the post: a reply_posts row exists and the PUT
//              is being sent, or was sent and its outcome is not known
//   posted     Google answered 200
//   published  a later read of the review showed the reply
//
// Two things put a draft on its way, and nothing else does: approve_draft
// (a person) and schedule_auto_post (the policy, for a location where a
// person switched auto-posting on). Both end in the same job and the same
// claim_post, so every guard below holds for both.
//
// The rule everything here protects: at most one PUT per approval, and no
// PUT to a review while an earlier one is unconfirmed. "No reply visible on
// Google" is never taken as permission to post again, because Google shows a
// new reply only minutes after accepting it (spike B6). The open row in
// reply_posts is what says "a reply may be on its way", and a unique index
// allows one open row per review.
#pragma once

#include "connectors/gbp/reader.hpp"
#include "core/clock.hpp"
#include "db/audit.hpp"
#include "db/failure.hpp"
#include "db/repo_notifications.hpp"
#include "domain/models.hpp"

#include <pqxx/pqxx>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace poni::repo {

inline constexpr std::string_view kPostReplyKind = "replies.post";
// Only failures before the PUT, or a PUT that Google refused outright, are retried.
inline constexpr int kPostReplyAttempts = 4;

// Lists of statuses as SQL, for `status IN {}`.
// An open post, and its draft: the PUT is being sent, or was sent and the
// reply has not been seen on Google. While a review has an open post,
// nothing is posted to it.
inline constexpr std::string_view kOpenPost = "('posting', 'posted')";
// A draft nothing was sent for yet: it waits for a person or for its job.
inline constexpr std::string_view kWaitingDrafts = "('draft', 'approved', 'scheduled')";
// A draft still in play: waiting, or with an open post.
inline constexpr std::string_view kLiveDrafts = "('draft', 'approved', 'scheduled', 'posting', 'posted')";

// ---- the approval queue ----------------------------------------------------------

// Replaces the text of a waiting draft and runs the reply checks on it again.
Result<model::ReplyDraft> edit_draft(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t draft_id, const std::string& text);

// Approves a waiting draft whose stored text is `text` and enqueues the job
// that posts it. Refuses unless every reply check passes on the text, both
// posting switches are on, and the review, as the mirror holds it, can be
// replied to. (The job checks the review again on Google itself.)
Result<model::ReplyDraft> approve_draft(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t draft_id, const std::string& text);

Result<model::ReplyDraft> reject_draft(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t draft_id, const std::string& reason);

// ---- auto-posting (D-069) --------------------------------------------------------

// Called by the pipeline in the transaction that stored `draft_id`, for a
// draft whose route is auto_post. If a person switched auto-posting on for
// the location and this star rating, nothing is paused, the cap is not
// reached and every other condition holds (see auto-posting in
// repo_posting.cpp), the draft becomes `scheduled` and the posting job is
// enqueued for post_at: now plus the policy's random delay (`roll` in
// [0, 1) picks the point in its range), moved into the posting window.
// Otherwise the draft stays in the approval queue, with the reason unless
// the switches simply don't ask for an auto-post here. True if it was
// scheduled. The caller must hold the review's row lock.
bool schedule_auto_post(pqxx::transaction_base& tx, std::int64_t draft_id, core::TimePoint now, double roll);

// A person takes a scheduled draft back: it waits in the approval queue
// like any other, and the job that would have posted it finds nothing to do.
Result<model::ReplyDraft> unschedule_draft(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t draft_id);

// ---- the posting job -------------------------------------------------------------

struct PostTarget {
    std::int64_t review_id = 0;
    std::int64_t location_id = 0;
    std::string draft_status;
    gbp::ReviewName name;
};
std::optional<PostTarget> post_target(pqxx::transaction_base& tx, std::int64_t draft_id);

struct Claim {
    enum class Outcome {
        claimed,           // the caller now owns the post and must send the PUT
        not_approved,      // nothing to do: the draft was not approved, or was dealt with already
        in_flight,         // a post for this review is already open
        stopped,           // an approved or a scheduled draft: the review is gone, was edited or has
                           // someone else's reply, or a kill switch is off or the location is no
                           // longer synced. `detail` says which
        auto_post_stopped, // a scheduled draft: the auto-posting conditions or the reply policy no
                           // longer allow it without a person
    };
    Outcome outcome = Outcome::not_approved;
    std::int64_t post_id = 0;
    // Of a claimed post: it was decided by the policy, not approved by a person.
    bool automatic = false;
    std::string text;
    // Why not, for the log.
    std::string detail;
};

// The last step before the PUT, in one transaction: locks the review and
// the draft, checks the review as the mirror now holds it (the job has just
// refreshed it from Google), checks both kill switches, and then opens the
// post and marks the draft `posting`. Anything else than `claimed` has
// already been dealt with (the draft cancelled, superseded or returned to
// the queue with the reason) and the caller must not post.
//
// A scheduled draft goes through the same checks and then through the
// auto-posting conditions again, as they are now: a switch flipped, a pause,
// the cap or a changed voice since it was scheduled stops it, and it goes to
// the approval queue with the reason and an "approval needed" notification.
Claim claim_post(pqxx::transaction_base& tx, std::int64_t draft_id, core::TimePoint now);

// Google answered 200: the post and the draft become `posted`, in one transaction.
void post_accepted(pqxx::transaction_base& tx, std::int64_t post_id, core::TimePoint now);

// Google refused: nothing was posted. With `retry` the draft stays approved
// (or scheduled) for the job's next attempt; otherwise it returns to the queue with the reason.
void post_refused(pqxx::transaction_base& tx, std::int64_t post_id, const std::string& error, bool retry, core::TimePoint now);

// The request may or may not have arrived (a timeout, a dropped connection,
// a 5xx). The post stays open, so nothing is posted to the review again
// until a read of the review settles it or the confirmation window ends.
void post_outcome_unknown(pqxx::transaction_base& tx, std::int64_t post_id, const std::string& error, core::TimePoint now);

struct OpenPost {
    std::int64_t post_id = 0;
    std::int64_t review_id = 0;
    std::int64_t location_id = 0;
    std::string location_title;
    gbp::ReviewName name;
    // It was opened before the cutoff: the confirmation window has ended.
    bool overdue = false;
    bool automatic = false;
};
// Posts that are `posting` or `posted`, oldest first.
std::vector<OpenPost> open_posts(pqxx::transaction_base& tx, core::TimePoint overdue_before);

// One open post, after the confirmation read its review from Google again:
// `review` is what Google returned, `gone` that it answered NOT_FOUND for
// this review, and neither that it could not be asked. Stores what Google
// said and settles the post if the reply is there. A post that is still open
// when its window has ended becomes `unconfirmed`: the draft returns to the
// queue for a person to decide, staff are notified, and an automatic reply
// pauses auto-posting at its location. Nothing is posted again. True if the
// post is no longer open.
bool settle_open_post(pqxx::transaction_base& tx, const OpenPost& post, const std::optional<gbp::Review>& review, bool gone,
                      core::TimePoint now);

// A system-problem notification about one reply.
NotificationEvent post_problem(std::string key, std::int64_t location_id, std::int64_t review_id, std::string subject, std::string body);

// The job could not get as far as the claim (Google could not be read, and
// its attempts are used up): the approval is given back, and the draft waits
// in the queue with the reason. False if the draft was not `approved` or `scheduled`.
bool release_approval(pqxx::transaction_base& tx, std::int64_t draft_id, const std::string& reason, core::TimePoint now);

// Approved and scheduled drafts that no queued or running job will post (a
// job that died for good) return to the queue. Returns their ids.
std::vector<std::int64_t> release_orphaned_approvals(pqxx::transaction_base& tx, core::TimePoint approved_before,
                                                     core::TimePoint now);

// ---- called by the review sync ---------------------------------------------------

// A reply appeared on the review, or its text changed. If it is the text of
// one of the engine's own posts, that post and its draft become `published`
// and the answer is "system". Otherwise it was written outside the system:
// open posts are closed as superseded, drafts of the review are cancelled,
// and the answer is "external".
std::string_view reconcile_reply(pqxx::transaction_base& tx, std::int64_t review_id, std::string_view reply_text,
                                 core::TimePoint now);

// The review is gone from Google: open posts and every draft still in play are closed.
void close_posts_of_removed_review(pqxx::transaction_base& tx, std::int64_t review_id, core::TimePoint now);

// Everything on the status page's posting block.
model::PostingStatus posting_status(pqxx::transaction_base& tx);

}  // namespace poni::repo
