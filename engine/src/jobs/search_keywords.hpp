// The search keywords of a profile for whoever shows them (the help with
// posts; a report reads them the same way). Google has a month's keywords
// only when the month is nearly over, so the newest of the last few months
// that has any is taken.
#pragma once

#include "connectors/gbp/performance.hpp"

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <format>
#include <string>
#include <vector>

namespace poni::jobs {

// "2026-10".
inline std::string month_text(std::chrono::year_month month) {
    return std::format("{:04}-{:02}", static_cast<int>(month.year()), static_cast<unsigned>(month.month()));
}

struct MonthKeywords {
    std::string month;  // month_text(); "" when none of the months has keywords
    std::vector<domain::SearchKeyword> keywords;
};

// The keywords of the newest of `months` calendar months, counted back from
// `newest`, that has any. A month Google could not be asked about ends the
// search with Google's error; a transport failure throws.
inline boost::asio::awaitable<gbp::Result<MonthKeywords>> newest_search_keywords(gbp::PerformanceReader& reader, std::string location_id,
                                                                                 std::chrono::year_month newest, int months) {
    for (int back = 0; back < months; ++back) {
        const auto month = newest - std::chrono::months{back};
        auto keywords = co_await reader.search_keywords(location_id, month);
        if (!keywords) co_return std::unexpected(keywords.error());
        if (!keywords->empty()) co_return MonthKeywords{.month = month_text(month), .keywords = std::move(*keywords)};
    }
    co_return MonthKeywords{};
}

}  // namespace poni::jobs
