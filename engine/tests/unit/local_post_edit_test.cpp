// What an edit of a post that is at Google changes there (D-099):
// domain::local_post_edit_mask, without a database. The mask names exactly
// the fields in which the edit differs from the post, out of six, and there
// is no mask at all where one could name something the engine cannot write
// back.

#include "domain/local_post.hpp"
#include "fakes.hpp"
#include "http/client.hpp"

#include <boost/url/url.hpp>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <format>
#include <functional>

using namespace poni;
using namespace std::chrono_literals;
using testing::ElementsAre;
using testing::HasSubstr;
using testing::IsEmpty;
using Kind = domain::EditObstacle::Kind;

namespace {

// 2026-11-20T12:00:00Z
const core::TimePoint kNow = std::chrono::sys_days{std::chrono::year{2026} / 11 / 20} + 12h;

domain::LocalPostEvent event(std::string title = "Winter tasting") {
    return {.title = std::move(title),
            .schedule = {.start_date = "2026-12-01", .start_time = "18:00", .end_date = "2026-12-01", .end_time = "21:00"},
            .recurrence = std::nullopt};
}

domain::LocalPostContent an_offer() {
    auto offer = test::standard("Two for one.");
    offer.topic_type = "offer";
    offer.event = event("Lunch week");
    offer.offer = domain::LocalPostOffer{.coupon_code = "TWO", .redeem_online_url = std::nullopt, .terms_conditions = "Dine-in only."};
    return offer;
}

// The mask of `post` with `change` made to it.
std::vector<std::string_view> mask_after(const domain::LocalPostContent& post, const std::function<void(domain::LocalPostContent&)>& change) {
    auto edit = post;
    change(edit);
    const auto normalized = domain::normalize_local_post(edit);
    EXPECT_TRUE(normalized) << (normalized ? "" : normalized.error());
    const auto mask = domain::local_post_edit_mask(post, *normalized);
    EXPECT_TRUE(mask) << (mask ? "" : mask.error().message);
    return mask ? *mask : std::vector<std::string_view>{"(no mask)"};
}

domain::EditObstacle obstacle(const domain::LocalPostContent& post, const domain::LocalPostContent& edit) {
    const auto mask = domain::local_post_edit_mask(post, edit);
    EXPECT_FALSE(mask);
    return mask ? domain::EditObstacle{} : mask.error();
}

}  // namespace

TEST(LocalPostEditMask, EachFieldThatDiffersIsNamedAndNoOther) {
    auto post = test::standard("Fall specials start Monday.");
    post.call_to_action = domain::LocalPostCallToAction{.action_type = "book", .url = "https://example.com/book"};
    post.media_ids = {7};
    EXPECT_THAT(mask_after(post, [](auto& c) { c.summary = "Fall specials start Tuesday."; }), ElementsAre("summary"));
    EXPECT_THAT(mask_after(post, [](auto& c) { c.call_to_action->url = "https://example.com/reserve"; }), ElementsAre("callToAction"));
    EXPECT_THAT(mask_after(post, [](auto& c) { c.call_to_action->action_type = "order"; }), ElementsAre("callToAction"));
    EXPECT_THAT(mask_after(post, [](auto& c) { c.call_to_action.reset(); }), ElementsAre("callToAction")) << "the button taken away";
    EXPECT_THAT(mask_after(post, [](auto& c) { c.language_code = "es"; }), ElementsAre("languageCode"));
    EXPECT_THAT(mask_after(post, [](auto& c) { c.media_ids = {8}; }), ElementsAre("media"));

    const auto offer = an_offer();
    EXPECT_THAT(mask_after(offer, [](auto& c) { c.event->title = "Lunch fortnight"; }), ElementsAre("event"));
    EXPECT_THAT(mask_after(offer, [](auto& c) { c.event->schedule.end_date = "2026-12-08"; }), ElementsAre("event"));
    EXPECT_THAT(mask_after(offer, [](auto& c) { c.event->schedule.start_time = "17:30"; }), ElementsAre("event"));
    EXPECT_THAT(mask_after(offer, [](auto& c) { c.offer->coupon_code = "THREE"; }), ElementsAre("offer"));
    EXPECT_THAT(mask_after(offer, [](auto& c) { c.offer->terms_conditions.reset(); }), ElementsAre("offer"));
    EXPECT_THAT(mask_after(offer, [](auto& c) { c.offer->redeem_online_url = "https://example.com/redeem"; }), ElementsAre("offer"));
}

