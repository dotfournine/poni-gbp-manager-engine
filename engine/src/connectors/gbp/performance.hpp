// The Business Profile Performance API, read only: daily metrics and monthly
// search keywords of one location. A report asks for them when it is built
// (D-085); nothing else does, and nothing is stored but the report.
//
// Both calls are GETs. Google's data is about three days behind: a day it
// has nothing for yet comes back without a value and is left out here.
#pragma once

#include "connectors/google/auth.hpp"
#include "domain/report.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/json/value.hpp>

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace poni::gbp {

using google::ApiError;
using google::Result;

// Rate limiter key. The quota is 300 requests a minute for the project.
inline constexpr std::string_view kPerformanceApi = "gbp.performance";

class PerformanceReader {
public:
    virtual ~PerformanceReader() = default;

    // Every daily metric a report shows, for each day from `from` to `to`
    // (both included) that Google has a value for.
    virtual boost::asio::awaitable<Result<std::vector<domain::MetricDay>>> daily_metrics(std::string location_id, domain::Day from,
                                                                                         domain::Day to) = 0;

    // The search keywords of one calendar month, most shown first.
    virtual boost::asio::awaitable<Result<std::vector<domain::SearchKeyword>>> search_keywords(std::string location_id,
                                                                                               std::chrono::year_month month) = 0;
};

// The mapping of Google's two answers, apart from the network so it can be
// tested with the sanitized samples. Nothing: the body is not what the API documents.
std::optional<std::vector<domain::MetricDay>> parse_daily_metrics(const boost::json::value& body);
std::optional<std::vector<domain::SearchKeyword>> parse_search_keywords(const boost::json::value& body);

class HttpPerformanceReader final : public PerformanceReader {
public:
    explicit HttpPerformanceReader(google::Api api) : api_(api) {}

    boost::asio::awaitable<Result<std::vector<domain::MetricDay>>> daily_metrics(std::string location_id, domain::Day from,
                                                                                 domain::Day to) override;
    boost::asio::awaitable<Result<std::vector<domain::SearchKeyword>>> search_keywords(std::string location_id,
                                                                                       std::chrono::year_month month) override;

private:
    google::Api api_;
};

}  // namespace poni::gbp
