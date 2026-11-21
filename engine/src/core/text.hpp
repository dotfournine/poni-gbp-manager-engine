// UTF-8 text helpers for the reply checks and the analysis. Review text and
// model output are untrusted: everything here tolerates invalid UTF-8 (an
// invalid byte counts as U+FFFD) and never throws on content.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace poni::text {

// The code points of `utf8`.
std::u32string decode(std::string_view utf8);
std::string encode(std::u32string_view code_points);
void append_utf8(std::string& out, char32_t code_point);

// Number of code points: the "characters" a length limit counts.
std::size_t length(std::string_view utf8);

// The code points of `utf8` with full-width letters, digits and punctuation
// as their ASCII forms and zero-width characters dropped, so a link or a
// phone number in a reply can't hide from the checks behind either. This is
// a few lines of code, not Unicode normalization (D-058).
std::u32string fold(std::string_view utf8);

// Without leading and trailing white space (ASCII and the common Unicode spaces).
std::string_view trim(std::string_view utf8);

// The first `max_code_points` code points, with "…" appended if anything was cut.
std::string truncate(std::string_view utf8, std::size_t max_code_points);

// A–Z as a–z; every other byte as it is.
std::string ascii_lower(std::string_view text);
// Equal but for the case of A–Z.
bool ascii_iequals(std::string_view a, std::string_view b);

bool is_han(char32_t c);
bool is_hangul(char32_t c);
bool is_kana(char32_t c);
// Latin letters, with the accented ones Western European languages use.
bool is_latin_letter(char32_t c);
bool is_emoji(char32_t c);

// Lower-case words made of Latin letters, in order.
std::vector<std::string> latin_words(std::string_view utf8);

}  // namespace poni::text
