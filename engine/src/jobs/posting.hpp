// Posting, approved by a person or decided by the policy (architecture 4.1,
// step 5): the only code that writes a reply to
// Google.
//
//   replies.post      one approved draft. Reads the review from Google
//                     again and stops unless it still exists, is unchanged
//                     since the draft was written and has no reply but the
//                     engine's own; checks both kill switches; then sends
//                     one PUT. Enqueued by the approval of a signed-in
//                     person (repo::approve_draft) or, where a person
//                     switched auto reply on, by the policy
//                     (repo::schedule_auto_reply, D-069). Nothing else.
//   replies.confirm   every minute: reads the reviews that have an open
//                     post and settles each one. Google shows a new reply
//                     minutes after accepting it (spike B6), so "posted"
//                     and "published" are different states.
//
// What can't be known is treated as possibly posted. A PUT that timed out,
// a 5xx, a crash between the PUT and the bookkeeping: each leaves the post
// open, and while a post is open the engine does not post to that review
// again. The confirmation settles it by reading; if the reply has not shown
// up when the window ends, the draft goes back to the approval queue for a
// person to look at. Nothing here retries a PUT whose outcome is unknown.
#pragma once

#include "connectors/gbp/reader.hpp"
#include "connectors/gbp/writer.hpp"
#include "core/clock.hpp"
#include "db/database.hpp"
#include "jobs/registry.hpp"

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <string_view>
#include <vector>

namespace poni::jobs {

inline constexpr std::string_view kConfirmPostsKind = "replies.confirm";

// How often open posts are looked at.
inline constexpr std::chrono::seconds kConfirmPostsTick{60};
// An approved draft whose job vanished returns to the queue after this long.
inline constexpr std::chrono::seconds kOrphanedApprovalAfter{15 * 60};

struct PostingOptions {
    // How long a sent reply may stay invisible on Google before the engine
    // gives up waiting. The spike saw 4 to 6 minutes.
    std::chrono::seconds confirm_window{30 * 60};
};

class ReplyPoster {
public:
    ReplyPoster(db::Database& db, const core::Clock& clock, gbp::Reader& reader, gbp::ReplyWriter& writer, PostingOptions options)
        : db_(db), clock_(clock), reader_(reader), writer_(writer), options_(options) {}

    // Posts the draft if it is approved and everything still holds. Throws
    // to be retried only when nothing was sent.
    boost::asio::awaitable<void> post(std::int64_t draft_id, bool last_attempt);

    // Settles open posts by reading their reviews; expires the ones whose
    // window has ended. Returns how many posts were still open afterwards.
    boost::asio::awaitable<std::size_t> confirm();

    const PostingOptions& options() const { return options_; }

private:
    db::Database& db_;
    const core::Clock& clock_;
    gbp::Reader& reader_;
    gbp::ReplyWriter& writer_;
    PostingOptions options_;
};

void install(Registry& registry, ReplyPoster& poster);

}  // namespace poni::jobs
