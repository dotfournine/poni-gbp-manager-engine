// The tiered reply policy (FR-5): which kind of reply a review gets, in which
// language, and where the draft goes. Every review ends on exactly one route.
//
// The route "auto_reply" says only that the policy would let the reply go out
// without a person: whether it does is up to repo::schedule_auto_reply.
#pragma once

#include "domain/settings.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace poni::domain {

inline constexpr std::string_view kRouteAutoReply = "auto_reply";
inline constexpr std::string_view kRouteApproval = "approval_queue";
inline constexpr std::string_view kRouteEscalate = "escalate";

inline constexpr std::string_view kModePositive = "positive";
inline constexpr std::string_view kModeComplaint = "complaint";
inline constexpr std::string_view kModeSeverity = "severity";
inline constexpr std::string_view kModeRatingOnly = "rating_only";
inline constexpr std::string_view kModeRatingOnlyLow = "rating_only_low";

// The kind of reply to write. Code decides this, never the model.
std::string_view reply_mode(int stars, bool has_text, bool has_severity, bool contains_complaint);

struct TargetLanguage {
    std::string language;                // "en", "zh" or "es"
    std::optional<std::string> script;   // "Hans" or "Hant" for Chinese
};

// The language and script the reply is written in: the review's language if
// the business replies in it, else the fallback; for Chinese, the script the
// content settings' Chinese variant names, or the review's own when none is set.
TargetLanguage target_language(std::string_view review_language, std::string_view review_script, const ResolvedSettings& settings);

struct GateInput {
    int stars = 0;  // 0 when Google sent no rating
    bool has_text = false;
    bool analysis_ok = true;
    bool generation_ok = true;
    bool contains_complaint = false;
    // Ids of the severity flags the analyzer raised.
    std::vector<std::string> severity_flags;
    // Suspected by the analyzer or the generator.
    bool manipulation_suspected = false;
    // Names of the mechanical checks the draft failed.
    std::vector<std::string> failed_checks;
};

struct GateDecision {
    std::string route;
    // Why the review did not go to auto reply; empty for auto_reply.
    std::vector<std::string> reasons;
};

GateDecision gate(const GateInput& input, const ReplySettings& reply);

}  // namespace poni::domain
