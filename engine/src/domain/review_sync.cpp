#include "domain/review_sync.hpp"

namespace poni::domain {

namespace {

boost::json::value json(const std::optional<std::string>& s) { return s ? boost::json::value(*s) : boost::json::value(nullptr); }
boost::json::value json(const std::optional<int>& n) { return n ? boost::json::value(*n) : boost::json::value(nullptr); }

}  // namespace

std::vector<ReviewEvent> diff_review(const ReviewSnapshot& stored, const ReviewSnapshot& incoming) {
    std::vector<ReviewEvent> events;

    if (stored.removed) events.push_back({"restored", {}});

    if (stored.rating != incoming.rating || stored.comment != incoming.comment ||
        stored.updated_time != incoming.updated_time) {
        events.push_back({"review_edited",
                          {{"before", {{"rating", json(stored.rating)},
                                       {"comment", json(stored.comment)},
                                       {"updated_time", stored.updated_time}}},
                           {"after", {{"rating", json(incoming.rating)},
                                      {"comment", json(incoming.comment)},
                                      {"updated_time", incoming.updated_time}}}}});
    }

    if (!stored.reply_text && incoming.reply_text) {
        // A reply we didn't have: someone answered outside this system (FR-2).
        events.push_back({"reply_added", {{"text", *incoming.reply_text}, {"updated_time", json(incoming.reply_updated_time)}}});
    } else if (stored.reply_text && !incoming.reply_text) {
        events.push_back({"reply_removed", {{"before", *stored.reply_text}}});
    } else if (stored.reply_text && incoming.reply_text &&
               (stored.reply_text != incoming.reply_text || stored.reply_updated_time != incoming.reply_updated_time)) {
        events.push_back({"reply_changed",
                          {{"before", *stored.reply_text},
                           {"after", *incoming.reply_text},
                           {"updated_time", json(incoming.reply_updated_time)}}});
    }
    return events;
}

}  // namespace poni::domain
