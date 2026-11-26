#include "domain/settings.hpp"

#include "core/text.hpp"
#include "domain/dates.hpp"
#include "domain/posting_schedule.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <string_view>
#include <utility>

namespace poni::domain {

namespace {

// The languages the mechanical checks can verify. A reply in any other
// language could not be checked, so it can't be configured.
constexpr std::array<std::string_view, 3> kReplyLanguages{"en", "zh", "es"};

constexpr std::size_t kMaxListItems = 50;
constexpr std::size_t kMaxTextLength = 2000;
constexpr std::size_t kMaxLineLength = 200;
constexpr std::size_t kMaxAudienceLength = 500;

std::optional<std::string> check_texts(std::string_view field, const std::vector<std::string>& list) {
    if (list.size() > kMaxListItems) return std::format("'{}' may hold at most {} entries", field, kMaxListItems);
    for (const auto& entry : list) {
        if (text::length(entry) > kMaxTextLength) {
            return std::format("an entry of '{}' is longer than {} characters", field, kMaxTextLength);
        }
        if (text::trim(entry).empty()) return std::format("'{}' has an empty entry", field);
    }
    return std::nullopt;
}

std::optional<std::string> check_length(const std::map<std::string, LengthLimits>& length) {
    // Complete means every language replies are written in: a language without
    // limits would be held to another's, or to none.
    for (const auto language : kReplyLanguages) {
        if (!length.contains(std::string(language))) return std::format("'length' needs limits for '{}'", language);
    }
    for (const auto& [language, limits] : length) {
        if (!std::ranges::contains(kReplyLanguages, std::string_view(language))) {
            return std::format("'length' has limits for '{}'; replies are written in en, zh or es", language);
        }
        for (const auto& [mode, range] : {std::pair<std::string_view, const LengthRange&>{"rating_only", limits.rating_only},
                                          {"positive", limits.positive},
                                          {"complaint", limits.complaint},
                                          {"severity", limits.severity}}) {
            if (range.min < 0 || range.max > 4000 || range.min > range.max) {
                return std::format("'length.{}.{}' needs 0 <= min <= max <= 4000", language, mode);
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> check_auto_reply(const ReplySettings& s) {
    if (s.auto_reply_min_stars < 1 || s.auto_reply_min_stars > 5) return "'auto_reply_min_stars' must be between 1 and 5";
    constexpr std::int64_t day = 24 * 60;
    if (s.auto_reply_delay_min_minutes < 0 || s.auto_reply_delay_min_minutes > day || s.auto_reply_delay_max_minutes < 0 ||
        s.auto_reply_delay_max_minutes > day) {
        return "'auto_reply_delay_min_minutes' and 'auto_reply_delay_max_minutes' must be between 0 and 1440";
    }
    for (const auto* field : {&s.auto_reply_window_start, &s.auto_reply_window_end}) {
        if (!field->empty() && !parse_clock_time(*field)) {
            return "'auto_reply_window_start' and 'auto_reply_window_end' must be a time like \"09:00\", or empty";
        }
    }
    // The delay and the window as pairs: the rule of whoever uses them, asked here so that a save and a post agree.
    if (const auto timing = posting_timing(s); !timing) return timing.error();
    return std::nullopt;
}

}  // namespace

std::optional<std::string> check_content_settings(const ContentSettings& s) {
    for (const auto& [field, list] : {std::pair<std::string_view, const std::vector<std::string>&>{"emoji_preferred", s.emoji_preferred},
                                      {"words_to_avoid", s.words_to_avoid},
                                      {"style_notes", s.style_notes}}) {
        if (auto problem = check_texts(field, list)) return problem;
    }
    if (text::length(s.business_facts) > kMaxTextLength) return std::format("'business_facts' may be up to {} characters", kMaxTextLength);
    if (text::length(s.base_style) > kMaxTextLength) return std::format("'base_style' may be up to {} characters", kMaxTextLength);
    if (s.emoji_max < 0 || s.emoji_max > 10) return "'emoji_max' must be between 0 and 10";
    if (s.chinese_variant && *s.chinese_variant != "zh-CN" && *s.chinese_variant != "zh-HK" && *s.chinese_variant != "zh-TW") {
        return "'chinese_variant' must be zh-CN, zh-HK, zh-TW or null";
    }
    if (s.english_variant && *s.english_variant != "en-US" && *s.english_variant != "en-CA") {
        return "'english_variant' must be en-US, en-CA or null";
    }
    if (text::length(s.audience) > kMaxAudienceLength) return std::format("'audience' may be up to {} characters", kMaxAudienceLength);
    return std::nullopt;
}

std::optional<std::string> check_reply_settings(const ReplySettings& s) {
    const auto known = [](const std::string& language) { return std::ranges::contains(kReplyLanguages, std::string_view(language)); };
    if (s.reply_languages.empty()) return "'reply_languages' needs at least one language";
    if (!std::ranges::all_of(s.reply_languages, known)) return "'reply_languages' may hold en, zh and es";
    if (!known(s.fallback_language)) return "'fallback_language' must be en, zh or es";
    if (text::length(s.signature) > kMaxLineLength) return std::format("'signature' may be up to {} characters", kMaxLineLength);
    if (s.signature.find_first_of("\r\n") != std::string::npos) return "'signature' is one line";
    if (text::length(s.sign_off_name) > kMaxLineLength) return std::format("'sign_off_name' may be up to {} characters", kMaxLineLength);
    if (auto problem = check_length(s.length)) return problem;
    if (auto problem = check_texts("example_replies", s.example_replies)) return problem;
    if (!(s.temperature >= 0 && s.temperature <= 2)) return "'temperature' must be between 0 and 2";
    return check_auto_reply(s);
}

const LengthRange& length_range(const ReplySettings& settings, const std::string& language, const std::string& mode) {
    static const LengthLimits none{};
    auto it = settings.length.find(language);
    if (it == settings.length.end()) it = settings.length.find("en");
    const auto& limits = it == settings.length.end() ? none : it->second;
    if (mode == "positive") return limits.positive;
    if (mode == "complaint") return limits.complaint;
    if (mode == "severity") return limits.severity;
    return limits.rating_only;  // rating_only and rating_only_low
}

}  // namespace poni::domain
