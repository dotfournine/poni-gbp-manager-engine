#include "domain/reply_checks.hpp"

#include "core/text.hpp"
#include "domain/language.hpp"

#include <algorithm>
#include <format>
#include <optional>

namespace poni::domain {

namespace {

// The contact checks are plain scans over the reply's code points, after
// text::fold has turned full-width forms into ASCII (D-058). They look for
// the three things a reply must never carry by accident or because a review
// asked for them: a link, an email address and a phone number.

bool is_digit(char32_t c) { return c >= U'0' && c <= U'9'; }
bool is_lower(char32_t c) { return c >= U'a' && c <= U'z'; }
bool is_alpha(char32_t c) { return is_lower(c) || (c >= U'A' && c <= U'Z'); }
bool is_alnum(char32_t c) { return is_alpha(c) || is_digit(c); }
bool is_name_char(char32_t c) { return is_alnum(c) || c == U'-'; }
char32_t lower(char32_t c) { return c >= U'A' && c <= U'Z' ? c + 32 : c; }

bool starts_with_at(std::u32string_view s, std::size_t at, std::u32string_view word) {
    if (at + word.size() > s.size()) return false;
    for (std::size_t i = 0; i < word.size(); ++i) {
        if (lower(s[at + i]) != word[i]) return false;
    }
    return true;
}

// From `begin` to the next white space, for the detail shown to staff.
std::string token_from(std::u32string_view s, std::size_t begin) {
    std::size_t end = begin;
    while (end < s.size() && end - begin < 80 && s[end] != U' ' && s[end] != U'\n' && s[end] != U'\t') ++end;
    return text::encode(s.substr(begin, end - begin));
}

bool is_chinese_numeral(char32_t c) {
    constexpr std::u32string_view numerals = U"零一二三四五六七八九〇";
    return numerals.find(c) != std::u32string_view::npos;
}

}  // namespace

// "http://", "https://" or "www." anywhere, or "name.tld": a name of two or
// more letters, digits or hyphens, a dot, and a lower-case ending of 2 to 24
// letters. "e.g." (a one-letter name) and "soon.Hope" (a missing space
// before a capital) are not links.
std::optional<std::string> scan_for_url(std::u32string_view s) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        const bool word_start = i == 0 || !is_alnum(s[i - 1]);
        if (word_start && (starts_with_at(s, i, U"http://") || starts_with_at(s, i, U"https://") || starts_with_at(s, i, U"www."))) {
            return token_from(s, i);
        }
        if (s[i] != U'.' || i < 2) continue;
        std::size_t name = i;
        while (name > 0 && is_name_char(s[name - 1])) --name;
        if (i - name < 2) continue;
        std::size_t end = i + 1;
        while (end < s.size() && is_lower(s[end])) ++end;
        const auto ending = end - (i + 1);
        if (ending < 2 || ending > 24) continue;
        if (end < s.size() && (is_alnum(s[end]) || s[end] == U'_')) continue;
        return token_from(s, name);
    }
    return std::nullopt;
}

// "@" with a letter or digit right before it and "name.tld" after it.
std::optional<std::string> scan_for_email(std::u32string_view s) {
    for (std::size_t i = 1; i < s.size(); ++i) {
        if (s[i] != U'@' || !is_alnum(s[i - 1])) continue;
        std::size_t end = i + 1;
        while (end < s.size() && (is_name_char(s[end]) || s[end] == U'.')) ++end;
        while (end > i + 1 && !is_alnum(s[end - 1])) --end;  // the full stop that ends the sentence
        const auto host = s.substr(i + 1, end - (i + 1));
        const auto dot = host.rfind(U'.');
        if (dot == std::u32string_view::npos || dot == 0 || host.size() - dot - 1 < 2) continue;
        std::size_t begin = i;
        while (begin > 0 && (is_alnum(s[begin - 1]) || s[begin - 1] == U'.' || s[begin - 1] == U'+' || s[begin - 1] == U'-' ||
                             s[begin - 1] == U'_')) {
            --begin;
        }
        return text::encode(s.substr(begin, end - begin));
    }
    return std::nullopt;
}

