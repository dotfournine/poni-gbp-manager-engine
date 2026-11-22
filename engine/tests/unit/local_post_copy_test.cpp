// What a copy of a post takes into a new draft (D-094): domain::fit_for_draft,
// without a database. What it leaves out is named for the person who reads
// the draft, and what it gives back is a draft the engine stores.

#include "domain/local_post.hpp"
#include "fakes.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace poni;
using namespace std::chrono_literals;
using testing::ElementsAre;
using testing::HasSubstr;
using testing::IsEmpty;

namespace {

// 2026-11-20T12:00:00Z
const core::TimePoint kNow = std::chrono::sys_days{std::chrono::year{2026} / 11 / 20} + 12h;

domain::LocalPostEvent event(std::string title = "Winter tasting") {
    return {.title = std::move(title),
            .schedule = {.start_date = "2026-12-01", .start_time = "18:00", .end_date = "2026-12-01", .end_time = "21:00"},
            .recurrence = std::nullopt};
}

// The copy is a draft the engine takes: normalize_local_post refuses nothing in it.
void expect_storable(const domain::DraftCopy& copy) {
    const auto normalized = domain::normalize_local_post(copy.content);
    EXPECT_TRUE(normalized) << (normalized ? "" : normalized.error());
}

}  // namespace

TEST(LocalPostCopy, APostWrittenHereIsCopiedWholeWithItsUpload) {
    auto content = test::standard("Fall specials start Monday.");
    content.call_to_action = domain::LocalPostCallToAction{.action_type = "book", .url = "https://example.com/book"};
    content.media_ids = {7};
    content.scheduled_time = "2026-12-19T17:00:00.000000Z";
    const auto copy = domain::fit_for_draft(content, kNow, {});
    EXPECT_EQ(copy.content, content);
    EXPECT_THAT(copy.left_out, IsEmpty());

    auto offer = test::standard("Two for one.");
    offer.topic_type = "offer";
    offer.event = event();
    offer.offer = domain::LocalPostOffer{.coupon_code = "TWO", .redeem_online_url = std::nullopt, .terms_conditions = "Dine-in only."};
    EXPECT_EQ(domain::fit_for_draft(offer, kNow, {}).content, offer);
}

TEST(LocalPostCopy, AnAlertBecomesAnUpdate) {
    auto content = test::standard("Closed for the storm today.");
    content.topic_type = "alert";
    const auto copy = domain::fit_for_draft(content, kNow, {});
    EXPECT_EQ(copy.content.topic_type, "standard");
    EXPECT_EQ(copy.content.summary, "Closed for the storm today.");
    EXPECT_THAT(copy.left_out, ElementsAre(HasSubstr("\"alert\"")));
    expect_storable(copy);

    // An alert Google gave an event to: the update has none.
    content.event = event();
    const auto with_event = domain::fit_for_draft(content, kNow, {});
    EXPECT_FALSE(with_event.content.event);
    EXPECT_THAT(with_event.left_out, ElementsAre(HasSubstr("\"alert\""), HasSubstr("the event")));
    expect_storable(with_event);
}

TEST(LocalPostCopy, AnEventOrOfferWhoseDatesGoogleLacksBecomesAnUpdate) {
    auto content = test::standard("Two for one.");
    content.topic_type = "offer";
    content.offer = domain::LocalPostOffer{.coupon_code = "TWO", .redeem_online_url = std::nullopt, .terms_conditions = std::nullopt};
    const auto copy = domain::fit_for_draft(content, kNow, {});
    EXPECT_EQ(copy.content.topic_type, "standard");
    EXPECT_FALSE(copy.content.offer);
    EXPECT_THAT(copy.left_out, ElementsAre(HasSubstr("whose dates Google's post lacks"), HasSubstr("the offer's code")));
    expect_storable(copy);
}