TEST(LocalPostEditMask, SeveralFieldsComeInTheOneOrderAndEachOnce) {
    auto post = an_offer();
    post.media_ids = {7};
    const auto everything = mask_after(post, [](auto& c) {
        c.language_code = "zh-Hant";
        c.media_ids = {9};
        c.offer->coupon_code = "THREE";
        c.event->title = "Lunch fortnight";
        c.summary = "Three for two.";
    });
    EXPECT_THAT(everything, ElementsAre("summary", "event", "offer", "media", "languageCode"));

    auto standard = test::standard("Open late on Fridays.");
    EXPECT_THAT(mask_after(standard,
                           [](auto& c) {
                               c.call_to_action = domain::LocalPostCallToAction{.action_type = "call", .url = std::nullopt};
                               c.summary = "Open late on Saturdays.";
                           }),
                ElementsAre("summary", "callToAction"));
    // Every name a mask can hold is one of the six, and the outbound client takes each of them and no other.
    for (const auto name : everything) EXPECT_TRUE(std::ranges::contains(domain::kLocalPostEditFields, name)) << name;
    for (const auto name : domain::kLocalPostEditFields) {
        const boost::urls::url address(std::format("https://mybusiness.googleapis.com/v4/accounts/1/locations/2/localPosts/3?updateMask={}", name));
        EXPECT_TRUE(http::is_local_post_update_url(address)) << name;
    }
    for (const auto* other : {"topicType", "scheduledTime", "state", "name", "createTime", "searchUrl", "alertType", "*"}) {
        const boost::urls::url address(std::format("https://mybusiness.googleapis.com/v4/accounts/1/locations/2/localPosts/3?updateMask={}", other));
        EXPECT_FALSE(http::is_local_post_update_url(address)) << other;
    }
}

TEST(LocalPostEditMask, AnEditThatSaysWhatThePostSaysHasAnEmptyMask) {
    auto post = an_offer();
    post.media_ids = {7};
    post.scheduled_time = "2026-11-01T17:00:00.000000Z";  // long past: the post is live
    EXPECT_THAT(mask_after(post, [](auto&) {}), IsEmpty());

    // The same content written another way is the same content: the two are compared as a draft holds them.
    auto as_google_has_it = test::standard("  Fall specials start Monday.\n");
    as_google_has_it.language_code = "en ";
    as_google_has_it.scheduled_time = "2026-11-01T17:00:00Z";
    auto edit = test::standard("Fall specials start Monday.");
    edit.scheduled_time = "2026-11-01T17:00:00.000000Z";
    const auto mask = domain::local_post_edit_mask(as_google_has_it, edit);
    ASSERT_TRUE(mask) << mask.error().message;
    EXPECT_THAT(*mask, IsEmpty());
}

TEST(LocalPostEditMask, MediaIsNamedOnlyWhenTheUploadIsAnotherOrIsTakenAway) {
    // A post made at Google: its photo is Google's own copy, and the engine knows no upload of it.
    const auto made_at_google = test::standard("Now open late on Fridays.");
    EXPECT_THAT(mask_after(made_at_google, [](auto& c) { c.summary = "Now open late on Saturdays."; }), ElementsAre("summary"))
        << "an edit without an upload never names the media of such a post";
    EXPECT_THAT(mask_after(made_at_google, [](auto& c) { c.media_ids = {4}; }), ElementsAre("media")) << "a person gave it an upload";

    auto with_upload = test::standard("A look at the new room.");
    with_upload.media_ids = {7};
    EXPECT_THAT(mask_after(with_upload, [](auto& c) { c.summary = "A look at the new rooms."; }), ElementsAre("summary"))
        << "the post's own upload, named again, is no change";
    EXPECT_THAT(mask_after(with_upload, [](auto& c) { c.media_ids.clear(); }), ElementsAre("media")) << "the upload taken away";
    EXPECT_THAT(mask_after(with_upload, [](auto& c) { c.media_ids = {8}; }), ElementsAre("media"));
}

TEST(LocalPostEditMask, TheUploadsAreOneFieldAndTheirOrderIsPartOfIt) {
    // Google replaces a post's media whole (D-101): any other list is a change of the media.
    auto post = test::standard("Three rooms.");
    post.media_ids = {7, 8, 9};
    EXPECT_THAT(mask_after(post, [](auto& c) { c.media_ids = {9, 8, 7}; }), ElementsAre("media")) << "another order";
    EXPECT_THAT(mask_after(post, [](auto& c) { c.media_ids = {7, 9}; }), ElementsAre("media")) << "one taken out";
    EXPECT_THAT(mask_after(post, [](auto& c) { c.media_ids.push_back(10); }), ElementsAre("media")) << "one added";
    EXPECT_THAT(mask_after(post, [](auto& c) { c.summary = "Three rooms, newly painted."; }), ElementsAre("summary"))
        << "the same list is no change";
}

