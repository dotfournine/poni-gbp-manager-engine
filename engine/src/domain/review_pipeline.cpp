#include "domain/review_pipeline.hpp"

#include "core/text.hpp"
#include "domain/language.hpp"
#include "domain/prompt_parts.hpp"

#include <algorithm>
#include <array>

namespace poni::domain {

using prompt_parts::bool_of;
using prompt_parts::string_of;

namespace {

constexpr std::array<std::string_view, 3> kPolarities{"positive", "negative", "neutral"};

constexpr std::size_t kMaxEvidence = 200;
constexpr std::size_t kMaxSummary = 400;
constexpr std::size_t kMaxListed = 30;

const boost::json::array* array_of(const boost::json::object& object, std::string_view key) {
    const auto* value = object.if_contains(key);
    return value && value->is_array() ? &value->get_array() : nullptr;
}

std::string polarity_of(const boost::json::object& object) {
    auto polarity = string_of(object, "polarity").value_or("neutral");
    return std::ranges::contains(kPolarities, std::string_view(polarity)) ? polarity : "neutral";
}

// "en", "zh-Hant" -> "zh", "EN" -> "en"; nothing if it isn't a language code.
std::optional<std::string> language_code(std::string_view raw) {
    std::string code;
    for (const char c : raw) {
        if (c == '-' || c == '_') break;
        if (c >= 'A' && c <= 'Z') {
            code.push_back(static_cast<char>(c - 'A' + 'a'));
        } else if (c >= 'a' && c <= 'z') {
            code.push_back(c);
        } else {
            return std::nullopt;
        }
    }
    if (code.size() < 2 || code.size() > 3 || code == "none") return std::nullopt;
    return code;
}

RaisedFlag& flag_entry(std::vector<RaisedFlag>& flags, const SeverityFlag& rule) {
    const auto it = std::ranges::find(flags, rule.id, &RaisedFlag::id);
    if (it != flags.end()) return *it;
    flags.push_back({.id = rule.id, .label = rule.label, .evidence = {}});
    return flags.back();
}

// Reads the model's answer into `analysis`. False if a field the routing
// depends on is missing or has the wrong type.
bool read_answer(const boost::json::object& json, const IndustryRules& industry, Analysis& analysis) {
    const auto complaint = bool_of(json, "contains_complaint");
    const auto manipulation = bool_of(json, "manipulation_suspected");
    const auto* flags = array_of(json, "severity_flags");
    if (!complaint || !manipulation || !flags) return false;

    analysis.contains_complaint = *complaint;
    if (*manipulation) {
        analysis.manipulation_suspected = true;
        analysis.manipulation_evidence = text::truncate(string_of(json, "manipulation_evidence").value_or(""), kMaxEvidence);
    }
    if (auto sentiment = string_of(json, "sentiment"); sentiment && std::ranges::contains(kSentiments, std::string_view(*sentiment))) {
        analysis.sentiment = std::move(*sentiment);
    }
    if (const auto language = string_of(json, "language")) {
        if (auto code = language_code(*language)) analysis.language = std::move(*code);
    }
    // Code sees Chinese characters; only the model can say which script they are in (D-056).
    if (analysis.script == "Hani") {
        if (const auto script = string_of(json, "script"); script && (*script == "Hans" || *script == "Hant")) analysis.script = *script;
    }
    analysis.summary_en = text::truncate(string_of(json, "summary_en").value_or(""), kMaxSummary);

    // Ids the profile doesn't define are dropped: the model may only choose from the lists it was given.
    if (const auto* topics = array_of(json, "topics")) {
        for (const auto& value : *topics) {
            if (!value.is_object() || analysis.topics.size() >= kMaxListed) continue;
            const auto id = string_of(value.get_object(), "id");
            if (!id || std::ranges::find(industry.topics, *id, &Topic::id) == industry.topics.end()) continue;
            if (std::ranges::find(analysis.topics, *id, &TopicMention::id) != analysis.topics.end()) continue;
            analysis.topics.push_back({*id, polarity_of(value.get_object())});
        }
    }
    if (const auto* items = array_of(json, "items")) {
        for (const auto& value : *items) {
            if (!value.is_object() || analysis.items.size() >= kMaxListed) continue;
            const auto& object = value.get_object();
            const auto name = string_of(object, "name");
            const auto type = string_of(object, "type");
            if (!name || text::trim(*name).empty() || !type) continue;
            if (std::ranges::find(industry.item_types, *type, &ItemType::id) == industry.item_types.end()) continue;
            analysis.items.push_back({text::truncate(*name, 100), *type, polarity_of(object)});
        }
    }
    for (const auto& value : *flags) {
        if (!value.is_object()) continue;
        const auto id = string_of(value.get_object(), "id");
        if (!id) continue;
        const auto rule = std::ranges::find(industry.severity_flags, *id, &SeverityFlag::id);
        if (rule == industry.severity_flags.end()) continue;
        auto& entry = flag_entry(analysis.severity_flags, *rule);
        if (entry.evidence.empty()) entry.evidence = text::truncate(string_of(value.get_object(), "evidence").value_or(""), kMaxEvidence);
    }
    return true;
}

}  // namespace

Analysis build_analysis(const IndustryRules& industry, std::string_view review_text,
                        const std::optional<AnalyzerAnswer>& answer) {
    Analysis analysis;
    const bool has_text = !text::trim(review_text).empty();
    analysis.language = has_text ? detect_language(review_text) : "none";
    analysis.script = has_text ? detect_script(review_text) : "none";
    analysis.sentiment = "none";
    if (!has_text) return analysis;

    if (answer) {
        if (!answer->json) {
            analysis.ok = false;
            analysis.error = answer->error.empty() ? "the analyzer gave no answer" : answer->error;
        } else if (!read_answer(*answer->json, industry, analysis)) {
            analysis.ok = false;
            analysis.error = "the analyzer's answer lacks a required field";
        }
    }
    if (analysis.language == "none") analysis.language = "other";  // text without letters: emoji, digits

    return analysis;
}

DraftPlan plan_draft(const Analysis& analysis, int stars, bool has_text, const Localization& localization) {
    DraftPlan plan;
    for (const auto& flag : analysis.severity_flags) {
        plan.severity_ids.push_back(flag.id);
        plan.severity_labels.push_back(flag.label);
    }
    plan.mode = std::string(reply_mode(stars, has_text, !analysis.severity_flags.empty(), analysis.contains_complaint));
    plan.target = target_language(has_text ? analysis.language : "none", analysis.script, localization);
    for (const auto& item : analysis.items) plan.items.push_back(item.name);
    return plan;
}

GeneratorInput generator_input(std::string business_name, const ResolvedSettings& settings, int stars, std::string review_text,
                               const DraftPlan& plan) {
    return {.business_name = std::move(business_name),
            .settings = settings,
            .stars = stars,
            .review_text = std::move(review_text),
            .mode = plan.mode,
            .target_language = plan.target.language,
            .target_script = plan.target.script,
            .severity_labels = plan.severity_labels,
            .items = plan.items,
            .recent_replies = {},
            .staff_instructions = {}};
}

DraftOutcome finish_draft(const Analysis& analysis, const DraftPlan& plan, int stars, bool has_text,
                          const std::optional<GeneratorAnswer>& answer, const ResolvedSettings& settings) {
    DraftOutcome outcome;
    GateInput input{.stars = stars,
                    .has_text = has_text,
                    .analysis_ok = analysis.ok,
                    .generation_ok = answer.has_value(),
                    .contains_complaint = analysis.contains_complaint,
                    .severity_flags = plan.severity_ids,
                    .manipulation_suspected = analysis.manipulation_suspected,
                    .failed_checks = {}};
    if (answer) {
        outcome.generation_ok = true;
        outcome.text = answer->reply;
        outcome.generator_manipulation_suspected = answer->manipulation_suspected;
        outcome.notes_for_staff = answer->notes_for_staff;
        const CheckContext context{.voice = settings.voice,
                                   .target_language = plan.target.language,
                                   .mode = plan.mode};
        auto report = run_reply_checks(answer->reply, context);
        input.failed_checks = report.failed();
        input.manipulation_suspected = input.manipulation_suspected || answer->manipulation_suspected;
        outcome.checks = std::move(report.results);
    }
    auto decision = gate(input, settings.policy);
    outcome.route = std::move(decision.route);
    outcome.route_reasons = std::move(decision.reasons);
    return outcome;
}

}  // namespace poni::domain
