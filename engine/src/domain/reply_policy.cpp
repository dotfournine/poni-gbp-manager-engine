#include "domain/reply_policy.hpp"

#include <algorithm>
#include <format>

namespace poni::domain {

namespace {

std::string joined(std::vector<std::string> parts) {
    std::ranges::sort(parts);
    const auto [first, last] = std::ranges::unique(parts);
    parts.erase(first, last);
    std::string out;
    for (const auto& part : parts) {
        if (!out.empty()) out += ", ";
        out += part;
    }
    return out;
}

}  // namespace

std::string_view reply_mode(int stars, bool has_text, bool has_severity, bool contains_complaint) {
    if (has_severity) return kModeSeverity;
    if (!has_text) return stars >= 4 ? kModeRatingOnly : kModeRatingOnlyLow;
    if (stars <= 3 || contains_complaint) return kModeComplaint;
    return kModePositive;
}

TargetLanguage target_language(std::string_view review_language, std::string_view review_script, const Localization& localization) {
    const bool replies_in_it = std::ranges::contains(localization.reply_languages, review_language);
    TargetLanguage target{.language = replies_in_it ? std::string(review_language) : localization.fallback_language,
                          .script = std::nullopt};
    if (target.language != "zh") return target;
    if (localization.chinese_script == "simplified") {
        target.script = "Hans";
    } else if (localization.chinese_script == "traditional") {
        target.script = "Hant";
    } else {
        target.script = review_script == "Hant" ? "Hant" : "Hans";
    }
    return target;
}

GateDecision gate(const GateInput& input, const PolicyRules& policy) {
    // A severity flag always escalates, whatever else is true (FR-5, FR-6).
    if (!input.severity_flags.empty()) {
        return {std::string(kRouteEscalate), {"severity: " + joined(input.severity_flags)}};
    }
    std::vector<std::string> reasons;
    if (!input.analysis_ok) reasons.emplace_back("analysis failed");
    if (!input.generation_ok) reasons.emplace_back("generation failed");
    if (input.manipulation_suspected) reasons.emplace_back("manipulation suspected");
    if (!input.failed_checks.empty()) reasons.push_back("check failed: " + joined(input.failed_checks));
    if (input.stars < policy.auto_post_min_stars) {
        reasons.push_back(input.stars > 0 ? std::format("{} stars", input.stars) : std::string("no star rating"));
    } else if (input.contains_complaint) {
        reasons.emplace_back("contains a complaint");
    } else if (!input.has_text && !policy.auto_post_rating_only) {
        reasons.emplace_back("auto reply is off for rating-only reviews");
    } else if (input.has_text && !policy.auto_post_positive) {
        reasons.emplace_back("auto reply is off for positive reviews");
    }
    if (!reasons.empty()) return {std::string(kRouteApproval), std::move(reasons)};
    return {std::string(kRouteAutoReply), {}};
}

}  // namespace poni::domain
