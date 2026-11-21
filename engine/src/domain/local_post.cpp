#include "domain/local_post.hpp"

#include "core/text.hpp"
#include "domain/dates.hpp"
#include "domain/review_text.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <format>

namespace poni::domain {

namespace {

constexpr std::array<std::string_view, 6> kActionTypes{"book", "order", "shop", "learn_more", "sign_up", "call"};
constexpr std::array<std::string_view, 3> kFrequencies{"daily", "weekly", "monthly"};

// Longer than this is not a text someone means to post; Google's own limits are checks.
constexpr std::size_t kSummaryStored = 6000;
constexpr std::size_t kTitleStored = 200;
constexpr std::size_t kUrlStored = 2000;
constexpr std::size_t kCouponStored = 58;
constexpr std::size_t kTermsStored = 5000;

std::unexpected<std::string> bad(std::string message) { return std::unexpected(std::move(message)); }

std::string trimmed(std::string_view value) { return std::string(text::trim(value)); }

// Trimmed; none if nothing is left.
std::optional<std::string> optional_text(const std::optional<std::string>& value) {
    if (!value) return std::nullopt;
    auto out = trimmed(*value);
    if (out.empty()) return std::nullopt;
    return out;
}

// "YYYY-MM-DD" naming a day of year 1 or later.
bool is_day(std::string_view value) {
    const auto day = parse_report_day(value);
    return day && std::chrono::year_month_day{*day}.year() >= std::chrono::year{1};
}

bool is_https(std::string_view url) {
    constexpr std::string_view scheme = "https://";
    if (url.size() <= scheme.size() || !text::ascii_iequals(url.substr(0, scheme.size()), scheme)) return false;
    // A host follows the scheme.
    const auto first = static_cast<unsigned char>(url[scheme.size()]);
    if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || (first >= '0' && first <= '9'))) return false;
    // An address, not a sentence: no space or control character in it, and nothing outside ASCII
    // (which is where the spaces and direction marks that hide part of an address live).
    return std::ranges::none_of(url, [](unsigned char c) { return c <= ' ' || c >= 0x7f; });
}

// The text as the phone scan reads it: the dashes and spaces people type between the groups of
// a number, which the scan would not take for separators, as "-" and " ".
std::u32string for_phone_scan(std::u32string folded) {
    for (auto& c : folded) {
        if ((c >= 0x2010 && c <= 0x2015) || c == 0x2212 || c == U'/') {
            c = U'-';
        } else if (c == 0x00A0 || (c >= 0x2000 && c <= 0x200A) || c == 0x202F || c == 0x3000) {
            c = U' ';
        }
    }
    return folded;
}

// The engine's canonical form, which compares as text.
std::string canonical(core::TimePoint t) { return std::format("{:%FT%T}Z", t); }

// Whether a canonical time is after `now`: how the check scheduled_in_future and a copy read a time to publish at.
bool still_ahead(std::string_view time, core::TimePoint now) { return time > canonical(now); }

// A time a person gave, in the canonical form, if it names a moment that exists: a day the calendar
// has and a second below 60. canonical_timestamp() takes the shape alone; a post's times are compared
// as text and kept as PostgreSQL would spell them, and for a moment that does not exist it spells another.
std::optional<std::string> existing_moment(std::string_view text) {
    auto moment = canonical_timestamp(text);
    if (moment && (!is_day(std::string_view(*moment).substr(0, 10)) || moment->compare(17, 2, "60") == 0)) return std::nullopt;
    return moment;
}

}  // namespace

