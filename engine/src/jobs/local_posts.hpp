// Local posts (FR-14, D-088): the jobs that send a post to Google and read a
// location's posts from it. With jobs/posting.hpp this is the only code
// that changes anything on a Google Business Profile.
//
//   local_posts.publish   one approved post. Claims it (both posting
//                         switches are read in that transaction), then sends
//                         one create with the post's scheduled time and
//                         repeat: Google publishes it, the engine has no
//                         timer. Enqueued only by the approval of a
//                         signed-in person (repo::approve_local_post).
//   local_posts.remove    one post a signed-in person asked to remove
//                         (repo::request_local_post_removal): reads the post
//                         under the address it will delete, then sends one
//                         DELETE. Gone after that read means gone, so a
//                         delete is safe to repeat and is tried again when
//                         Google could not be reached. A NOT_FOUND for a
//                         post the job could not read first proves nothing
//                         (a wrong account answers the same): the post is
//                         removed only if the location's own list confirms
//                         it. It does not read the posting switches:
//                         deleting is how a post is stopped.
//   local_posts.edit      one approved edit of a post that is at Google
//                         (D-099). Claims it (both posting switches are read
//                         in that transaction), reads the post from Google
//                         under its location's own ids, and sends one PATCH
//                         that names exactly the fields in which the edit
//                         differs from what was just read. Enqueued only by
//                         the approval of a signed-in person
//                         (repo::approve_local_post_edit). The PATCH is sent
//                         once per approval: a certain refusal gives the edit
//                         back with Google's message (a rate limit or a token
//                         is tried again), and after any other answer the
//                         edit is given back as one Google did not confirm,
//                         and is not sent again unless a person approves again.
//   local_posts.sync      one location: lists its posts at Google and stores
//                         what it finds, so states, posts made in Google's
//                         own interface and changes made there show up. It
//                         also settles a send whose outcome was unknown (the
//                         post is adopted if Google has it, failed if it is
//                         not there 15 minutes after the send), and asks one
//                         by one for posts the list lacks: a post is marked
//                         removed only when Google answers NOT_FOUND for it.
//   local_posts.sync_due  every few minutes: enqueues the sync for every
//                         location that was not read for an hour, or that
//                         holds a send that is not settled. It also gives
//                         back approvals, requests to remove and edits whose
//                         job is gone.
//
// What can't be known is treated as possibly sent. A create that timed out,
// a 5xx, an answer that can't be read, a crash between the create and the
// bookkeeping: each leaves the post `unknown` (or `sending`, which the sync
// takes for unknown after a few minutes), and the create is never sent
// again. The sync settles it by reading. Nothing here retries a create
// whose outcome is unknown.
//
// An edit has no `unknown` state and nothing settles it: an edit that may
// have arrived cannot leave a second post behind, and the next read of the
// location shows what the post says. So an edit whose outcome is not known
// goes back to the person as a draft that says so. A job that finds its
// edit `sending` already sends nothing: an edit left so by an engine that
// stopped between the claim and the bookkeeping is given back the same way,
// by that job or by local_posts.sync_due, once it has been `sending` for
// five minutes.
#pragma once

#include "connectors/gbp/local_posts.hpp"
#include "core/clock.hpp"
#include "db/database.hpp"
#include "db/repo_local_posts.hpp"
#include "jobs/queue.hpp"
#include "jobs/registry.hpp"

#include <boost/asio/awaitable.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace poni::jobs {

inline constexpr std::string_view kSyncDueLocalPostsKind = "local_posts.sync_due";

struct LocalPostOptions {
    // The public bucket's address, from which Google fetches a post's photo.
    std::string media_base;
};

class LocalPosts {
public:
    LocalPosts(db::Database& db, const core::Clock& clock, gbp::LocalPostReader& reader, gbp::LocalPostWriter& writer, JobQueue& queue,
               LocalPostOptions options)
        : db_(db), clock_(clock), reader_(reader), writer_(writer), queue_(queue), options_(std::move(options)) {}

    // Sends the post if a person approved it and everything still holds.
    // Throws to be run again only when Google refused before creating anything.
    boost::asio::awaitable<void> publish(std::int64_t local_post_id, bool last_attempt);

    // Deletes the post at Google if a person asked for it. Throws to be run
    // again when Google could not be reached: a delete is safe to repeat.
    boost::asio::awaitable<void> remove(std::int64_t local_post_id, bool last_attempt);

    // Sends the post's approved edit to Google, once, if everything still
    // holds. Throws to be run again only when nothing was sent, or Google
    // refused before changing anything.
    boost::asio::awaitable<void> edit(std::int64_t local_post_id, bool last_attempt);

    // Reads the location's posts and stores them. Does nothing for a
    // location that is not synced. Throws when Google could not be read, so
    // the job runs again; the error is kept on the location meanwhile.
    boost::asio::awaitable<repo::LocalPostsSyncOutcome> sync(std::int64_t location_id);

    // Enqueues a sync for each location that is due, and returns approvals
    // (of a post and of an edit) whose job is gone to the drafts. Returns how
    // many syncs were enqueued.
    boost::asio::awaitable<std::size_t> enqueue_due();

    const LocalPostOptions& options() const { return options_; }

private:
    db::Database& db_;
    const core::Clock& clock_;
    gbp::LocalPostReader& reader_;
    gbp::LocalPostWriter& writer_;
    JobQueue& queue_;
    LocalPostOptions options_;
};

// Against the Google mock, which has no posts, nothing recurs (D-088 point 13).
void install(Registry& registry, LocalPosts& local_posts, bool google_mock);

}  // namespace poni::jobs
