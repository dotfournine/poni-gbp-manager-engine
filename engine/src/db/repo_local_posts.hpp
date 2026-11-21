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

// An approval is carried out within this time or not at all, and so is a
// request to remove a post: a job that runs late, after an outage or a
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

// The values of local_posts.google_state.
inline constexpr std::array<std::string_view, 5> kLocalPostGoogleStates{"processing", "scheduled", "live", "recurring", "rejected"};
// What the list's google_state filter takes: those, and `none` for a post without one (D-098).
// `none` is a word of the filter, never a state.
inline constexpr std::string_view kNoGoogleState = "none";
inline constexpr std::array<std::string_view, 6> kLocalPostGoogleStateFilter{"processing", "scheduled", "live",
                                                                             "recurring",  "rejected",  kNoGoogleState};

struct LocalPostFilter {
    std::optional<std::int64_t> client_id;
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

// A new draft for the location, which must be linked to an active client.
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
// Google has, of a linked location, also one made in Google's own
// interface. The only function that enqueues local_posts.remove. It works
// while posting is switched off: deleting is the only way to stop a
// scheduled post or to take one down. The audit entry keeps a copy of the
// post's content and of what it showed. Asking again while a removal is
// under way, or for a post that is removed, changes nothing.
Result<model::LocalPost> request_local_post_removal(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id,
                                                    std::string_view media_base);

// What the removal job is to delete, read with the post's row locked: only
// a post whose removal a person asked for, by the location's own ids at
// Google and the post's id from its name. Otherwise why nothing is deleted,
// for the log: no person asked, the post is removed already, or the request
// can't be carried out and was given back with the reason. A post whose name
// does not belong to its location, or whose location is no longer linked, is
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
