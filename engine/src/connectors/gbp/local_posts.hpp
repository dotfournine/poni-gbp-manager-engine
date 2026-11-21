// Google Business Profile, local posts (FR-14, D-088): the posts a business
// publishes on its profile. They exist only in the v4 API.
//
// Two interfaces. LocalPostReader lists and reads. LocalPostWriter is the
// engine's second, third and fourth way to change a profile: it creates a
// post, deletes a post, and updates a post in the fields an approved edit
// changes (D-099), and nothing else. The writer is to be called only by the
// jobs local_posts.publish, local_posts.remove and local_posts.edit
// (CLAUDE.md).
//
// Payload structs mirror Google's JSON (camelCase names); nearly everything
// is optional because Google omits what is empty or zero: a start time of
// midnight arrives as `{}`, an offer without a code as no `offer` at all.
#pragma once

#include "connectors/google/auth.hpp"
#include "domain/local_post.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/describe/class.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace poni::gbp {

using google::ApiError;
using google::Result;

// google.type.Date and google.type.TimeOfDay.
struct Date {
    std::optional<int> year;
    std::optional<int> month;
    std::optional<int> day;
};
BOOST_DESCRIBE_STRUCT(Date, (), (year, month, day))

struct TimeOfDay {
    std::optional<int> hours;
    std::optional<int> minutes;
};
BOOST_DESCRIBE_STRUCT(TimeOfDay, (), (hours, minutes))

struct TimeInterval {
    std::optional<Date> startDate;
    std::optional<TimeOfDay> startTime;
    std::optional<Date> endDate;
    std::optional<TimeOfDay> endTime;
};
BOOST_DESCRIBE_STRUCT(TimeInterval, (), (startDate, startTime, endDate, endTime))

// One of the three patterns is set. Their contents (the weekdays of a weekly
// pattern, the day of a monthly one) are kept as Google sent them.
struct RecurrenceInfo {
    std::optional<std::string> seriesEndTime;
    std::optional<boost::json::value> dailyPattern;
    std::optional<boost::json::value> weeklyPattern;
    std::optional<boost::json::value> monthlyPattern;
};
BOOST_DESCRIBE_STRUCT(RecurrenceInfo, (), (seriesEndTime, dailyPattern, weeklyPattern, monthlyPattern))

struct PostEvent {
    std::optional<std::string> title;
    std::optional<TimeInterval> schedule;
    std::optional<RecurrenceInfo> recurrenceInfo;
};
BOOST_DESCRIBE_STRUCT(PostEvent, (), (title, schedule, recurrenceInfo))

struct CallToAction {
    std::optional<std::string> actionType;  // BOOK, ORDER, SHOP, LEARN_MORE, SIGN_UP, CALL
    std::optional<std::string> url;
};
BOOST_DESCRIBE_STRUCT(CallToAction, (), (actionType, url))

struct PostOffer {
    std::optional<std::string> couponCode;
    std::optional<std::string> redeemOnlineUrl;
    std::optional<std::string> termsConditions;
};
BOOST_DESCRIBE_STRUCT(PostOffer, (), (couponCode, redeemOnlineUrl, termsConditions))

struct PostMedia {
    std::optional<std::string> mediaFormat;  // PHOTO or VIDEO
    std::optional<std::string> googleUrl;
};
BOOST_DESCRIBE_STRUCT(PostMedia, (), (mediaFormat, googleUrl))

struct LocalPost {
    std::string name;  // "accounts/{a}/locations/{l}/localPosts/{p}"
    std::optional<std::string> languageCode;
    std::optional<std::string> summary;
    std::optional<CallToAction> callToAction;
    std::optional<std::string> createTime;  // RFC 3339
    std::optional<std::string> updateTime;
    std::optional<std::string> scheduledTime;
    std::optional<PostEvent> event;
    // PROCESSING, SCHEDULED, LIVE, RECURRING or REJECTED.
    std::optional<std::string> state;
    std::optional<std::vector<PostMedia>> media;
    std::optional<std::string> searchUrl;
    std::optional<std::string> topicType;  // STANDARD, EVENT, OFFER or ALERT
    std::optional<PostOffer> offer;
    boost::json::value raw;  // not mapped: set by the reader
};
BOOST_DESCRIBE_STRUCT(LocalPost, (),
                      (name, languageCode, summary, callToAction, createTime, updateTime, scheduledTime, event, state, media,
                       searchUrl, topicType, offer))

// The three ids inside a local post's resource name.
struct LocalPostName {
    std::string account_id;
    std::string location_id;
    std::string post_id;

    std::string str() const;  // "accounts/{a}/locations/{l}/localPosts/{p}"
};
// Nothing unless `name` has exactly that form with ids of letters, digits,
// '-' and '_': the ids the outbound client takes in a local post's address.
std::optional<LocalPostName> parse_local_post_name(std::string_view name);

struct LocalPostsPage {
    std::vector<LocalPost> posts;
    std::optional<std::string> next_page_token;
    // Entries that couldn't be mapped (a post without a usable name, say).
    std::size_t skipped = 0;
};

// ---- between Google's spelling and the engine's (no I/O) ---------------------------------

// A post's photo or video as Google takes it: a public address it fetches.
struct MediaSource {
    std::string format;  // "photo" or "video"
    std::string url;
};

// The body of a create for `content`, with its media if it has one. The
// caller has checked the content: this only spells it Google's way.
boost::json::object create_body(const domain::LocalPostContent& content, const std::optional<MediaSource>& media);

