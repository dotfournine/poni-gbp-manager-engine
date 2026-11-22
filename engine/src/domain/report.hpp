// The numbers of a location's report (FR-8, D-085, D-102), computed from what the
// engine has stored and from the visibility numbers read from Google for
// the report. No I/O and no model: every figure a report shows comes out of
// these functions, and the summary is written on top of them.
//
// ReportData is what a report stores as JSON and what the page is rendered
// from (domain/report_html.hpp).
#pragma once

#include "domain/report_schedule.hpp"
#include "domain/review_pipeline.hpp"

#include <boost/describe/class.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace poni::domain {

// ---- what goes in -----------------------------------------------------------------

struct ReportReview {
    std::int64_t location_id = 0;
    int rating = 0;  // 1 to 5; 0: Google gave none
    // The day the review was written, in the report's time zone.
    Day day;
    core::TimePoint created;
    // The reviewer's own words; empty for a rating-only review. Untrusted.
    std::string text;
    std::string reviewer_name;
    std::optional<std::string> reviewer_photo_url;
    // When the reply on Google was last written, if the review has one.
    std::optional<core::TimePoint> replied_at;
    // From the review's current analysis; "" when it has none.
    std::string sentiment;
    std::vector<TopicMention> topics;
    std::vector<ItemMention> items;
};

struct ReportLocation {
    std::int64_t id = 0;
    std::string title;
    // Every review up to the end of the period.
    std::int64_t running_count = 0;
    std::int64_t running_rating_sum = 0;
};

// One day's value of one of Google's daily metrics
// ("BUSINESS_IMPRESSIONS_MOBILE_MAPS", "CALL_CLICKS", ...). Days Google has
// no data for yet are simply not there.
struct MetricDay {
    std::string metric;
    Day day;
    std::int64_t value = 0;
};

struct SearchKeyword {
    std::string keyword;
    // Google gives a count, or only "fewer than" a threshold.
    std::optional<std::int64_t> impressions;
    std::optional<std::int64_t> below;
};
BOOST_DESCRIBE_STRUCT(SearchKeyword, (), (keyword, impressions, below))

struct LocationPerformance {
    std::int64_t location_id = 0;
    // Why Google could not be asked for this location; then the rest is empty.
    std::optional<std::string> error;
    // The period and the one before it.
    std::vector<MetricDay> days;
    // "YYYY-MM" the keywords are of; empty: none were read.
    std::string keywords_month;
    std::vector<SearchKeyword> keywords;
};

struct ReportInput {
    // The location the report covers (D-102); `locations` holds it alone.
    std::int64_t location_id = 0;
    std::string location_title;
    Period period;
    std::string time_zone;
    std::string generated_at;
    std::vector<ReportLocation> locations;
    // Reviews written in the period and in the period before it, not removed.
    std::vector<ReportReview> reviews;
    std::vector<ReportReview> previous_reviews;
    // Topic id -> label, from the location's industry profile.
    std::map<std::string, std::string> topic_labels;
    // The item types of the location's industry that are people (a named staff
    // member). Empty: the industry has none, and the report has no employee
    // recognition section.
    std::set<std::string> person_item_types;
    std::int64_t escalations_opened = 0;
    std::int64_t escalations_closed = 0;
    std::vector<LocationPerformance> performance;
};

// ---- what comes out ---------------------------------------------------------------

struct ReviewNumbers {
    std::int64_t new_reviews = 0;
    // Nothing when no review of the period has a rating.
    std::optional<double> average_rating;
    // How many reviews with 1, 2, 3, 4 and 5 stars.
    std::vector<std::int64_t> stars = {0, 0, 0, 0, 0};
    std::int64_t replied = 0;
    // replied / new_reviews; nothing without reviews.
    std::optional<double> response_rate;
    std::optional<double> median_response_hours;
    // By the analysis of each review; reviews without one are in none of them.
    std::int64_t positive = 0;
    std::int64_t mixed = 0;
    std::int64_t neutral = 0;
    std::int64_t negative = 0;
};
BOOST_DESCRIBE_STRUCT(ReviewNumbers, (),
                      (new_reviews, average_rating, stars, replied, response_rate, median_response_hours, positive, mixed, neutral,
                       negative))