std::expected<LocalPostContent, std::string> normalize_local_post(LocalPostContent content) {
    if (!std::ranges::contains(kPostTopicTypes, std::string_view(content.topic_type))) {
        return bad("\"topic_type\" must be \"standard\", \"event\" or \"offer\".");
    }
    content.summary = trimmed(content.summary);
    if (text::length(content.summary) > kSummaryStored) return bad("\"summary\" is far longer than a post can be.");
    content.language_code = trimmed(content.language_code);
    if (content.language_code.size() > 35) return bad("\"language_code\" is not a language tag.");

    if (content.call_to_action) {
        auto& action = *content.call_to_action;
        if (!std::ranges::contains(kActionTypes, action.action_type)) {
            return bad("\"call_to_action.action_type\" must be \"book\", \"order\", \"shop\", \"learn_more\", \"sign_up\" or \"call\".");
        }
        action.url = optional_text(action.url);
        if (action.url && action.url->size() > kUrlStored) return bad("\"call_to_action.url\" is too long.");
        if (action.action_type == "call" && action.url) return bad("A call button has no address: it dials the profile's number.");
        if (content.topic_type == "offer") return bad("An offer has no button: Google ignores it there.");
    }

    const bool wants_event = content.topic_type != "standard";
    if (!wants_event && (content.event || content.offer)) return bad("A \"standard\" post has no \"event\" and no \"offer\".");
    if (wants_event && !content.event) return bad("An \"event\" and an \"offer\" need an \"event\" with a title and dates.");
    if (content.event) {
        auto& event = *content.event;
        event.title = trimmed(event.title);
        if (text::length(event.title) > kTitleStored) return bad("\"event.title\" is far longer than a title can be.");
        const auto& schedule = event.schedule;
        if (!is_day(schedule.start_date) || !is_day(schedule.end_date)) {
            return bad("\"event.schedule.start_date\" and \"end_date\" must be days, YYYY-MM-DD.");
        }
        if (!parse_clock_time(schedule.start_time) || !parse_clock_time(schedule.end_time)) {
            return bad("\"event.schedule.start_time\" and \"end_time\" must be times, HH:MM.");
        }
        if (event.recurrence) {
            if (!std::ranges::contains(kFrequencies, event.recurrence->frequency)) {
                return bad("\"event.recurrence.frequency\" must be \"daily\", \"weekly\" or \"monthly\".");
            }
            if (event.recurrence->until) {
                auto until = existing_moment(*event.recurrence->until);
                if (!until) return bad("\"event.recurrence.until\" must be a time in UTC, such as 2027-01-31T00:00:00Z.");
                event.recurrence->until = std::move(until);
            }
        }
    }

    if (content.topic_type == "offer") {
        // An offer always has its three fields, set or not.
        LocalPostOffer offer;
        if (content.offer) {
            offer.coupon_code = optional_text(content.offer->coupon_code);
            offer.redeem_online_url = optional_text(content.offer->redeem_online_url);
            offer.terms_conditions = optional_text(content.offer->terms_conditions);
        }
        if (offer.coupon_code && text::length(*offer.coupon_code) > kCouponStored) return bad("\"offer.coupon_code\" is too long.");
        if (offer.redeem_online_url && offer.redeem_online_url->size() > kUrlStored) return bad("\"offer.redeem_online_url\" is too long.");
        if (offer.terms_conditions && text::length(*offer.terms_conditions) > kTermsStored) return bad("\"offer.terms_conditions\" is too long.");
        content.offer = std::move(offer);
    } else if (content.offer) {
        return bad("Only an \"offer\" has an \"offer\".");
    }

    if (content.media_id && *content.media_id <= 0) return bad("\"media_id\" must be the id of an uploaded file, or null.");
    if (content.scheduled_time) {
        auto scheduled = existing_moment(*content.scheduled_time);
        if (!scheduled) return bad("\"scheduled_time\" must be a time in UTC, such as 2026-12-19T17:00:00Z.");
        content.scheduled_time = std::move(scheduled);
    }
    return content;
}

DraftCopy fit_for_draft(LocalPostContent content, core::TimePoint now, std::optional<std::string> media_at_google) {
    DraftCopy out;
    const auto leave_out = [&](std::string what) { out.left_out.push_back(std::move(what)); };

    // A kind only Google writes, or an event or offer whose dates Google's post lacks (a draft of that kind needs them).
    const bool known = std::ranges::contains(kPostTopicTypes, std::string_view(content.topic_type));
    if (!known || (content.topic_type != "standard" && !content.event)) {
        leave_out(std::format("the kind \"{}\", {}: the copy is an update", content.topic_type,
                              known ? "whose dates Google's post lacks" : "which only Google writes"));
        content.topic_type = "standard";
    }
    if (content.topic_type == "standard" && content.event) {
        leave_out("the event, which an update does not have");
        content.event.reset();
    }
    if (content.topic_type != "offer" && content.offer) {
        leave_out("the offer's code, link and terms, which only an offer has");
        content.offer.reset();
    }
    if (content.call_to_action) {
        auto& action = *content.call_to_action;
        if (content.topic_type == "offer") {
            leave_out("the button, which an offer does not have");
            content.call_to_action.reset();
        } else if (!std::ranges::contains(kActionTypes, action.action_type)) {
            leave_out(std::format("the button \"{}\", which a draft cannot have", action.action_type));
            content.call_to_action.reset();
        } else if (action.action_type == "call" && action.url) {
            leave_out("the call button's address: it dials the profile's number");
            action.url.reset();
        }
    }
    if (content.scheduled_time && !still_ahead(*content.scheduled_time, now)) {
        leave_out("the time to publish at, which has passed");
        content.scheduled_time.reset();
    }
    // An upload is the engine's own and is kept. Without one, what Google shows is its own copy of a file
    // (of a video, a still), which is not an upload.
    if (!content.media_id && media_at_google) {
        leave_out(std::format("Google's copy of the {0}: upload the {0} again", *media_at_google == "video" ? "video" : "photo"));
        content.media_id.reset();
    }
    out.content = std::move(content);
    return out;
}

