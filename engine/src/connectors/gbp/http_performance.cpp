#include "connectors/gbp/performance.hpp"

#include <array>
#include <charconv>
#include <format>

namespace poni::gbp {

namespace asio = boost::asio;
using namespace std::chrono;
using google::invalid_response;

namespace {

constexpr std::string_view kOrigin = "https://businessprofileperformance.googleapis.com";
// At most this many pages of keywords are read for one month.
constexpr int kKeywordPages = 3;

// What a report shows (FR-8). Messaging clicks are left out: no client uses them.
constexpr std::array<std::string_view, 9> kMetrics = {
    "BUSINESS_IMPRESSIONS_MOBILE_SEARCH", "BUSINESS_IMPRESSIONS_DESKTOP_SEARCH", "BUSINESS_IMPRESSIONS_MOBILE_MAPS",
    "BUSINESS_IMPRESSIONS_DESKTOP_MAPS",  "CALL_CLICKS",                         "BUSINESS_DIRECTION_REQUESTS",
    "WEBSITE_CLICKS",                     "BUSINESS_FOOD_MENU_CLICKS",           "BUSINESS_BOOKINGS"};

const boost::json::object* object_member(const boost::json::value& value, std::string_view key) {
    if (!value.is_object()) return nullptr;
    const auto* member = value.get_object().if_contains(key);
    return member && member->is_object() ? &member->get_object() : nullptr;
}

// Google writes 64-bit numbers as strings ("478"); a plain number is taken too.
std::optional<std::int64_t> int64_of(const boost::json::value* value) {
    if (!value) return std::nullopt;
    if (value->is_int64()) return value->get_int64();
    if (!value->is_string()) return std::nullopt;
    const auto& text = value->get_string();
    std::int64_t out = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    if (ec != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return out;
}

std::optional<domain::Day> day_of(const boost::json::object& date) {
    const auto y = int64_of(date.if_contains("year")), m = int64_of(date.if_contains("month")), d = int64_of(date.if_contains("day"));
    if (!y || !m || !d) return std::nullopt;
    const year_month_day ymd{year{static_cast<int>(*y)}, month{static_cast<unsigned>(*m)}, day{static_cast<unsigned>(*d)}};
    if (!ymd.ok()) return std::nullopt;
    return sys_days{ymd};
}

// The query parameters are snake_case although the JSON is camelCase (spike B9).
void add_date(boost::urls::url& url, std::string_view prefix, domain::Day day) {
    const year_month_day ymd{day};
    url.params().append({std::format("{}.year", prefix), std::to_string(static_cast<int>(ymd.year()))});
    url.params().append({std::format("{}.month", prefix), std::to_string(static_cast<unsigned>(ymd.month()))});
    url.params().append({std::format("{}.day", prefix), std::to_string(static_cast<unsigned>(ymd.day()))});
}

}  // namespace

std::optional<std::vector<domain::MetricDay>> parse_daily_metrics(const boost::json::value& body) {
    if (!body.is_object()) return std::nullopt;
    std::vector<domain::MetricDay> out;
    const auto* groups = google::array_member(body, "multiDailyMetricTimeSeries");
    if (!groups) return out;  // Google omits empty arrays
    for (const auto& group : *groups) {
        const auto* series_list = google::array_member(group, "dailyMetricTimeSeries");
        if (!series_list) continue;
        for (const auto& series : *series_list) {
            if (!series.is_object()) return std::nullopt;
            const auto* metric = series.get_object().if_contains("dailyMetric");
            const auto* time_series = object_member(series, "timeSeries");
            if (!metric || !metric->is_string()) return std::nullopt;
            const auto* values = time_series ? time_series->if_contains("datedValues") : nullptr;
            if (!values || !values->is_array()) continue;
            for (const auto& dated : values->get_array()) {
                const auto* date = object_member(dated, "date");
                const auto day = date ? day_of(*date) : std::nullopt;
                if (!day) return std::nullopt;
                // No value: Google has nothing for that day (yet), or it was zero. Either way there is nothing to add.
                if (const auto value = int64_of(dated.get_object().if_contains("value"))) {
                    out.push_back({.metric = std::string(metric->get_string()), .day = *day, .value = *value});
                }
            }
        }
    }
    return out;
}

std::optional<std::vector<domain::SearchKeyword>> parse_search_keywords(const boost::json::value& body) {
    if (!body.is_object()) return std::nullopt;
    std::vector<domain::SearchKeyword> out;
    const auto* counts = google::array_member(body, "searchKeywordsCounts");
    if (!counts) return out;
    for (const auto& entry : *counts) {
        if (!entry.is_object()) return std::nullopt;
        const auto* keyword = entry.get_object().if_contains("searchKeyword");
        const auto* insights = object_member(entry, "insightsValue");
        if (!keyword || !keyword->is_string() || !insights) continue;
        out.push_back({.keyword = std::string(keyword->get_string()),
                       .impressions = int64_of(insights->if_contains("value")),
                       .below = int64_of(insights->if_contains("threshold"))});
    }
    return out;
}

asio::awaitable<Result<std::vector<domain::MetricDay>>> HttpPerformanceReader::daily_metrics(std::string location_id, domain::Day from,
                                                                                            domain::Day to) {
    http::ClientRequest request;
    // The id and the verb are one segment, percent-encoded by Boost.URL: the id can't change the path.
    request.url = google::url(kOrigin, {"v1", "locations", location_id + ":fetchMultiDailyMetricsTimeSeries"});
    for (const auto metric : kMetrics) request.url.params().append({"dailyMetrics", metric});
    add_date(request.url, "dailyRange.start_date", from);
    add_date(request.url, "dailyRange.end_date", to);
    const auto body = co_await google::fetch_json(api_, kPerformanceApi, std::move(request));
    if (!body) co_return std::unexpected(body.error());
    auto parsed = parse_daily_metrics(*body);
    if (!parsed) co_return std::unexpected(invalid_response("the daily metrics could not be read"));
    co_return std::move(*parsed);
}

asio::awaitable<Result<std::vector<domain::SearchKeyword>>> HttpPerformanceReader::search_keywords(std::string location_id,
                                                                                                  year_month month) {
    std::vector<domain::SearchKeyword> all;
    std::optional<std::string> page_token;
    for (int page = 0; page < kKeywordPages; ++page) {
        http::ClientRequest request;
        request.url = google::url(kOrigin, {"v1", "locations", location_id, "searchkeywords", "impressions", "monthly"});
        const auto year_text = std::to_string(static_cast<int>(month.year()));
        const auto month_text = std::to_string(static_cast<unsigned>(month.month()));
        request.url.params().append({"monthlyRange.start_month.year", year_text});
        request.url.params().append({"monthlyRange.start_month.month", month_text});
        request.url.params().append({"monthlyRange.end_month.year", year_text});
        request.url.params().append({"monthlyRange.end_month.month", month_text});
        request.url.params().append({"pageSize", "100"});
        if (page_token) request.url.params().append({"pageToken", *page_token});
        const auto body = co_await google::fetch_json(api_, kPerformanceApi, std::move(request));
        if (!body) co_return std::unexpected(body.error());
        auto parsed = parse_search_keywords(*body);
        if (!parsed) co_return std::unexpected(invalid_response("the search keywords could not be read"));
        all.append_range(std::move(*parsed));
        page_token = google::next_page_token(*body);
        if (!page_token) break;
    }
    co_return all;
}

}  // namespace poni::gbp
