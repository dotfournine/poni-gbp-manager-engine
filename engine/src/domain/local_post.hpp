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
#include <string_view>
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
    // The content is an edit of a post that is at Google (D-099). Its time to
    // publish at and its repeat are the post's own, which an edit does not
    // send and cannot change, so scheduled_in_future and recurrence_end pass:
    // a post that was scheduled and is live has a time in the past. (The
    // caller gives a media_problem only for an upload that is not the post's.)
    bool edit_at_google = false;
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

// ---- an edit of a post that is at Google (D-099) ------------------------------------------

// The fields of Google's LocalPost that an edit may change, by Google's
// names, in the order a mask lists them. Nothing else is ever named in the
// updateMask of the one PATCH the engine sends; the outbound client holds
// the same six (http::kLocalPostUpdateFields).
inline constexpr std::array<std::string_view, 6> kLocalPostEditFields{"summary", "callToAction", "event",
                                                                      "offer",   "media",        "languageCode"};
// The same six, in the same order, by the API's names: what LocalPostEdit.fields lists.
inline constexpr std::array<std::string_view, 6> kLocalPostEditMembers{"summary", "call_to_action", "event",
                                                                       "offer",   "media",          "language_code"};
// The API's name of a field given by Google's ("callToAction" -> "call_to_action"); empty for a name that is none of the six.
std::string_view edit_member(std::string_view field);

// Why there is no mask for an edit.
struct EditObstacle {
    enum class Kind {
        // The post, as the engine holds it, is not content a draft can hold
        // (an alert, a button outside the six, an event without its dates):
        // an edit, which is such content, could not say how to keep that
        // part, so the post is not edited here at all.
        not_editable,
        // The edit differs from the post in a part an edit does not change.
        // `member` names it: "topic_type", "scheduled_time" or "event.recurrence".
        fixed_part,
        // Only from local_post_edit_request: the post changed at Google since
        // the edit was written, in a field the edit would write over.
        // `member` names it by the API's name.
        changed_at_google,
    };
    Kind kind = Kind::not_editable;
    std::string member;
    // A sentence for the person who asked.
    std::string message;
};

// What an edit changes: the fields, out of kLocalPostEditFields and in that
// order, in which `edit` differs from `post`, the post's content as the
// engine held it when the edit was written (what Google returned, and in
// `media_id` the upload the engine knows the post carries). `edit` has been
// through normalize_local_post. The two are compared as a draft would hold
// them, so space around a text is no difference. An empty mask: the edit
// says what the post says.
//
// "media" is named only when the two name different uploads: an edit with
// an upload other than the post's, or with none where the post has one. A
// post whose photo is Google's own copy has no upload, and an edit without
// one leaves it alone.
//
// The kind of post, the time Google publishes it at and its repeat are not
// among the six: an edit that differs there has no mask (fixed_part), and
// neither has an edit of a post the engine cannot hold as a draft
// (not_editable), so that no mask ever names a field whose content at
// Google the engine could not write back.
std::expected<std::vector<std::string_view>, EditObstacle> local_post_edit_mask(const LocalPostContent& post, const LocalPostContent& edit);

// The post as it would say with the edit applied now: `now`, the post's
// content as the engine holds it now, with the fields the edit changes
// (local_post_edit_mask(base, edit)) laid over it, and nothing else of the
// edit. So a field the edit's author did not change always says what the
// post says now, whatever it said when the edit was written, and the kind,
// the time and the repeat are the post's. This is what the API shows as the
// edit's content and what a person approves.
LocalPostContent local_post_with_edit(const LocalPostContent& base, const LocalPostContent& edit, const LocalPostContent& now);

// What an approved edit sends: the mask of the one PATCH. `base` is the
// post's content the edit was written against, `at_google` the post as
// Google has it now. An edit never writes a field its author did not change,
// so only the fields of local_post_edit_mask(base, edit) are looked at, and
// for each of them:
//   - Google has the edit's value already: not named, there is nothing to change;
//   - Google's value is neither the edit's nor the base's: that same field
//     was changed at Google since the edit was written, and the edit would
//     write over a change its author never saw. No mask (changed_at_google);
//   - otherwise it is named.
// A field the edit does not change is never named, whatever Google holds
// now. An empty mask: Google has everything the edit changes. The post must
// still be content a draft can hold (not_editable).
std::expected<std::vector<std::string_view>, EditObstacle> local_post_edit_request(const LocalPostContent& base, const LocalPostContent& edit,
                                                                                 const LocalPostContent& at_google);

// Of `fields` (Google's names, a mask that was sent), those in which
// `answer`, the post as Google returned it afterwards, does not say what
// `sent` says. Each field is compared on its own, so nothing else about the
// answer counts; "media" is left out, because Google answers with addresses
// of its own, which say nothing about which file they came from.
std::vector<std::string_view> edit_fields_not_shown(const LocalPostContent& sent, const std::vector<std::string_view>& fields,
                                                    const LocalPostContent& answer);

}  // namespace poni::domain