struct VisibilityTotals {
    std::int64_t search_mobile = 0;
    std::int64_t search_desktop = 0;
    std::int64_t maps_mobile = 0;
    std::int64_t maps_desktop = 0;
    std::int64_t calls = 0;
    std::int64_t direction_requests = 0;
    std::int64_t website_clicks = 0;
    std::int64_t menu_clicks = 0;
    std::int64_t bookings = 0;

    std::int64_t views() const { return search_mobile + search_desktop + maps_mobile + maps_desktop; }
};
BOOST_DESCRIBE_STRUCT(VisibilityTotals, (),
                      (search_mobile, search_desktop, maps_mobile, maps_desktop, calls, direction_requests, website_clicks,
                       menu_clicks, bookings))

struct VisibilityDay {
    std::string day;
    std::int64_t views = 0;
};
BOOST_DESCRIBE_STRUCT(VisibilityDay, (), (day, views))

struct Visibility {
    // False: Google could not be asked, or has no data for the period; `note` says which.
    bool available = false;
    // For the page, which the business reads: no error text of Google's.
    std::string note;
    // Google's own words when it could not be asked. For staff; never on the page.
    std::string detail;
    // The last day every location's view counts go up to. Google publishes
    // its metrics with different delays; nothing after this day is counted,
    // for any metric, so the totals are of the same days.
    std::optional<std::string> as_of;
    // True when that is the period's last day. Only then is `previous` set:
    // part of a period compared with a whole one would mislead.
    bool complete = false;
    VisibilityTotals totals;
    std::optional<VisibilityTotals> previous;
    std::vector<VisibilityDay> daily;
};
BOOST_DESCRIBE_STRUCT(Visibility, (), (available, note, detail, as_of, complete, totals, previous, daily))

struct LocationNumbers {
    std::int64_t location_id = 0;
    std::string title;
    ReviewNumbers reviews;
    std::int64_t running_count = 0;
    std::optional<double> running_average;
    // Nothing when Google gave no data for this location.
    std::optional<VisibilityTotals> visibility;
};
BOOST_DESCRIBE_STRUCT(LocationNumbers, (), (location_id, title, reviews, running_count, running_average, visibility))

// A day, or seven days from `start` when the period is longer than a month.
struct TrendBucket {
    std::string start;
    std::int64_t new_reviews = 0;
    std::int64_t positive = 0;
    std::int64_t mixed = 0;
    std::int64_t neutral = 0;
    std::int64_t negative = 0;
};
BOOST_DESCRIBE_STRUCT(TrendBucket, (), (start, new_reviews, positive, mixed, neutral, negative))

struct Ranked {
    std::string label;
    std::int64_t count = 0;
};
BOOST_DESCRIBE_STRUCT(Ranked, (), (label, count))

struct Highlight {
    std::string location_title;
    int rating = 0;
    std::string day;
    std::string reviewer_name;
    std::optional<std::string> reviewer_photo_url;
    std::string text;
};
BOOST_DESCRIBE_STRUCT(Highlight, (), (location_title, rating, day, reviewer_name, reviewer_photo_url, text))

// A staff member named in reviews (D-086): a name as a reviewer wrote it, at
// one location. The same name at two locations is two rows.
struct StaffMention {
    // Equal counts share a rank: 1, 1, 3.
    int rank = 0;
    std::int64_t location_id = 0;
    std::string location_title;
    std::string name;
    // Reviews in the period that mention the person positively, and negatively.
    std::int64_t positive = 0;
    std::int64_t negative = 0;
};
BOOST_DESCRIBE_STRUCT(StaffMention, (), (rank, location_id, location_title, name, positive, negative))

struct StaffRecognition {
    // Reviews in the period that mention at least one person positively or negatively.
    std::int64_t reviews_naming = 0;
    // Everyone with a positive mention: the most first, then fewer negative ones.
    std::vector<StaffMention> recognized;
    // Everyone with a negative mention: the most first, then fewer positive ones.
    std::vector<StaffMention> complained;
};
BOOST_DESCRIBE_STRUCT(StaffRecognition, (), (reviews_naming, recognized, complained))

