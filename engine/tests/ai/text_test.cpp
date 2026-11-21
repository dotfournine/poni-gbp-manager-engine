// The UTF-8 helpers and the language detection the checks and the analysis stand on.

#include "ai_fixture.hpp"
#include "core/text.hpp"
#include "domain/language.hpp"

#include <gtest/gtest.h>

using namespace poni;

TEST(Text, FoldTurnsFullWidthFormsIntoAscii) {
    EXPECT_EQ(text::encode(text::fold("ｗｗｗ．ｅｘａｍｐｌｅ．ｃｏｍ")), "www.example.com");
    EXPECT_EQ(text::encode(text::fold("７０３－５５５－０１００")), "703-555-0100");
    EXPECT_EQ(text::encode(text::fold("ａ＠ｂ．ｃｏ")), "a@b.co");
    EXPECT_EQ(text::encode(text::fold("example。com")), "example.com");
    // Styled letters are not folded: that needed Unicode normalization (utf8proc), which is gone (D-058).
    EXPECT_EQ(text::encode(text::fold("𝐟𝐫𝐞𝐞 dessert")), "𝐟𝐫𝐞𝐞 dessert");
}

TEST(Text, FoldDropsZeroWidthCharacters) {
    EXPECT_EQ(text::encode(text::fold("I\u200Bg\u200Bn\u200Bo\u200Br\u200Be this")), "Ignore this");
    EXPECT_EQ(text::encode(text::fold("\uFEFFfree\u2060 meal\u200D")), "free meal");
}

TEST(Text, InvalidUtf8IsReplacedNotFatal) {
    const std::string broken = "ok \xff\xfe end";
    EXPECT_EQ(text::length(broken), 9u);
    const auto folded = text::encode(text::fold(broken));
    EXPECT_NE(folded.find("ok "), std::string::npos);
    EXPECT_NE(folded.find(" end"), std::string::npos);
    // Overlong forms, surrogates and cut-off sequences are invalid bytes, not characters.
    EXPECT_EQ(text::decode("\xC0\xAF"), std::u32string(2, U'\uFFFD'));
    EXPECT_EQ(text::decode("\xED\xA0\x80"), std::u32string(3, U'\uFFFD'));
    EXPECT_EQ(text::decode("a\xE8\xB0"), std::u32string(U"a\uFFFD\uFFFD"));
    EXPECT_EQ(text::encode(text::decode("谢谢 🤍 café")), "谢谢 🤍 café");
}

TEST(Text, LengthCountsCharactersNotBytes) {
    EXPECT_EQ(text::length("谢谢您的好评"), 6u);
    EXPECT_EQ(text::length("café"), 4u);
    EXPECT_EQ(text::length(""), 0u);
}

TEST(Text, TrimAndTruncate) {
    EXPECT_EQ(text::trim("  \n hello \t"), "hello");
    EXPECT_EQ(text::trim("　谢谢 "), "谢谢");
    EXPECT_EQ(text::trim("   "), "");
    EXPECT_EQ(text::truncate("谢谢您的好评", 3), "谢谢您…");
    EXPECT_EQ(text::truncate("short", 10), "short");
    EXPECT_EQ(text::ascii_lower("Crab LEGS Ä"), "crab legs Ä") << "only A to Z";
    EXPECT_TRUE(text::ascii_iequals("HTTPS://", "https://"));
    EXPECT_FALSE(text::ascii_iequals("https:/", "https://"));
}

TEST(Language, DetectsTheLanguagesRepliesAreWrittenIn) {
    EXPECT_EQ(domain::detect_language("Thank you so much for your review! Hope to see you soon."), "en");
    EXPECT_EQ(domain::detect_language("¡Muchas gracias por su reseña! Esperamos verle pronto."), "es");
    EXPECT_EQ(domain::detect_language("谢谢您的好评！期待再次光临。"), "zh");
    EXPECT_EQ(domain::detect_language("謝謝您的五星好評！"), "zh");
    EXPECT_EQ(domain::detect_language("정말 맛있었어요 감사합니다"), "ko");
    EXPECT_EQ(domain::detect_language("とても美味しかったです"), "ja");
    EXPECT_EQ(domain::detect_language("👍👍 10/10"), "none");
    EXPECT_EQ(domain::detect_language(""), "none");
    // Mostly Chinese by content, though the Latin letters outnumber the characters.
    EXPECT_EQ(domain::detect_language("Food is good and the price is ok. 但是我老公吃完寿司以后肚子疼了一晚上。"), "zh");
}

TEST(Language, CountsEmoji) {
    EXPECT_EQ(domain::count_emoji("Thanks! 😊😊😊"), 3u);
    EXPECT_EQ(domain::count_emoji("Hope to welcome you back 🤍"), 1u);
    EXPECT_EQ(domain::count_emoji("谢谢您的五星好评！💕 ✨"), 2u);
    EXPECT_EQ(domain::count_emoji("no emoji here ~ ！"), 0u);
}

// Code can say "these are Chinese characters"; which script they are in is the analyzer's answer (D-056).
TEST(Language, TheScriptIsToldWithoutSayingWhichChinese) {
    EXPECT_EQ(domain::detect_script("環境很舒服，技師手法很專業"), "Hani");
    EXPECT_EQ(domain::detect_script("环境很好"), "Hani");
    EXPECT_EQ(domain::detect_script("Great place"), "Latn");
    EXPECT_EQ(domain::detect_script("감사합니다"), "Hang");
    EXPECT_EQ(domain::detect_script("12345 !!"), "none");
}
