// Google Business Profile, read side: accounts, locations and reviews.
//
// Reader is the interface the engine uses; HttpReader talks to Google and
// tests use a fake. Nothing here can post, change or delete anything at
// Google: the four writes have interfaces of their own (writer.hpp,
// local_posts.hpp).
//
// Payload structs mirror Google's JSON (camelCase names). Google omits empty
// arrays and unset fields, so nearly everything is optional. `raw` is the
// payload as received, stored as jsonb next to the typed columns.
#pragma once

#include "connectors/google/auth.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/describe/class.hpp>
#include <boost/json/value.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace poni::gbp {

using google::ApiError;
using google::Result;

struct Account {
    std::string name;  // "accounts/{id}"
};
BOOST_DESCRIBE_STRUCT(Account, (), (name))

struct PhoneNumbers {
    std::optional<std::string> primaryPhone;
};
BOOST_DESCRIBE_STRUCT(PhoneNumbers, (), (primaryPhone))

struct Category {
    std::optional<std::string> displayName;
};
BOOST_DESCRIBE_STRUCT(Category, (), (displayName))

struct Categories {
    std::optional<Category> primaryCategory;
};
BOOST_DESCRIBE_STRUCT(Categories, (), (primaryCategory))

struct PostalAddress {
    std::optional<std::string> regionCode;
    std::optional<std::string> postalCode;
    std::optional<std::string> administrativeArea;
    std::optional<std::string> locality;
    std::optional<std::vector<std::string>> addressLines;
};
BOOST_DESCRIBE_STRUCT(PostalAddress, (), (regionCode, postalCode, administrativeArea, locality, addressLines))

struct LocationMetadata {
    std::optional<std::string> placeId;
    std::optional<std::string> mapsUri;
};
BOOST_DESCRIBE_STRUCT(LocationMetadata, (), (placeId, mapsUri))

struct Location {
    std::string name;  // "locations/{id}"
    std::optional<std::string> title;
    std::optional<std::string> storeCode;
    std::optional<std::string> websiteUri;
    std::optional<PhoneNumbers> phoneNumbers;
    std::optional<Categories> categories;
    std::optional<PostalAddress> storefrontAddress;
    std::optional<LocationMetadata> metadata;
    boost::json::value raw;  // not mapped: set by the reader
};
BOOST_DESCRIBE_STRUCT(Location, (), (name, title, storeCode, websiteUri, phoneNumbers, categories, storefrontAddress, metadata))

struct Reviewer {
    std::optional<std::string> displayName;
    std::optional<std::string> profilePhotoUrl;
    std::optional<bool> isAnonymous;
};
BOOST_DESCRIBE_STRUCT(Reviewer, (), (displayName, profilePhotoUrl, isAnonymous))

struct ReviewReply {
    std::optional<std::string> comment;
    std::optional<std::string> updateTime;
    std::optional<std::string> reviewReplyState;
};
BOOST_DESCRIBE_STRUCT(ReviewReply, (), (comment, updateTime, reviewReplyState))

struct Review {
    std::string name;  // "accounts/{a}/locations/{l}/reviews/{r}"
    std::string reviewId;
    std::optional<Reviewer> reviewer;
    std::optional<std::string> starRating;  // "ONE" .. "FIVE"
    std::optional<std::string> comment;     // absent for a rating-only review
    std::string createTime;                 // RFC 3339
    std::string updateTime;
    std::optional<ReviewReply> reviewReply;
    boost::json::value raw;  // not mapped: set by the reader
};
BOOST_DESCRIBE_STRUCT(Review, (), (name, reviewId, reviewer, starRating, comment, createTime, updateTime, reviewReply))

struct ReviewsPage {
    std::vector<Review> reviews;
    std::optional<std::string> next_page_token;
    // Google's own count. It has disagreed with the list (spike B4), so it is
    // recorded for information and never used to decide completeness.
    std::optional<std::int64_t> total_review_count;
    // Entries that couldn't be mapped (a review without an id, say). They are
    // skipped so one odd review can't block a whole location.
    std::size_t skipped = 0;
};

// The three ids inside a review's resource name.
struct ReviewName {
    std::string account_id;
    std::string location_id;
    std::string review_id;
};
std::optional<ReviewName> parse_review_name(std::string_view name);

// The three ids of "accounts/{a}/locations/{l}/{collection}/{id}", the form
// of a review's and of a local post's name. Nothing for any other form, or
// with an empty id.
std::optional<std::array<std::string_view, 3>> split_resource_name(std::string_view name, std::string_view collection);

// "accounts/123" -> "123"; nothing if `name` doesn't have that prefix.
std::optional<std::string> strip_prefix(std::string_view name, std::string_view prefix);

class Reader {
public:
    virtual ~Reader() = default;

    // Every account the signed-in user can see (all pages).
    virtual boost::asio::awaitable<Result<std::vector<Account>>> list_accounts() = 0;

    // Every location under the account (all pages).
    virtual boost::asio::awaitable<Result<std::vector<Location>>> list_locations(std::string account_id) = 0;

    // One page (50) of a location's reviews, most recently updated first.
    virtual boost::asio::awaitable<Result<ReviewsPage>> list_reviews(std::string account_id, std::string location_id,
                                                                     std::optional<std::string> page_token) = 0;

    // One page (50) of reviews across several locations of one account, most
    // recently updated first. The main polling path: one call covers them all.
    virtual boost::asio::awaitable<Result<ReviewsPage>> batch_get_reviews(std::string account_id,
                                                                          std::vector<std::string> location_ids,
                                                                          std::optional<std::string> page_token) = 0;

    // One review. ApiError::Kind::not_found means it is gone: deleted by its
    // author or taken down by Google.
    virtual boost::asio::awaitable<Result<Review>> get_review(ReviewName name) = 0;
};

// Where the v4 API is: reviews, the reply to a review, and local posts.
inline constexpr std::string_view kV4Origin = "https://mybusiness.googleapis.com";

// Rate limiter keys. The quotas are per Google Cloud project and per API.
inline constexpr std::string_view kAccountsApi = "gbp.accounts";    // Account Management v1, 300/min
inline constexpr std::string_view kLocationsApi = "gbp.locations";  // Business Information v1, 300/min
inline constexpr std::string_view kReviewsApi = "gbp.reviews";      // My Business v4, 600/min; the reply to a review too

// gbp::Reader over HTTPS, as the managing account.
class HttpReader final : public Reader {
public:
    explicit HttpReader(google::Api api) : api_(api) {}

    boost::asio::awaitable<Result<std::vector<Account>>> list_accounts() override;
    boost::asio::awaitable<Result<std::vector<Location>>> list_locations(std::string account_id) override;
    boost::asio::awaitable<Result<ReviewsPage>> list_reviews(std::string account_id, std::string location_id,
                                                             std::optional<std::string> page_token) override;
    boost::asio::awaitable<Result<ReviewsPage>> batch_get_reviews(std::string account_id, std::vector<std::string> location_ids,
                                                                  std::optional<std::string> page_token) override;
    boost::asio::awaitable<Result<Review>> get_review(ReviewName name) override;

private:
    google::Api api_;
};

}  // namespace poni::gbp
