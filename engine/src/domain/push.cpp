#include "domain/push.hpp"

#include "core/crypto.hpp"
#include "core/text.hpp"

#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>

#include <algorithm>
#include <format>
#include <ranges>

namespace poni::domain {

std::string push_payload(const PushNotice& notice) {
    const bool escalation = notice.event_type == "escalation";
    boost::json::object aps{
        {"alert", boost::json::object{{"title", escalation ? "New escalation" : "System problem"},
                                      {"body", text::truncate(notice.subject, kPushLineLength)}}},
        {"sound", "default"},
        {"category", escalation ? "ESCALATION" : "SYSTEM"},
        // Alerts of a kind are grouped together on the phone.
        {"thread-id", escalation ? "escalation" : "system"},
    };
    boost::json::object out{{"aps", std::move(aps)}, {"event_type", notice.event_type}};
    if (notice.escalation_id) out["escalation_id"] = *notice.escalation_id;
    if (notice.review_id) out["review_id"] = *notice.review_id;
    if (notice.location_id) out["location_id"] = *notice.location_id;
    if (notice.local_post_id) out["local_post_id"] = *notice.local_post_id;
    return boost::json::serialize(out);
}

std::string push_collapse_id(std::string_view event_key) { return core::sha256_hex(event_key); }

bool is_device_token(std::string_view token) {
    return token.size() >= 64 && token.size() <= 200 && token.size() % 2 == 0 &&
           std::ranges::all_of(token, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

PushOutcome push_outcome(const std::vector<PushAnswer>& answers, bool last_attempt) {
    using Kind = PushAnswer::Kind;
    const auto total = static_cast<long>(answers.size());
    const auto of = [&](Kind kind) { return std::ranges::count(answers, kind, &PushAnswer::kind); };
    const auto accepted = of(Kind::accepted);
    const auto gone = of(Kind::device_gone);

    PushOutcome out;
    out.summary = std::format("reached {} of {} device(s)", accepted, total);
    if (gone > 0) out.summary += std::format("; {} no longer registered and dropped", gone);
    // Apple's word for the answers that were neither a success nor a device that is gone.
    const auto other = std::ranges::find_if(answers | std::views::reverse,
                                            [](const PushAnswer& a) { return a.kind != Kind::accepted && a.kind != Kind::device_gone; });
    if (other != (answers | std::views::reverse).end()) out.summary += "; Apple answered " + other->reason;
    if (const auto refused = std::ranges::find(answers, Kind::sender_refused, &PushAnswer::kind); refused != answers.end()) {
        out.sender_problem = refused->reason;
    }

    if (of(Kind::retry) > 0 && !last_attempt) {
        out.status = "pending";
    } else if (accepted > 0) {
        out.status = "sent";
    } else if (gone == total) {
        out.status = "not_sent";
    } else {
        out.status = "failed";
    }
    return out;
}

}  // namespace poni::domain
