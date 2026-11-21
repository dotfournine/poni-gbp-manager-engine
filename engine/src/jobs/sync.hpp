// The read-only mirror: location discovery and review sync.
//
// Reviews reach the store by three paths that all end in repo::apply_review,
// so running any of them twice changes nothing:
//
//   poll           every minute: one batchGetReviews call per account covers
//                  every location whose poll is due, newest first, and stops
//                  at the first page that holds nothing new. The main path.
//   full sync      per location, on linking and then every hour or so: lists
//                  every review, then checks our store against the list.
//                  Catches what the poll can't see (a reply added outside the
//                  system doesn't move a review's updateTime) and reviews
//                  that vanished.
//   notification   a Pub/Sub message names a review; it is fetched and stored.
//                  A second source: the first notification measured took 15
//                  minutes (spike B7).
//
// A review is marked removed only when Google answers NOT_FOUND for that
// review; a list that merely lacks it is never enough (and Google's
// totalReviewCount is not trusted either, spike B4).
#pragma once

#include "connectors/gbp/reader.hpp"
#include "connectors/pubsub/subscriber.hpp"
#include "core/clock.hpp"
#include "db/database.hpp"
#include "db/repo_catalog.hpp"
#include "db/repo_reviews.hpp"
#include "jobs/queue.hpp"
#include "jobs/registry.hpp"

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace poni::jobs {

inline constexpr std::string_view kPollReviewsKind = "reviews.poll";
inline constexpr std::string_view kFullSyncKind = "reviews.full_sync";
inline constexpr std::string_view kIngestNotificationKind = "reviews.ingest_notification";
inline constexpr std::string_view kPullNotificationsKind = "notifications.pull";

struct SyncOptions {
    std::chrono::seconds discovery_interval{900};
    // How often the poll job runs. Each location has its own interval
    // (locations.review_poll_interval_s); the job polls those that are due.
    std::chrono::seconds poll_tick{60};
    std::chrono::seconds full_sync_interval{3600};
    std::chrono::seconds notification_pull_interval{30};
};

class Sync {
public:
    Sync(db::Database& db, const core::Clock& clock, gbp::Reader& reader, JobQueue& queue, SyncOptions options)
        : db_(db), clock_(clock), reader_(reader), queue_(queue), options_(options) {}

    // Lists every account and location and stores the result. Google errors
    // are recorded for the status page, not thrown: the next run retries.
    boost::asio::awaitable<void> discover_locations();

    // Enqueues full syncs that are due, then polls the locations that are due.
    boost::asio::awaitable<void> poll_reviews();

    // Throws when Google can't be read, so the job is retried with backoff.
    boost::asio::awaitable<void> full_sync(std::int64_t location_id);

    // Fetches the review a notification named. A review that is gone is marked removed.
    boost::asio::awaitable<void> ingest_notification(std::string review_name);

    // Pulls waiting notifications, enqueues one ingest job per review
    // message, then acknowledges. Errors are recorded, not thrown.
    boost::asio::awaitable<void> pull_notifications(pubsub::Subscriber& subscriber);

    const SyncOptions& options() const { return options_; }

private:
    // A sync stored a new or changed review: enqueues the pipeline's sweep
    // (reviews.analyze_pending), so the analysis doesn't wait for its next minute.
    boost::asio::awaitable<void> reviews_changed();

    // One batch: locations of one account, polled with one call per page.
    boost::asio::awaitable<void> poll_batch(std::vector<repo::SyncLocation> locations);

    db::Database& db_;
    const core::Clock& clock_;
    gbp::Reader& reader_;
    JobQueue& queue_;
    SyncOptions options_;
};

// `subscriber` may be null: Pub/Sub is then off and only polling runs.
void install(Registry& registry, Sync& sync, pubsub::Subscriber* subscriber);

}  // namespace poni::jobs
