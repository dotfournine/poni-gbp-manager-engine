// Local posts (FR-14, D-088): the posts of a location's Google Business
// Profile as the engine holds them. A row is a draft written here, a post
// the engine sent, or a post made in Google's own interface that the sync
// found. Blocking calls on a transaction the caller owns: run them through
// Database::run.
//
// A post written here moves   draft -> approved -> sending -> sent
// and may end as              rejected (a person), failed (a send that never
//                             showed up at Google) or removed (gone at Google).
// `unknown` sits between sending and sent when Google's answer never came.
// A draft or a rejected post, which Google never had, may also be deleted
// from the table (delete_local_post, D-094); nothing that was sent is.
// What Google does with a post it has is in google_state, not in status.
//
// A post that is at Google is changed there by an edit (D-099): a row of
// local_post_edits beside the post, at most one, which moves
//                             draft -> approved -> sending
// and from each of the last two back to draft with a reason. The post's own
// row is untouched until Google has accepted the edit; then it takes
// Google's answer and the edit's row goes.
#pragma once

#include "connectors/gbp/local_posts.hpp"
#include "core/clock.hpp"
#include "db/audit.hpp"
#include "db/failure.hpp"
#include "db/repo_reviews.hpp"
#include "db/sql.hpp"
#include "domain/models.hpp"