struct Keywords {
    std::string month;  // "YYYY-MM"
    std::vector<SearchKeyword> items;
};
BOOST_DESCRIBE_STRUCT(Keywords, (), (month, items))

struct ReportData {
    // 2: has `staff` (D-086). A report stored before it loads with none.
    int version = 2;
    std::int64_t location_id = 0;
    std::string location_title;
    std::string period_start;
    std::string period_end;
    std::string previous_start;
    std::string previous_end;
    std::string time_zone;
    std::string generated_at;

    ReviewNumbers reviews;
    ReviewNumbers previous;
    std::int64_t running_count = 0;
    std::optional<double> running_average;
    std::vector<LocationNumbers> locations;
    std::vector<TrendBucket> trend;
    std::vector<Ranked> topics_praised;
    std::vector<Ranked> topics_complained;
    std::vector<Ranked> items_praised;
    std::vector<Ranked> items_complained;
    // Nothing when the location's industry has no item type for people, and in
    // reports built before the section existed.
    std::optional<StaffRecognition> staff;
    std::vector<Highlight> highlights_best;
    std::vector<Highlight> highlights_worst;
    std::int64_t escalations_opened = 0;
    std::int64_t escalations_closed = 0;
    Visibility visibility;
    std::optional<Keywords> keywords;

    // Written by the model on top of the figures; nothing when it gave no
    // usable answer, and `summary_note` then says why.
    std::optional<std::string> summary;
    std::string summary_note;
};
BOOST_DESCRIBE_STRUCT(ReportData, (),
                      (version, location_id, location_title, period_start, period_end, previous_start, previous_end, time_zone,
                       generated_at, reviews, previous, running_count, running_average, locations, trend, topics_praised,
                       topics_complained, items_praised, items_complained, staff, highlights_best, highlights_worst, escalations_opened,
                       escalations_closed, visibility, keywords, summary, summary_note))

inline constexpr std::size_t kRankedLimit = 5;
inline constexpr std::size_t kHighlightLimit = 3;
inline constexpr std::size_t kHighlightTextLimit = 700;
inline constexpr std::size_t kKeywordLimit = 10;
inline constexpr std::size_t kStaffLimit = 10;

// The most counted first, at most `limit`; equal counts in the order of the
// label, so the same data gives the same list.
std::vector<Ranked> ranked(const std::map<std::string, Ranked>& counted, std::size_t limit);

// What reviews praise and complain about, for a report and for the help with
// posts. A review counts once for a topic or an item and a polarity, however
// often it names it; items are told apart by their name in lower case. A
// topic without a label is shown by its id. Items of a person type are
// counted nowhere: a report lists people apart (D-086), and the help with
// posts names nobody.
class MentionCounter {
public:
    MentionCounter(std::map<std::string, std::string> topic_labels, std::set<std::string> person_types)
        : topic_labels_(std::move(topic_labels)), person_types_(std::move(person_types)) {}

    void add(const std::vector<TopicMention>& topics, const std::vector<ItemMention>& items);

    // Sets out's topics_praised, topics_complained, items_praised and items_complained, at most `limit` each.
    template <class Out>
    void rank_into(Out& out, std::size_t limit) const {
        out.topics_praised = ranked(topics_praised_, limit);
        out.topics_complained = ranked(topics_complained_, limit);
        out.items_praised = ranked(items_praised_, limit);
        out.items_complained = ranked(items_complained_, limit);
    }

private:
    std::map<std::string, std::string> topic_labels_;
    std::set<std::string> person_types_;
    std::map<std::string, Ranked> topics_praised_, topics_complained_, items_praised_, items_complained_;
};

ReviewNumbers review_numbers(const std::vector<ReportReview>& reviews);

// Everything but the summary.
ReportData build_report(const ReportInput& input);

// The sums of Google's daily metrics over the days given.
VisibilityTotals sum_metrics(const std::vector<MetricDay>& days);

// The change from `before` to `now` in percent; nothing when `before` is 0.
std::optional<double> percent_change(double now, double before);

}  // namespace poni::domain
