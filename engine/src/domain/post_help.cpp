#include "domain/post_help.hpp"

#include "core/text.hpp"

#include <map>
#include <set>

namespace poni::domain {

namespace {

std::set<std::string> person_types(const IndustryRules* industry) {
    std::set<std::string> out;
    if (!industry) return out;
    for (const auto& type : industry->item_types) {
        if (type.is_person()) out.insert(type.id);
    }
    return out;
}

}  // namespace

PostHelpCounts count_post_help(const std::vector<HelpReview>& reviews, const IndustryRules* industry) {
    std::map<std::string, std::string> labels;
    if (industry) {
        for (const auto& topic : industry->topics) labels[topic.id] = topic.label;
    }
    MentionCounter mentions(std::move(labels), person_types(industry));
    std::map<std::string, Ranked> languages;
    for (const auto& review : reviews) {
        mentions.add(review.topics, review.items);
        if (!review.language.empty()) {
            auto& entry = languages[review.language];
            entry.label = review.language;
            ++entry.count;
        }
    }
    PostHelpCounts out;
    mentions.rank_into(out, kPostHelpRanked);
    out.languages = ranked(languages, languages.size());
    return out;
}

std::vector<HelpReview> reviews_for_model(const std::vector<HelpReview>& reviews, const IndustryRules* industry) {
    const auto people = person_types(industry);
    std::vector<HelpReview> out;
    for (const auto& review : reviews) {
        if (out.size() == kPostHelpMaxReviews) break;
        auto one = review;
        one.text = text::truncate(text::trim(one.text), kPostHelpReviewLength);
        std::erase_if(one.items, [&](const ItemMention& item) { return people.contains(item.type); });
        out.push_back(std::move(one));
    }
    return out;
}

}  // namespace poni::domain
