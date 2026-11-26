#include "domain/go_live.hpp"

#include "core/crypto.hpp"
#include "domain/corpus.hpp"
#include "domain/prompts.hpp"
#include "domain/reply_policy.hpp"

#include <boost/json/serialize.hpp>
#include <boost/json/value_from.hpp>

#include <algorithm>
#include <format>
#include <set>

namespace poni::domain {

namespace {

// A review that stands in for any review. The system messages don't contain
// the review, so its words don't matter; the star rating is part of the
// writer's instructions and is fixed per mode.
constexpr std::string_view kPlaceholderReview = "placeholder";

struct ModeSample {
    std::string_view mode;
    int stars;
};
constexpr ModeSample kModes[] = {{kModePositive, 5},
                                 {kModeComplaint, 2},
                                 {kModeSeverity, 1},
                                 {kModeRatingOnly, 5},
                                 {kModeRatingOnlyLow, 2}};

std::string short_hash(std::string_view text) { return core::sha256_hex(text).substr(0, 16); }

boost::json::object model_json(const ModelSetup& setup) {
    return {{"provider", setup.provider},
            {"model", setup.model},
            {"temperature", setup.temperature ? boost::json::value(*setup.temperature) : boost::json::value(nullptr)},
            {"max_tokens", setup.max_tokens},
            {"extras", setup.extras}};
}

// The languages and scripts a reply can be written in with these settings.
std::vector<std::pair<std::string, std::optional<std::string>>> targets(const ResolvedSettings& settings) {
    std::set<std::string> languages(settings.reply.reply_languages.begin(), settings.reply.reply_languages.end());
    languages.insert(settings.reply.fallback_language);
    // A set variant fixes the script; none follows the review, so both scripts can be asked for.
    const auto& variant = settings.content.chinese_variant;
    const bool simplified = !variant || *variant == "zh-CN";
    const bool traditional = !variant || *variant != "zh-CN";
    std::vector<std::pair<std::string, std::optional<std::string>>> out;
    for (const auto& language : languages) {
        if (language != "zh") {
            out.emplace_back(language, std::nullopt);
            continue;
        }
        if (simplified) out.emplace_back(language, "Hans");
        if (traditional) out.emplace_back(language, "Hant");
    }
    return out;
}

}  // namespace

Fingerprint fingerprint(const FingerprintInput& input) {
    boost::json::object prompts;
    prompts["analyzer"] =
        short_hash(analyzer_messages(input.business_name, input.industry, 5, kPlaceholderReview, "").front().content);
    for (const auto& [language, script] : targets(input.settings)) {
        for (const auto& sample : kModes) {
            const bool rating_only = sample.mode == kModeRatingOnly || sample.mode == kModeRatingOnlyLow;
            const GeneratorInput writer{.business_name = input.business_name,
                                        .settings = input.settings,
                                        .stars = sample.stars,
                                        .review_text = rating_only ? std::string{} : std::string(kPlaceholderReview),
                                        .mode = std::string(sample.mode),
                                        .target_language = language,
                                        .target_script = script,
                                        .severity_labels = {},
                                        .items = {},
                                        .recent_replies = {},
                                        .staff_instructions = {}};
            prompts[std::format("writer:{}:{}{}", sample.mode, language, script ? "-" + *script : std::string{})] =
                short_hash(generator_messages(writer, "").front().content);
        }
    }

    const auto& content = input.settings.content;
    const auto& reply = input.settings.reply;
    Fingerprint out;
    out.inputs = {{"gate", kGateVersion},
                  {"prompts", std::move(prompts)},
                  {"analyzer", model_json(input.analyzer)},
                  {"writer", model_json(input.writer)},
                  // What run_reply_checks reads of the settings.
                  {"checks", boost::json::object{{"length", boost::json::value_from(reply.length)},
                                                 {"emoji_allowed", content.emoji_allowed},
                                                 {"emoji_max", content.emoji_max},
                                                 {"contact_allowed", reply.contact_allowed}}}};
    // Boost.JSON keeps an object's members in the order they were added, and
    // every member above is added in a fixed order (std::map and std::set
    // iterate sorted), so equal inputs serialize to equal text.
    out.hash = short_hash(boost::json::serialize(out.inputs));
    return out;
}

AutoReplyScope scope_of(const ReplySettings& reply) {
    return {.min_stars = static_cast<int>(std::clamp<std::int64_t>(reply.auto_reply_min_stars, 4, 5))};
}

bool may_auto_reply(std::string_view category) { return category == kCaseControl || category == kCaseReal; }

}  // namespace poni::domain
