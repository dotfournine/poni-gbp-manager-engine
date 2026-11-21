#include "core/text.hpp"

#include <algorithm>

namespace poni::text {

namespace {

constexpr char32_t kReplacement = 0xFFFD;

bool is_zero_width(char32_t c) {
    // Zero-width space, joiners and direction marks; word joiner; byte-order mark; soft hyphen.
    return (c >= 0x200B && c <= 0x200F) || c == 0x2060 || c == 0xFEFF || c == 0x00AD;
}

bool is_space(char32_t c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v' || c == 0x00A0 || c == 0x3000 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F;
}

// One code point starting at `at`, or nothing if the bytes there aren't
// well-formed UTF-8 (overlong forms, surrogates and values past U+10FFFF
// included). `used` is how many bytes it took.
bool decode_one(std::string_view utf8, std::size_t at, char32_t& code, std::size_t& used) {
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(utf8[i]); };
    const unsigned char first = byte(at);
    if (first < 0x80) {
        code = first;
        used = 1;
        return true;
    }
    std::size_t size = 0;
    char32_t value = 0;
    char32_t smallest = 0;
    if ((first & 0xE0) == 0xC0) {
        size = 2, value = first & 0x1F, smallest = 0x80;
    } else if ((first & 0xF0) == 0xE0) {
        size = 3, value = first & 0x0F, smallest = 0x800;
    } else if ((first & 0xF8) == 0xF0) {
        size = 4, value = first & 0x07, smallest = 0x10000;
    } else {
        return false;
    }
    if (at + size > utf8.size()) return false;
    for (std::size_t i = 1; i < size; ++i) {
        if ((byte(at + i) & 0xC0) != 0x80) return false;
        value = (value << 6) | (byte(at + i) & 0x3F);
    }
    if (value < smallest || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) return false;
    code = value;
    used = size;
    return true;
}

}  // namespace

std::u32string decode(std::string_view utf8) {
    std::u32string out;
    out.reserve(utf8.size());
    for (std::size_t pos = 0; pos < utf8.size();) {
        char32_t code = 0;
        std::size_t used = 0;
        if (!decode_one(utf8, pos, code, used)) {
            code = kReplacement;
            used = 1;
        }
        out.push_back(code);
        pos += used;
    }
    return out;
}

void append_utf8(std::string& out, char32_t c) {
    if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) c = kReplacement;
    if (c < 0x80) {
        out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (c >> 18)));
        out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}

std::string encode(std::u32string_view code_points) {
    std::string out;
    out.reserve(code_points.size() * 3);
    for (const auto c : code_points) append_utf8(out, c);
    return out;
}

std::size_t length(std::string_view utf8) { return decode(utf8).size(); }

std::u32string fold(std::string_view utf8) {
    std::u32string out;
    for (const auto c : decode(utf8)) {
        if (is_zero_width(c)) continue;
        if (c >= 0xFF01 && c <= 0xFF5E) {
            out.push_back(c - 0xFEE0);  // full-width ASCII
        } else if (c == 0x3000) {
            out.push_back(U' ');
        } else if (c == 0x3002 || c == 0xFF61) {
            out.push_back(U'.');  // 。 as in "example。com"
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string_view trim(std::string_view utf8) {
    std::size_t begin = std::string_view::npos;
    std::size_t end = 0;
    for (std::size_t pos = 0; pos < utf8.size();) {
        char32_t code = 0;
        std::size_t used = 0;
        if (!decode_one(utf8, pos, code, used)) {  // an invalid byte is content, not white space
            code = kReplacement;
            used = 1;
        }
        if (!is_space(code)) {
            if (begin == std::string_view::npos) begin = pos;
            end = pos + used;
        }
        pos += used;
    }
    if (begin == std::string_view::npos) return {};
    return utf8.substr(begin, end - begin);
}

std::string truncate(std::string_view utf8, std::size_t max_code_points) {
    const auto points = decode(utf8);
    if (points.size() <= max_code_points) return encode(points);
    return encode(std::u32string_view(points).substr(0, max_code_points)) + "…";
}

std::string ascii_lower(std::string_view text) {
    std::string out(text);
    for (auto& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

bool ascii_iequals(std::string_view a, std::string_view b) {
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    return a.size() == b.size() && std::ranges::equal(a, b, {}, lower, lower);
}

bool is_han(char32_t c) {
    return (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0x20000 && c <= 0x2FA1F);
}

bool is_hangul(char32_t c) { return (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0x1100 && c <= 0x11FF); }

bool is_kana(char32_t c) { return c >= 0x3040 && c <= 0x30FF; }

bool is_latin_letter(char32_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= 0xC0 && c <= 0xD6) || (c >= 0xD8 && c <= 0xF6) ||
           (c >= 0xF8 && c <= 0xFF);
}

bool is_emoji(char32_t c) {
    return (c >= 0x1F000 && c <= 0x1FAFF) || (c >= 0x2600 && c <= 0x27BF) || c == 0x2B50 || c == 0x2B55;
}

std::vector<std::string> latin_words(std::string_view utf8) {
    std::vector<std::string> words;
    std::string word;
    for (const auto c : decode(utf8)) {
        if (is_latin_letter(c)) {
            // Lower case: ASCII, and Latin-1 capitals (which sit 0x20 below their small letters).
            char32_t lower = c;
            if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7)) lower = c + 0x20;
            append_utf8(word, lower);
        } else if (!word.empty()) {
            words.push_back(std::move(word));
            word.clear();
        }
    }
    if (!word.empty()) words.push_back(std::move(word));
    return words;
}

}  // namespace poni::text
