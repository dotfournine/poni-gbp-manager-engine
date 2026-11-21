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
    post.media_id = 7;
    EXPECT_THAT(mask_after(post, [](auto& c) { c.summary = "Fall specials start Tuesday."; }), ElementsAre("summary"));
    EXPECT_THAT(mask_after(post, [](auto& c) { c.call_to_action->url = "https://example.com/reserve"; }), ElementsAre("callToAction"));
    EXPECT_THAT(mask_after(post, [](auto& c) { c.call_to_action->action_type = "order"; }), ElementsAre("callToAction"));
    EXPECT_THAT(mask_after(post, [](auto& c) { c.call_to_action.reset(); }), ElementsAre("callToAction")) << "the button taken away";
    EXPECT_THAT(mask_after(post, [](auto& c) { c.language_code = "es"; }), ElementsAre("languageCode"));
    EXPECT_THAT(mask_after(post, [](auto& c) { c.media_id = 8; }), ElementsAre("media"));

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
    post.media_id = 7;
    const auto everything = mask_after(post, [](auto& c) {
        c.language_code = "zh-Hant";
        c.media_id = 9;
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
    // Every name a mask can hold is one of the six, and the outbound client takes each of them and no other:
    // its own list, held apart on purpose, is the same six in the same order.
    EXPECT_TRUE(std::ranges::equal(domain::kLocalPostEditFields, http::kLocalPostUpdateFields));
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
    post.media_id = 7;
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
    EXPECT_THAT(mask_after(made_at_google, [](auto& c) { c.media_id = 4; }), ElementsAre("media")) << "a person gave it an upload";

    auto with_upload = test::standard("A look at the new room.");
    with_upload.media_id = 7;
    EXPECT_THAT(mask_after(with_upload, [](auto& c) { c.summary = "A look at the new rooms."; }), ElementsAre("summary"))
        << "the post's own upload, named again, is no change";
    EXPECT_THAT(mask_after(with_upload, [](auto& c) { c.media_id.reset(); }), ElementsAre("media")) << "the upload taken away";
    EXPECT_THAT(mask_after(with_upload, [](auto& c) { c.media_id = 8; }), ElementsAre("media"));
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
    names_one.media_id = 4;
    EXPECT_THAT(domain::run_local_post_checks(names_one, {.now = kNow, .media_problem = "the uploaded file no longer exists", .edit_at_google = true})
                    .failed(),
                ElementsAre("media_usable"));
}
