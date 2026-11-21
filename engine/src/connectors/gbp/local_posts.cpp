#include "connectors/gbp/local_posts.hpp"

#include "connectors/gbp/reader.hpp"
#include "core/text.hpp"
#include "domain/dates.hpp"
#include "domain/review_text.hpp"
#include "http/client.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>

namespace poni::gbp {

namespace {

std::string upper(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

// "YYYY-MM-DD" -> {"year", "month", "day"}; an empty object if it is not a day.
boost::json::object date_object(std::string_view day) {
    const auto parsed = domain::parse_report_day(day);
    if (!parsed) return {};
    const std::chrono::year_month_day ymd(*parsed);
    return {{"year", static_cast<int>(ymd.year())},
            {"month", static_cast<int>(static_cast<unsigned>(ymd.month()))},
            {"day", static_cast<int>(static_cast<unsigned>(ymd.day()))}};
}

// "HH:MM" -> {"hours", "minutes"}.
boost::json::object time_object(std::string_view time) {
    const auto minutes = domain::parse_clock_time(time);
    if (!minutes) return {};
    return {{"hours", *minutes / 60}, {"minutes", *minutes % 60}};
}

std::optional<std::string> day_text(const std::optional<Date>& date) {
    if (!date || !date->year || !date->month || !date->day) return std::nullopt;
    if (*date->year < 1 || *date->year > 9999 || *date->month < 1 || *date->month > 12 || *date->day < 1 || *date->day > 31) {
        return std::nullopt;
    }
    return std::format("{:04}-{:02}-{:02}", *date->year, *date->month, *date->day);
}

// Google leaves out what is zero, so a missing part is 0.
std::string time_text(const std::optional<TimeOfDay>& time) {
    const int hours = time ? time->hours.value_or(0) : 0;
    const int minutes = time ? time->minutes.value_or(0) : 0;
    return std::format("{:02}:{:02}", std::clamp(hours, 0, 23), std::clamp(minutes, 0, 59));
}

}  // namespace

std::string LocalPostName::str() const { return std::format("accounts/{}/locations/{}/localPosts/{}", account_id, location_id, post_id); }

std::optional<LocalPostName> parse_local_post_name(std::string_view name) {
    const auto ids = split_resource_name(name, "localPosts");
    if (!ids || !std::ranges::all_of(*ids, http::is_plain_id)) return std::nullopt;
    return LocalPostName{std::string((*ids)[0]), std::string((*ids)[1]), std::string((*ids)[2])};
}

boost::json::object create_body(const domain::LocalPostContent& content, const std::optional<MediaSource>& media) {
    boost::json::object body{{"languageCode", content.language_code}, {"summary", content.summary}, {"topicType", upper(content.topic_type)}};
    if (content.call_to_action) {
        boost::json::object action{{"actionType", upper(content.call_to_action->action_type)}};
        if (content.call_to_action->url) action["url"] = *content.call_to_action->url;
        body["callToAction"] = std::move(action);
    }
    if (content.event) {
        const auto& schedule = content.event->schedule;
        boost::json::object event{{"title", content.event->title},
                                  {"schedule",
                                   {{"startDate", date_object(schedule.start_date)},
                                    {"startTime", time_object(schedule.start_time)},
                                    {"endDate", date_object(schedule.end_date)},
                                    {"endTime", time_object(schedule.end_time)}}}};
        if (content.event->recurrence) {
            const auto& recurrence = *content.event->recurrence;
            boost::json::object info;
            if (recurrence.frequency == "daily") {
                info["dailyPattern"] = boost::json::object{};
            } else if (recurrence.frequency == "weekly") {
                // No weekdays: Google repeats on the weekday of the event's start.
                info["weeklyPattern"] = boost::json::object{};
            } else if (recurrence.frequency == "monthly") {
                // On the day of the month the event starts on.
                boost::json::object pattern;
                if (const auto start = date_object(schedule.start_date); start.contains("day")) pattern["dayOfMonth"] = start.at("day");
                info["monthlyPattern"] = std::move(pattern);
            }
            if (recurrence.until) info["seriesEndTime"] = *recurrence.until;
            event["recurrenceInfo"] = std::move(info);
        }
        body["event"] = std::move(event);
    }
    if (content.offer) {
        boost::json::object offer;
        if (content.offer->coupon_code) offer["couponCode"] = *content.offer->coupon_code;
        if (content.offer->redeem_online_url) offer["redeemOnlineUrl"] = *content.offer->redeem_online_url;
        if (content.offer->terms_conditions) offer["termsConditions"] = *content.offer->terms_conditions;
        if (!offer.empty()) body["offer"] = std::move(offer);
    }
    if (media) {
        body["media"] = boost::json::array{boost::json::object{{"mediaFormat", upper(media->format)}, {"sourceUrl", media->url}}};
    }
    if (content.scheduled_time) body["scheduledTime"] = *content.scheduled_time;
    return body;
}

domain::LocalPostContent to_content(const LocalPost& post) {
    domain::LocalPostContent content;
    content.topic_type = post.topicType ? text::ascii_lower(*post.topicType) : std::string("standard");
    content.summary = post.summary.value_or("");
    content.language_code = post.languageCode.value_or("");
    if (post.callToAction && post.callToAction->actionType) {
        content.call_to_action =
            domain::LocalPostCallToAction{.action_type = text::ascii_lower(*post.callToAction->actionType), .url = post.callToAction->url};
    }
    if (post.event && post.event->schedule) {
        const auto start = day_text(post.event->schedule->startDate);
        const auto end = day_text(post.event->schedule->endDate);
        if (start && end) {
            domain::LocalPostEvent event{.title = post.event->title.value_or(""),
                                         .schedule = {.start_date = *start,
                                                      .start_time = time_text(post.event->schedule->startTime),
                                                      .end_date = *end,
                                                      .end_time = time_text(post.event->schedule->endTime)},
                                         .recurrence = std::nullopt};
            if (const auto& info = post.event->recurrenceInfo) {
                const std::string_view frequency = info->dailyPattern ? "daily" : info->weeklyPattern ? "weekly" : info->monthlyPattern ? "monthly" : "";
                if (!frequency.empty()) {
                    std::optional<std::string> until;
                    if (info->seriesEndTime) until = domain::canonical_timestamp(*info->seriesEndTime);
                    event.recurrence = domain::LocalPostRecurrence{.frequency = std::string(frequency), .until = std::move(until)};
                }
            }
            content.event = std::move(event);
        }
    }
    if (content.topic_type == "offer") {
        domain::LocalPostOffer offer;
        if (post.offer) {
            offer.coupon_code = post.offer->couponCode;
            offer.redeem_online_url = post.offer->redeemOnlineUrl;
            offer.terms_conditions = post.offer->termsConditions;
        }
        content.offer = std::move(offer);
    }
    if (post.scheduledTime) content.scheduled_time = domain::canonical_timestamp(*post.scheduledTime);
    return content;
}

std::optional<std::string> to_google_state(const LocalPost& post) {
    if (!post.state) return std::nullopt;
    for (const std::string_view known : {"PROCESSING", "SCHEDULED", "LIVE", "RECURRING", "REJECTED"}) {
        if (*post.state == known) return text::ascii_lower(known);
    }
    return std::nullopt;
}

}  // namespace poni::gbp