CheckReport run_local_post_checks(const LocalPostContent& content, const LocalPostCheckContext& context) {
    CheckReport report;
    const auto add = [&](std::string_view name, bool passed, std::string detail = {}) {
        report.results.push_back({std::string(name), passed, passed ? std::string{} : std::move(detail)});
    };
    const auto now = canonical(context.now);
    const auto* event = content.event ? &*content.event : nullptr;

    {
        const auto size = text::length(content.summary);
        if (content.topic_type == "standard" && size == 0) {
            add("summary_length", false, "the post has no text");
        } else {
            add("summary_length", size <= kLocalPostSummaryMax, std::format("{} characters, at most {} allowed", size, kLocalPostSummaryMax));
        }
    }
    {
        const auto size = event ? text::length(event->title) : 0;
        add("title_length", !event || (size >= 1 && size <= kLocalPostTitleMax),
            size == 0 ? std::string("an event or an offer needs a title") : std::format("{} characters, at most {} allowed", size, kLocalPostTitleMax));
    }
    {
        // Google removes posts whose text carries contact details; the call button exists for that.
        const auto folded = text::fold(content.summary + "\n" + (event ? event->title : std::string{}));
        const auto phone = scan_for_phone(for_phone_scan(folded));
        add("no_phone", !phone, phone.value_or(""));
        const auto email = scan_for_email(folded);
        add("no_email", !email, email.value_or(""));
        const auto handle = scan_for_handle(folded);
        add("no_handle", !handle, handle.value_or(""));
    }
    {
        const auto* action = content.call_to_action ? &*content.call_to_action : nullptr;
        const bool needs_url = action && action->action_type != "call";
        add("button_url", !needs_url || (action->url && is_https(*action->url)),
            action && action->url ? "the button's address must start with https://" : "the button needs an address");
    }
    {
        const auto* url = content.offer && content.offer->redeem_online_url ? &*content.offer->redeem_online_url : nullptr;
        add("offer_url", !url || is_https(*url), "the offer's link must start with https://");
    }
    {
        // Days and times of one format compare as text.
        const bool in_order = !event || event->schedule.start_date < event->schedule.end_date ||
                              (event->schedule.start_date == event->schedule.end_date && event->schedule.start_time <= event->schedule.end_time);
        add("event_dates", in_order, "the event ends before it starts");
    }
    {
        const auto* recurrence = event && event->recurrence ? &*event->recurrence : nullptr;
        if (!recurrence) {
            add("recurrence_end", true);
        } else if (!recurrence->until) {
            add("recurrence_end", false, "a repeating post needs a day on which it ends");
        } else {
            // The series ends after it starts, and has not ended already.
            add("recurrence_end", recurrence->until->substr(0, 10) >= event->schedule.start_date && *recurrence->until > now,
                "the repeat ends before the event starts, or has ended already");
        }
    }
    add("scheduled_in_future", !content.scheduled_time || still_ahead(*content.scheduled_time, context.now), "the time to publish at is in the past");
    add("language", is_post_language_tag(content.language_code), "the language must be a tag such as en or zh-Hant");
    add("media_usable", !content.media_id || !context.media_problem, context.media_problem.value_or(""));
    return report;
}

std::optional<std::string> contact_detail_in(std::string_view text) {
    const auto folded = text::fold(text);
    if (scan_for_phone(for_phone_scan(folded))) return "a phone number";
    if (scan_for_email(folded)) return "an email address";
    if (scan_for_handle(folded)) return "a social handle";
    return std::nullopt;
}

bool is_post_language_tag(std::string_view tag) {
    if (tag.size() < 2 || tag.size() > 35) return false;
    const auto letter = [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    if (!letter(tag[0]) || !letter(tag[1]) || tag.back() == '-') return false;
    return std::ranges::all_of(tag, [&](unsigned char c) { return letter(c) || (c >= '0' && c <= '9') || c == '-'; });
}

}  // namespace poni::domain