TEST(LocalPostContent, APostHasAtMostTenPhotosAndVideosEachOnce) {
    auto content = test::standard("Ten rooms.");
    for (std::int64_t id = 10; id >= 1; --id) content.media_ids.push_back(id);
    const auto ten = domain::normalize_local_post(content);
    ASSERT_TRUE(ten) << ten.error();
    EXPECT_EQ(ten->media_ids, content.media_ids) << "kept in the order given";
    content.media_ids.push_back(11);
    EXPECT_THAT(domain::normalize_local_post(content).error(), HasSubstr("at most 10"));
    content.media_ids = {4, 5, 4};
    EXPECT_THAT(domain::normalize_local_post(content).error(), HasSubstr("twice"));
    content.media_ids = {4, 0};
    EXPECT_THAT(domain::normalize_local_post(content).error(), HasSubstr("media_ids"));
}

TEST(LocalPostEditMask, TheKindTheTimeAndTheRepeatAreNotAnEditsToChange) {
    auto post = test::standard("Doors open at five.");
    post.scheduled_time = "2026-12-19T17:00:00.000000Z";

    auto other_time = post;
    other_time.scheduled_time = "2026-12-20T17:00:00.000000Z";
    auto found = obstacle(post, other_time);
    EXPECT_EQ(found.kind, Kind::fixed_part);
    EXPECT_EQ(found.member, "scheduled_time");
    EXPECT_THAT(found.message, HasSubstr("\"scheduled_time\""));
    auto no_time = post;
    no_time.scheduled_time.reset();
    EXPECT_EQ(obstacle(post, no_time).member, "scheduled_time");
    auto a_time = test::standard("Doors open at five.");
    a_time.scheduled_time = "2026-12-19T17:00:00.000000Z";
    EXPECT_EQ(obstacle(test::standard("Doors open at five."), a_time).member, "scheduled_time") << "nor is a time given to a post that has none";

    auto as_event = post;
    as_event.topic_type = "event";
    as_event.event = event();
    found = obstacle(post, as_event);
    EXPECT_EQ(found.kind, Kind::fixed_part);
    EXPECT_EQ(found.member, "topic_type");

    auto repeating = test::standard("Live music every Friday.");
    repeating.topic_type = "event";
    repeating.event = event("Friday live music");
    repeating.event->recurrence = domain::LocalPostRecurrence{.frequency = "weekly", .until = "2027-01-31T00:00:00.000000Z"};
    for (const auto& change : std::vector<std::function<void(domain::LocalPostContent&)>>{
             [](auto& c) { c.event->recurrence->frequency = "daily"; },
             [](auto& c) { c.event->recurrence->until = "2027-02-28T00:00:00.000000Z"; },
             [](auto& c) { c.event->recurrence->until.reset(); },
             [](auto& c) { c.event->recurrence.reset(); },
         }) {
        auto edit = repeating;
        change(edit);
        found = obstacle(repeating, edit);
        EXPECT_EQ(found.kind, Kind::fixed_part);
        EXPECT_EQ(found.member, "event.recurrence");
    }
    auto once = repeating;
    once.event->recurrence.reset();
    EXPECT_EQ(obstacle(once, repeating).member, "event.recurrence") << "nor is a repeat given to an event that has none";
    // The rest of a repeating event is an edit's to change, and the mask says "event".
    EXPECT_THAT(mask_after(repeating, [](auto& c) { c.event->title = "Friday jazz"; }), ElementsAre("event"));
}

