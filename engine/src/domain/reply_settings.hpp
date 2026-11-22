// Brand voice, localization preferences and the reply policy (FR-4, FR-5): a
// location's reply settings. They are the location's own and complete
// (D-093): nothing is merged in from a client or from the knowledge base
// when they are read, and a part that lacks a field is an error.
//
// The structs match the schemas of the same name in api/openapi.yaml.
#pragma once

#include "domain/dates.hpp"  // parse_clock_time, which reads the policy's posting window

#include <boost/describe/class.hpp>
#include <boost/json/value.hpp>

#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace poni::domain {

struct EmojiRule {
    bool allowed = false;
    std::int64_t max = 0;
    std::vector<std::string> preferred;
};
BOOST_DESCRIBE_STRUCT(EmojiRule, (), (allowed, max, preferred))

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

struct Voice {
    // How replies refer to the business. Empty: the location's title (D-102).
    std::string reply_name;
    // The only facts about the business the generator may rely on.
    std::vector<std::string> business_facts;
    // May contain "{owner_name}".
    std::string sign_off;
    std::string owner_name;
    EmojiRule emoji;
    // May a reply mention promotions, discounts or free items?
    bool promos_allowed = false;
    // May a reply carry a phone number, an email address or a link?
    bool contact_allowed = false;
    std::vector<std::string> words_to_avoid;
    // By language ("en", "zh", "es"); other languages use "en".
    std::map<std::string, LengthLimits> length;
    std::vector<std::string> style_notes;
    // Replies the business wrote itself: a tone reference, never copied.
    std::vector<std::string> example_replies;
    // The temperature the writer is asked with, 0 to 2 (D-074). Not part of
    // the prompt; sent with the request unless the analysis settings say
    // that the writer's model takes none.
    double temperature = 0.7;
    // One line the writer is given as the base of the style. Taking a voice
    // template fills it with the template's label and description (D-080);
    // empty for none.
    std::string base_style;
};
BOOST_DESCRIBE_STRUCT(Voice, (),
                      (reply_name, business_facts, sign_off, owner_name, emoji, promos_allowed, contact_allowed,
                       words_to_avoid, length, style_notes, example_replies, temperature, base_style))

// How replies and posts name the business: the voice's reply_name, or the location's title (D-102).
std::string business_name(const Voice& voice, std::string_view location_title);

// The length a reply in `language` and reply `mode` must have: the writer is
// told it and the check holds the reply to it. A language without limits
// uses "en"'s; "rating_only_low" uses "rating_only".
const LengthRange& length_range(const Voice& voice, const std::string& language, const std::string& mode);

struct Localization {
    // Languages replies are written in. A review in any other language gets `fallback_language`.
    std::vector<std::string> reply_languages;
    std::string chinese_script;     // "match", "simplified" or "traditional"
    std::string chinese_region;     // "mainland", "taiwan" or "hong_kong"
    std::string english_variant;    // "us" or "ca"
    std::string fallback_language;
};
BOOST_DESCRIBE_STRUCT(Localization, (), (reply_languages, chinese_script, chinese_region, english_variant, fallback_language))

struct PolicyRules {
    // 4-5 stars with text and no complaint: would auto-post when every check passes.
    bool auto_post_positive = true;
    // 4-5 stars without text.
    bool auto_post_rating_only = true;
    // Reviews below this many stars always wait for a person.
    std::int64_t auto_post_min_stars = 4;
    // Timing of an auto-post (FR-5): a random wait of min to max minutes, so
    // replies don't all arrive at once; 0 and 0 for none.
    std::int64_t auto_post_delay_min_minutes = 0;
    std::int64_t auto_post_delay_max_minutes = 0;
    // The posting window in the location's local time, "HH:MM" each; both
    // empty for none. A start later than the end crosses midnight.
    std::string auto_post_window_start;
    std::string auto_post_window_end;
};
BOOST_DESCRIBE_STRUCT(PolicyRules, (),
                      (auto_post_positive, auto_post_rating_only, auto_post_min_stars, auto_post_delay_min_minutes,
                       auto_post_delay_max_minutes, auto_post_window_start, auto_post_window_end))

// A location's reply settings, and the body that replaces them: every field of every part.
struct ResolvedSettings {
    Voice voice;
    Localization localization;
    PolicyRules policy;
};
BOOST_DESCRIBE_STRUCT(ResolvedSettings, (), (voice, localization, policy))

// The three stored parts as settings. Nothing is filled in: a part that
// lacks a field is the error, which names the part and the field.
std::expected<ResolvedSettings, std::string> read_settings(const boost::json::value& voice, const boost::json::value& localization,
                                                           const boost::json::value& policy);

// What is wrong with settings a person saves (a limit, a language the checks
// can't verify, a pair that does not fit), as the API's 400 says it; nothing if they are fine.
std::optional<std::string> check_settings(const ResolvedSettings& settings);

}  // namespace poni::domain