// The body of an update of the post `at_google` (as Google returned it a
// moment ago) to what `edit` says, in the fields `mask` names and in no
// other (domain::local_post_edit_request). Each field is written whole, as
// Google replaces it whole; a field named in the mask and left out of the
// body is cleared at Google (the button taken away, an offer without its
// three parts). Two things are Google's own and go back as they came:
//   - an event's repeat (`recurrenceInfo`), which an edit does not change,
//     with its weekdays or day of the month as Google holds them;
//   - the days and times of an event's schedule that the edit does not
//     change: Google leaves a time out that the engine reads as 00:00, and
//     writing 00:00 back could give a time to an event that had none.
// `media`: the edit's upload, when "media" is in the mask and the edit has
// one; without it "media" is sent empty, which takes the post's media away.
boost::json::object update_body(const LocalPost& at_google, const domain::LocalPostContent& edit,
                                const std::vector<std::string_view>& mask, const std::optional<MediaSource>& media);

// What a post at Google says, in the engine's terms. `media_id` is never
// set: Google knows nothing of the engine's uploads. An event is kept only
// when it has both dates; a missing time is 00:00, which is how Google
// writes midnight.
domain::LocalPostContent to_content(const LocalPost& post);

// "PROCESSING" -> "processing" and so on; nothing for a state the engine does not know.
std::optional<std::string> to_google_state(const LocalPost& post);

// ---- the interfaces ----------------------------------------------------------------------

class LocalPostReader {
public:
    virtual ~LocalPostReader() = default;

    // One page (up to 100) of a location's posts.
    virtual boost::asio::awaitable<Result<LocalPostsPage>> list_local_posts(std::string account_id, std::string location_id,
                                                                            std::optional<std::string> page_token) = 0;

    // One post. ApiError::Kind::not_found means it is gone: only this marks a
    // post removed, never a list that lacks it (D-088 point 8).
    virtual boost::asio::awaitable<Result<LocalPost>> get_local_post(LocalPostName name) = 0;
};

class LocalPostWriter {
public:
    virtual ~LocalPostWriter() = default;

    // POST .../localPosts. An ApiError other than `unavailable` or
    // `invalid_response` means Google answered and refused: nothing was
    // created, and for a validation error the message names the field and
    // the limit. A 5xx (`unavailable`), a 2xx that can't be read
    // (`invalid_response`) and a transport failure (which throws
    // boost::system::system_error) say nothing certain about the outcome:
    // the post may exist, and the caller must never send the create again
    // (D-088 point 6).
    virtual boost::asio::awaitable<Result<LocalPost>> create_local_post(std::string account_id, std::string location_id,
                                                                        boost::json::object body) = 0;

    // DELETE .../localPosts/{p}. `not_found` means the post is already gone,
    // so a delete is safe to repeat (D-088 point 7).
    virtual boost::asio::awaitable<Result<void>> delete_local_post(LocalPostName name) = 0;

    // PATCH .../localPosts/{p}?updateMask=<mask>, with `body` holding the
    // fields the mask names (update_body). Returns the post as Google has it
    // after the edit. An ApiError other than `unavailable` or
    // `invalid_response` means Google answered and refused: the post is as
    // it was, and the message says what Google would not take (a field the
    // mask may not name, a text too long). A 5xx (`unavailable`), a 2xx that
    // can't be read (`invalid_response`) and a transport failure (which
    // throws boost::system::system_error) say nothing certain about the
    // outcome: the post may or may not have changed, and the caller must not
    // send the update again for the same approval (D-099). Which statuses
    // count as a certain refusal is the job's list (`definitely_refused`),
    // the same as for a create. `mask` must hold names the outbound client
    // takes (domain::kLocalPostEditFields); anything else throws
    // std::invalid_argument and nothing is sent.
    virtual boost::asio::awaitable<Result<LocalPost>> update_local_post(LocalPostName name, std::vector<std::string_view> mask,
                                                                        boost::json::object body) = 0;
};

// The names an update's mask may hold are kept twice, by the rule for what
// may go out and by the code that works a mask out, so that widening the
// second does not widen the first. They are the same six, in the same order.
static_assert(domain::kLocalPostEditFields == http::kLocalPostUpdateFields);

// Rate limiter key for everything about local posts. Google publishes no
// quota for v4 posts; its limit for edits of one profile is 10 a minute.
inline constexpr std::string_view kLocalPostsApi = "gbp.local_posts";

// The two interfaces over HTTPS (the v4 API), as the managing account. The
// outbound client holds the three writes to their addresses (http/client.hpp).
class HttpLocalPostReader final : public LocalPostReader {
public:
    explicit HttpLocalPostReader(google::Api api) : api_(api) {}

    boost::asio::awaitable<Result<LocalPostsPage>> list_local_posts(std::string account_id, std::string location_id,
                                                                    std::optional<std::string> page_token) override;
    boost::asio::awaitable<Result<LocalPost>> get_local_post(LocalPostName name) override;

private:
    google::Api api_;
};

class HttpLocalPostWriter final : public LocalPostWriter {
public:
    explicit HttpLocalPostWriter(google::Api api) : api_(api) {}

    boost::asio::awaitable<Result<LocalPost>> create_local_post(std::string account_id, std::string location_id,
                                                                boost::json::object body) override;
    boost::asio::awaitable<Result<void>> delete_local_post(LocalPostName name) override;
    boost::asio::awaitable<Result<LocalPost>> update_local_post(LocalPostName name, std::vector<std::string_view> mask,
                                                                boost::json::object body) override;

private:
    google::Api api_;
};

}  // namespace poni::gbp
