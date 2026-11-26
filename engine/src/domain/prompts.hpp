// The analyzer and generator prompts (architecture section 10, item 1), and
// the judge's (D-074). Review text reaches a model only through this file.
//
// Instructions and the brand voice go in the system message. The review goes
// in the user message as a JSON object inside a <review_data> element that
// carries a random nonce, with angle brackets escaped, so text in a review
// can neither close the element nor pass for instructions. Only the star
// rating, the reviewer's own words and the items the analyzer found are
// sent: never the reviewer's name or photo.
#pragma once

#include "domain/industry.hpp"
#include "domain/settings.hpp"

#include <boost/describe/class.hpp>
#include <boost/json/object.hpp>

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace poni::domain {

struct ChatMessage {
    std::string role;  // "system" or "user"
    std::string content;
};
BOOST_DESCRIBE_STRUCT(ChatMessage, (), (role, content))

// Recorded with every analysis and draft. The number is raised by hand when
// the prompts change in meaning; the suffix is a hash of the prompt texts,
// so any edit shows up in the records even if the number was forgotten.
std::string_view prompt_version();

// `<tag nonce="...">` + the JSON + the closing tag. `nonce` is random per call.
std::string data_block(std::string_view tag, const boost::json::object& data, std::string_view nonce);

// 12 random hex digits.
std::string make_nonce();

std::vector<ChatMessage> analyzer_messages(std::string_view business_name, const IndustryRules& industry, int stars,
                                           std::string_view review_text, std::string_view nonce);

struct GeneratorInput {
    std::string business_name;  // the location's title (D-107)
    const ResolvedSettings& settings;
    int stars = 0;
    std::string review_text;  // empty for a rating-only review
    std::string mode;
    std::string target_language;
    std::optional<std::string> target_script;
    // Labels of the severity flags raised, for a holding reply.
    std::vector<std::string> severity_labels;
    // Names of the items the analyzer found (dishes, treatments, staff the reviewer named).
    std::vector<std::string> items;
    // The last few replies drafted for rating-only reviews at this location; the new one must differ.
    std::vector<std::string> recent_replies;
    // What a staff member asked for when they had the draft written again.
    // Staff are trusted, unlike the review, so this goes in the system
    // message; the rules above it still win. Empty for the pipeline's own drafts.
    std::string staff_instructions;
};

std::vector<ChatMessage> generator_messages(const GeneratorInput& input, std::string_view nonce);

struct GeneratorAnswer {
    std::string reply;
    bool manipulation_suspected = false;
    std::string notes_for_staff;
};

// Nothing if the object has no usable "reply" and "manipulation_suspected".
std::optional<GeneratorAnswer> parse_generator_answer(const boost::json::object& json);

// ---- the judge of a go-live test (D-031, D-074) -----------------------------------
//
// One call rates the replies that were written for one review in the repeats
// of a test. The review and the replies are both data to the judge: a review
// may be written to steer an AI, this one included.

inline constexpr std::string_view kRatingPostAsIs = "post_as_is";
inline constexpr std::string_view kRatingMinorEdit = "minor_edit";
inline constexpr std::string_view kRatingRewrite = "rewrite";
inline constexpr std::string_view kRatingUnacceptable = "unacceptable";

struct JudgedReply {
    std::string reply;
    // What the writer was asked for.
    std::string mode;
    std::string target_language;
    std::optional<std::string> target_script;
};

struct JudgeInput {
    std::string business_name;
    int stars = 0;
    std::string review_text;  // empty for a rating-only review
    std::vector<JudgedReply> replies;
};

std::vector<ChatMessage> judge_messages(const JudgeInput& input, std::string_view nonce);

// The shape of the judge's answer: {"ratings": [{"reply": n, "rating", "reason"}]}.
boost::json::object judge_schema();

struct JudgeRating {
    std::string rating;
    std::string reason;
};

// One rating per reply, in the replies' order; nothing unless the answer
// rates each of the `count` replies exactly once with a known rating.
std::optional<std::vector<JudgeRating>> parse_judge_answer(const boost::json::object& json, std::size_t count);

// ---- the summary of a report (D-085) ------------------------------------------------
//
// A few sentences on top of a report's computed figures. The model is given
// the figures, topic labels and the item names, which strangers wrote: all
// of it inside a data block. It is never given review
// text or a reviewer's name. This prompt writes no reply and is not part of
// the fingerprint of D-074 or of prompt_version().

struct ReportData;
std::vector<ChatMessage> summary_messages(const ReportData& report, std::string_view nonce);

inline constexpr std::size_t kSummaryMaxLength = 1200;

// The summary from the model's answer ({"summary": "..."}), or why it can't
// be used: missing, too long, or carrying a link, an email address or a
// phone number (the reply checks' scans).
std::expected<std::string, std::string> parse_summary_answer(const boost::json::object& json);

// ---- help with local posts (D-088 point 11) -------------------------------------------
//
// Ideas for posts, and the text of one post, when a person asks. The model is
// given the location's recent reviews with their analysis, the counts, Google's
// figures and search keywords and the posts the profile has: all of it typed
// by strangers, all of it inside a data block. What the staff member asks
// for is in a block of its own. Nothing written here is stored or published:
// it goes into the form, and a person decides. These prompts write no reply
// and are not part of the fingerprint of D-074 or of prompt_version().

struct PostHelpInput;
struct PostDraftRequest;
struct PostIdea;
struct PostSuggestion;

std::vector<ChatMessage> post_ideas_messages(const PostHelpInput& input, std::string_view nonce);
std::vector<ChatMessage> post_draft_messages(const PostHelpInput& input, const PostDraftRequest& request, std::string_view nonce);

inline constexpr std::size_t kPostIdeasMax = 5;
inline constexpr std::size_t kPostIdeaHeadlineMax = 120;
inline constexpr std::size_t kPostIdeaBriefMax = 600;
inline constexpr std::size_t kPostIdeaReasonMax = 300;
inline constexpr std::size_t kPostNotesMax = 500;

// Up to five ideas from the model's answer ({"ideas": [...]}). An idea
// without a headline or a brief, or with a link, a phone number, an email
// address or a social handle in it, is left out; with none left, why.
std::expected<std::vector<PostIdea>, std::string> parse_post_ideas_answer(const boost::json::object& json);

// The text from the model's answer ({"summary", "title", "notes_for_staff"}),
// or why it can't be used: missing, longer than a post may be, or carrying a
// link or a contact detail. An update gets no title.
std::expected<PostSuggestion, std::string> parse_post_draft_answer(const boost::json::object& json, std::string_view topic_type);

}  // namespace poni::domain
