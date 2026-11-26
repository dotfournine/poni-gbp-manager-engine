// A location's settings, as two flat bundles named for what reads them
// (D-107): its content settings (what the business is, how it sounds, who its
// customers are) and its reply settings (how a reply to a review is written
// and when it is posted by itself). Each is the location's own and complete
// row: nothing is merged in from a client or from the knowledge base when it
// is read, and a row that lacks a field is an error.
//
// The structs match the schemas of the same name in api/openapi.yaml; each is
// also the body that replaces it, sent whole.
#pragma once

#include <boost/describe/class.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace poni::domain {

struct ContentSettings {
    static constexpr bool sent_whole = true;
    // The only facts about the business the writer may rely on, as typed; empty for none.
    std::string business_facts;
    // One line the writer is given as the base of the style. Taking a voice
    // template fills it with the template's label and description (D-080).
    std::string base_style;
    bool emoji_allowed = false;
    std::int64_t emoji_max = 0;
    std::vector<std::string> emoji_preferred;
    std::vector<std::string> words_to_avoid;
    std::vector<std::string> style_notes;
    // "zh-CN", "zh-HK" or "zh-TW"; nothing when it is not set.
    std::optional<std::string> chinese_variant;
    // "en-US" or "en-CA"; nothing when it is not set.
    std::optional<std::string> english_variant;
    // Who the customers are, in a line written by staff; read by the help with local posts only.
    std::string audience;
};
BOOST_DESCRIBE_STRUCT(ContentSettings, (),
                      (business_facts, base_style, emoji_allowed, emoji_max, emoji_preferred, words_to_avoid, style_notes, chinese_variant,
                       english_variant, audience))

struct LengthRange {
    std::int64_t min = 0;
    std::int64_t max = 0;
};
BOOST_DESCRIBE_STRUCT(LengthRange, (), (min, max))

// Characters per reply mode. A low rating without text uses rating_only.
struct LengthLimits {
    LengthRange rating_only;
    LengthRange positive;
    LengthRange complaint;
    LengthRange severity;
};
BOOST_DESCRIBE_STRUCT(LengthLimits, (), (rating_only, positive, complaint, severity))

struct ReplySettings {
    static constexpr bool sent_whole = true;
    // Languages replies are written in ("en", "zh", "es"). A review in any other language gets `fallback_language`.
    std::vector<std::string> reply_languages;
    std::string fallback_language;
    // The line a reply ends with; empty for none.
    std::string signature;
    std::string sign_off_name;
    // By language ("en", "zh", "es").
    std::map<std::string, LengthLimits> length;
    // Replies the business wrote itself: a tone reference, never copied.
    std::vector<std::string> example_replies;
    // May a reply mention promotions, discounts or free items?
    bool promos_allowed = false;
    // May a reply carry a phone number, an email address or a link?
    bool contact_allowed = false;
    // The temperature the writer is asked with, 0 to 2 (D-074). Not part of
    // the prompt; sent with the request unless the analysis settings say
    // that the writer's model takes none.
    double temperature = 0.7;
    // 4-5 stars with text and no complaint: would auto reply when every check passes.
    bool auto_reply_positive = true;
    // 4-5 stars without text.
    bool auto_reply_rating_only = true;
    // Reviews below this many stars always wait for a person.
    std::int64_t auto_reply_min_stars = 4;
    // Timing of an auto reply (FR-5): a random wait of min to max minutes, so
    // replies don't all arrive at once; 0 and 0 for none.
    std::int64_t auto_reply_delay_min_minutes = 0;
    std::int64_t auto_reply_delay_max_minutes = 0;
    // The posting window in the location's local time, "HH:MM" each; both
    // empty for none. A start later than the end crosses midnight.
    std::string auto_reply_window_start;
    std::string auto_reply_window_end;
};
BOOST_DESCRIBE_STRUCT(ReplySettings, (),
                      (reply_languages, fallback_language, signature, sign_off_name, length, example_replies, promos_allowed,
                       contact_allowed, temperature, auto_reply_positive, auto_reply_rating_only, auto_reply_min_stars,
                       auto_reply_delay_min_minutes, auto_reply_delay_max_minutes, auto_reply_window_start, auto_reply_window_end))

// What prompts, checks, the fingerprint and the goldens read: the two stored bundles together.
struct ResolvedSettings {
    ContentSettings content;
    ReplySettings reply;
};
BOOST_DESCRIBE_STRUCT(ResolvedSettings, (), (content, reply))

// What is wrong with settings a person saves (a limit, a language the checks
// can't verify, a pair that does not fit), as the API's 400 says it; nothing if they are fine.
std::optional<std::string> check_content_settings(const ContentSettings& settings);
std::optional<std::string> check_reply_settings(const ReplySettings& settings);

// The length a reply in `language` and reply `mode` must have: the writer is
// told it and the check holds the reply to it. A language without limits
// uses "en"'s; "rating_only_low" uses "rating_only".
const LengthRange& length_range(const ReplySettings& settings, const std::string& language, const std::string& mode);

}  // namespace poni::domain
