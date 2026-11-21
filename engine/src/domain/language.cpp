#include "domain/language.hpp"

#include "core/text.hpp"

#include <algorithm>
#include <array>

namespace poni::domain {

namespace {

// Frequent words that tell English from Spanish in a short text.
constexpr std::array<std::string_view, 44> kEnglish{
    "the", "and", "you", "your", "we", "to", "of", "for", "our", "is", "it", "in", "with", "thank", "thanks",
    "glad", "hope", "back", "so", "was", "were", "are", "this", "that", "very", "great", "food", "service", "staff",
    "love", "loved", "enjoyed", "again", "soon", "visit", "place", "good", "not", "but", "they", "my", "had", "have", "will"};
constexpr std::array<std::string_view, 46> kSpanish{
    "de", "la", "el", "que", "y", "en", "los", "las", "por", "para", "muy", "gracias", "nos", "su", "con", "una",
    "un", "es", "del", "lo", "le", "se", "más", "mas", "esperamos", "comida", "servicio", "buen", "buena", "bueno",
    "lugar", "todo", "me", "encantó", "encanto", "está", "esta", "fue", "pero", "mi", "muchas", "pronto", "haya",
    "al", "volveremos", "excelente"};

}  // namespace

std::string detect_language(std::string_view utf8) {
    std::size_t han = 0, hangul = 0, kana = 0, latin = 0;
    for (const auto c : text::decode(utf8)) {
        han += text::is_han(c);
        hangul += text::is_hangul(c);
        kana += text::is_kana(c);
        latin += text::is_latin_letter(c);
    }
    const auto total = han + hangul + kana + latin;
    if (total == 0) return "none";
    if (hangul * 10 >= total * 4) return "ko";
    if (kana > 0 && (kana + han) * 10 >= total * 4) return "ja";
    // A Chinese character carries about as much as three Latin letters.
    if (han * 3 >= latin && han > 0) return "zh";
    std::size_t english = 0, spanish = 0;
    for (const auto& word : text::latin_words(utf8)) {
        english += std::ranges::contains(kEnglish, std::string_view(word));
        spanish += std::ranges::contains(kSpanish, std::string_view(word));
    }
    if (spanish > english) return "es";
    if (english == 0 && spanish == 0 && latin < 3) return "other";
    return "en";
}

std::string detect_script(std::string_view utf8) {
    std::size_t han = 0, hangul = 0, latin = 0, other = 0;
    for (const auto c : text::decode(utf8)) {
        if (text::is_han(c)) {
            ++han;
        } else if (text::is_hangul(c)) {
            ++hangul;
        } else if (text::is_latin_letter(c)) {
            ++latin;
        } else if (c > 0x2FF && !text::is_emoji(c) && c != 0xFFFD && !(c >= 0x2000 && c <= 0x303F) && !(c >= 0xFE00 && c <= 0xFE0F) &&
                   !(c >= 0xFF00 && c <= 0xFFEF)) {
            ++other;  // Cyrillic, Arabic, kana, Thai, ...
        }
    }
    if (han + hangul + latin + other == 0) return "none";
    if (han * 3 >= latin && han >= hangul && han >= other && han > 0) {
        return "Hani";
    }
    if (hangul >= latin && hangul >= other && hangul > 0) return "Hang";
    if (latin >= other) return "Latn";
    return "other";
}

std::size_t count_emoji(std::string_view utf8) {
    std::size_t n = 0;
    for (const auto c : text::decode(utf8)) n += text::is_emoji(c);
    return n;
}

}  // namespace poni::domain
