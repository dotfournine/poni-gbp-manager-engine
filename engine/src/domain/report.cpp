#include "domain/report.hpp"

#include "core/text.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <set>

namespace poni::domain {

namespace {

using namespace std::chrono;

bool in_period(Day day, const Period& period) { return day >= period.start && day <= period.end; }

void count_sentiment(std::string_view sentiment, auto& into) {
    if (sentiment == "positive") {
        ++into.positive;
    } else if (sentiment == "mixed") {
        ++into.mixed;
    } else if (sentiment == "neutral") {
        ++into.neutral;
    } else if (sentiment == "negative") {
        ++into.negative;
    }
}

std::optional<double> average(std::int64_t sum, std::int64_t count) {
    if (count == 0) return std::nullopt;
    return static_cast<double>(sum) / static_cast<double>(count);
}

// Equal counts share a rank (1, 1, 3); equal people are in the order of their name.
std::vector<StaffMention> rank_staff(std::vector<StaffMention> people, bool by_positive) {
    const auto key = [by_positive](const StaffMention& m) {
        // The count the list is about first, then the fewer of the other.
        return std::pair{by_positive ? m.positive : m.negative, -(by_positive ? m.negative : m.positive)};
    };
    std::ranges::stable_sort(people, [&](const StaffMention& a, const StaffMention& b) {
        if (key(a) != key(b)) return key(a) > key(b);
        return text::ascii_lower(a.name) < text::ascii_lower(b.name);
    });
    for (std::size_t i = 0; i < people.size(); ++i) {
        people[i].rank = i > 0 && key(people[i]) == key(people[i - 1]) ? people[i - 1].rank : static_cast<int>(i + 1);
    }
    if (people.size() > kStaffLimit) people.resize(kStaffLimit);
    return people;
}

StaffRecognition staff_recognition(const ReportInput& input) {
    StaffRecognition out;
    // Name folded -> the person; the name as first written.
    std::map<std::string, StaffMention> people;
    for (const auto& review : input.reviews) {
        // A review counts once for a person and a polarity, however often it names them.
        std::set<std::string> seen;
        for (const auto& item : review.items) {
            if (!input.person_item_types.contains(item.type)) continue;
            if (item.polarity != "positive" && item.polarity != "negative") continue;
            const auto name = std::string(text::trim(item.name));
            if (name.empty()) continue;
            const auto folded = text::ascii_lower(name);
            if (!seen.insert(item.polarity + ":" + folded).second) continue;
            auto& person = people[folded];
            if (person.name.empty()) person.name = text::truncate(name, 80);
            ++(item.polarity == "positive" ? person.positive : person.negative);
        }
        if (!seen.empty()) ++out.reviews_naming;
    }
    std::vector<StaffMention> recognized, complained;
    for (const auto& [key, person] : people) {
        if (person.positive > 0) recognized.push_back(person);
        if (person.negative > 0) complained.push_back(person);
    }
    out.recognized = rank_staff(std::move(recognized), true);
    out.complained = rank_staff(std::move(complained), false);
    return out;
}

void add_metric(VisibilityTotals& totals, std::string_view metric, std::int64_t value) {
    if (metric == "BUSINESS_IMPRESSIONS_MOBILE_SEARCH") {
        totals.search_mobile += value;
    } else if (metric == "BUSINESS_IMPRESSIONS_DESKTOP_SEARCH") {
        totals.search_desktop += value;
    } else if (metric == "BUSINESS_IMPRESSIONS_MOBILE_MAPS") {
        totals.maps_mobile += value;
    } else if (metric == "BUSINESS_IMPRESSIONS_DESKTOP_MAPS") {
        totals.maps_desktop += value;
    } else if (metric == "CALL_CLICKS") {
        totals.calls += value;
    } else if (metric == "BUSINESS_DIRECTION_REQUESTS") {
        totals.direction_requests += value;
    } else if (metric == "WEBSITE_CLICKS") {
        totals.website_clicks += value;
    } else if (metric == "BUSINESS_FOOD_MENU_CLICKS") {
        totals.menu_clicks += value;
    } else if (metric == "BUSINESS_BOOKINGS") {
        totals.bookings += value;
    }
}

bool is_view(std::string_view metric) { return metric.starts_with("BUSINESS_IMPRESSIONS_"); }

std::vector<Highlight> highlights(const ReportInput& input, bool best) {
    std::vector<const ReportReview*> picked;
    for (const auto& review : input.reviews) {
        if (review.rating == 0 || text::trim(review.text).empty()) continue;
        if (best ? review.rating >= 4 : review.rating <= 3) picked.push_back(&review);
    }
    // Best: the most stars, then the most words. Worst: the fewest stars, then the most words.
    std::ranges::stable_sort(picked, [best](const ReportReview* a, const ReportReview* b) {
        if (a->rating != b->rating) return best ? a->rating > b->rating : a->rating < b->rating;
        const auto la = text::length(a->text), lb = text::length(b->text);
        if (la != lb) return la > lb;
        return a->created < b->created;
    });
    if (picked.size() > kHighlightLimit) picked.resize(kHighlightLimit);
    std::vector<Highlight> out;
    for (const auto* review : picked) {
        out.push_back({.rating = review->rating,
                       .day = format_day(review->day),
                       .reviewer_name = review->reviewer_name,
                       .reviewer_photo_url = review->reviewer_photo_url,
                       .text = text::truncate(text::trim(review->text), kHighlightTextLimit)});
    }
    return out;
}

Visibility visibility(const ReportInput& input, const Period& previous) {
    Visibility out;
    const auto& performance = input.performance;
    if (performance.error) {
        out.detail = *performance.error;
        out.note = "Google could not be asked for the visibility numbers when this report was made.";
        return out;
    }
    // Google publishes each metric with its own delay (Maps views a day or
    // two after Search views), and a day without a value is either not there
    // yet or zero. The view counts are the metrics a business has every day,
    // so the day the numbers go up to is the earliest "last day" among them:
    // up to that day every metric is in.
    std::map<std::string, Day> last_day;
    for (const auto& value : performance.days) {
        if (!is_view(value.metric) || !in_period(value.day, input.period)) continue;
        auto [entry, inserted] = last_day.try_emplace(value.metric, value.day);
        if (!inserted && value.day > entry->second) entry->second = value.day;
    }
    std::optional<Day> as_of;
    for (const auto& [metric, day] : last_day) {
        if (!as_of || day < *as_of) as_of = day;
    }
    if (!as_of) {
        out.note = "Google has no visibility numbers for this period yet.";
        return out;
    }
    out.available = true;
    out.as_of = format_day(*as_of);
    out.complete = *as_of == input.period.end;

    VisibilityTotals before;
    std::map<Day, std::int64_t> views_by_day;
    for (auto day = input.period.start; day <= *as_of; day += days{1}) views_by_day[day] = 0;
    for (const auto& value : performance.days) {
        if (in_period(value.day, input.period)) {
            if (value.day > *as_of) continue;  // a metric that is ahead of the others: not counted yet
            add_metric(out.totals, value.metric, value.value);
            if (is_view(value.metric)) views_by_day[value.day] += value.value;
        } else if (in_period(value.day, previous)) {
            add_metric(before, value.metric, value.value);
        }
    }
    // The period is whole: the two periods can be compared.
    if (out.complete) out.previous = before;
    for (const auto& [day, views] : views_by_day) out.daily.push_back({format_day(day), views});
    return out;
}

std::optional<Keywords> keywords(const LocationPerformance& performance) {
    if (performance.keywords.empty()) return std::nullopt;
    Keywords out{.month = performance.keywords_month, .items = performance.keywords};
    // Counted keywords first, by count; then the ones Google only gives a threshold for.
    std::ranges::stable_sort(out.items, [](const SearchKeyword& a, const SearchKeyword& b) {
        if (a.impressions.has_value() != b.impressions.has_value()) return a.impressions.has_value();
        if (a.impressions != b.impressions) return a.impressions > b.impressions;
        if (a.below != b.below) return a.below > b.below;
        return a.keyword < b.keyword;
    });
    if (out.items.size() > kKeywordLimit) out.items.resize(kKeywordLimit);
    return out;
}

}  // namespace

std::vector<Ranked> ranked(const std::map<std::string, Ranked>& counted, std::size_t limit) {
    std::vector<Ranked> out;
    for (const auto& [key, entry] : counted) out.push_back(entry);
    std::ranges::stable_sort(out, [](const Ranked& a, const Ranked& b) { return a.count != b.count ? a.count > b.count : a.label < b.label; });
    if (out.size() > limit) out.resize(limit);
    return out;
}

void MentionCounter::add(const std::vector<TopicMention>& topics, const std::vector<ItemMention>& items) {
    std::set<std::string> seen;
    for (const auto& topic : topics) {
        if (topic.polarity != "positive" && topic.polarity != "negative") continue;
        if (!seen.insert("t:" + topic.polarity + ":" + topic.id).second) continue;
        const auto label = topic_labels_.find(topic.id);
        auto& entry = (topic.polarity == "positive" ? topics_praised_ : topics_complained_)[topic.id];
        entry.label = label == topic_labels_.end() ? topic.id : label->second;
        ++entry.count;
    }
    for (const auto& item : items) {
        if (item.polarity != "positive" && item.polarity != "negative") continue;
        if (person_types_.contains(item.type)) continue;
        const auto name = std::string(text::trim(item.name));
        if (name.empty()) continue;
        const auto key = text::ascii_lower(name);
        if (!seen.insert("i:" + item.polarity + ":" + key).second) continue;
        auto& entry = (item.polarity == "positive" ? items_praised_ : items_complained_)[key];
        if (entry.label.empty()) entry.label = text::truncate(name, 80);
        ++entry.count;
    }
}

std::optional<double> percent_change(double now, double before) {
    if (before == 0) return std::nullopt;
    return (now - before) / before * 100.0;
}

ReviewNumbers review_numbers(const std::vector<ReportReview>& reviews) {
    ReviewNumbers out;
    std::int64_t rated = 0, rating_sum = 0;
    std::vector<double> response_hours;
    for (const auto& review : reviews) {
        ++out.new_reviews;
        if (review.rating >= 1 && review.rating <= 5) {
            ++rated;
            rating_sum += review.rating;
            ++out.stars[static_cast<std::size_t>(review.rating - 1)];
        }
        if (review.replied_at) {
            ++out.replied;
            const auto waited = duration_cast<seconds>(*review.replied_at - review.created).count();
            response_hours.push_back(static_cast<double>(std::max<std::int64_t>(waited, 0)) / 3600.0);
        }
        count_sentiment(review.sentiment, out);
    }
    out.average_rating = average(rating_sum, rated);
    if (out.new_reviews > 0) out.response_rate = static_cast<double>(out.replied) / static_cast<double>(out.new_reviews);
    if (!response_hours.empty()) {
        std::ranges::sort(response_hours);
        const auto middle = response_hours.size() / 2;
        out.median_response_hours =
            response_hours.size() % 2 == 1 ? response_hours[middle] : (response_hours[middle - 1] + response_hours[middle]) / 2.0;
    }
    return out;
}

VisibilityTotals sum_metrics(const std::vector<MetricDay>& days) {
    VisibilityTotals totals;
    for (const auto& day : days) add_metric(totals, day.metric, day.value);
    return totals;
}

std::vector<std::string> all_report_sections() { return {kReportSections.begin(), kReportSections.end()}; }

std::optional<std::string> report_sections_problem(const std::vector<std::string>& sections) {
    if (sections.empty()) return "\"sections\" must name at least one section.";
    std::set<std::string_view> seen;
    for (const auto& key : sections) {
        if (std::ranges::find(kReportSections, key) == kReportSections.end()) {
            std::string known;
            for (const auto section : kReportSections) known += std::format("{}{}", known.empty() ? "" : ", ", section);
            return std::format("\"sections\" has an unknown section '{}'; the sections are {}.", key, known);
        }
        if (!seen.insert(key).second) return std::format("\"sections\" names '{}' twice.", key);
    }
    return std::nullopt;
}

bool has_section(const std::vector<std::string>& sections, std::string_view key) { return std::ranges::find(sections, key) != sections.end(); }

ReportData build_report(const ReportInput& input) {
    const auto previous = previous_period(input.period);
    ReportData out;
    out.location_id = input.location_id;
    out.location_title = input.location_title;
    out.period_start = format_day(input.period.start);
    out.period_end = format_day(input.period.end);
    out.previous_start = format_day(previous.start);
    out.previous_end = format_day(previous.end);
    out.time_zone = input.time_zone;
    out.generated_at = input.generated_at;
    out.sections = input.sections;
    const auto has = [&](std::string_view key) { return has_section(input.sections, key); };

    // Always: the email's headline and the report's row read them, whatever the sections.
    out.reviews = review_numbers(input.reviews);
    out.previous = review_numbers(input.previous_reviews);

    out.running_count = input.running_count;
    out.running_average = average(input.running_rating_sum, input.running_count);

    if (has("reviews")) {
        // Days, or weeks for a period longer than a month.
        const auto length = (input.period.end - input.period.start).count() + 1;
        const auto step = days{length > 31 ? 7 : 1};
        for (auto start = input.period.start; start <= input.period.end; start += step) {
            TrendBucket bucket{.start = format_day(start)};
            for (const auto& review : input.reviews) {
                if (review.day < start || review.day >= start + step) continue;
                ++bucket.new_reviews;
                count_sentiment(review.sentiment, bucket);
            }
            out.trend.push_back(std::move(bucket));
        }
    }

    if (has("topics")) {
        MentionCounter mentions(input.topic_labels, input.person_item_types);
        for (const auto& review : input.reviews) mentions.add(review.topics, review.items);
        mentions.rank_into(out, kRankedLimit);
    }
    if (has("employee_recognition") && !input.person_item_types.empty()) out.staff = staff_recognition(input);

    if (has("highlights")) {
        out.highlights_best = highlights(input, true);
        out.highlights_worst = highlights(input, false);
    }
    out.escalations_opened = input.escalations_opened;
    out.escalations_closed = input.escalations_closed;
    if (has("visibility")) {
        out.visibility = visibility(input, previous);
        out.keywords = keywords(input.performance);
    }
    return out;
}

}  // namespace poni::domain