#include <array>
#include <pqxx/pqxx>

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace poni::repo {

inline constexpr std::string_view kSyncLocalPostsKind = "local_posts.sync";
// A read changes nothing at Google, so all of it may run again.
inline constexpr int kSyncLocalPostsAttempts = 3;
inline constexpr std::string_view kPublishLocalPostKind = "local_posts.publish";
// Only a create that Google refused before doing anything (a rate limit, a token) is tried again.
inline constexpr int kPublishLocalPostAttempts = 4;
inline constexpr std::string_view kRemoveLocalPostKind = "local_posts.remove";
inline constexpr int kRemoveLocalPostAttempts = 5;
inline constexpr std::string_view kEditLocalPostKind = "local_posts.edit";
// Only an edit that was not sent, or that Google refused before doing anything (a rate limit, a token), is tried again.
inline constexpr int kEditLocalPostAttempts = 4;

// An approval is carried out within this time or not at all (of a draft and
// of an edit alike), and so is a request to remove a post: a job that runs late, after an outage or a
// restored backup, sends nothing and deletes nothing. A job's own attempts,
// with their waits, take a few minutes.
inline constexpr std::chrono::seconds approval_valid_for = std::chrono::minutes(30);
inline constexpr std::chrono::seconds removal_valid_for = std::chrono::minutes(60);

// How the sync settles a send whose outcome is unknown (D-088 point 6).
// A post Google created this long before the engine's clock says the send
// started can still be that send's: the two clocks differ.
inline constexpr std::chrono::seconds kLocalPostAdoptTolerance{120};
// A send is given up as failed after this long without the post showing at
// Google. A post created later than this after the send started is not that
// send's. The messages to staff say "15 minutes".
inline constexpr std::chrono::seconds kLocalPostFailAfter = std::chrono::minutes(15);
// A post still `sending` after this long was left by an engine that stopped
// between the claim and the bookkeeping: its outcome is unknown.
inline constexpr std::chrono::seconds kLocalPostStaleSendingAfter = std::chrono::minutes(5);
// How many posts that a list lacks are asked for one by one in a run.
inline constexpr int kLocalPostsMaxMissing = 20;

// The values of local_posts.status and of a post's topic_type.
inline constexpr std::array<std::string_view, 8> kLocalPostStatuses{"draft",   "approved", "sending",  "sent",
                                                                    "unknown", "failed",   "rejected", "removed"};
// "alert" only on a post made at Google: the engine writes domain::kPostTopicTypes.
inline constexpr std::array<std::string_view, 4> kLocalPostTopicTypes{"standard", "event", "offer", "alert"};

// What the list's google_state filter takes: the values the CHECK on local_posts.google_state
// allows, and `none` for a post without one (D-098). `none` is a word of the filter, never a state.
inline constexpr std::string_view kNoGoogleState = "none";
inline constexpr std::array<std::string_view, 6> kLocalPostGoogleStateFilter{"processing", "scheduled", "live",
                                                                             "recurring",  "rejected",  kNoGoogleState};

struct LocalPostFilter {
    std::optional<std::int64_t> location_id;
    // Any of these; empty: any. google_states may hold kNoGoogleState.
    std::vector<std::string> statuses;
    std::vector<std::string> google_states;
    bool descending = true;
    db::Page page;
};

// By the time Google has for the post or, before it has one, the time the
// draft was written, then by id: newest first, or oldest first when not
// `descending`. `media_base` is the public bucket's
// address (ObjectStore::base_url()), to which an uploaded file's name is added.
model::LocalPostList list_local_posts(pqxx::transaction_base& tx, const LocalPostFilter& filter, std::string_view media_base);
std::optional<model::LocalPost> get_local_post(pqxx::transaction_base& tx, std::int64_t id, std::string_view media_base);

// ---- drafts and their approval (D-088 points 3 and 10) -----------------------------------

// A new draft for the location, which must be switched on and accessible.
// The content is normalized (domain::normalize_local_post) and its checks
// are stored with it; a draft with a failed check is stored all the same.
Result<model::LocalPost> create_local_post(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t location_id,
                                           const domain::LocalPostContent& content, std::string_view media_base);

// Replaces the content of a post that is still a draft.
Result<model::LocalPost> edit_local_post(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id,
                                         const domain::LocalPostContent& content, std::string_view media_base);

// A new draft for the post's location with the post's content as it is now,
// for any post, also one made at Google (D-094). What a draft cannot hold is
// left out and named in its status_reason (domain::fit_for_draft); an upload
// is kept, Google's copy of a file is not. Otherwise as create_local_post:
// the location must be synced, and nothing is sent or queued. The copy's
// copied_from_id names the post copied, which nothing else sets (D-098).
Result<model::LocalPost> copy_local_post(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id, std::string_view media_base);

// Deletes a post that Google never had: a draft or a rejected one, and
// nothing else (D-094). The audit entry keeps what it said. Its upload stays.
Result<void> delete_local_post(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id);

// A person decides against a draft. `reason` may be empty.
Result<model::LocalPost> reject_local_post(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id,
                                           const std::string& reason, std::string_view media_base);

// Approves a draft whose stored content is `content`, the content the person
// read, and enqueues the job that sends it. The only function that enqueues
// local_posts.publish. Refuses unless the post is a draft, the content
// equals what is stored, every check passes on it as things are now, both
// posting switches are on and the location is synced.
Result<model::LocalPost> approve_local_post(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id,
                                            const domain::LocalPostContent& content, std::string_view media_base);

// ---- the publish job (D-088 points 5 and 6) -----------------------------------------------

struct LocalPostSendClaim {
    enum class Outcome {
        claimed,  // the caller now owns the send and must send the create, once
        // Nothing is sent. There is no approval to carry out (none, or the post was dealt with
        // already), or it could not be carried out (it is too old, the content changed, a posting
        // switch is off, the location is not synced, a check fails now) and the post is a draft again.
        not_approved,
    };
    Outcome outcome = Outcome::not_approved;
    std::string gbp_account_id;
    std::string gbp_location_id;
    // The create's body, in Google's spelling.
    boost::json::object body;
    // Why not, for the log.
    std::string detail;
};

// The last step before the create, in one transaction: locks the post,
// checks that a person approved exactly the content it holds now, reads both
// posting switches and the location, runs the checks again, and then marks
// the post `sending` with the time of the claim. An approval older than
// `approval_valid_for` is not carried out: a job that ran late does not send
// what a person approved long ago. Anything else than `claimed` has been
// dealt with (the post is back in `draft` with the reason) and the caller
// must not send.
LocalPostSendClaim claim_local_post_send(pqxx::transaction_base& tx, std::int64_t id, core::TimePoint now, std::string_view media_base);

// Google accepted the create: the post is `sent` and takes Google's name,
// state and times. Three more reads of the location are enqueued, 1, 5 and
// 15 minutes on (D-088 point 8). An answer that arrives after a read has
// already taken the send for unknown, or given it up, still settles it.
void local_post_send_accepted(pqxx::transaction_base& tx, std::int64_t id, const gbp::LocalPost& created, core::TimePoint now);

// Google refused: nothing was created. With `retry` the post is `approved`
// again for the job's next attempt; otherwise it returns to `draft` with
// Google's message and staff are notified.
void local_post_send_refused(pqxx::transaction_base& tx, std::int64_t id, const std::string& error, bool retry, core::TimePoint now);

// The create may or may not have arrived. The post is `unknown`: it is never
// sent again, staff are notified, and the reads that follow settle it.
void local_post_send_unknown(pqxx::transaction_base& tx, std::int64_t id, const std::string& error, core::TimePoint now);

// Approved posts that no queued or running job will send (a job that died
// for good) return to `draft`. Returns their ids.
std::vector<std::int64_t> release_orphaned_local_post_approvals(pqxx::transaction_base& tx, core::TimePoint approved_before,
                                                                core::TimePoint now);

// ---- an edit of a post that is at Google (D-099) ------------------------------------------

// Writes the post's open edit, or replaces it while it is a draft: `content`
// is the whole content the post is to have, and `based_on` the post's
// content as the person saw it when they wrote that. Nothing goes to Google.
// A post can have an edit while it is `sent`, has a name at Google, has no
// removal requested and belongs to a synced location, and only if its
// content is something a draft can hold. `based_on` must be the post's
// content as the engine holds it now (conflict `local_post_changed` if not):
// it becomes the edit's base, and the fields the edit changes are those in
// which `content` differs from it. An edit never writes another field. The
// content is normalized; a kind, a time to publish at or a repeat other than
// the post's is refused (invalid, naming the member). Every call rewrites
// the edit whole, also one with the content the edit already has: its
// writer, its time, its checks (a failed one included) and its base.
Result<model::LocalPost> write_local_post_edit(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id,
                                               const domain::LocalPostContent& content, const domain::LocalPostContent& based_on,
                                               std::string_view media_base);

// Discards the post's edit while it is a draft, whatever has become of the
// post. The audit entry keeps what it said.
Result<void> discard_local_post_edit(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id);

// Approves the post's draft edit. `content` is what the person read: the
// post as it would say with the edit applied (LocalPost.edit.content, which
// is domain::local_post_with_edit of the post as it is now). Enqueues the
// job that sends the edit, and is the only function that enqueues
// local_posts.edit. Refuses unless `content` equals that, the post can still
// have an edit, the edit changes something that Google does not have yet, no
// field it changes was changed at Google since it was written
// (domain::local_post_edit_request), every check passes as things are now,
// both posting switches are on and the location is synced. What was approved
// is kept: the claim and the job send nothing for a post that would say
// anything else.
Result<model::LocalPost> approve_local_post_edit(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id,
                                                 const domain::LocalPostContent& content, std::string_view media_base);

struct LocalPostEditClaim {
    enum class Outcome {
        claimed,  // the caller now owns this edit's one request to Google
        // Nothing is sent. There is no approval to carry out (none, or the edit is being sent already by
        // an earlier run), or it could not be carried out (it is too old, the content changed, a posting
        // switch is off, the post no longer qualifies, a check fails now) and the edit is a draft again
        // with the reason.
        not_approved,
    };
    Outcome outcome = Outcome::not_approved;
    // The moment of the claim, which names it: the edit's send_started_at. The run carries it to
    // every later step, and a step acts only on the edit as claimed at this moment.
    core::TimePoint claimed_at{};
    // The post, under its location's ids at Google as they are now.
    gbp::LocalPostName name;
    // Why not, for the log.
    std::string detail;
};

// The first step of carrying an approved edit out, in one transaction: locks
// the post and its edit, checks that the approval is no older than
// `approval_valid_for`, that what the post would say with the edit applied
// is exactly what a person approved, that the post still is one that can
// have an edit (sent, named, no removal requested, its name one of its
// location's), reads both posting switches and the location, runs the
// checks again, and then marks the edit `sending` as of `now`. Anything else
// than `claimed` has been dealt with and the caller must not send. An edit
// that is `sending` already is left exactly as it is.
LocalPostEditClaim claim_local_post_edit(pqxx::transaction_base& tx, std::int64_t id, core::TimePoint now, std::string_view media_base);

struct LocalPostEditRequest {
    enum class Outcome {
        send,     // the caller sends one update with `mask` and `body`
        settled,  // nothing is sent: the edit was dealt with here (`detail` says how)
    };
    Outcome outcome = Outcome::settled;
    // The fields to write, by Google's names: those the edit changes that Google does not have yet.
    std::vector<std::string_view> mask;
    // Those fields as the edit says them, in Google's spelling.
    boost::json::object body;
    // What the post is to say: the approved content.
    domain::LocalPostContent content;
    std::string detail;
    // Settled because the post no longer would say what was approved: the parts in which it differs, by
    // the API's names ("summary", "scheduled_time"). Names only, for the reason and the log.
    std::vector<std::string> changed_since_approval;
};

// The second step, with the post as Google returned it a moment ago. Like
// the bookkeeping below it acts only on the edit as claimed at `claimed_at`:
// an edit that was given back and claimed again since is another run's, and
// nothing is sent for it from here. Nor for a claim older than
// `approval_valid_for`. It stores the read on the post's row (as the sync
// would), then works out what the edit writes against it
// (domain::local_post_edit_request, with the edit's base). Nothing to write:
// Google has what the edit changes, and the edit is done without a request.
// A field the edit changes was changed at Google since it was written, the
// post would no longer say what was approved (a field the edit leaves alone
// changed at Google since the approval), a removal was requested meanwhile,
// or the post holds what a draft cannot hold: the edit is a draft again with
// the reason. Otherwise the mask and the body to send.
LocalPostEditRequest prepare_local_post_edit(pqxx::transaction_base& tx, std::int64_t id, core::TimePoint claimed_at, const gbp::LocalPost& read,
                                             core::TimePoint now, std::string_view media_base);

// The three functions below are the job's bookkeeping. Each settles the
// edit only if it is `sending` under the claim made at `claimed_at`, and
// does nothing otherwise.

// Google accepted the update: the post's row takes Google's answer (through
// apply_local_post), its edited_by and edited_at name the edit's author and
// this moment, the edit's row goes, and the audit entry keeps the content
// before and after and who approved. `sent` and `mask` are what was sent.
// Where the answer does not say what was sent in a field of the mask
// (domain::edit_fields_not_shown; nothing else about the answer counts),
// staff are notified: the edit is settled all the same. The location is read
// again 1, 5 and 15 minutes on.
void local_post_edit_accepted(pqxx::transaction_base& tx, std::int64_t id, core::TimePoint claimed_at, const domain::LocalPostContent& sent,
                              const std::vector<std::string_view>& mask, const gbp::LocalPost& updated, core::TimePoint now);

// Why an edit was not made, when that is certain: the post is as it was.
enum class LocalPostEditFailure {
    refused,     // Google answered the update and refused it
    unreadable,  // the post could not be read from Google first, so nothing was sent
    gone,        // Google does not find the post, so nothing was sent
    internal,    // the engine itself failed before the request (the database, say), so nothing was sent
};

// The edit was not made, for certain. With `retry` it is `approved` again
// for the job's next attempt; otherwise it returns to `draft` with the
// reason and staff are notified.
void local_post_edit_not_made(pqxx::transaction_base& tx, std::int64_t id, core::TimePoint claimed_at, LocalPostEditFailure failure,
                              const std::string& error, bool retry, core::TimePoint now);

// The update may or may not have arrived. It is never sent again for this
// approval: the edit returns to `draft` saying that Google did not confirm
// it, staff are notified, and the post's row is left for the reads that
// follow (1, 5 and 15 minutes on) to bring what Google has.
void local_post_edit_unknown(pqxx::transaction_base& tx, std::int64_t id, core::TimePoint claimed_at, const std::string& error,
                             core::TimePoint now);

// Edits on their way that no queued or running job will carry out (a job
// that died for good, or ended without its bookkeeping). One approved at or
// before `approved_before` returns to `draft`, nothing having been sent. One
// that is `sending` returns to `draft` as an edit Google did not confirm, and
// staff are notified. Never while a job for the edit is queued or running,
// however long ago it was claimed: that job may still send. Returns their
// posts' ids.
std::vector<std::int64_t> release_orphaned_local_post_edits(pqxx::transaction_base& tx, core::TimePoint approved_before, core::TimePoint now);

// ---- media (D-088 point 9) -------------------------------------------------------------------

struct NewLocalPostMedia {
    // Its name in the public bucket, p/<32 random bytes>.<ext>.
    std::string object_name;
    std::string format;  // "photo" or "video"
    std::string content_type;
    std::int64_t bytes = 0;
    std::optional<std::int64_t> width;
    std::optional<std::int64_t> height;
    std::optional<std::int64_t> duration_ms;  // a video's length; nothing for a photo
};

// Records a file that is already in the public bucket. The object's name is
// kept out of the audit entry, as a share link's token is.
model::LocalPostMedia create_local_post_media(pqxx::transaction_base& tx, audit::Writer& audit,
                                              const NewLocalPostMedia& media, std::string_view media_base);

// ---- removal (D-088 point 7) ----------------------------------------------------------------

// A signed-in person asks for a post to be deleted at Google: any post that
// Google has, of an accessible location, also while it is switched off
// (D-102), also one made in Google's own interface. The only function that enqueues local_posts.remove. It works
// while posting is switched off: deleting is the only way to stop a
// scheduled post or to take one down. The audit entry keeps a copy of the
// post's content and of what it showed. Asking again while a removal is
// under way, or for a post that is removed, changes nothing. An edit never
// stands in its way (D-099): a draft edit of the post is discarded with the
// request, its content kept in the same audit entry, and an approved one is
// stopped when its job claims it.
Result<model::LocalPost> request_local_post_removal(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id,
                                                    std::string_view media_base);

// What the removal job is to delete, read with the post's row locked: only
// a post whose removal a person asked for, by the location's own ids at
// Google and the post's id from its name. Otherwise why nothing is deleted,
// for the log: no person asked, the post is removed already, or the request
// can't be carried out and was given back with the reason. A post whose name
// does not belong to its location, or whose location is no longer accessible, is
// not deleted. Nor is one whose request is older than `removal_valid_for`: a
// job that ran late deletes nothing. An answer without a name may have
// written (the request given back, its audit entry, the notification): the
// caller commits either way, so run it in Database::tx, never as a refusal
// that is rolled back.
std::expected<gbp::LocalPostName, std::string> local_post_removal_target(pqxx::transaction_base& tx, std::int64_t id, core::TimePoint now);

// The post as Google has it a moment before it is deleted, stored on its
// row: the last copy of it there will be.
void apply_local_post_before_removal(pqxx::transaction_base& tx, std::int64_t id, const gbp::LocalPost& post, core::TimePoint now);

// Google deleted the post, or it is confirmed gone: it is `removed`. The
// audit entry keeps what the post said when it was last read.
void local_post_removed(pqxx::transaction_base& tx, std::int64_t id, core::TimePoint now);

// The delete could not be carried out: the request is given back with the
// reason, the post stays as it is, and staff are notified. `uncertain`: the
// delete may have happened all the same (an answer that never came).
void local_post_removal_failed(pqxx::transaction_base& tx, std::int64_t id, const std::string& error, bool uncertain, core::TimePoint now);

// Requests to remove a post that no queued or running job will carry out (a
// job that died for good) are given back, so that a post never says
// "removal requested" with nothing behind it. Returns their ids.
std::vector<std::int64_t> release_orphaned_local_post_removals(pqxx::transaction_base& tx, core::TimePoint requested_before,
                                                               core::TimePoint now);

// ---- the sync (D-088 point 8) ----------------------------------------------------------

// "Refresh": queues the read of one location's posts, now, for the person
// who asked. Refused for a location the engine does not sync.
Result<void> request_local_posts_sync(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t location_id);

// Syncable locations whose posts were never read, were last read at or
// before `read_before`, or that hold a send whose outcome is not settled
// (which is looked at on every tick).
std::vector<SyncLocation> locations_due_for_local_posts_sync(pqxx::transaction_base& tx, core::TimePoint read_before);

// A post the engine holds as being at Google that the list did not show.
struct MissingLocalPost {
    std::int64_t id = 0;
    gbp::LocalPostName name;
};

struct LocalPostsSyncOutcome {
    int listed = 0;
    int added = 0;     // posts the engine had not seen: made at Google
    int adopted = 0;   // unknown sends found at Google
    int failed = 0;    // unknown sends given up
    int rejected = 0;  // posts Google has rejected since the last read
    // To be asked for one by one: only NOT_FOUND removes a post.
    std::vector<MissingLocalPost> missing;
};

// Stores a complete list of the location's posts as Google returned it, in
// this order: settles sends whose outcome is unknown (a listed post that no
// row has yet, with the content that was sent and created while that send
// was open, is adopted by the send that started last before it; a send left
// without one fails once kLocalPostFailAfter has passed), then inserts or updates every listed post by
// its name, then marks the location read. Notifications for a failed send and a
// post Google rejected are raised here. Nothing is marked removed.
LocalPostsSyncOutcome store_local_posts(pqxx::transaction_base& tx, std::int64_t location_id,
                                        const std::vector<gbp::LocalPost>& posts, core::TimePoint now);

struct AppliedLocalPost {
    bool inserted = false;      // the engine had not seen the post
    bool rejected_now = false;  // Google has rejected it since the last read
};

// One post as Google returned it, inserted or updated by its name. A row
// that is `removed` is left alone: Google said NOT_FOUND for it, and a list
// that still shows it is behind.
AppliedLocalPost apply_local_post(pqxx::transaction_base& tx, std::int64_t location_id, const gbp::LocalPost& post, core::TimePoint now);

// Google answered NOT_FOUND for this post: it becomes `removed`. Never call
// it for a post that a list merely lacks. False if it was removed already.
bool mark_local_post_removed(pqxx::transaction_base& tx, std::int64_t id, core::TimePoint now);

// The location's posts could not be read: the error is kept for the
// dashboard, and nothing else changes.
void record_local_posts_sync_error(pqxx::transaction_base& tx, std::int64_t location_id, const std::string& error,
                                   core::TimePoint now);

}  // namespace poni::repo