TEST(LocalPostCopy, AButtonADraftCannotHaveIsDropped) {
    auto content = test::standard("Order ahead.");
    content.call_to_action = domain::LocalPostCallToAction{.action_type = "get_offer", .url = "https://example.com"};
    const auto unknown = domain::fit_for_draft(content, kNow, {});
    EXPECT_FALSE(unknown.content.call_to_action);
    EXPECT_THAT(unknown.left_out, ElementsAre(HasSubstr("the button \"get_offer\"")));
    expect_storable(unknown);

    // A call button dials the profile's number: an address on it goes, the button stays.
    content.call_to_action = domain::LocalPostCallToAction{.action_type = "call", .url = "https://example.com"};
    const auto call = domain::fit_for_draft(content, kNow, {});
    ASSERT_TRUE(call.content.call_to_action);
    EXPECT_EQ(call.content.call_to_action->action_type, "call");
    EXPECT_FALSE(call.content.call_to_action->url);
    EXPECT_THAT(call.left_out, ElementsAre(HasSubstr("call button's address")));
    expect_storable(call);

    // Google ignores a button on an offer, and a draft can't have one.
    auto offer = test::standard("Two for one.");
    offer.topic_type = "offer";
    offer.event = event();
    offer.offer = domain::LocalPostOffer{};
    offer.call_to_action = domain::LocalPostCallToAction{.action_type = "shop", .url = "https://example.com"};
    const auto on_offer = domain::fit_for_draft(offer, kNow, {});
    EXPECT_FALSE(on_offer.content.call_to_action);
    EXPECT_THAT(on_offer.left_out, ElementsAre(HasSubstr("an offer does not have")));
    expect_storable(on_offer);
}

// Times as the engine stores them (domain::canonical_timestamp), with six digits of fraction.
TEST(LocalPostCopy, ATimeToPublishAtThatHasPassedIsDropped) {
    auto content = test::standard();
    content.scheduled_time = "2026-11-19T17:00:00.000000Z";
    const auto passed = domain::fit_for_draft(content, kNow, {});
    EXPECT_FALSE(passed.content.scheduled_time);
    EXPECT_THAT(passed.left_out, ElementsAre(HasSubstr("has passed")));

    // As the check scheduled_in_future counts it: the moment itself is no longer in the future.
    content.scheduled_time = "2026-11-20T12:00:00.000000Z";
    EXPECT_FALSE(domain::fit_for_draft(content, kNow, {}).content.scheduled_time);
    content.scheduled_time = "2026-11-20T12:00:00.000001Z";
    const auto ahead = domain::fit_for_draft(content, kNow, {});
    EXPECT_EQ(ahead.content.scheduled_time, "2026-11-20T12:00:00.000001Z");
    EXPECT_THAT(ahead.left_out, IsEmpty());
}

TEST(LocalPostCopy, GooglesCopyOfAPhotoOrAVideoIsNotAnUpload) {
    const auto photo = domain::fit_for_draft(test::standard(), kNow, {"photo"});
    EXPECT_TRUE(photo.content.media_ids.empty());
    EXPECT_THAT(photo.left_out, ElementsAre("Google's copy of the photo: upload the photo again"));
    const auto video = domain::fit_for_draft(test::standard(), kNow, {"video"});
    EXPECT_THAT(video.left_out, ElementsAre("Google's copy of the video: upload the video again"));
    // Several, as a post made at Google can show (D-101).
    EXPECT_THAT(domain::fit_for_draft(test::standard(), kNow, {"photo", "photo", "photo"}).left_out,
                ElementsAre("Google's copies of the 3 photos: upload them again"));
    EXPECT_THAT(domain::fit_for_draft(test::standard(), kNow, {"video", "video"}).left_out,
                ElementsAre("Google's copies of the 2 videos: upload them again"));
    EXPECT_THAT(domain::fit_for_draft(test::standard(), kNow, {"photo", "video"}).left_out,
                ElementsAre("Google's copies of the 2 photos and videos: upload them again"));

    // A post sent from here shows Google's copy of its upload too: the upload is what the copy keeps.
    auto sent = test::standard();
    sent.media_ids = {7, 3};
    const auto kept = domain::fit_for_draft(sent, kNow, {"photo", "video"});
    EXPECT_EQ(kept.content.media_ids, (std::vector<std::int64_t>{7, 3})) << "every upload, in its order";
    EXPECT_THAT(kept.left_out, IsEmpty());
}
