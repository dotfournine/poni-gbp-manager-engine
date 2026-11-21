#include "jobs/local_posts.hpp"

#include "core/log.hpp"

#include <boost/system/system_error.hpp>

#include <chrono>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>

namespace poni::jobs {

namespace asio = boost::asio;
using google::ApiError;

namespace {

// How often a location's posts are read (D-088 point 8), and how often the engine looks for locations that are due.
constexpr std::chrono::seconds kSyncInterval = std::chrono::hours(1);
constexpr std::chrono::seconds kSyncTick = std::chrono::minutes(5);
// Pages of 100 posts read for one location before the engine stops: a guard, not a limit anyone reaches.
constexpr int kMaxPages = 50;
// An approved post or edit, or a request to remove a post, whose job vanished is given back after this long.
constexpr std::chrono::seconds kOrphanAfter = std::chrono::minutes(15);

struct Listing {
    std::vector<gbp::LocalPost> posts;
    std::string error;
};

asio::awaitable<Listing> list_all(gbp::LocalPostReader& reader, std::string account_id, std::string location_id) {
    Listing out;
    std::optional<std::string> page_token;
    for (int page = 0; page < kMaxPages; ++page) {
        auto result = co_await google::guarded(reader.list_local_posts(account_id, location_id, page_token));
        if (!result) {
            out.error = result.error().message;
            co_return out;
        }
        for (auto& post : result->posts) out.posts.push_back(std::move(post));
        page_token = std::move(result->next_page_token);
        if (!page_token) co_return out;
    }
    // Half a list would make the rest look missing.
    out.error = std::format("the location has more than {} pages of posts", kMaxPages);
    co_return out;
}

// Whether the answer says for certain that Google created nothing (and, for
// an edit, changed nothing): no request went out because there is no token,
// or Google answered with one of the statuses it gives before it does
// anything (a bad request, a token or permission it refuses, an address it
// does not know, a quota). Every other answer leaves it open: a 5xx whatever
// its body says, a 408, a 409, a 499, a redirect, a 2xx whose body names no
// post.
bool definitely_refused(const ApiError& error) {
    if (error.kind == ApiError::Kind::invalid_response) return false;
    if (error.http_status == 0) return error.kind == ApiError::Kind::not_configured || error.kind == ApiError::Kind::unauthenticated;
    return error.http_status == 400 || error.http_status == 401 || error.http_status == 403 || error.http_status == 404 ||
           error.http_status == 429;
}

}  // namespace

asio::awaitable<void> LocalPosts::publish(std::int64_t local_post_id, bool last_attempt) {
    // ---- 1. the claim: the approval, both switches and the checks, in one transaction ----
    auto claim = co_await db_.tx([&, now = clock_.now()](pqxx::work& tx) {
        return repo::claim_local_post_send(tx, local_post_id, now, options_.media_base);
    });
    if (claim.outcome != repo::LocalPostSendClaim::Outcome::claimed) {
        // Run twice, or a post that was dealt with meanwhile. A post that is
        // `sending` or `unknown` belongs to an earlier run and is never sent again from here.
        log::info("local post not sent", {{"local_post_id", local_post_id}, {"reason", claim.detail}});
        co_return;
    }

    // ---- 2. the one create --------------------------------------------------------------
    // From here on nothing throws until the bookkeeping is done: the job must not run again for this send.
    std::optional<gbp::LocalPost> created;
    std::optional<ApiError> refused;
    std::string unknown;
    try {
        auto result = co_await writer_.create_local_post(claim.gbp_account_id, claim.gbp_location_id, std::move(claim.body));
        if (result) {
            created = std::move(*result);
        } else if (definitely_refused(result.error())) {
            refused = result.error();
        } else {
            unknown = result.error().message;
        }
    } catch (const std::exception& e) {
        unknown = e.what();
    }

    // ---- 3. the bookkeeping ---------------------------------------------------------------
    std::string retry_error;
    std::string bookkeeping_failure;
    try {
        if (created) {
            co_await db_.tx([&, now = clock_.now()](pqxx::work& tx) {
                repo::local_post_send_accepted(tx, local_post_id, *created, now);
            });
            // No text in the log: ids only.
            log::info("local post sent", {{"local_post_id", local_post_id}, {"google_name", created->name}});
        } else if (refused) {
            const bool retry = google::worth_retrying(refused->kind) && !last_attempt;
            co_await db_.tx([&, now = clock_.now()](pqxx::work& tx) {
                repo::local_post_send_refused(tx, local_post_id, refused->message, retry, now);
            });
            log::warn("local post refused by Google", {{"local_post_id", local_post_id}, {"error", refused->message}, {"will_retry", retry}});
            // Nothing was created, so another attempt is safe.
            if (retry) retry_error = std::format("local post {}: Google refused the create: {}", local_post_id, refused->message);
        } else {
            co_await db_.tx([&, now = clock_.now()](pqxx::work& tx) {
                repo::local_post_send_unknown(tx, local_post_id, unknown, now);
            });
            log::warn("local post sent, outcome unknown; looking for it at Google", {{"local_post_id", local_post_id}, {"error", unknown}});
        }
    } catch (const std::exception& e) {
        bookkeeping_failure = e.what();
    }
    if (!bookkeeping_failure.empty()) {
        // The create happened (or may have) and the database could not be told.
        // The post stays `sending`, which is the safe state: the sync takes it
        // for a send of unknown outcome and settles it by reading. Not thrown:
        // the job must not run again.
        log::error("local post bookkeeping failed; the post stays as being sent", {{"local_post_id", local_post_id}, {"error", bookkeeping_failure}});
        co_return;
    }
    if (!retry_error.empty()) throw std::runtime_error(retry_error);
}

namespace {

// What one step of a removal came to.
struct Step {
    enum class Kind {
        done,     // go on
        gone,     // the post is not at Google any more
        again,    // Google could not be reached or was busy: the whole removal may run again
        refused,  // Google answered, and will not do it
    };
    Kind kind = Kind::done;
    std::string detail;
};

Step from_error(const ApiError& error) {
    if (error.kind == ApiError::Kind::not_found) return {Step::Kind::gone, error.message};
    const bool again = google::worth_retrying(error.kind) || error.kind == ApiError::Kind::unavailable || error.kind == ApiError::Kind::invalid_response ||
                       error.http_status == 408 || error.http_status >= 500;
    return {again ? Step::Kind::again : Step::Kind::refused, error.message};
}

}  // namespace

asio::awaitable<void> LocalPosts::remove(std::int64_t local_post_id, bool last_attempt) {
    const auto target = co_await db_.tx([&, now = clock_.now()](pqxx::work& tx) { return repo::local_post_removal_target(tx, local_post_id, now); });
    if (!target) {
        log::info("local post not removed", {{"local_post_id", local_post_id}, {"reason", target.error()}});
        co_return;
    }
    const auto& name = *target;

    // ---- 1. the post as Google has it now ------------------------------------------------------
    // Read first, under the address the delete will use. It shows that the address is a post of this
    // location that exists, so that a NOT_FOUND afterwards is about the post and not about the address;
    // and it makes the row the last copy of what is about to be deleted.
    Step step;
    std::optional<gbp::LocalPost> last_copy;
    try {
        auto read = co_await reader_.get_local_post(name);
        if (read) {
            last_copy = std::move(*read);
        } else {
            step = from_error(read.error());
        }
    } catch (const boost::system::system_error& e) {
        step = {Step::Kind::again, "network error: " + e.code().message()};
    } catch (const std::exception& e) {
        step = {Step::Kind::refused, e.what()};
    }

    if (step.kind == Step::Kind::gone) {
        // NOT_FOUND for a post the engine has not just seen: Google says the same for an address that
        // names no location of the account. Only a list of the location, read under the same ids, that
        // answers and does not hold the post makes it "gone".
        const auto listing = co_await list_all(reader_, name.account_id, name.location_id);
        if (!listing.error.empty()) {
            step = {Step::Kind::again, std::format("Google does not find the post, and the location's posts could not be read to confirm that it is gone: {}", listing.error)};
        } else {
            const bool listed = std::ranges::any_of(listing.posts, [&](const gbp::LocalPost& post) {
                const auto listed_name = gbp::parse_local_post_name(post.name);
                return listed_name && listed_name->post_id == name.post_id;
            });
            if (listed) step = {Step::Kind::refused, "Google does not find the post under its address, but still lists it for the location"};
        }
    } else if (last_copy) {
        co_await db_.tx([&, now = clock_.now()](pqxx::work& tx) {
            repo::apply_local_post_before_removal(tx, local_post_id, *last_copy, now);
        });

        // ---- 2. one DELETE of that post -----------------------------------------------------------
        try {
            const auto result = co_await writer_.delete_local_post(name);
            // Deleted, or deleted by someone else since the read a moment ago.
            step = result ? Step{Step::Kind::gone, {}} : from_error(result.error());
        } catch (const boost::system::system_error& e) {
            step = {Step::Kind::again, "network error: " + e.code().message()};
        } catch (const std::exception& e) {
            // The outbound client would not send it: not an address of a local post.
            step = {Step::Kind::refused, e.what()};
        }
    }

    // ---- 3. the bookkeeping -----------------------------------------------------------------------
    // A delete is safe to repeat, so anything that left it open runs again while attempts are left.
    if (step.kind == Step::Kind::again && !last_attempt) {
        throw std::runtime_error(std::format("local post {}: the delete could not be carried out: {}", local_post_id, step.detail));
    }
    co_await db_.tx([&, now = clock_.now()](pqxx::work& tx) {
        if (step.kind == Step::Kind::gone) {
            repo::local_post_removed(tx, local_post_id, now);
        } else {
            repo::local_post_removal_failed(tx, local_post_id, step.detail, step.kind == Step::Kind::again, now);
        }
    });
    if (step.kind == Step::Kind::gone) {
        log::info("local post removed", {{"local_post_id", local_post_id}});
    } else {
        log::warn("local post could not be removed", {{"local_post_id", local_post_id}, {"error", step.detail}});
    }
}

asio::awaitable<void> LocalPosts::edit(std::int64_t local_post_id, bool last_attempt) {
    // ---- 1. the claim: the approval, the post, both switches and the checks, in one transaction ----
    const auto claim = co_await db_.tx([&, now = clock_.now()](pqxx::work& tx) {
        return repo::claim_local_post_edit(tx, local_post_id, now, options_.media_base);
    });
    if (claim.outcome != repo::LocalPostEditClaim::Outcome::claimed) {
        // Run twice, an edit that was dealt with meanwhile, or one that is `sending` already: that one
        // belongs to an earlier run and is never sent from here.
        log::info("local post edit not sent", {{"local_post_id", local_post_id}, {"reason", claim.detail}});
        co_return;
    }

    // From here on nothing throws until the bookkeeping is done.
    //
    // ---- 2. the post as Google has it now, and what the edit changes against it ------------------
    // Read under the address the PATCH will use: the location's own ids as they are now and the post's
    // id. Until the PATCH itself starts (`sent`), nothing has gone to Google that could change anything.
    bool sent = false;
    bool settled = false;                   // dealt with before anything was sent: done already, or given back
    std::optional<Step> unread;             // the post could not be read
    std::string failed_here;                // the engine itself failed before the request
    std::optional<gbp::LocalPost> updated;  // Google's answer to the PATCH
    std::optional<ApiError> refused;        // a certain refusal of the PATCH
    std::string unknown;                    // anything else about the PATCH
    repo::LocalPostEditRequest request;
    try {
        auto read = co_await reader_.get_local_post(claim.name);
        if (!read) {
            unread = from_error(read.error());
        } else {
            request = co_await db_.tx([&, now = clock_.now()](pqxx::work& tx) {
                return repo::prepare_local_post_edit(tx, local_post_id, *read, now, options_.media_base);
            });
            if (request.outcome != repo::LocalPostEditRequest::Outcome::send) {
                settled = true;
            } else {
                // ---- 3. the one PATCH ------------------------------------------------------------------
                sent = true;
                auto result = co_await writer_.update_local_post(claim.name, request.mask, std::move(request.body));
                const auto answered = result ? gbp::parse_local_post_name(result->name) : std::nullopt;
                if (result && answered && answered->location_id == claim.name.location_id && answered->post_id == claim.name.post_id) {
                    updated = std::move(*result);
                } else if (result) {
                    // A 2xx about some other post is no answer about this one.
                    unknown = "Google's answer names another post";
                } else if (definitely_refused(result.error())) {
                    refused = result.error();
                } else {
                    unknown = result.error().message;
                }
            }
        }
    } catch (const boost::system::system_error& e) {
        if (sent) {
            unknown = "network error: " + e.code().message();
        } else {
            unread = Step{Step::Kind::again, "network error: " + e.code().message()};
        }
    } catch (const std::exception& e) {
        if (sent) {
            unknown = e.what();
        } else {
            // The engine's own failure (the database, say): its words are for the log, not for the post's page.
            log::error("local post edit could not be prepared", {{"local_post_id", local_post_id}, {"error", e.what()}});
            failed_here = "an error of its own";
        }
    }
    if (settled) {
        log::info("local post edit not sent", {{"local_post_id", local_post_id}, {"reason", request.detail}});
        co_return;
    }

    // ---- 4. the bookkeeping -----------------------------------------------------------------------
    std::string retry_error;
    std::string bookkeeping_failure;
    try {
        if (updated) {
            co_await db_.tx([&, now = clock_.now()](pqxx::work& tx) {
                repo::local_post_edit_accepted(tx, local_post_id, request.content, request.mask, *updated, now);
            });
            // No text in the log: ids and field names only.
            log::info("local post edited", {{"local_post_id", local_post_id}, {"fields", static_cast<std::int64_t>(request.mask.size())}});
        } else if (unread) {
            // Nothing was sent. A read is safe to repeat, so anything but a plain "no" runs again while attempts are left.
            const bool retry = unread->kind == Step::Kind::again && !last_attempt;
            const auto failure = unread->kind == Step::Kind::gone ? repo::LocalPostEditFailure::gone : repo::LocalPostEditFailure::unreadable;
            co_await db_.tx([&, now = clock_.now()](pqxx::work& tx) {
                repo::local_post_edit_not_made(tx, local_post_id, failure, unread->detail, retry, now);
            });
            log::warn("local post edit not sent: the post could not be read", {{"local_post_id", local_post_id}, {"error", unread->detail}, {"will_retry", retry}});
            if (retry) retry_error = std::format("local post {}: the post could not be read before its edit: {}", local_post_id, unread->detail);
        } else if (!failed_here.empty()) {
            // Nothing was sent either, and trying again is as safe.
            const bool retry = !last_attempt;
            co_await db_.tx([&, now = clock_.now()](pqxx::work& tx) {
                repo::local_post_edit_not_made(tx, local_post_id, repo::LocalPostEditFailure::internal, failed_here, retry, now);
            });
            if (retry) retry_error = std::format("local post {}: its edit could not be prepared", local_post_id);
        } else if (refused) {
            const bool retry = google::worth_retrying(refused->kind) && !last_attempt;
            co_await db_.tx([&, now = clock_.now()](pqxx::work& tx) {
                repo::local_post_edit_not_made(tx, local_post_id, repo::LocalPostEditFailure::refused, refused->message, retry, now);
            });
            log::warn("local post edit refused by Google", {{"local_post_id", local_post_id}, {"error", refused->message}, {"will_retry", retry}});
            // Nothing was changed, so another attempt is safe.
            if (retry) retry_error = std::format("local post {}: Google refused the edit: {}", local_post_id, refused->message);
        } else {
            co_await db_.tx([&, now = clock_.now()](pqxx::work& tx) {
                repo::local_post_edit_unknown(tx, local_post_id, unknown, now);
            });
            log::warn("local post edit sent, outcome unknown; it is not sent again", {{"local_post_id", local_post_id}, {"error", unknown}});
        }
    } catch (const std::exception& e) {
        bookkeeping_failure = e.what();
    }
    if (!bookkeeping_failure.empty()) {
        // The PATCH happened (or may have) and the database could not be told. The edit stays `sending`,
        // which is the safe state: nothing sends it again, and once this job has ended the sweep gives it
        // back as not confirmed.
        // Not thrown: the job must not run again.
        log::error("local post edit bookkeeping failed; the edit stays as being sent", {{"local_post_id", local_post_id}, {"error", bookkeeping_failure}});
        co_return;
    }
    if (!retry_error.empty()) throw std::runtime_error(retry_error);
}

asio::awaitable<repo::LocalPostsSyncOutcome> LocalPosts::sync(std::int64_t location_id) {
    const auto location = co_await db_.tx([location_id](pqxx::work& tx) { return repo::syncable_location(tx, location_id); });
    if (!location) {
        log::info("local posts not read: the location is not synced", {{"location_id", location_id}});
        co_return repo::LocalPostsSyncOutcome{};
    }

    const auto listing = co_await list_all(reader_, location->gbp_account_id, location->gbp_location_id);
    if (!listing.error.empty()) {
        // A list that could not be read says nothing: no post is removed, no send is given up.
        co_await db_.tx([&, now = clock_.now()](pqxx::work& tx) {
            repo::record_local_posts_sync_error(tx, location_id, listing.error, now);
        });
        log::warn("local posts could not be read", {{"location_id", location_id}, {"error", listing.error}});
        throw std::runtime_error(std::format("location {}: its local posts could not be read: {}", location_id, listing.error));
    }

    auto outcome = co_await db_.tx([&, now = clock_.now()](pqxx::work& tx) {
        return repo::store_local_posts(tx, location_id, listing.posts, now);
    });

    // Posts the engine holds that the list did not show. A list that lacks a
    // post is never enough (as D-037 for reviews): each is asked for by name.
    int removed = 0;
    for (const auto& missing : outcome.missing) {
        const auto read = co_await google::read(reader_.get_local_post(missing.name));
        if (!read.error.empty()) {
            log::warn("a local post could not be read", {{"local_post_id", missing.id}, {"error", read.error}});
            continue;
        }
        const bool marked = co_await db_.tx([&, now = clock_.now()](pqxx::work& tx) {
            if (read.gone) return repo::mark_local_post_removed(tx, missing.id, now);
            repo::apply_local_post(tx, location_id, *read.value, now);
            return false;
        });
        if (marked) ++removed;
    }
    log::info("local posts read",
              {{"location_id", location_id},
               {"listed", outcome.listed},
               {"added", outcome.added},
               {"adopted", outcome.adopted},
               {"failed", outcome.failed},
               {"rejected", outcome.rejected},
               {"removed", removed}});
    co_return outcome;
}

asio::awaitable<std::size_t> LocalPosts::enqueue_due() {
    const auto now = clock_.now();
    const auto due = co_await db_.tx([before = now - kSyncInterval](pqxx::work& tx) {
        return repo::locations_due_for_local_posts_sync(tx, before);
    });
    // One job per location and tick: a job that is still running or has
    // failed is not enqueued again until the next tick.
    const auto slot = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()) / kSyncTick;
    std::size_t enqueued = 0;
    for (const auto& location : due) {
        const auto id = co_await queue_.enqueue({.kind = std::string(repo::kSyncLocalPostsKind),
                                                 .payload = boost::json::object{{"location_id", location.id}},
                                                 .run_at = std::nullopt,
                                                 .idempotency_key = std::format("{}@{}", location.id, slot),
                                                 .max_attempts = repo::kSyncLocalPostsAttempts});
        if (id) ++enqueued;
    }

