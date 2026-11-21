// What the engine works out from a review's text and fields on its own:
// Google's translated/original split, a first guess at the language, the
// star rating as a number, and timestamps in one canonical form.
// Review text is untrusted input; nothing here interprets it.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace poni::domain {

struct CommentParts {
    // What the reviewer wrote. Equals the whole comment when Google didn't translate it.
    std::string original;
    // Google's translation, when the comment carried one.
    std::optional<std::string> translated;
};

// Google delivers a non-English review as
//     (Translated by Google) <translation>\n\n(Original)\n<original>
// (spike B4). The reverse order, "<original>\n\n(Translated by Google)\n
// <translation>", is handled too. Anything else is returned unchanged.
CommentParts split_translation(std::string_view comment);

// A guess at the language from the script of the reviewer's own words; reviews carry
// no language field. Returns "zh", "ja", "ko", "en", "es", "fr" or "other";
// nothing when the text has no letters (empty, or only emoji and digits).
// `was_translated`: Google translated the review, so it isn't English.
// The analyzer replaces this guess (and tells Simplified from Traditional).
std::optional<std::string> guess_language(std::string_view original, bool was_translated);

// "ONE".."FIVE" -> 1..5; nothing for STAR_RATING_UNSPECIFIED or anything else.
std::optional<int> parse_star_rating(std::string_view rating);

// Google's RFC 3339 timestamps ("2026-07-19T22:34:20.376896Z", any number of
// fraction digits, always UTC) as "YYYY-MM-DDTHH:MM:SS.ffffffZ". The canonical
// form compares as text and is what the database returns. Nothing if `text`
// isn't such a timestamp.
std::optional<std::string> canonical_timestamp(std::string_view text);

}  // namespace poni::domain