// Seven or more digits in one run of digits and the separators people put
// between them (spaces, dots, hyphens, brackets, a plus), or seven or more
// Chinese numerals in a row.
std::optional<std::string> scan_for_phone(std::u32string_view s) {
    const auto is_separator = [](char32_t c) { return c == U' ' || c == U'.' || c == U'-' || c == U'(' || c == U')' || c == U'+'; };
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (is_digit(s[i])) {
            std::size_t end = i, last_digit = i, digits = 0;
            while (end < s.size() && (is_digit(s[end]) || is_separator(s[end]))) {
                if (is_digit(s[end])) {
                    ++digits;
                    last_digit = end;
                }
                ++end;
            }
            if (digits >= 7) return text::encode(s.substr(i, last_digit + 1 - i));
            i = end;
        } else if (is_chinese_numeral(s[i])) {
            std::size_t end = i;
            while (end < s.size() && is_chinese_numeral(s[end])) ++end;
            if (end - i >= 7) return text::encode(s.substr(i, end - i));
            i = end;
        }
    }
    return std::nullopt;
}

// "@name" that is not the middle of an email address.
std::optional<std::string> scan_for_handle(std::u32string_view s) {
    for (std::size_t i = 0; i + 2 < s.size(); ++i) {
        if (s[i] != U'@' || (i > 0 && is_alnum(s[i - 1]))) continue;
        std::size_t end = i + 1;
        while (end < s.size() && (is_alnum(s[end]) || s[end] == U'_' || s[end] == U'.')) ++end;
        while (end > i + 1 && s[end - 1] == U'.') --end;  // the full stop that ends the sentence
        if (end - (i + 1) >= 2) return text::encode(s.substr(i, end - i));
    }
    return std::nullopt;
}

bool CheckReport::passed() const {
    return std::ranges::all_of(results, [](const CheckResult& r) { return r.passed; });
}

std::vector<std::string> CheckReport::failed() const {
    std::vector<std::string> out;
    for (const auto& result : results) {
        if (!result.passed) out.push_back(result.name);
    }
    return out;
}

std::string CheckReport::failed_names() const {
    std::string out;
    for (const auto& name : failed()) out += (out.empty() ? "" : ", ") + name;
    return out;
}

CheckReport run_reply_checks(std::string_view reply, const CheckContext& context) {
    const auto& voice = context.voice;
    CheckReport report;
    const auto folded = text::fold(reply);
    const auto folded_utf8 = text::encode(folded);
    const auto body = text::trim(folded_utf8);
    const auto add = [&](std::string_view name, bool passed, std::string detail = {}) {
        report.results.push_back({std::string(name), passed, passed ? std::string{} : std::move(detail)});
    };

    add("not_empty", !body.empty(), "the reply is empty");

    const auto url = scan_for_url(folded);
    add("no_url", voice.contact_allowed || !url, url.value_or(""));
    const auto email = scan_for_email(folded);
    add("no_email", voice.contact_allowed || !email, email.value_or(""));
    const auto phone = scan_for_phone(folded);
    add("no_phone", voice.contact_allowed || !phone, phone.value_or(""));
    {
        const auto allowed = voice.emoji.allowed ? static_cast<std::size_t>(std::max<std::int64_t>(voice.emoji.max, 0)) : 0;
        const auto used = count_emoji(reply);
        add("emoji_limit", used <= allowed, std::format("{} emoji, at most {} allowed", used, allowed));
    }
    {
        const auto& range = length_range(voice, context.target_language, context.mode);
        const auto size = static_cast<std::int64_t>(text::length(body));
        add("length", size >= range.min && size <= range.max,
            std::format("{} characters, expected {} to {}", size, range.min, range.max));
    }
    {
        const auto language = detect_language(folded_utf8);
        add("language_match", language == context.target_language,
            std::format("the reply looks like '{}', expected '{}'", language, context.target_language));
    }
    return report;
}

}  // namespace poni::domain
