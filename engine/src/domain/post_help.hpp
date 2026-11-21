// What the help with local posts knows about a location (D-088 point 11):
// its reviews of the last 90 days with their stored analysis, what is counted
// from them, and what is handed to the model when a person asks for ideas or
// for a text. No I/O here; the prompts themselves are in domain/prompts.hpp.
//
// A review is in here only with a current analysis that raised no severity
// flag and suspected no manipulation; the repository leaves the others out.
// A reviewer's name or photo is in none of these structs.
#pragma once

#include "domain/industry.hpp"
#include "domain/report.hpp"
#include "domain/reply_settings.hpp"
#include "domain/review_pipeline.hpp"

#include <boost/describe/class.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace poni::domain {

// The period the help looks back over, in days.
inline constexpr int kPostHelpDays = 90;
// How many reviews the model is given at most, the newest first.
inline constexpr std::size_t kPostHelpMaxReviews = 200;
// Fewer usable reviews than this and the model is not asked for ideas.
inline constexpr std::int64_t kPostHelpMinReviews = 10;
// A review's text is cut to this many code points for the model.
inline constexpr std::size_t kPostHelpReviewLength = 600;
// How many entries a ranking has at most.
inline constexpr std::size_t kPostHelpRanked = 8;

struct HelpReview {
    std::int64_t id = 0;
    int rating = 0;        // 0: Google sent none
    std::string day;       // "YYYY-MM-DD", UTC
    std::string language;  // as the analysis found it
    std::string text;      // the reviewer's own words; untrusted
    std::string sentiment;
    std::vector<TopicMention> topics;
    std::vector<ItemMention> items;
};
BOOST_DESCRIBE_STRUCT(HelpReview, (), (id, rating, day, language, text, sentiment, topics, items))

struct PostHelpCounts {
    std::vector<Ranked> topics_praised;
    std::vector<Ranked> topics_complained;
    std::vector<Ranked> items_praised;
    std::vector<Ranked> items_complained;
    // Every language, most reviews first; the label is the language's tag.
    std::vector<Ranked> languages;
};

// Counted as MentionCounter counts. `industry` may be absent: then a topic
// is shown by its id and no item type is a person.
PostHelpCounts count_post_help(const std::vector<HelpReview>& reviews, const IndustryRules* industry);

// The reviews as the model gets them: at most kPostHelpMaxReviews, the text
// cut, and no item of a person type.
std::vector<HelpReview> reviews_for_model(const std::vector<HelpReview>& reviews, const IndustryRules* industry);

// A post the profile has, so the model does not repeat it. Its text may have
// been typed at Google: untrusted.
struct PastPost {
    std::string day;
    std::string topic_type;
    std::string title;
    std::string summary;
};
BOOST_DESCRIBE_STRUCT(PastPost, (), (day, topic_type, title, summary))

struct PostHelpPerformance {
    std::string from;  // "YYYY-MM-DD"
    std::string to;
    VisibilityTotals totals;
};

struct PostHelpInput {
    std::string business_name;   // the voice's reply_name, or the client's name
    std::string location_title;  // as the profile has it
    std::string industry_label;  // "" without an industry
    // The industry profile's topics, id to label: a review's topics are given to the model by their labels.
    std::map<std::string, std::string> topic_labels;
    std::string audience;        // the client's setting; "" when not set
    std::string today;           // in words
    ResolvedSettings settings;
    std::vector<HelpReview> reviews;  // from reviews_for_model
    PostHelpCounts counts;
    std::optional<PostHelpPerformance> performance;
    std::string keywords_month;  // "YYYY-MM"; "" without keywords
    std::vector<SearchKeyword> keywords;
    std::vector<PastPost> recent_posts;
};

// What a person asks the model to write.
struct PostDraftRequest {
    std::string topic_type;     // "standard", "event" or "offer"
    std::string language_code;  // a language tag
    std::string brief;          // may be empty when `summary` is not
    std::string summary;        // the current text, to improve; may be empty
    std::optional<std::string> title;
};

struct PostIdea {
    std::string topic_type;
    std::string headline;
    std::string brief;
    std::string reason;
};
BOOST_DESCRIBE_STRUCT(PostIdea, (), (topic_type, headline, brief, reason))

struct PostSuggestion {
    std::string summary;
    std::optional<std::string> title;
    std::string notes_for_staff;
};

}  // namespace poni::domain
