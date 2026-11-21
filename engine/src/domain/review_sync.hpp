// What changed between the review we hold and the review Google just sent.
// Pure logic, so the sync's decisions can be tested without a database.
#pragma once

#include <boost/json/object.hpp>

#include <optional>
#include <string>
#include <vector>

namespace poni::domain {

// The parts of a review the sync compares. Timestamps are canonical
// (domain::canonical_timestamp).
struct ReviewSnapshot {
    std::optional<int> rating;
    std::optional<std::string> comment;
    std::string updated_time;
    std::optional<std::string> reply_text;
    std::optional<std::string> reply_updated_time;
    bool removed = false;
};

struct ReviewEvent {
    // One of review_events.kind: restored, review_edited, reply_added,
    // reply_changed, reply_removed.
    std::string kind;
    boost::json::object detail;
};

// Events for moving from `stored` to `incoming`; empty when nothing changed,
// which is what makes re-running a sync harmless. `incoming.removed` is
// ignored: Google returned the review, so it exists.
std::vector<ReviewEvent> diff_review(const ReviewSnapshot& stored, const ReviewSnapshot& incoming);

}  // namespace poni::domain
