#include "db/repo_post_help.hpp"

#include "core/text.hpp"
#include "db/database.hpp"
#include "db/repo_knowledge.hpp"
#include "db/repo_settings.hpp"
#include "db/sql.hpp"
#include "domain/report_schedule.hpp"

#include <chrono>

namespace poni::repo {

using db::to_timestamptz;

namespace {

using OptString = std::optional<std::string>;
using OptId = std::optional<std::int64_t>;

constexpr std::size_t kPastPosts = 10;
constexpr std::size_t kPastPostLength = 400;
constexpr std::size_t kRecentHighlights = 5;
// More reviews than this in 90 days are not read: the counts then cover the newest.
constexpr int kMaxRows = 5000;

// A review of the period, and whether the help may use it: a current analysis that raised no
// severity flag and suspected no manipulation.
struct PeriodReview {
    bool usable = false;
    domain::HelpReview review;
};
BOOST_DESCRIBE_STRUCT(PeriodReview, (), (usable, review))

}  // namespace

std::optional<PostHelp> load_post_help(pqxx::transaction_base& tx, std::int64_t location_id, core::TimePoint now) {
    const auto rows = tx.exec("SELECT l.client_id, coalesce(c.name, ''), l.title, l.gbp_location_id, "
                              "l.access_status = 'active' AND c.id IS NOT NULL AND NOT c.archived, coalesce(c.audience, ''), c.industry_key "
                              "FROM locations l LEFT JOIN clients c ON c.id = l.client_id WHERE l.id = $1",
                              pqxx::params{location_id});
    if (rows.empty()) return std::nullopt;
    const auto row = rows[0];
    PostHelp help;
    help.location_id = location_id;
    help.client_id = row[0].as<OptId>();
    help.client_name = row[1].as<std::string>();
    help.location_title = row[2].as<std::string>();
    help.gbp_location_id = row[3].as<std::string>();
    help.accessible = row[4].as<bool>();
    help.audience = row[5].as<std::string>();
    if (const auto industry = row[6].as<OptString>()) help.industry = industry_rules(tx, *industry);
    if (help.client_id) help.reply_settings = stored_reply_settings(tx, location_id);
    help.settings = get_ai_settings(tx);

    help.period_end = std::chrono::floor<std::chrono::days>(now);
    help.period_start = help.period_end - std::chrono::days{domain::kPostHelpDays};

    // `a` is the newest analysis of the review as it is now. A review that was edited since its
    // analysis has none, and is not used until it is analysed again. No reviewer's name or photo is selected.
    auto reviews = db::rows_as<PeriodReview>(tx, R"sql(
        SELECT v.usable, to_jsonb(v) AS review
          FROM (SELECT r.id, coalesce(r.rating, 0) AS rating, ((r.created_time AT TIME ZONE 'UTC')::date)::text AS day,
                       coalesce(r.original_text, r.comment, '') AS text,
                       coalesce(a.status = 'ok' AND jsonb_array_length(a.severity_flags) = 0 AND NOT a.manipulation_suspected, false) AS usable,
                       coalesce(a.language, '') AS language, coalesce(a.sentiment, '') AS sentiment,
                       coalesce(a.topics, '[]') AS topics, coalesce(a.items, '[]') AS items
                  FROM reviews r
                  LEFT JOIN LATERAL (SELECT x.status, x.severity_flags, x.manipulation_suspected, x.language, x.sentiment, x.topics, x.items
                                       FROM review_analyses x
                                      WHERE x.review_id = r.id AND x.review_updated_time = r.updated_time
                                      ORDER BY x.id DESC LIMIT 1) a ON true
                 WHERE r.location_id = $1 AND r.removed_at IS NULL
                   AND r.created_time >= $2::timestamptz AND r.created_time <= $3::timestamptz
                 ORDER BY r.created_time DESC, r.id DESC
                 LIMIT $4) v)sql",
                                             pqxx::params{location_id, to_timestamptz(core::TimePoint{help.period_start}), to_timestamptz(now), kMaxRows});
    help.reviews = std::ssize(reviews);
    for (auto& row : reviews) {
        if (row.usable) help.usable.push_back(std::move(row.review));
    }

    // What the profile has or had, whoever made it; a draft says nothing about the profile.
    help.posts = db::rows_as<domain::PastPost>(
        tx,
        "SELECT ((coalesce(google_created_at, sent_at, created_at) AT TIME ZONE 'UTC')::date)::text AS day, "
        "content->>'topic_type' AS topic_type, coalesce(content#>>'{event,title}', '') AS title, content->>'summary' AS summary "
        "FROM local_posts WHERE location_id = $1 AND google_name IS NOT NULL AND content->>'topic_type' <> 'alert' "
        "ORDER BY coalesce(google_created_at, sent_at, created_at) DESC, id DESC LIMIT $2",
        pqxx::params{location_id, static_cast<int>(kPastPosts)});
    for (auto& post : help.posts) post.summary = text::truncate(post.summary, kPastPostLength);
    return help;
}

model::ReviewHighlights review_highlights(const PostHelp& help) {
    const auto* industry = help.industry ? &*help.industry : nullptr;
    const auto counted = domain::count_post_help(help.usable, industry);
    model::ReviewHighlights out;
    out.location_id = help.location_id;
    out.period_start = domain::format_day(help.period_start);
    out.period_end = domain::format_day(help.period_end);
    out.reviews = help.reviews;
    out.analysed_reviews = static_cast<std::int64_t>(help.usable.size());
    out.enough_for_ideas = out.analysed_reviews >= domain::kPostHelpMinReviews;
    out.topics = counted.topics_praised;
    out.items = counted.items_praised;
    out.languages = counted.languages;
    for (const auto& review : help.usable) {
        if (out.recent.size() == kRecentHighlights) break;
        const auto words = text::trim(review.text);
        if (review.rating < 4 || words.empty()) continue;
        out.recent.push_back({.review_id = review.id,
                              .rating = review.rating,
                              .day = review.day,
                              .language = review.language,
                              .text = text::truncate(words, domain::kPostHelpReviewLength)});
    }
    return out;
}

}  // namespace poni::repo