TEST(LocalPostEditMask, APostThatHoldsWhatADraftCannotHoldHasNoMaskAtAll) {
    const auto refused = [](const std::function<void(domain::LocalPostContent&)>& make) {
        auto post = test::standard("As Google has it.");
        make(post);
        // Whatever the edit says: here the plainest one, which changes the text alone.
        const auto found = obstacle(post, test::standard("As a person would have it."));
        EXPECT_EQ(found.kind, Kind::not_editable);
        EXPECT_TRUE(found.member.empty());
        return found.message;
    };
    EXPECT_THAT(refused([](auto& c) { c.topic_type = "alert"; }), HasSubstr("cannot be edited here"));
    // A button a draft does not have: an edit could only drop it or replace it, never keep it.
    EXPECT_THAT(refused([](auto& c) { c.call_to_action = domain::LocalPostCallToAction{.action_type = "get_offer", .url = std::nullopt}; }),
                HasSubstr("call_to_action.action_type"));
    EXPECT_THAT(refused([](auto& c) { c.call_to_action = domain::LocalPostCallToAction{.action_type = "call", .url = "https://example.com"}; }),
                HasSubstr("call button"));
    // An event or an offer whose dates Google's post lacks, and an event on a post that is none.
    EXPECT_THAT(refused([](auto& c) { c.topic_type = "event"; }), HasSubstr("need an \"event\""));
    EXPECT_THAT(refused([](auto& c) { c.event = event(); }), HasSubstr("no \"event\""));
    EXPECT_THAT(refused([](auto& c) {
        c.topic_type = "offer";
        c.event = event();
        c.call_to_action = domain::LocalPostCallToAction{.action_type = "shop", .url = "https://example.com"};
    }),
                HasSubstr("offer has no button"));

    // The same edit of a post a draft can hold has a mask.
    const auto mask = domain::local_post_edit_mask(test::standard("As Google has it."), test::standard("As a person would have it."));
    ASSERT_TRUE(mask);
    EXPECT_THAT(*mask, ElementsAre("summary"));
}

// ---- what an approved edit sends: only what its author changed -------------------------------------

namespace {

// The post an edit was written against, with a button.
domain::LocalPostContent a_base() {
    auto base = test::standard("Open late on Fridays.");
    base.call_to_action = domain::LocalPostCallToAction{.action_type = "book", .url = "https://example.com/book"};
    base.media_ids = {7};
    return base;
}

domain::LocalPostContent with(domain::LocalPostContent content, const std::function<void(domain::LocalPostContent&)>& change) {
    change(content);
    return domain::normalize_local_post(content).value();
}

}  // namespace

TEST(LocalPostEditRequest, AFieldTheEditDidNotChangeIsNeverNamedWhateverGoogleHoldsNow) {
    const auto base = a_base();
    const auto edit = with(base, [](auto& c) { c.summary = "Open late on Saturdays."; });
    // Nothing changed at Google: the one field the edit changes.
    EXPECT_THAT(domain::local_post_edit_request(base, edit, base).value(), ElementsAre("summary"));
    // Everything else changed at Google since: still the one field, and none of the others is written back.
    const auto moved_on = with(base, [](auto& c) {
        c.call_to_action->url = "https://example.com/elsewhere";
        c.language_code = "en-US";
        c.media_ids = {9};
    });
    EXPECT_THAT(domain::local_post_edit_request(base, edit, moved_on).value(), ElementsAre("summary"));
    auto no_button = moved_on;
    no_button.call_to_action.reset();
    no_button.media_ids.clear();
    EXPECT_THAT(domain::local_post_edit_request(base, edit, no_button).value(), ElementsAre("summary"));
    // Against the post as it is, the plain mask would name them all: that is what must not be sent.
    EXPECT_THAT(domain::local_post_edit_mask(moved_on, edit).value(), ElementsAre("summary", "callToAction", "media", "languageCode"));
}

TEST(LocalPostEditRequest, AFieldGoogleAlreadyHasIsDroppedAndAnEditGoogleHasWholeSendsNothing) {
    const auto base = a_base();
    const auto edit = with(base, [](auto& c) {
        c.summary = "Open late on Saturdays.";
        c.call_to_action->url = "https://example.com/reserve";
    });
    EXPECT_THAT(domain::local_post_edit_request(base, edit, base).value(), ElementsAre("summary", "callToAction"));
    const auto half_there = with(base, [](auto& c) { c.call_to_action->url = "https://example.com/reserve"; });
    EXPECT_THAT(domain::local_post_edit_request(base, edit, half_there).value(), ElementsAre("summary"));
    EXPECT_THAT(domain::local_post_edit_request(base, edit, edit).value(), IsEmpty());
    // Space around a text at Google is no difference.
    auto spaced = edit;
    spaced.summary = " Open late on Saturdays.\n";
    EXPECT_THAT(domain::local_post_edit_request(base, edit, spaced).value(), IsEmpty());
    // An edit that changes nothing sends nothing, whatever Google holds.
    EXPECT_THAT(domain::local_post_edit_request(base, base, half_there).value(), IsEmpty());
}

