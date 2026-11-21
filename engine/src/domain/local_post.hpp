// Local posts (FR-14, D-088): what a post on a Google Business Profile
// says, in the engine's spelling of Google's LocalPost. No I/O.
//
// LocalPostContent is the unit a person writes, edits and approves: the API
// takes it whole, the repository stores it, and an approval is accepted only
// for content equal to what is stored (D-088 point 3). The structs map to
// the schemas of the same names in api/openapi.yaml.
#pragma once

#include "core/clock.hpp"
#include "domain/reply_checks.hpp"

#include <array>
#include <boost/describe/class.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace poni::domain {

// The kinds of post the engine writes. Google has one more, "alert", which only it makes.
inline constexpr std::array<std::string_view, 3> kPostTopicTypes{"standard", "event", "offer"};

// The post's button.
struct LocalPostCallToAction {
    // "book", "order", "shop", "learn_more", "sign_up" or "call" in a draft. A
    // post made at Google may carry another value, in lower case.
    std::string action_type;
    // An https address; none for "call", which dials the profile's number.
    std::optional<std::string> url;
    // Sent whole, like the content it is part of (core/body.hpp): so are the structs below with a member that may be none.
    static constexpr bool sent_whole = true;

    bool operator==(const LocalPostCallToAction&) const = default;
};
BOOST_DESCRIBE_STRUCT(LocalPostCallToAction, (), (action_type, url))

// Google's TimeInterval: days as "YYYY-MM-DD" and times as "HH:MM", in the
// business's own time. No time zone travels with it.
struct LocalPostSchedule {
    std::string start_date;
    std::string start_time;
    std::string end_date;
    std::string end_time;

    bool operator==(const LocalPostSchedule&) const = default;
};
BOOST_DESCRIBE_STRUCT(LocalPostSchedule, (), (start_date, start_time, end_date, end_time))

// Google repeats the post: "daily", "weekly" (on the weekday of the event's
// start) or "monthly" (on its day of the month).
struct LocalPostRecurrence {
    std::string frequency;
    // When the series ends, a canonical timestamp. A draft needs one; a post
    // made at Google may have none.
    std::optional<std::string> until;
    static constexpr bool sent_whole = true;

    bool operator==(const LocalPostRecurrence&) const = default;
};
BOOST_DESCRIBE_STRUCT(LocalPostRecurrence, (), (frequency, until))

struct LocalPostEvent {
    std::string title;
    LocalPostSchedule schedule;
    std::optional<LocalPostRecurrence> recurrence;
    static constexpr bool sent_whole = true;

    bool operator==(const LocalPostEvent&) const = default;
};
BOOST_DESCRIBE_STRUCT(LocalPostEvent, (), (title, schedule, recurrence))

struct LocalPostOffer {
    std::optional<std::string> coupon_code;
    std::optional<std::string> redeem_online_url;
    std::optional<std::string> terms_conditions;
    static constexpr bool sent_whole = true;

    bool operator==(const LocalPostOffer&) const = default;
};
BOOST_DESCRIBE_STRUCT(LocalPostOffer, (), (coupon_code, redeem_online_url, terms_conditions))

struct LocalPostContent {
    // "standard", "event" or "offer"; "alert" only on a post made at Google.
    std::string topic_type;
    std::string summary;
    std::string language_code;
    std::optional<LocalPostCallToAction> call_to_action;
    // With "event" and "offer".
    std::optional<LocalPostEvent> event;
    // With "offer".
    std::optional<LocalPostOffer> offer;
    // The uploaded photo, a row of local_post_media.
    std::optional<std::int64_t> media_id;
    // When Google is to publish the post, a canonical timestamp; none: at once.
    std::optional<std::string> scheduled_time;

    // As a request body it travels whole, as the API answers with it: every
    // member is there, an empty one as null (core/body.hpp).
    static constexpr bool sent_whole = true;

    bool operator==(const LocalPostContent&) const = default;
};
BOOST_DESCRIBE_STRUCT(LocalPostContent, (),
                      (topic_type, summary, language_code, call_to_action, event, offer, media_id, scheduled_time))

// Google's limits, learned from its own validation errors (the spike of 2026-11-19).
inline constexpr std::size_t kLocalPostSummaryMax = 1500;
inline constexpr std::size_t kLocalPostTitleMax = 58;

// The content as the engine stores it, or why it is not a post at all (the
// API answers 400 with that sentence). Text is trimmed, an optional string
// that is empty becomes none, and timestamps become canonical. What is
// refused here is structure: a kind the engine does not write ("alert"),
// parts that do not belong to the kind (an event on a "standard" post, a
// button on an offer), a button outside Google's six, a day or a time that
// is none, a text too long to be meant. Limits that a person can still fix
// in a draft are checks, below.
std::expected<LocalPostContent, std::string> normalize_local_post(LocalPostContent content);

// A post's content fitted to what a draft can hold, and what was left out
// of it, each in words a staff member reads ("the time to publish at, which
// has passed").
struct DraftCopy {
    LocalPostContent content;
    std::vector<std::string> left_out;
};

// What a new draft takes from a post's content as the engine holds it now
// (D-094): an alert becomes an update, parts that do not belong to the kind
// go, a button outside the six goes, and a time to publish at that is not
// after `now` goes. An upload (`media_id`) is kept. `media_at_google` is the
// format ("photo" or "video") of the first file the post shows at Google:
// without an upload the copy has no media then, because Google's copy of a
// file is not an upload. The result still goes through
// normalize_local_post, which may refuse a part longer than a draft takes.
DraftCopy fit_for_draft(LocalPostContent content, core::TimePoint now, std::optional<std::string> media_at_google);

struct LocalPostCheckContext {
    core::TimePoint now;
    // Why the post's uploaded photo can't be used, if it can't: decided by
    // the caller, which knows the upload and the bucket.
    std::optional<std::string> media_problem;
};

// The checks an approval needs (D-088 point 10), on normalized content.
// Names, in the order they run:
//   summary_length, title_length, no_phone, no_email, no_handle, button_url,
//   offer_url, event_dates, recurrence_end, scheduled_in_future, language,
//   media_usable.
// A draft with a failed check is stored; it can't be approved.
CheckReport run_local_post_checks(const LocalPostContent& content, const LocalPostCheckContext& context);

// What the checks no_phone, no_email and no_handle would find in `text`, in
// words ("a phone number"); nothing if they would pass.
std::optional<std::string> contact_detail_in(std::string_view text);

// A language tag such as "en", "es-419" or "zh-Hant", as the check "language" reads it.
bool is_post_language_tag(std::string_view tag);

}  // namespace poni::domain
