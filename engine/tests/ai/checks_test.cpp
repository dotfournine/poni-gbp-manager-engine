// Every mechanical check with fixed inputs (architecture section 10): clean
// replies pass all of them, and a reply with a link, an email address or a
// phone number fails the check that exists for it. The checks are plain
// code since D-058; what they no longer catch is asserted here too.

#include "ai_fixture.hpp"
#include "domain/reply_checks.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace poni;
using domain::CheckReport;
using testing::Contains;
using testing::IsEmpty;
using testing::Not;

namespace {

class Checks : public ::testing::Test {
protected:
    CheckReport check(std::string_view client, std::string_view reply, std::string language = "en", std::string mode = "positive") {
        const auto setup = seed_.client(client);
        return check_with(setup.settings, reply, std::move(language), std::move(mode));
    }

    static CheckReport check_with(const domain::ResolvedSettings& settings, std::string_view reply,
                                  std::string language = "en", std::string mode = "positive") {
        const domain::CheckContext context{.settings = settings,
                                           .target_language = std::move(language),
                                           .mode = std::move(mode)};
        return domain::run_reply_checks(reply, context);
    }

    static std::string detail(const CheckReport& report, std::string_view name) {
        for (const auto& result : report.results) {
            if (result.name == name) return result.detail;
        }
        return "(no such check)";
    }

    test::KnowledgeFiles seed_;
};

// The made-up businesses of the two packs (knowledge/industries/<key>/test_client.json).
constexpr std::string_view kSpa = "wellness";
constexpr std::string_view kBuffet = "food_beverage";

}  // namespace

// ---- clean replies ---------------------------------------------------------------

TEST_F(Checks, CleanRepliesPassEveryCheck) {
    EXPECT_THAT(check(kSpa, "Thank you so much for your lovely review! We'll share your kind words with Eva. Hope to welcome you back soon 🤍").failed(), IsEmpty());
    EXPECT_THAT(check(kSpa,
                      "Thank you for taking the time to share this, and we're truly sorry the head spa felt rushed. Please "
                      "feel free to reach out to us directly so our team can learn more and look into it.",
                      "en", "complaint")
                    .failed(),
                IsEmpty());
    EXPECT_THAT(check(kSpa, "Thank you for choosing Willow Moon Head Spa! We're so glad your Head Spa with Lily was relaxing. Hope to see you soon 🤍").failed(), IsEmpty());
    EXPECT_THAT(check(kBuffet, "Thanks for taking a chance on us! We're glad you enjoyed the sushi and the noodle soup station. Hope to welcome you back soon.").failed(), IsEmpty());
    EXPECT_THAT(check(kBuffet, "感谢您的五星好评！很高兴您喜欢我们的寿司和海鲜，期待您再次光临。", "zh").failed(), IsEmpty());
    EXPECT_THAT(check(kSpa, "謝謝您的五星好評！很開心您喜歡我們的環境和頭療體驗，期待下次再為您服務。", "zh").failed(), IsEmpty());
    EXPECT_THAT(check(kBuffet, "¡Muchas gracias por su reseña! Nos alegra que haya disfrutado de la comida y del servicio. Esperamos verle pronto.", "es").failed(), IsEmpty());
}

TEST_F(Checks, EveryCheckRunsAndIsNamed) {
    const auto report = check(kBuffet, "感谢您的五星好评！很高兴您喜欢我们的寿司和海鲜，期待您再次光临。", "zh");
    std::vector<std::string> names;
    for (const auto& result : report.results) names.push_back(result.name);
    EXPECT_THAT(names, testing::ElementsAre("not_empty", "no_url", "no_email", "no_phone", "emoji_limit", "length", "language_match"));
    EXPECT_TRUE(report.passed());
}

TEST(CheckReport, TheFailedChecksAreNamedInOneLine) {
    const CheckReport report{.results = {{"no_url", false, "x"}, {"length", true, ""}, {"language_match", false, "y"}}};
    EXPECT_EQ(report.failed_names(), "no_url, language_match");
    EXPECT_EQ(CheckReport{}.failed_names(), "");
}

TEST_F(Checks, AnEmptyReplyFails) {
    EXPECT_THAT(check(kSpa, "").failed(), Contains("not_empty"));
    EXPECT_THAT(check(kSpa, "  \n ").failed(), Contains("not_empty"));
}

// ---- contact details -------------------------------------------------------------

TEST_F(Checks, UrlsFail) {
    for (const auto* reply : {"Thank you! Visit ludream.example to book again.",
                              "Thanks! Book at https://bit.ly/abc123 anytime.",
                              "Thanks! See www.gangnam-buffet.com for hours.",
                              "Thanks! Book at ｗｗｗ．ｌｕｄｒｅａｍ．ｃｏｍ today.",
                              "Thanks! Book at ludream\u200B.example today.",
                              "谢谢！请访问 ludream.com 预约。"}) {
        EXPECT_THAT(check(kSpa, reply).failed(), Contains("no_url")) << reply;
    }
    // Plain code finds a written-out address, not one that is spelled out in words (D-058).
    EXPECT_THAT(check(kSpa, "Thanks! Find us at gangnambuffet dot com.").failed(), Not(Contains("no_url")));
    // A missing space after a full stop is not a domain, and neither is "e.g.".
    const auto ok = check(kSpa, "Thank you for visiting us, and for the kind words about Eva.Hope to welcome you back soon, e.g. for a head spa!");
    EXPECT_THAT(ok.failed(), Not(Contains("no_url"))) << detail(ok, "no_url");
}

