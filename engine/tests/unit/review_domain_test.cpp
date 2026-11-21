// What the engine derives from a review on its own, and how it decides what
// changed between two versions of a review.

#include "domain/review_sync.hpp"
#include "domain/review_text.hpp"

#include <gtest/gtest.h>

#include <algorithm>

using namespace poni;
using domain::ReviewSnapshot;

namespace {

std::vector<std::string> kinds(const std::vector<domain::ReviewEvent>& events) {
    std::vector<std::string> out;
    for (const auto& event : events) out.push_back(event.kind);
    return out;
}

ReviewSnapshot snapshot() {
    return {.rating = 5,
            .comment = "Great",
            .updated_time = "2026-11-01T10:00:00.000000Z",
            .reply_text = std::nullopt,
            .reply_updated_time = std::nullopt,
            .removed = false};
}

}  // namespace

TEST(SplitTranslation, SeparatesGooglesTranslationFromTheOriginal) {
    const auto parts = domain::split_translation(
        "(Translated by Google) A very comfortable experience\n\n(Original)\n很舒服的一次体验");
    EXPECT_EQ(parts.original, "很舒服的一次体验");
    EXPECT_EQ(parts.translated, "A very comfortable experience");
}

TEST(SplitTranslation, HandlesTheReverseOrder) {
    const auto parts = domain::split_translation("Muy buena comida\n\n(Translated by Google)\nVery good food");
    EXPECT_EQ(parts.original, "Muy buena comida");
    EXPECT_EQ(parts.translated, "Very good food");
}

TEST(SplitTranslation, LeavesOrdinaryTextAlone) {
    const auto plain = domain::split_translation("Loved it. (Original) recipe, they said.");
    EXPECT_EQ(plain.original, "Loved it. (Original) recipe, they said.");
    EXPECT_FALSE(plain.translated);

    // The marker without an original part is not Google's format.
    const auto odd = domain::split_translation("(Translated by Google) only this");
    EXPECT_EQ(odd.original, "(Translated by Google) only this");
    EXPECT_FALSE(odd.translated);
}

TEST(SplitTranslation, UsesTheLastOriginalMarker) {
    // A reviewer can type "(Original)" too; Google's own marker comes last.
    const auto parts = domain::split_translation("(Translated by Google) The (Original) one\n\n(Original)\n原版");
    EXPECT_EQ(parts.original, "原版");
    EXPECT_EQ(parts.translated, "The (Original) one");
}

TEST(GuessLanguage, GoesByScript) {
    EXPECT_EQ(domain::guess_language("Great food and friendly staff!", false), "en");
    EXPECT_EQ(domain::guess_language("很舒服的一次体验", true), "zh");
    EXPECT_EQ(domain::guess_language("服务很好! Great massage 👍", false), "zh") << "a few characters carry a lot";
    EXPECT_EQ(domain::guess_language("とても美味しかったです", true), "ja");
    EXPECT_EQ(domain::guess_language("정말 맛있어요", true), "ko");
    EXPECT_EQ(domain::guess_language("Очень вкусно", true), "other");
}

TEST(GuessLanguage, TellsSpanishAndFrenchApartWhenGoogleTranslated) {
    EXPECT_EQ(domain::guess_language("La comida es muy buena y el servicio excelente", true), "es");
    EXPECT_EQ(domain::guess_language("Le service est très bien et les plats sont bons", true), "fr");
    EXPECT_EQ(domain::guess_language("Sehr gutes Essen", true), "other");
}

TEST(GuessLanguage, HasNoAnswerWithoutLetters) {
    EXPECT_FALSE(domain::guess_language("", false));
    EXPECT_FALSE(domain::guess_language("👍👍 10/10", false));
    EXPECT_FALSE(domain::guess_language("\xff\xfe\x80", false)) << "invalid UTF-8 must not crash or loop";
}

TEST(StarRating, MapsGooglesNames) {
    EXPECT_EQ(domain::parse_star_rating("ONE"), 1);
    EXPECT_EQ(domain::parse_star_rating("FIVE"), 5);
    EXPECT_FALSE(domain::parse_star_rating("STAR_RATING_UNSPECIFIED"));
    EXPECT_FALSE(domain::parse_star_rating("five"));
}

TEST(CanonicalTimestamp, PadsAndTruncatesTheFraction) {
    EXPECT_EQ(domain::canonical_timestamp("2026-07-19T22:34:20.376896Z"), "2026-07-19T22:34:20.376896Z");
    EXPECT_EQ(domain::canonical_timestamp("2026-07-19T22:34:20Z"), "2026-07-19T22:34:20.000000Z");
    EXPECT_EQ(domain::canonical_timestamp("2026-07-19T22:34:20.5Z"), "2026-07-19T22:34:20.500000Z");
    EXPECT_EQ(domain::canonical_timestamp("2026-07-19T22:34:20.123456789Z"), "2026-07-19T22:34:20.123456Z");
}

TEST(CanonicalTimestamp, RejectsAnythingElse) {
    for (const auto* bad : {"", "2026-07-19", "2026-07-19T22:34:20", "2026-07-19T22:34:20+02:00", "2026-13-19T22:34:20Z",
                            "2026-07-19T25:34:20Z", "2026-07-19T22:34:20.Z", "2026-07-19T22:34:20.12x4Z",
                            "'; DROP TABLE reviews; --"}) {
        EXPECT_FALSE(domain::canonical_timestamp(bad)) << bad;
    }
}

TEST(DiffReview, SamePayloadMeansNoEvents) {
    EXPECT_TRUE(domain::diff_review(snapshot(), snapshot()).empty());
}

TEST(DiffReview, NoticesAnEditByTheReviewer) {
    auto incoming = snapshot();
    incoming.rating = 2;
    incoming.comment = "Changed my mind";
    incoming.updated_time = "2026-11-02T10:00:00.000000Z";
    const auto events = domain::diff_review(snapshot(), incoming);
    ASSERT_EQ(kinds(events), std::vector<std::string>{"review_edited"});
    EXPECT_EQ(events[0].detail.at("before").at("rating").as_int64(), 5);
    EXPECT_EQ(events[0].detail.at("after").at("comment").as_string(), "Changed my mind");
}

TEST(DiffReview, NoticesAReplyPostedOutsideTheSystem) {
    auto incoming = snapshot();
    incoming.reply_text = "Thank you!";
    incoming.reply_updated_time = "2026-11-03T09:00:00.000000Z";
    const auto events = domain::diff_review(snapshot(), incoming);
    ASSERT_EQ(kinds(events), std::vector<std::string>{"reply_added"});
    EXPECT_EQ(events[0].detail.at("text").as_string(), "Thank you!");
}

TEST(DiffReview, NoticesAChangedAndARemovedReply) {
    auto stored = snapshot();
    stored.reply_text = "Thanks";
    stored.reply_updated_time = "2026-11-03T09:00:00.000000Z";

    auto edited = stored;
    edited.reply_text = "Thanks a lot";
    edited.reply_updated_time = "2026-11-04T09:00:00.000000Z";
    EXPECT_EQ(kinds(domain::diff_review(stored, edited)), std::vector<std::string>{"reply_changed"});

    auto gone = stored;
    gone.reply_text.reset();
    gone.reply_updated_time.reset();
    EXPECT_EQ(kinds(domain::diff_review(stored, gone)), std::vector<std::string>{"reply_removed"});
}

TEST(DiffReview, AReviewThatComesBackIsRestored) {
    auto stored = snapshot();
    stored.removed = true;
    EXPECT_EQ(kinds(domain::diff_review(stored, snapshot())), std::vector<std::string>{"restored"});
}
