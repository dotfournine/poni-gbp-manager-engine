#include "jobs/sync.hpp"

#include "core/json.hpp"
#include "core/log.hpp"
#include "db/repo_auth.hpp"
#include "db/repo_catalog.hpp"
#include "jobs/analysis.hpp"
#include "jobs/scheduler.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <set>
#include <stdexcept>

namespace poni::jobs {

namespace asio = boost::asio;
using google::ApiError;
using google::Result;

namespace {

// One batchGetReviews call names at most this many locations.
constexpr std::size_t kBatchLocations = 20;
// A poll that keeps finding new reviews stops here; the full sync covers the rest.
constexpr int kMaxPollPages = 20;
// 50 reviews a page: a location with more than 50,000 reviews is not expected.
constexpr int kMaxFullSyncPages = 1000;
// Reviews missing from a full listing are checked one by one, this many per run.
constexpr std::size_t kMaxMissingChecks = 100;
constexpr int kPullBatch = 100;
constexpr int kMaxPullRounds = 5;

std::vector<std::int64_t> ids_of(const std::vector<repo::SyncLocation>& locations) {
    std::vector<std::int64_t> ids;
    for (const auto& location : locations) ids.push_back(location.id);
    return ids;
}

}  // namespace

asio::awaitable<void> Sync::discover_locations() {
    std::vector<repo::SeenLocation> seen;
    std::optional<std::string> error;

    const auto accounts = co_await google::guarded(reader_.list_accounts());
    if (!accounts) {
        error = accounts.error().message;
    } else {
        for (const auto& account : *accounts) {
            const auto account_id = gbp::strip_prefix(account.name, "accounts/");
            if (!account_id) continue;
            auto locations = co_await google::guarded(reader_.list_locations(*account_id));
            if (!locations) {
                // A partial listing would count the rest as missing: stop here.
                error = locations.error().message;
                break;
            }
            for (auto& location : *locations) seen.push_back({*account_id, std::move(location)});
        }
    }

    const auto now = clock_.now();
    if (error) {
        co_await db_.tx([error = *error, now](pqxx::work& tx) {
            repo::mark_integration_error(tx, repo::kDiscoveryStatus, error, now);
        });
        log::warn("location discovery failed", {{"error", *error}});
        co_return;
    }

    const auto account_count = accounts->size();
    const auto outcome = co_await db_.tx([seen = std::move(seen), now, account_count](pqxx::work& tx) {
        const auto result = repo::apply_discovery(tx, seen, now);
        repo::mark_integration_ok(tx, repo::kDiscoveryStatus, now,
                                  {{"accounts", account_count}, {"locations", result.seen}});
        return result;
    });
    log::info("locations discovered", {{"accounts", account_count},
                                       {"locations", outcome.seen},
                                       {"new", outcome.created},
                                       {"restored", outcome.restored},
                                       {"lost", outcome.lost}});
}

asio::awaitable<void> Sync::poll_reviews() {
    const auto now = clock_.now();
    auto due = co_await db_.tx([now](pqxx::work& tx) {
        return std::pair{repo::locations_due_for_full_sync(tx, now), repo::locations_due_for_poll(tx, now)};
    });

    // One job per location and per interval slot: a job that is still running
    // or has failed isn't enqueued again until the next slot.
    const auto slot = std::chrono::duration_cast<std::chrono::seconds>(
                          slot_start(now, options_.full_sync_interval).time_since_epoch())
                          .count();
    for (const auto& location : due.first) {
        co_await queue_.enqueue({.kind = std::string(kFullSyncKind),
                                 .payload = boost::json::object{{"location_id", location.id}},
                                 .run_at = std::nullopt,
                                 .idempotency_key = std::format("{}@{}", location.id, slot)});
    }

    std::map<std::string, std::vector<repo::SyncLocation>> by_account;
    for (auto& location : due.second) by_account[location.gbp_account_id].push_back(std::move(location));
    for (auto& [account_id, locations] : by_account) {
        for (std::size_t start = 0; start < locations.size(); start += kBatchLocations) {
            const auto end = std::min(start + kBatchLocations, locations.size());
            co_await poll_batch({locations.begin() + static_cast<std::ptrdiff_t>(start),
                                 locations.begin() + static_cast<std::ptrdiff_t>(end)});
        }
    }
}

asio::awaitable<void> Sync::reviews_changed() { co_await queue_.enqueue({.kind = std::string(kAnalyzePendingKind), .max_attempts = 1}); }

asio::awaitable<void> Sync::poll_batch(std::vector<repo::SyncLocation> locations) {
    const auto now = clock_.now();
    const auto& account_id = locations.front().gbp_account_id;
    std::vector<std::string> gbp_ids;
    for (const auto& location : locations) gbp_ids.push_back(location.gbp_location_id);

    repo::PageResult total;
    std::optional<std::string> error;
    std::optional<std::string> page_token;
    for (int pages = 0; pages < kMaxPollPages; ++pages) {
        auto page = co_await google::guarded(reader_.batch_get_reviews(account_id, gbp_ids, page_token));
        if (!page) {
            error = page.error().message;
            break;
        }
        const auto result = co_await db_.tx([&locations, reviews = std::move(page->reviews), now](pqxx::work& tx) {
            return repo::apply_reviews(tx, locations, reviews, repo::kSourcePoll, now);
        });
        total += result;
        total.skipped += static_cast<int>(page->skipped);
        page_token = std::move(page->next_page_token);
        // Newest first: a page with nothing new means the older pages hold nothing new either.
        if (!page_token || result.created + result.changed == 0) break;
    }

    const auto ids = ids_of(locations);
    if (error) {
        co_await db_.tx([ids, error = *error, now](pqxx::work& tx) {
            repo::record_sync_error(tx, ids, error, now, true);
        });
        log::warn("review poll failed", {{"locations", ids.size()}, {"error", *error}});
        co_return;
    }
    co_await db_.tx([ids, now](pqxx::work& tx) {
        repo::finish_poll(tx, ids, now);
    });
    const boost::json::object fields{{"locations", ids.size()},
                                     {"new", total.created},
                                     {"changed", total.changed},
                                     {"unchanged", total.unchanged},
                                     {"skipped", total.skipped}};
    if (total.created + total.changed > 0) co_await reviews_changed();
    if (total.created + total.changed + total.skipped > 0) {
        log::info("reviews polled", fields);
    } else {
        log::debug("reviews polled", fields);
    }
}

asio::awaitable<void> Sync::full_sync(std::int64_t location_id) {
    const auto location = co_await db_.tx([location_id](pqxx::work& tx) { return repo::syncable_location(tx, location_id); });
    if (!location) {
        log::debug("full review sync skipped: the location is not synced", {{"location_id", location_id}});
        co_return;
    }
    const auto now = clock_.now();
    const std::vector<repo::SyncLocation> only{*location};

    // Failing the job (to have it retried) also shows the error on the status page.
    const auto fail = [&](std::string error) -> asio::awaitable<void> {
        co_await db_.tx([location_id, error, now](pqxx::work& tx) {
            repo::record_sync_error(tx, {location_id}, error, now, false);
        });
        throw std::runtime_error(std::format("full review sync of location {}: {}", location_id, error));
    };

    repo::PageResult total;
    std::set<std::string> listed;
    std::optional<std::int64_t> google_total;
    std::optional<std::string> page_token;
    for (int pages = 0;; ++pages) {
        if (pages >= kMaxFullSyncPages) co_await fail("the review list did not end");
        auto page = co_await google::guarded(reader_.list_reviews(location->gbp_account_id, location->gbp_location_id, page_token));
        if (!page) co_await fail(page.error().message);

        std::vector<std::string> ids;
        for (const auto& review : page->reviews) ids.push_back(review.reviewId);
        const auto result = co_await db_.tx([&only, &ids, reviews = std::move(page->reviews), location_id, now](pqxx::work& tx) {
            auto applied = repo::apply_reviews(tx, only, reviews, repo::kSourceFullSync, now);
            repo::mark_reviews_seen(tx, location_id, ids, now);
            return applied;
        });
        total += result;
        total.skipped += static_cast<int>(page->skipped);
        listed.insert(ids.begin(), ids.end());
        if (page->total_review_count) google_total = page->total_review_count;
        page_token = std::move(page->next_page_token);
        if (!page_token) break;
    }

    // Completeness against our own store: every review we hold that Google
    // didn't list is asked for by name. Gone means removed; still there means
    // the list was incomplete, and the review is refreshed.
    const auto held = co_await db_.tx([location_id](pqxx::work& tx) { return repo::active_review_ids(tx, location_id); });
    std::vector<std::string> missing;
    for (const auto& id : held) {
        if (!listed.contains(id)) missing.push_back(id);
    }
    int removed = 0;
    int still_there = 0;
    int unchecked = 0;
    for (std::size_t i = 0; i < missing.size(); ++i) {
        if (i >= kMaxMissingChecks) {
            unchecked += static_cast<int>(missing.size() - i);
            break;
        }
        const auto& review_id = missing[i];
        const auto review =
            co_await google::guarded(reader_.get_review({location->gbp_account_id, location->gbp_location_id, review_id}));
        if (review) {
            co_await db_.tx([&review, location_id, now](pqxx::work& tx) {
                repo::apply_review(tx, location_id, *review, repo::kSourceFullSync, now);
            });
            ++still_there;
        } else if (review.error().kind == ApiError::Kind::not_found) {
            co_await db_.tx([&review_id, location_id, now](pqxx::work& tx) {
                repo::mark_review_removed(tx, location_id, review_id, repo::kSourceFullSync, now);
            });
            ++removed;
        } else {
            ++unchecked;
        }
    }

    boost::json::object detail{{"listed", listed.size()},
                               {"new", total.created},
                               {"changed", total.changed},
                               {"skipped", total.skipped},
                               {"missing_from_list", missing.size()},
                               {"removed", removed},
                               {"still_there", still_there},
                               {"unchecked", unchecked}};
    if (google_total) detail["google_total_review_count"] = *google_total;
    co_await db_.tx([location_id, detail, now, next = options_.full_sync_interval](pqxx::work& tx) {
        repo::finish_full_sync(tx, location_id, detail, now, next);
    });
    if (total.created + total.changed + removed > 0) co_await reviews_changed();
    detail["location_id"] = location_id;
    log::info("full review sync done", detail);
}

asio::awaitable<void> Sync::ingest_notification(std::string review_name) {
    const auto name = gbp::parse_review_name(review_name);
    if (!name) {
        log::warn("notification names no review", {{"name", review_name}});
        co_return;
    }
    const auto location = co_await db_.tx([gbp_id = name->location_id](pqxx::work& tx) { return repo::syncable_location_by_gbp_id(tx, gbp_id); });
    if (!location) {
        log::debug("notification for a location that is not synced", {{"gbp_location_id", name->location_id}});
        co_return;
    }

    const auto now = clock_.now();
    const auto review = co_await reader_.get_review(*name);
    if (review) {
        const auto result = co_await db_.tx([&review, location_id = location->id, now](pqxx::work& tx) {
            return repo::apply_review(tx, location_id, *review, repo::kSourceNotification, now);
        });
        if (result && (result->created || !result->events.empty())) co_await reviews_changed();
        log::info("notified review stored", {{"location_id", location->id},
                                             {"new", result && result->created},
                                             {"changed", result && !result->events.empty()}});
        co_return;
    }
    if (review.error().kind == ApiError::Kind::not_found) {
        // Notified, then gone: deleted by its author or taken down by Google (spike B7).
        const bool marked = co_await db_.tx([location_id = location->id, review_id = name->review_id, now](pqxx::work& tx) {
            return repo::mark_review_removed(tx, location_id, review_id, repo::kSourceNotification, now);
        });
        log::info("notified review is gone", {{"location_id", location->id}, {"marked_removed", marked}});
        co_return;
    }
    throw std::runtime_error(std::format("notified review could not be fetched: {}", review.error().message));
}

asio::awaitable<void> Sync::pull_notifications(pubsub::Subscriber& subscriber) {
    std::optional<std::string> error;
    std::size_t received = 0;
    std::size_t enqueued = 0;

    for (int round = 0; round < kMaxPullRounds && !error; ++round) {
        const auto messages = co_await google::guarded(subscriber.pull(kPullBatch));
        if (!messages) {
            error = messages.error().message;
            break;
        }
        if (messages->empty()) break;

        std::vector<std::string> ack_ids;
        for (const auto& message : *messages) {
            ack_ids.push_back(message.ack_id);
            const auto notification = core::parse_json<pubsub::GbpNotification>(message.data);
            const bool about_review = notification && notification->review &&
                                      (notification->type == "NEW_REVIEW" || notification->type == "UPDATED_REVIEW");
            if (!about_review) {
                // GOOGLE_UPDATE and the like belong to profile accuracy, after the MVP.
                log::debug("notification ignored", {{"type", notification ? notification->type : "unreadable"}});
                continue;
            }
            // Enqueue before acknowledging: a crash in between redelivers the
            // message, and the message id as idempotency key absorbs the repeat.
            const auto id = co_await queue_.enqueue(
                {.kind = std::string(kIngestNotificationKind),
                 .payload = boost::json::object{{"review", *notification->review},
                                                {"type", notification->type},
                                                {"published_at", message.publish_time}},
                 .run_at = std::nullopt,
                 .idempotency_key = message.message_id});
            if (id) ++enqueued;
        }
        const auto acknowledged = co_await google::guarded(subscriber.acknowledge(std::move(ack_ids)));
        if (!acknowledged) error = acknowledged.error().message;
        received += messages->size();
        if (messages->size() < static_cast<std::size_t>(kPullBatch)) break;
    }

    const auto now = clock_.now();
    co_await db_.tx([error, received, now](pqxx::work& tx) {
        boost::json::object detail;
        if (received > 0) detail["last_message_at"] = std::format("{:%FT%T}Z", now);
        repo::record_integration(tx, repo::kPubsubStatus, error, now, detail);
    });
    if (error) {
        log::warn("notification pull failed", {{"error", *error}});
    } else if (received > 0) {
        log::info("notifications pulled", {{"received", received}, {"enqueued", enqueued}});
    }
}

void install(Registry& registry, Sync& sync, pubsub::Subscriber* subscriber) {
    const auto& options = sync.options();
    registry.every(std::string(repo::kDiscoverLocationsKind), options.discovery_interval, 1,
                   [&sync](const Job&) -> asio::awaitable<void> { co_await sync.discover_locations(); });
    registry.every(std::string(kPollReviewsKind), options.poll_tick, 1,
                   [&sync](const Job&) -> asio::awaitable<void> { co_await sync.poll_reviews(); });
    registry.add(std::string(kFullSyncKind), [&sync](const Job& job) -> asio::awaitable<void> { co_await sync.full_sync(job.payload_id("location_id")); });
    registry.add(std::string(kIngestNotificationKind), [&sync](const Job& job) -> asio::awaitable<void> {
        co_await sync.ingest_notification(std::string(job.payload.at("review").as_string()));
    });
    if (subscriber) {
        registry.every(std::string(kPullNotificationsKind), options.notification_pull_interval, 1,
                       [&sync, subscriber](const Job&) -> asio::awaitable<void> { co_await sync.pull_notifications(*subscriber); });
    }
}

}  // namespace poni::jobs
