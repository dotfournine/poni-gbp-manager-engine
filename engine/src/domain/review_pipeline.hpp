// From a review to an analysis, a draft and a route: the decisions of the
// pipeline, without any I/O. The job in jobs/analysis.cpp calls the
// model and the database around these functions; the offline adversarial
// suite calls them with a scripted model that gives in to every attack.
//
//   build_analysis   the analyzer's answer -> Analysis
//   plan_draft       Analysis + localization -> reply mode and target language
//   finish_draft     the generator's answer -> checks, final text, route
#pragma once

#include "domain/industry.hpp"
#include "domain/prompts.hpp"
#include "domain/reply_checks.hpp"
#include "domain/reply_policy.hpp"
#include "domain/reply_settings.hpp"

#include <array>
#include <boost/describe/class.hpp>
#include <boost/json/object.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace poni::domain {

// What a review's sentiment can be ("none": a review without text that says anything).
inline constexpr std::array<std::string_view, 5> kSentiments{"positive", "mixed", "neutral", "negative", "none"};

struct TopicMention {
    std::string id;
    std::string polarity;  // "positive", "negative" or "neutral"
};
BOOST_DESCRIBE_STRUCT(TopicMention, (), (id, polarity))

struct ItemMention {
    std::string name;  // as the reviewer wrote it
    std::string type;  // an item type of the industry profile
    std::string polarity;
};
BOOST_DESCRIBE_STRUCT(ItemMention, (), (name, type, polarity))

struct RaisedFlag {
    std::string id;
    std::string label;
    // A short quote from the review.
    std::string evidence;
};
BOOST_DESCRIBE_STRUCT(RaisedFlag, (), (id, label, evidence))

struct Analysis {
    // False: the analyzer was asked and gave no usable answer. The gate keeps
    // such a review away from auto reply.
    bool ok = true;
    std::string error;
    std::string language;  // ISO 639-1, "other" or "none"
    // "Hans" or "Hant" when the analyzer told them apart, "Hani" for Chinese it
    // didn't, "Latn", "Hang", "other" or "none".
    std::string script;
    std::string sentiment; // "positive", "mixed", "neutral", "negative" or "none"
    bool contains_complaint = false;
    std::vector<TopicMention> topics;
    std::vector<ItemMention> items;
    std::vector<RaisedFlag> severity_flags;
    bool manipulation_suspected = false;
    std::string manipulation_evidence;
    std::string summary_en;
};

// What the analyzer returned, if it was asked.
struct AnalyzerAnswer {
    // The model's JSON object; nothing when the call failed.
    const boost::json::object* json = nullptr;
    // Why there is no usable answer (transport, invalid JSON, refusal).
    std::string error;
};

// `answer` is nothing for a review without text: no model is asked then.
Analysis build_analysis(const IndustryRules& industry, std::string_view review_text,
                        const std::optional<AnalyzerAnswer>& answer);

struct DraftPlan {
    std::string mode;
    TargetLanguage target;
    std::vector<std::string> severity_ids;
    std::vector<std::string> severity_labels;
    // Names of the items found, for the generator.
    std::vector<std::string> items;
};

DraftPlan plan_draft(const Analysis& analysis, int stars, bool has_text, const Localization& localization);

// What the writer is asked for the review a plan was made for. Without recent
// replies and staff instructions: a caller that has them adds them.
GeneratorInput generator_input(std::string business_name, const ResolvedSettings& settings, int stars, std::string review_text,
                               const DraftPlan& plan);

struct DraftOutcome {
    bool generation_ok = false;
    // The reply exactly as the model wrote it; empty when generation failed.
    std::string text;
    bool generator_manipulation_suspected = false;
    std::string notes_for_staff;
    std::vector<CheckResult> checks;
    std::string route;
    std::vector<std::string> route_reasons;
};

// `answer` is nothing when the generator gave no usable answer; the draft is
// then empty and can't go to auto reply.
DraftOutcome finish_draft(const Analysis& analysis, const DraftPlan& plan, int stars, bool has_text,
                          const std::optional<GeneratorAnswer>& answer, const ResolvedSettings& settings);

}  // namespace poni::domain
