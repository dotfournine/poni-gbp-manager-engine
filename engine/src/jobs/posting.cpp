#include "jobs/posting.hpp"

#include "core/log.hpp"
#include "db/repo_auto_posting.hpp"
#include "db/repo_notifications.hpp"
#include "db/repo_posting.hpp"
#include "db/repo_reviews.hpp"

#include <format>
#include <optional>
#include <stdexcept>
#include <string>

namespace poni::jobs {

namespace asio = boost::asio;
using google::ApiError;

asio::awaitable<void> ReplyPoster::post(std::int64_t draft_id, bool last_attempt) {
    const auto target = co_await db_.tx([draft_id](pqxx::work& tx) { return repo::post_target(tx, draft_id); });
    if (!target || (target->draft_status != "approved" && target->draft_status != "scheduled")) {
        // Run twice, or a draft that was dealt with meanwhile: nothing to do. A
        // draft that is `posting` belongs to an earlier run whose outcome the
        // confirmation settles; it is never sent again from here.
        log::info("reply not posted", {{"draft_id", draft_id}, {"reason", target ? "the draft is " + target->draft_status : "no such draft"}});
        co_return;
    }

    // ---- 1. the review as Google has it now ----------------------------------------
    const auto read = co_await google::read(reader_.get_review(target->name));
    if (!read.error.empty()) {
        if (!last_attempt) throw std::runtime_error(std::format("draft {}: the review could not be read before posting: {}", draft_id, read.error));
        const auto reason = std::format("The review could not be read from Google before posting ({}). Nothing was posted.", read.error);
        co_await db_.tx([&, now = clock_.now()](pqxx::work& tx) {
            if (repo::release_approval(tx, draft_id, reason, now)) {
                repo::emit(tx,
                           repo::post_problem(std::format("draft:{}:unreadable:{}", draft_id, db::to_timestamptz(now)), target->location_id,
                                              target->review_id, "A reply could not be posted", reason),
                           now);
            }
        });
        log::warn("reply not posted: the review could not be read", {{"draft_id", draft_id}, {"error", read.error}});
        co_return;
    }

    // ---- 2. store what Google said, then check and claim, in one transaction --------
    const auto claim = co_await db_.tx([&, now = clock_.now()](pqxx::work& tx) {
        if (read.gone) {
            // Only NOT_FOUND for this very review marks it removed (D-037).
            repo::mark_review_removed(tx, target->location_id, target->name.review_id, repo::kSourcePosting, now);
        } else {
            repo::apply_review(tx, target->location_id, *read.value, repo::kSourcePosting, now);
        }
        return repo::claim_post(tx, draft_id, now);
    });
    if (claim.outcome != repo::Claim::Outcome::claimed) {
        log::info("reply not posted", {{"draft_id", draft_id}, {"review_id", target->review_id}, {"reason", claim.detail}});
        co_return;
    }

    // ---- 3. the one write ----------------------------------------------------------
    // From here on nothing throws: the job must not run again for this post.
    std::optional<ApiError> refused;
    std::string unknown;
    try {
        const auto result = co_await writer_.put_reply(target->name, claim.text);
        if (!result) {
            // A 5xx says nothing about whether the reply was stored.
            if (result.error().kind == ApiError::Kind::unavailable) {
                unknown = result.error().message;
            } else {
                refused = result.error();
            }
        }
    } catch (const std::exception& e) {
        unknown = e.what();
    }

    std::string retry_error;
    std::string bookkeeping_failure;
    try {
        if (refused) {
            const bool retry = google::worth_retrying(refused->kind) && !last_attempt;
            co_await db_.tx([&, now = clock_.now()](pqxx::work& tx) {
                repo::post_refused(tx, claim.post_id, refused->message, retry, now);
                if (!retry && claim.automatic) {
                    // Google refusing an automatic reply stops the automation until a
                    // person has looked (D-072). A token or permission the whole
                    // account depends on stops it everywhere.
                    const auto detail = std::format("Google refused an automatic reply ({}). Nothing was posted.", refused->message);
                    if (refused->kind == ApiError::Kind::unauthenticated || refused->kind == ApiError::Kind::not_configured) {
                        repo::pause_auto_posting(tx, std::nullopt, repo::kPausePostRefused, detail, now);
                    } else {
                        repo::pause_auto_posting(tx, target->location_id, repo::kPausePostRefused, detail, now);
                    }
                }
                if (!retry) {
                    repo::emit(tx,
                               repo::post_problem(std::format("post:{}:refused", claim.post_id), target->location_id, target->review_id,
                                                  "Google refused a reply",
                                                  std::format("Google refused the reply to a review ({}). Nothing was posted. The draft "
                                                              "is back in the approval queue.",
                                                              refused->message)),
                               now);
                }
            });
            log::warn("reply refused by Google", {{"draft_id", draft_id}, {"review_id", target->review_id}, {"error", refused->message}, {"will_retry", retry}});
            // Nothing was posted, so another attempt is safe.
            if (retry) retry_error = std::format("draft {}: Google refused the reply: {}", draft_id, refused->message);
        } else if (!unknown.empty()) {
            co_await db_.tx([&, now = clock_.now()](pqxx::work& tx) {
                repo::post_outcome_unknown(tx, claim.post_id, unknown, now);
            });
            log::warn("reply sent, outcome unknown; waiting for it to show on Google",
                      {{"draft_id", draft_id}, {"review_id", target->review_id}, {"error", unknown}});
        } else {
            co_await db_.tx([&, now = clock_.now()](pqxx::work& tx) {
                repo::post_accepted(tx, claim.post_id, now);
            });
            // No reply text in the log: ids only.
            log::info("reply posted", {{"draft_id", draft_id}, {"review_id", target->review_id}, {"post_id", claim.post_id}});
        }
    } catch (const std::exception& e) {
        bookkeeping_failure = e.what();
    }
    if (!bookkeeping_failure.empty()) {
        // The PUT happened (or may have) and the database could not be told.
        // The post is still open, which is the safe state: the confirmation
        // reads the review and settles it. Not thrown: the job must not run again.
        log::error("reply bookkeeping failed; the post stays open",
                   {{"draft_id", draft_id}, {"post_id", claim.post_id}, {"error", bookkeeping_failure}});
        co_return;
    }
    if (!retry_error.empty()) throw std::runtime_error(retry_error);
}

asio::awaitable<std::size_t> ReplyPoster::confirm() {
    const auto started = clock_.now();
    const auto posts = co_await db_.tx([cutoff = started - options_.confirm_window](pqxx::work& tx) { return repo::open_posts(tx, cutoff); });

    std::size_t still_open = 0;
    for (const auto& post : posts) {
        const auto read = co_await google::read(reader_.get_review(post.name));
        const bool settled = co_await db_.tx([&, now = clock_.now()](pqxx::work& tx) {
            return repo::settle_open_post(tx, post, read.value, read.gone, now);
        });
        if (!read.error.empty()) log::warn("open post: the review could not be read", {{"post_id", post.post_id}, {"error", read.error}});
        if (settled) {
            log::info("open post settled", {{"post_id", post.post_id}, {"review_id", post.review_id}});
        } else {
            ++still_open;
        }
    }

    const auto released = co_await db_.tx([now = clock_.now(), before = started - kOrphanedApprovalAfter](pqxx::work& tx) {
        return repo::release_orphaned_approvals(tx, before, now);
    });
    for (const auto id : released) log::warn("approved draft returned to the queue: its posting job is gone", {{"draft_id", id}});
    co_return still_open;
}

void install(Registry& registry, ReplyPoster& poster) {
    registry.add(std::string(repo::kPostReplyKind), [&poster](const Job& job) -> asio::awaitable<void> {
        co_await poster.post(job.payload_id("draft_id"), job.last_attempt());
    });
    registry.every(std::string(kConfirmPostsKind), kConfirmPostsTick, 1,
                   [&poster](const Job&) -> asio::awaitable<void> { co_await poster.confirm(); });
}

}  // namespace poni::jobs