TEST_F(Checks, EmailAddressesFail) {
    for (const auto* reply : {"Thanks! Email manager@gangnam.example with questions.", "Thanks! Write to first.last+spa@mail.ludream.example.",
                              "Thanks! Write to hello＠ludream.example."}) {
        EXPECT_THAT(check(kSpa, reply).failed(), Contains("no_email")) << reply;
    }
    // An "@" that isn't an address, and an address spelled out in words (not caught any more, D-058).
    EXPECT_THAT(check(kSpa, "Thank you! See you @ the spa soon.").failed(), Not(Contains("no_email")));
    EXPECT_THAT(check(kSpa, "Thank you! Write to hello [at] ludream dot example.").failed(), Not(Contains("no_email")));
}

TEST_F(Checks, PhoneNumbersFail) {
    for (const auto* reply : {"Thank you! Call us at 646 555 0100 anytime.", "Thanks! Our number is (703) 555-0123.",
                              "Thanks! Call +1 212-555-0147.", "Thanks! Call ７０３－５５５－０１００ today.",
                              "谢谢！电话七〇三五五五零一零零。"}) {
        EXPECT_THAT(check(kSpa, reply).failed(), Contains("no_phone")) << reply;
    }
    const auto ok = check(kSpa, "Thank you for the 5-star rating! We're glad all 3 of you enjoyed the head spa. See you soon 🤍");
    EXPECT_THAT(ok.failed(), Not(Contains("no_phone"))) << detail(ok, "no_phone");
}

TEST_F(Checks, ContactDetailsPassWhenTheReplySettingsAllowThem) {
    auto settings = seed_.client(kSpa).settings;
    settings.reply.contact_allowed = true;
    const auto report = check_with(settings, "Thank you! Call us at 646 555 0100 or visit willowmoon.example to book again.");
    EXPECT_THAT(report.failed(), Not(Contains("no_phone")));
    EXPECT_THAT(report.failed(), Not(Contains("no_url")));
}

// ---- emoji, length, language -----------------------------------------------------

TEST_F(Checks, EmojiAreLimitedByTheVoice) {
    // The spa allows two; the buffet none.
    EXPECT_THAT(check(kSpa, "Thank you so much for visiting us! Hope to welcome you back soon 🤍✨").failed(), Not(Contains("emoji_limit")));
    EXPECT_THAT(check(kSpa, "Thank you so much for visiting us! Hope to welcome you back soon 🤍✨💕").failed(), Contains("emoji_limit"));
    EXPECT_THAT(check(kBuffet, "Thanks! 😊😊😊").failed(), Contains("emoji_limit"));
    EXPECT_THAT(check(kBuffet, "Thank you for the kind review! We hope to welcome you back soon 😊").failed(), Contains("emoji_limit"));
}

TEST_F(Checks, LengthFollowsTheModeAndTheLanguage) {
    EXPECT_THAT(check(kBuffet, "Thanks!").failed(), Contains("length"));
    const std::string too_long(700, 'a');
    EXPECT_THAT(check(kBuffet, "Thank you. " + too_long).failed(), Contains("length"));
    // A short thank-you fits a rating-only review but not a text review.
    const std::string short_reply = "Thank you for the five stars! See you soon.";
    EXPECT_THAT(check(kBuffet, short_reply, "en", "rating_only").failed(), Not(Contains("length")));
    EXPECT_THAT(check(kBuffet, short_reply, "en", "rating_only_low").failed(), Not(Contains("length")));
    EXPECT_THAT(check(kBuffet, short_reply, "en", "positive").failed(), Contains("length"));
    // Chinese limits count characters, not bytes.
    EXPECT_THAT(check(kBuffet, "感谢您的五星好评！很高兴您喜欢我们的寿司和海鲜，期待您再次光临。", "zh").failed(), Not(Contains("length")));
    EXPECT_THAT(check(kBuffet, "谢谢！", "zh").failed(), Contains("length"));
}

TEST_F(Checks, TheReplyMustBeInTheTargetLanguage) {
    EXPECT_THAT(check(kSpa, "谢谢您的好评！期待再次光临。").failed(), Contains("language_match"));
    EXPECT_THAT(check(kSpa, "¡Muchas gracias por su reseña! Nos alegra que haya disfrutado su visita. Esperamos verle pronto.").failed(), Contains("language_match"));
    EXPECT_THAT(check(kBuffet, "Thank you so much for your wonderful review! Hope to welcome you back soon.", "es").failed(), Contains("language_match"));
    EXPECT_THAT(check(kBuffet, "Thank you so much for your wonderful review! Hope to welcome you back soon.", "zh").failed(), Contains("language_match"));
}

// D-056: nothing converts or verifies a Chinese reply's script any more.
TEST_F(Checks, TheScriptOfAChineseReplyIsNotChecked) {
    // One Simplified character (为) in a Traditional reply passes as the model wrote it.
    const auto report = check(kSpa, "謝謝您的五星好評！很開心您喜歡我們的環境和頭療體驗，期待下次再为您服務。", "zh");
    EXPECT_TRUE(report.passed());
    for (const auto& result : report.results) EXPECT_NE(result.name, "script_match");
}