TEST(LocalPostEditRequest, AFieldChangedOnBothSidesHasNoMaskAndIsNamed) {
    const auto base = a_base();
    const auto edit = with(base, [](auto& c) {
        c.summary = "Open late on Saturdays.";
        c.call_to_action->url = "https://example.com/reserve";
    });
    const auto link_moved = with(base, [](auto& c) { c.call_to_action->url = "https://example.com/elsewhere"; });
    auto found = domain::local_post_edit_request(base, edit, link_moved);
    ASSERT_FALSE(found);
    EXPECT_EQ(found.error().kind, Kind::changed_at_google);
    EXPECT_EQ(found.error().member, "call_to_action") << "by the API's name, for the person";
    EXPECT_THAT(found.error().message, HasSubstr("changed at Google"));
    EXPECT_THAT(found.error().message, HasSubstr("\"call_to_action\""));
    // Each of the six, when both sides changed it.
    const std::vector<std::pair<std::string_view, std::function<void(domain::LocalPostContent&, int)>>> fields{
        {"summary", [](auto& c, int n) { c.summary = std::format("Text {}.", n); }},
        {"call_to_action", [](auto& c, int n) { c.call_to_action->url = std::format("https://example.com/{}", n); }},
        {"event", [](auto& c, int n) { c.event->title = std::format("Title {}", n); }},
        {"offer", [](auto& c, int n) { c.offer->coupon_code = std::format("CODE{}", n); }},
        {"media", [](auto& c, int n) { c.media_ids = {100 + n}; }},
        {"language_code", [](auto& c, int n) { c.language_code = n == 1 ? "es" : "fr"; }},
    };
    for (const auto& [member, change] : fields) {
        auto post = std::string_view(member) == "call_to_action" ? a_base() : an_offer();
        post.media_ids = {7};
        const auto mine = with(post, [&](auto& c) { change(c, 1); });
        const auto theirs = with(post, [&](auto& c) { change(c, 2); });
        const auto stopped = domain::local_post_edit_request(post, mine, theirs);
        ASSERT_FALSE(stopped) << member;
        EXPECT_EQ(stopped.error().kind, Kind::changed_at_google);
        EXPECT_EQ(stopped.error().member, member);
        EXPECT_EQ(domain::edit_member(domain::local_post_edit_mask(post, mine).value().at(0)), member) << "the name the API lists it under";
    }
    EXPECT_EQ(domain::edit_member("topicType"), "");

    // The post's time moved at Google: that is not a field of the edit's, and stops nothing it changes.
    auto base_with_time = base;
    base_with_time.scheduled_time = "2026-12-19T17:00:00.000000Z";
    const auto timed_edit = with(base_with_time, [](auto& c) { c.summary = "Open late on Saturdays."; });
    const auto rescheduled = with(base_with_time, [](auto& c) { c.scheduled_time = "2026-12-20T17:00:00.000000Z"; });
    EXPECT_THAT(domain::local_post_edit_request(base_with_time, timed_edit, rescheduled).value(), ElementsAre("summary"));
    // A post that came to hold what a draft cannot hold is not written to.
    auto alert = base;
    alert.topic_type = "alert";
    EXPECT_EQ(domain::local_post_edit_request(base, edit, alert).error().kind, Kind::not_editable);
}

TEST(LocalPostEditRequest, TheEditAsItIsShownIsThePostAsItIsNowWithTheChangedFieldsLaidOver) {
    auto base = a_base();
    base.scheduled_time = "2026-12-19T17:00:00.000000Z";
    const auto edit = with(base, [](auto& c) { c.call_to_action->url = "https://example.com/reserve"; });
    // Nothing changed at Google: the edit as it was written.
    EXPECT_EQ(domain::local_post_with_edit(base, edit, base), edit);
    // Everything the edit leaves alone has changed at Google since: all of it shows, under the edit's one change.
    auto now = base;
    now.summary = "Open late on Fridays and Saturdays.";
    now.language_code = "en-US";
    now.media_ids = {9};
    now.scheduled_time = "2026-12-20T17:00:00.000000Z";
    auto expected = now;
    expected.call_to_action->url = "https://example.com/reserve";
    EXPECT_EQ(domain::local_post_with_edit(base, edit, now), expected);
    EXPECT_EQ(domain::local_post_with_edit(base, edit, now).summary, "Open late on Fridays and Saturdays.")
        << "the old text the edit was sent with is not the edit's";
    // The field the edit changes is the edit's, whatever Google holds there now.
    now.call_to_action.reset();
    EXPECT_EQ(domain::local_post_with_edit(base, edit, now).call_to_action->url, "https://example.com/reserve");
    // An edit that changes nothing shows the post as it is, and a field taken away is taken away.
    EXPECT_EQ(domain::local_post_with_edit(base, base, expected), expected);
    const auto no_upload = with(base, [](auto& c) { c.media_ids.clear(); });
    EXPECT_TRUE(domain::local_post_with_edit(base, no_upload, expected).media_ids.empty());
    // What is sent for it is the same question asked of the same three: only the changed field.
    EXPECT_THAT(domain::local_post_edit_request(base, edit, expected).value(), IsEmpty()) << "Google has it";
    auto elsewhere = expected;
    elsewhere.call_to_action = base.call_to_action;
    EXPECT_THAT(domain::local_post_edit_request(base, edit, elsewhere).value(), ElementsAre("callToAction"));
}

