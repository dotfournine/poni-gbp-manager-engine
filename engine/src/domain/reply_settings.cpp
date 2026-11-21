#include "domain/reply_settings.hpp"

#include "core/json.hpp"
#include "core/text.hpp"
#include "domain/dates.hpp"
#include "domain/posting_schedule.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <string_view>

namespace poni::domain {

namespace {

// The languages the mechanical checks can verify. A reply in any other
// language could not be checked, so it can't be configured.
constexpr std::array<std::string_view, 3> kReplyLanguages{"en", "zh", "es"};

constexpr std::size_t kMaxListItems = 50;
constexpr std::size_t kMaxTextLength = 2000;

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

std::optional<std::string> check_voice(const Voice& voice) {
    for (const auto& [field, list] : {std::pair<std::string_view, const std::vector<std::string>&>{"business_facts", voice.business_facts},
                                      {"words_to_avoid", voice.words_to_avoid},
                                      {"style_notes", voice.style_notes},
                                      {"example_replies", voice.example_replies},
                                      {"emoji.preferred", voice.emoji.preferred}}) {
        if (auto problem = check_texts(field, list)) return problem;
    }
    if (text::length(voice.reply_name) > 200 || text::length(voice.sign_off) > 200 || text::length(voice.owner_name) > 200) {
        return "'reply_name', 'sign_off' and 'owner_name' may be up to 200 characters";
    }
    if (text::length(voice.base_style) > kMaxTextLength) return std::format("'base_style' may be up to {} characters", kMaxTextLength);
    if (voice.emoji.max < 0 || voice.emoji.max > 10) return "'emoji.max' must be between 0 and 10";
    if (!(voice.temperature >= 0 && voice.temperature <= 2)) return "'temperature' must be between 0 and 2";
    // Complete means every language replies are written in: a language without
    // limits would be held to another's, or to none.
    for (const auto language : kReplyLanguages) {
        if (!voice.length.contains(std::string(language))) return std::format("'length' needs limits for '{}'", language);
    }
    for (const auto& [language, limits] : voice.length) {
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

std::optional<std::string> check_localization(const Localization& l) {
    const auto known = [](const std::string& language) { return std::ranges::contains(kReplyLanguages, std::string_view(language)); };
    if (l.reply_languages.empty()) return "'reply_languages' needs at least one language";
    if (!std::ranges::all_of(l.reply_languages, known)) return "'reply_languages' may hold en, zh and es";
    if (l.chinese_script != "match" && l.chinese_script != "simplified" && l.chinese_script != "traditional") {
        return "'chinese_script' must be match, simplified or traditional";
    }
    if (l.chinese_region != "mainland" && l.chinese_region != "taiwan" && l.chinese_region != "hong_kong") {
        return "'chinese_region' must be mainland, taiwan or hong_kong";
    }
    if (l.english_variant != "us" && l.english_variant != "ca") return "'english_variant' must be us or ca";
    if (!known(l.fallback_language)) return "'fallback_language' must be en, zh or es";
    return std::nullopt;
}

std::optional<std::string> check_policy(const PolicyRules& rules) {
    if (rules.auto_post_min_stars < 1 || rules.auto_post_min_stars > 5) return "'auto_post_min_stars' must be between 1 and 5";
    constexpr std::int64_t day = 24 * 60;
    if (rules.auto_post_delay_min_minutes < 0 || rules.auto_post_delay_min_minutes > day || rules.auto_post_delay_max_minutes < 0 ||
        rules.auto_post_delay_max_minutes > day) {
        return "'auto_post_delay_min_minutes' and 'auto_post_delay_max_minutes' must be between 0 and 1440";
    }
    for (const auto* field : {&rules.auto_post_window_start, &rules.auto_post_window_end}) {
        if (!field->empty() && !parse_clock_time(*field)) {
            return "'auto_post_window_start' and 'auto_post_window_end' must be a time like \"09:00\", or empty";
        }
    }
    // The delay and the window as pairs: the rule of whoever uses them, asked here so that a save and a post agree.
    if (const auto timing = posting_timing(rules); !timing) return timing.error();
    return std::nullopt;
}

}  // namespace

std::string business_name(const Voice& voice, std::string_view client_name) {
    return voice.reply_name.empty() ? std::string(client_name) : voice.reply_name;
}

const LengthRange& length_range(const Voice& voice, const std::string& language, const std::string& mode) {
    static const LengthLimits none{};
    auto it = voice.length.find(language);
    if (it == voice.length.end()) it = voice.length.find("en");
    const auto& limits = it == voice.length.end() ? none : it->second;
    if (mode == "positive") return limits.positive;
    if (mode == "complaint") return limits.complaint;
    if (mode == "severity") return limits.severity;
    return limits.rating_only;  // rating_only and rating_only_low
}

std::expected<ResolvedSettings, std::string> read_settings(const boost::json::value& voice, const boost::json::value& localization,
                                                           const boost::json::value& policy) {
    auto v = core::from_json<Voice>(voice);
    if (!v) return std::unexpected("voice: " + v.error());
    auto l = core::from_json<Localization>(localization);
    if (!l) return std::unexpected("localization: " + l.error());
    auto p = core::from_json<PolicyRules>(policy);
    if (!p) return std::unexpected("policy: " + p.error());
    return ResolvedSettings{.voice = std::move(*v), .localization = std::move(*l), .policy = std::move(*p)};
}

std::optional<std::string> check_settings(const ResolvedSettings& settings) {
    if (auto problem = check_voice(settings.voice)) return "voice: " + *problem;
    if (auto problem = check_localization(settings.localization)) return "localization: " + *problem;
    if (auto problem = check_policy(settings.policy)) return "policy: " + *problem;
    return std::nullopt;
}

}  // namespace poni::domain