    const auto released = co_await db_.tx([now, before = now - kOrphanAfter](pqxx::work& tx) {
        return repo::release_orphaned_local_post_approvals(tx, before, now);
    });
    for (const auto id : released) log::warn("approved local post returned to the drafts: its job is gone", {{"local_post_id", id}});
    const auto given_back = co_await db_.tx([now, before = now - kOrphanAfter](pqxx::work& tx) {
        return repo::release_orphaned_local_post_removals(tx, before, now);
    });
    for (const auto id : given_back) log::warn("a request to remove a local post was given back: its job is gone", {{"local_post_id", id}});
    const auto edits = co_await db_.tx([now, before = now - kOrphanAfter](pqxx::work& tx) {
        return repo::release_orphaned_local_post_edits(tx, before, now);
    });
    for (const auto id : edits) log::warn("an edit of a local post was given back: its job is gone", {{"local_post_id", id}});
    co_return enqueued;
}

void install(Registry& registry, LocalPosts& local_posts, bool google_mock) {
    registry.add(std::string(repo::kRemoveLocalPostKind), [&local_posts](const Job& job) -> asio::awaitable<void> {
        co_await local_posts.remove(job.payload_id("local_post_id"), job.last_attempt());
    });
    registry.add(std::string(repo::kPublishLocalPostKind), [&local_posts](const Job& job) -> asio::awaitable<void> {
        co_await local_posts.publish(job.payload_id("local_post_id"), job.last_attempt());
    });
    registry.add(std::string(repo::kEditLocalPostKind), [&local_posts](const Job& job) -> asio::awaitable<void> {
        co_await local_posts.edit(job.payload_id("local_post_id"), job.last_attempt());
    });
    registry.add(std::string(repo::kSyncLocalPostsKind), [&local_posts](const Job& job) -> asio::awaitable<void> {
        co_await local_posts.sync(job.payload_id("location_id"));
    });
    const Handler enqueue_due = [&local_posts](const Job&) -> asio::awaitable<void> { co_await local_posts.enqueue_due(); };
    if (google_mock) {
        registry.add(std::string(kSyncDueLocalPostsKind), enqueue_due);
    } else {
        registry.every(std::string(kSyncDueLocalPostsKind), kSyncTick, 1, enqueue_due);
    }
}

}  // namespace poni::jobs