TEST(LocalPostEditRequest, AnAnswerIsComparedInTheFieldsThatWereSentAndInNothingElse) {
    const auto sent = with(an_offer(), [](auto& c) {
        c.summary = "Three for two.";
        c.event->title = "Lunch fortnight";
    });
    const std::vector<std::string_view> mask{"summary", "event"};
    EXPECT_THAT(domain::edit_fields_not_shown(sent, mask, sent), IsEmpty());
    // The answer says the text and not the title.
    auto half = sent;
    half.event->title = "Lunch week";
    EXPECT_THAT(domain::edit_fields_not_shown(sent, mask, half), ElementsAre("event"));
    half.summary = "Two for one.";
    EXPECT_THAT(domain::edit_fields_not_shown(sent, mask, half), ElementsAre("summary", "event"));
    // Whatever else the answer says or lacks is not what was sent: another kind, time, language, offer, or no button.
    auto other = sent;
    other.topic_type = "alert";
    other.scheduled_time = "2031-01-01T00:00:00.000000Z";
    other.language_code = "fr";
    other.offer.reset();
    other.media_ids = {9};
    EXPECT_THAT(domain::edit_fields_not_shown(sent, mask, other), IsEmpty());
    // An answer without the event at all does not show the title either; space around a text is no difference.
    auto bare = sent;
    bare.event.reset();
    bare.summary = " Three for two.\n";
    EXPECT_THAT(domain::edit_fields_not_shown(sent, mask, bare), ElementsAre("event"));
    // The media is never compared: Google answers with addresses of its own.
    auto with_media = sent;
    with_media.media_ids = {7};
    EXPECT_THAT(domain::edit_fields_not_shown(with_media, {"media"}, sent), IsEmpty());
}

TEST(LocalPostEditChecks, AnEditIsNotCheckedForTheTimeAndTheRepeatItDoesNotSend) {
    // A post that was scheduled and is live, and repeats without an end as a post made at Google may.
    auto content = test::standard("Live music every Friday.");
    content.topic_type = "event";
    content.event = event("Friday live music");
    content.event->recurrence = domain::LocalPostRecurrence{.frequency = "weekly", .until = std::nullopt};
    content.scheduled_time = "2026-11-01T17:00:00.000000Z";
    const auto failed = [&](bool edit) {
        return domain::run_local_post_checks(content, {.now = kNow, .media_problem = std::nullopt, .edit_at_google = edit}).failed();
    };
    EXPECT_THAT(failed(false), ElementsAre("recurrence_end", "scheduled_in_future")) << "a draft with these could not be approved";
    EXPECT_THAT(failed(true), IsEmpty());

    // Every other check holds for an edit as for a draft, and the list is the same twelve.
    content.summary = "Call (212) 555-0100 to book.";
    content.event->schedule.end_date = "2026-11-30";
    EXPECT_THAT(failed(true), ElementsAre("no_phone", "event_dates"));
    EXPECT_EQ(domain::run_local_post_checks(content, {.now = kNow, .media_problem = std::nullopt, .edit_at_google = true}).results.size(), 12u);
    const auto with_media = domain::run_local_post_checks(test::standard("With a picture."),
                                                          {.now = kNow, .media_problem = "the uploaded file no longer exists", .edit_at_google = true});
    EXPECT_TRUE(with_media.passed()) << "no upload named, nothing to use";
    auto names_one = test::standard("With a picture.");
    names_one.media_ids = {4};
    EXPECT_THAT(domain::run_local_post_checks(names_one, {.now = kNow, .media_problem = "the uploaded file no longer exists", .edit_at_google = true})
                    .failed(),
                ElementsAre("media_usable"));
}
