// Reviews: what the sync writes and what the inbox reads. Blocking calls on
// a transaction the caller owns: run them through Database::run.
#pragma once

#include "connectors/gbp/reader.hpp"
#include "core/clock.hpp"
#include "db/audit.hpp"
#include "db/failure.hpp"
#include "db/sql.hpp"
#include "domain/models.hpp"
#include "domain/reply_policy.hpp"

#include <array>
#include <boost/describe/class.hpp>
#include <boost/json/object.hpp>
#include <pqxx/pqxx>

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace poni::repo {

// ---- sync ----------------------------------------------------------------------

// Where a review was seen; stored in review_events.source.
inline constexpr std::string_view kSourcePoll = "poll";
inline constexpr std::string_view kSourceFullSync = "full_sync";
inline constexpr std::string_view kSourceNotification = "notification";
// The posting job, which reads a review just before and after it posts.
inline constexpr std::string_view kSourcePosting = "posting";

// A location whose reviews are synced: switched on (D-102) and still accessible.
struct SyncLocation {
    std::int64_t id = 0;
    std::string gbp_location_id;
    std::string gbp_account_id;
};
BOOST_DESCRIBE_STRUCT(SyncLocation, (), (id, gbp_location_id, gbp_account_id))

// Locations whose incremental poll is due. A location joins only after its
// first full sync, so the poll never has to page through a whole history.
std::vector<SyncLocation> locations_due_for_poll(pqxx::transaction_base& tx, core::TimePoint now);
std::vector<SyncLocation> locations_due_for_full_sync(pqxx::transaction_base& tx, core::TimePoint now);
std::optional<SyncLocation> syncable_location(pqxx::transaction_base& tx, std::int64_t id);
std::optional<SyncLocation> syncable_location_by_gbp_id(pqxx::transaction_base& tx, std::string_view gbp_location_id);

struct ApplyResult {
    bool created = false;
    // review_events kinds written for this review; empty when nothing changed.
    std::vector<std::string> events;
};

// Stores one review as Google returned it: inserts it, or updates it and
// records what changed. Applying the same payload again changes nothing.
// Nothing is returned (and nothing stored) if the payload has no usable
// timestamps.
std::optional<ApplyResult> apply_review(pqxx::transaction_base& tx, std::int64_t location_id, const gbp::Review& review,
                                        std::string_view source, core::TimePoint now);

struct PageResult {
    int created = 0;
    int changed = 0;
    int unchanged = 0;
    int skipped = 0;  // unusable payload, or a review of a location not in `locations`
    PageResult& operator+=(const PageResult& other) {
        created += other.created;
        changed += other.changed;
        unchanged += other.unchanged;
        skipped += other.skipped;
        return *this;
    }
};

// Applies a page of reviews; each goes to the location named in its resource name.
PageResult apply_reviews(pqxx::transaction_base& tx, const std::vector<SyncLocation>& locations,
                         const std::vector<gbp::Review>& reviews, std::string_view source, core::TimePoint now);

// Marks a review removed because Google answered NOT_FOUND for it. False if
// we don't hold the review or it is already marked.
bool mark_review_removed(pqxx::transaction_base& tx, std::int64_t location_id, std::string_view gbp_review_id,
                         std::string_view source, core::TimePoint now);

// Ids of the location's reviews that aren't marked removed.
std::vector<std::string> active_review_ids(pqxx::transaction_base& tx, std::int64_t location_id);

// Notes that Google just listed these reviews.
void mark_reviews_seen(pqxx::transaction_base& tx, std::int64_t location_id, const std::vector<std::string>& gbp_review_ids,
                       core::TimePoint now);

// A poll of these locations succeeded: schedules the next one and clears the error.
void finish_poll(pqxx::transaction_base& tx, const std::vector<std::int64_t>& location_ids, core::TimePoint now,
                 std::chrono::seconds poll_interval);

// A full reconciliation succeeded. `detail` holds its numbers.
void finish_full_sync(pqxx::transaction_base& tx, std::int64_t location_id, const boost::json::object& detail,
                      core::TimePoint now, std::chrono::seconds next_in);

// A sync of these locations failed. With `reschedule_poll` the next poll is
// pushed out by the normal interval, so a failing API isn't hammered.
void record_sync_error(pqxx::transaction_base& tx, const std::vector<std::int64_t>& location_ids, std::string_view error,
                       core::TimePoint now, std::chrono::seconds poll_interval, bool reschedule_poll);

// ---- inbox ---------------------------------------------------------------------

// What the inbox can be filtered and sorted by. The API checks a request
// against these lists before a value reaches a query.
inline constexpr std::array<std::string_view, 3> kReviewStatuses{"active", "removed", "all"};
inline constexpr std::array<std::string_view, 3> kDraftRoutes{domain::kRouteAutoReply, domain::kRouteApproval, domain::kRouteEscalate};
inline constexpr std::array<std::string_view, 9> kDraftStatuses{"draft",  "scheduled", "approved",  "posting",   "posted",
                                                                "published", "rejected",  "cancelled", "superseded"};
inline constexpr std::array<std::string_view, 4> kReviewSorts{"created_time", "updated_time", "rating", "draft_time"};

struct ReviewFilter {
    std::optional<std::int64_t> location_id;
    std::vector<int> ratings;  // empty: any
    std::string status = "active";  // one of kReviewStatuses
    std::string language;           // empty: any; "none": reviews without text
    std::optional<bool> has_reply;
    std::string q;                  // part of the comment or the reviewer's name
    // By the review's newest analysis and newest draft:
    std::optional<bool> analyzed;
    std::string sentiment;          // one of domain::kSentiments; empty: any
    std::optional<bool> flagged;    // a severity flag was raised
    std::optional<bool> manipulation;
    std::optional<bool> ignored;     // a person said it needs no reply
    std::optional<bool> has_draft;
    std::string route;              // one of kDraftRoutes; empty: any
    std::string draft_status;       // one of kDraftStatuses; empty: any
    std::string sort = "created_time";  // one of kReviewSorts
    bool descending = true;
    db::Page page;
};

model::ReviewList list_reviews(pqxx::transaction_base& tx, const ReviewFilter& filter);
std::optional<model::Review> get_review(pqxx::transaction_base& tx, std::int64_t id);
// The reviews with these ids that exist, each as get_review gives it, in no particular order.
std::vector<model::Review> get_reviews(pqxx::transaction_base& tx, const std::vector<std::int64_t>& ids);
model::ReviewEventList list_review_events(pqxx::transaction_base& tx, std::int64_t review_id);

// ---- "no reply needed" (D-068) -------------------------------------------------

// Records a person's decision that the review needs no reply and cancels
// its waiting draft. Refused for a review that was removed, has a reply, or
// has a reply approved or on its way. Doing it again changes nothing.
Result<model::Review> ignore_review(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id, const std::string& reason);
// Takes the decision back; the pipeline drafts for the review again.
Result<model::Review> unignore_review(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id);

}  // namespace poni::repo
