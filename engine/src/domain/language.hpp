// The language and script of a text, worked out mechanically (no model):
// what the reply checks compare a reply against, and the fallback when the
// analyzer gives no language.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace poni::domain {

// "en", "zh", "es", "ko", "ja", "other" (some other script or too little to
// tell) or "none" (no letters at all). Latin-script languages other than
// English and Spanish come out as "en": the analyzer tells those apart.
std::string detect_language(std::string_view text);

// "Hani" (Chinese characters; code can't tell Simplified from Traditional,
// the analyzer does, D-056), "Hang", "Latn", "other" or "none".
std::string detect_script(std::string_view text);

std::size_t count_emoji(std::string_view text);

}  // namespace poni::domain
