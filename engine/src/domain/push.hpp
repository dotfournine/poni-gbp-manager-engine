// What a push to the phone app says, and what a delivery's answers come to
// (D-097). A lock screen is visible to others, so a push carries the event's
// subject (which never holds review text, a rule of every event) under a
// fixed title, and ids the app opens; the app fetches the rest when the
// alert is opened. The body of an event, which may quote a review, is not an
// input here at all.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace poni::domain {

struct PushNotice {
    std::string_view event_type;  // "escalation" or "system_problem"
    std::string_view subject;
    std::optional<std::int64_t> escalation_id;
    std::optional<std::int64_t> review_id;
    std::optional<std::int64_t> location_id;
    std::optional<std::int64_t> local_post_id;
};

// The longest line shown, in code points; a longer subject is cut.
inline constexpr std::size_t kPushLineLength = 178;

// The JSON Apple is given: {"aps": {alert, sound, category, thread-id},
// "event_type", and each id that is set}.
std::string push_payload(const PushNotice& notice);

// Apple's collapse id for an event (at most 64 bytes): the same event sent
// again replaces the alert already shown.
std::string push_collapse_id(std::string_view event_key);

// A device token as APNs hands it to the app: lower-case hex, 32 bytes
// today and at most 100 here, since Apple says the length may grow.
bool is_device_token(std::string_view token);

// What Apple answered to one push.
struct PushAnswer {
    enum class Kind {
        accepted,        // Apple took it; that it is seen is not known
        device_gone,     // the token will never work again: drop the device
        retry,           // worth another attempt (rate limit, Apple's 5xx, an expired provider token)
        refused,         // Apple refused this push; sending it again changes nothing
        sender_refused,  // Apple refused the key, the team or the topic: a setting is wrong
    };
    Kind kind = Kind::accepted;
    std::string reason;  // Apple's word and status, "400 BadDeviceToken"
    // The provider token ran out: the next attempt makes a new one.
    bool token_expired = false;
};

// What one attempt at a push delivery came to.
struct PushOutcome {
    // "sent" when a device took it; "pending" when an answer may pass, and
    // then every device is sent it again; "not_sent" when every device is
    // gone; "failed" otherwise.
    std::string_view status;
    std::string summary;         // how many devices it reached, and Apple's word
    std::string sender_problem;  // Apple's word when it refused the engine's settings
};

PushOutcome push_outcome(const std::vector<PushAnswer>& answers, bool last_attempt);

}  // namespace poni::domain
