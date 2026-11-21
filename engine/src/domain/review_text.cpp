#include "domain/review_text.hpp"

#include "core/text.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace poni::domain {

namespace {

constexpr std::string_view kTranslated = "(Translated by Google)";
constexpr std::string_view kOriginal = "(Original)";

std::string_view trim(std::string_view s) {
    constexpr std::string_view ws = " \t\r\n";
    const auto first = s.find_first_not_of(ws);
    if (first == std::string_view::npos) return {};
    return s.substr(first, s.find_last_not_of(ws) - first + 1);
}

bool in(char32_t cp, char32_t low, char32_t high) { return cp >= low && cp <= high; }

// Counts whole-word matches of `words` in lower-cased ASCII-ish text.
int count_words(std::string_view text, std::initializer_list<std::string_view> words) {
    int hits = 0;
    std::size_t start = 0;
    const auto is_letter = [](unsigned char c) { return std::isalpha(c) || c >= 0x80; };
    while (start < text.size()) {
        while (start < text.size() && !is_letter(static_cast<unsigned char>(text[start]))) ++start;
        auto end = start;
        while (end < text.size() && is_letter(static_cast<unsigned char>(text[end]))) ++end;
        const auto word = text.substr(start, end - start);
        if (std::ranges::find(words, word) != words.end()) ++hits;
        start = end;
    }
    return hits;
}

}  // namespace

CommentParts split_translation(std::string_view comment) {
    const auto text = trim(comment);
    if (text.starts_with(kTranslated)) {
        // rfind: if the reviewer's own text contains the marker too, the last one is Google's.
        const auto marker = text.rfind(kOriginal);
        if (marker != std::string_view::npos && marker > kTranslated.size()) {
            const auto translated = trim(text.substr(kTranslated.size(), marker - kTranslated.size()));
            const auto original = trim(text.substr(marker + kOriginal.size()));
            if (!original.empty()) return {std::string(original), std::string(translated)};
        }
        return {std::string(comment), std::nullopt};
    }
    if (const auto marker = text.rfind(kTranslated); marker != std::string_view::npos && marker > 0) {
        const auto original = trim(text.substr(0, marker));
        const auto translated = trim(text.substr(marker + kTranslated.size()));
        if (!original.empty() && !translated.empty()) return {std::string(original), std::string(translated)};
    }
    return {std::string(comment), std::nullopt};
}

std::optional<std::string> guess_language(std::string_view original, bool was_translated) {
    int han = 0, kana = 0, hangul = 0, latin = 0, other = 0;
    // The ranges are this guess's own (wider for Hangul and Latin, narrower for
    // Han than text::is_*), so that no guess changes.
    for (const auto cp : text::decode(original)) {
        if (in(cp, 0x4E00, 0x9FFF) || in(cp, 0x3400, 0x4DBF) || in(cp, 0x20000, 0x2A6DF) || in(cp, 0xF900, 0xFAFF)) {
            ++han;
        } else if (text::is_kana(cp)) {
            ++kana;
        } else if (in(cp, 0xAC00, 0xD7AF) || in(cp, 0x1100, 0x11FF) || in(cp, 0x3130, 0x318F)) {
            ++hangul;
        } else if (in(cp, 'A', 'Z') || in(cp, 'a', 'z') || in(cp, 0x00C0, 0x024F)) {
            ++latin;
        } else if (in(cp, 0x0370, 0x03FF) || in(cp, 0x0400, 0x052F) || in(cp, 0x0590, 0x06FF) || in(cp, 0x0900, 0x0DFF) ||
                   in(cp, 0x0E00, 0x0EFF)) {
            ++other;  // Greek, Cyrillic, Hebrew, Arabic, Indic, Thai, Lao
        }
    }
    const int total = han + kana + hangul + latin + other;
    if (total == 0) return std::nullopt;

    // A CJK character carries about a word, so a minority of them already
    // makes the review a CJK-language one ("很好! great service").
    if (kana > 0 && (kana + han) * 5 >= total) return "ja";
    if (hangul * 5 >= total) return "ko";
    if (han * 5 >= total) return "zh";
    if (other * 2 >= total) return "other";
    if (!was_translated) return "en";

    // Latin script that Google translated: Spanish and French are the ones
    // PONI's markets see; tell them apart by their commonest words.
    const auto lower = text::ascii_lower(original);
    const int spanish = count_words(lower, {"el", "la", "los", "las", "que", "muy", "y", "es", "con", "para", "una", "pero",
                                             "comida", "servicio", "excelente", "gracias", "lugar", "bueno", "buena"});
    const int french = count_words(lower, {"le", "les", "des", "et", "est", "très", "une", "avec", "pour", "nous", "mais",
                                            "pas", "c'est", "était", "merci", "bien", "bon", "bonne"});
    if (spanish > french && spanish > 0) return "es";
    if (french > spanish && french > 0) return "fr";
    return "other";
}

std::optional<int> parse_star_rating(std::string_view rating) {
    constexpr std::array<std::string_view, 5> names{"ONE", "TWO", "THREE", "FOUR", "FIVE"};
    const auto it = std::ranges::find(names, rating);
    if (it == names.end()) return std::nullopt;
    return static_cast<int>(it - names.begin()) + 1;
}

std::optional<std::string> canonical_timestamp(std::string_view text) {
    // YYYY-MM-DDTHH:MM:SS[.fraction]Z
    constexpr std::string_view shape = "dddd-dd-ddTdd:dd:dd";
    if (text.size() < shape.size() + 1 || text.back() != 'Z') return std::nullopt;
    for (std::size_t i = 0; i < shape.size(); ++i) {
        const bool ok = shape[i] == 'd' ? std::isdigit(static_cast<unsigned char>(text[i])) != 0 : text[i] == shape[i];
        if (!ok) return std::nullopt;
    }
    const auto number = [&](std::size_t pos, std::size_t len) {
        int n = 0;
        for (std::size_t i = pos; i < pos + len; ++i) n = n * 10 + (text[i] - '0');
        return n;
    };
    if (number(5, 2) < 1 || number(5, 2) > 12 || number(8, 2) < 1 || number(8, 2) > 31 || number(11, 2) > 23 ||
        number(14, 2) > 59 || number(17, 2) > 60) {
        return std::nullopt;
    }
    std::string fraction;
    const auto rest = text.substr(shape.size(), text.size() - shape.size() - 1);
    if (!rest.empty()) {
        if (rest.front() != '.' || rest.size() == 1) return std::nullopt;
        for (const char c : rest.substr(1)) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
        }
        fraction = rest.substr(1, 6);  // PostgreSQL keeps microseconds
    }
    fraction.resize(6, '0');
    return std::string(text.substr(0, shape.size())) + "." + fraction + "Z";
}

}  // namespace poni::domain
