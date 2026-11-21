#include "connectors/gbp/reader.hpp"

#include "core/json.hpp"

#include <boost/json/serialize.hpp>

#include <array>
#include <format>

namespace poni::gbp {

namespace asio = boost::asio;
using google::invalid_response;

namespace {

constexpr int kReviewPageSize = 50;  // the API's maximum
constexpr std::string_view kNewestFirst = "updateTime desc";
// What the engine stores about a location. A read mask is mandatory.
constexpr std::string_view kLocationReadMask =
    "name,title,storeCode,languageCode,phoneNumbers,categories,storefrontAddress,websiteUri,"
    "regularHours,specialHours,openInfo,metadata,profile";

// Maps one review and keeps the payload. Nothing if it can't be used.
std::optional<Review> to_review(const boost::json::value& value) {
    auto review = core::from_json<Review>(value);
    if (!review || review->reviewId.empty() || !parse_review_name(review->name)) return std::nullopt;
    review->raw = value;
    return std::move(*review);
}

// The reviews of one page: the entries of `key`, or of each entry its member
// `inside` where the review is wrapped (the batch call's). An entry that
// can't be used is counted and skipped.
ReviewsPage reviews_page(const boost::json::value& body, std::string_view key, std::string_view inside = {}) {
    ReviewsPage page;
    if (const auto* entries = google::array_member(body, key)) {
        for (const auto& entry : *entries) {
            const auto* review_value = &entry;
            if (!inside.empty()) review_value = entry.is_object() ? entry.get_object().if_contains(inside) : nullptr;
            if (auto review = review_value ? to_review(*review_value) : std::nullopt) {
                page.reviews.push_back(std::move(*review));
            } else {
                ++page.skipped;
            }
        }
    }
    page.next_page_token = google::next_page_token(body);
    return page;
}

// Every page of a list: the page at `first`, then each one Google names
// next, with the entries of `key` mapped by `map`. An entry that can't be
// mapped ends the read with its error, before the next page is asked for.
template <class T, class Map>
asio::awaitable<Result<std::vector<T>>> list_all(google::Api api, std::string_view limiter_key, boost::urls::url first, std::string_view key,
                                                 Map map) {
    std::vector<T> all;
    std::optional<std::string> page_token;
    do {
        http::ClientRequest request;
        request.url = first;
        if (page_token) request.url.params().append({"pageToken", *page_token});
        const auto page = co_await google::fetch_json(api, limiter_key, std::move(request));
        if (!page) co_return std::unexpected(page.error());
        if (const auto* entries = google::array_member(*page, key)) {
            for (const auto& value : *entries) {
                auto entry = map(value);
                if (!entry) co_return std::unexpected(std::move(entry.error()));
                all.push_back(std::move(*entry));
            }
        }
        page_token = google::next_page_token(*page);
    } while (page_token);
    co_return all;
}

}  // namespace

std::optional<std::string> strip_prefix(std::string_view name, std::string_view prefix) {
    if (!name.starts_with(prefix) || name.size() == prefix.size()) return std::nullopt;
    const auto rest = name.substr(prefix.size());
    if (rest.contains('/')) return std::nullopt;
    return std::string(rest);
}

std::optional<std::array<std::string_view, 3>> split_resource_name(std::string_view name, std::string_view collection) {
    std::array<std::string_view, 6> parts;
    std::size_t count = 0;
    while (true) {
        const auto slash = name.find('/');
        if (count == parts.size()) return std::nullopt;
        parts[count++] = name.substr(0, slash);
        if (slash == std::string_view::npos) break;
        name.remove_prefix(slash + 1);
    }
    if (count != 6 || parts[0] != "accounts" || parts[2] != "locations" || parts[4] != collection) return std::nullopt;
    if (parts[1].empty() || parts[3].empty() || parts[5].empty()) return std::nullopt;
    return std::array{parts[1], parts[3], parts[5]};
}

std::optional<ReviewName> parse_review_name(std::string_view name) {
    const auto ids = split_resource_name(name, "reviews");
    if (!ids) return std::nullopt;
    return ReviewName{std::string((*ids)[0]), std::string((*ids)[1]), std::string((*ids)[2])};
}

asio::awaitable<Result<std::vector<Account>>> HttpReader::list_accounts() {
    return list_all<Account>(api_, kAccountsApi, google::url("https://mybusinessaccountmanagement.googleapis.com", {"v1", "accounts"}), "accounts",
                             [](const boost::json::value& value) -> Result<Account> {
                                 auto account = core::from_json<Account>(value);
                                 if (!account) return std::unexpected(invalid_response(std::format("accounts: {}", account.error())));
                                 return std::move(*account);
                             });
}

asio::awaitable<Result<std::vector<Location>>> HttpReader::list_locations(std::string account_id) {
    auto first = google::url("https://mybusinessbusinessinformation.googleapis.com", {"v1", "accounts", account_id, "locations"});
    first.params().append({"readMask", kLocationReadMask});
    first.params().append({"pageSize", "100"});
    return list_all<Location>(api_, kLocationsApi, std::move(first), "locations", [](const boost::json::value& value) -> Result<Location> {
        auto location = core::from_json<Location>(value);
        if (!location) return std::unexpected(invalid_response(std::format("locations: {}", location.error())));
        if (!strip_prefix(location->name, "locations/")) return std::unexpected(invalid_response("locations: a location has an unexpected name"));
        location->raw = value;
        return std::move(*location);
    });
}

asio::awaitable<Result<ReviewsPage>> HttpReader::list_reviews(std::string account_id, std::string location_id,
                                                              std::optional<std::string> page_token) {
    http::ClientRequest request;
    request.url = google::url(kV4Origin, {"v4", "accounts", account_id, "locations", location_id, "reviews"});
    request.url.params().append({"pageSize", std::to_string(kReviewPageSize)});
    request.url.params().append({"orderBy", kNewestFirst});
    if (page_token) request.url.params().append({"pageToken", *page_token});
    const auto body = co_await google::fetch_json(api_, kReviewsApi, std::move(request));
    if (!body) co_return std::unexpected(body.error());

    auto page = reviews_page(*body, "reviews");
    if (body->is_object()) {
        if (const auto* total = body->get_object().if_contains("totalReviewCount"); total && total->is_int64()) {
            page.total_review_count = total->get_int64();
        }
    }
    co_return page;
}

asio::awaitable<Result<ReviewsPage>> HttpReader::batch_get_reviews(std::string account_id,
                                                                   std::vector<std::string> location_ids,
                                                                   std::optional<std::string> page_token) {
    // A POST, but a read: Google put the location list in the body.
    http::ClientRequest request;
    request.method = http::ClientMethod::post;
    request.url = google::url(kV4Origin, {"v4", "accounts", account_id, "locations:batchGetReviews"});
    boost::json::array names;
    for (const auto& location_id : location_ids) {
        names.emplace_back(std::format("accounts/{}/locations/{}", account_id, location_id));
    }
    boost::json::object payload{{"locationNames", std::move(names)},
                                {"pageSize", kReviewPageSize},
                                {"orderBy", kNewestFirst},
                                {"ignoreRatingOnlyReviews", false}};
    if (page_token) payload["pageToken"] = *page_token;
    request.content_type = "application/json";
    request.body = boost::json::serialize(payload);
    const auto body = co_await google::fetch_json(api_, kReviewsApi, std::move(request));
    if (!body) co_return std::unexpected(body.error());

    co_return reviews_page(*body, "locationReviews", "review");
}

asio::awaitable<Result<Review>> HttpReader::get_review(ReviewName name) {
    http::ClientRequest request;
    request.url = google::url(kV4Origin, {"v4", "accounts", name.account_id, "locations", name.location_id, "reviews", name.review_id});
    const auto body = co_await google::fetch_json(api_, kReviewsApi, std::move(request));
    if (!body) co_return std::unexpected(body.error());
    auto review = to_review(*body);
    if (!review) co_return std::unexpected(invalid_response("the review could not be read"));
    co_return std::move(*review);
}

}  // namespace poni::gbp
