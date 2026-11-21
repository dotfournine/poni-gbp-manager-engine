// Cloud Pub/Sub over REST (D-012): pull and acknowledge, nothing else.
// GBP publishes a small message when a review is created or updated; the
// message only names the review, which is then fetched through gbp::Reader.
#pragma once

#include "core/config.hpp"
#include "connectors/google/auth.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/describe/class.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace poni::pubsub {

using google::ApiError;
using google::Result;

struct Message {
    std::string ack_id;
    std::string message_id;
    std::string publish_time;  // RFC 3339
    std::string data;          // decoded payload
};

// What GBP puts in a notification (the decoded `data`).
struct GbpNotification {
    std::string type;  // NEW_REVIEW, UPDATED_REVIEW, GOOGLE_UPDATE, ...
    std::optional<std::string> review;  // "accounts/{a}/locations/{l}/reviews/{r}"
};
BOOST_DESCRIBE_STRUCT(GbpNotification, (), (type, review))

class Subscriber {
public:
    virtual ~Subscriber() = default;
    // Up to `max_messages` waiting messages; returns at once when there are none.
    virtual boost::asio::awaitable<Result<std::vector<Message>>> pull(int max_messages) = 0;
    // Messages that aren't acknowledged are delivered again after the ack deadline.
    virtual boost::asio::awaitable<Result<void>> acknowledge(std::vector<std::string> ack_ids) = 0;
};

// Rate limiter key.
inline constexpr std::string_view kPubsubApi = "pubsub";

class HttpSubscriber final : public Subscriber {
public:
    // `subscription` is the project and the name of "projects/{project}/subscriptions/{name}",
    // as parse_subscription returns them. User credentials (gcloud locally)
    // need `quota_project`; a service account doesn't.
    HttpSubscriber(google::Api api, std::pair<std::string, std::string> subscription, std::optional<std::string> quota_project)
        : api_(api), project_(std::move(subscription.first)), name_(std::move(subscription.second)), quota_project_(std::move(quota_project)) {}

    boost::asio::awaitable<Result<std::vector<Message>>> pull(int max_messages) override;
    boost::asio::awaitable<Result<void>> acknowledge(std::vector<std::string> ack_ids) override;

private:
    boost::asio::awaitable<Result<std::string>> post(std::string_view action, std::string body);

    google::Api api_;
    std::string project_;
    std::string name_;
    std::optional<std::string> quota_project_;
};

// "projects/{p}/subscriptions/{s}" -> {p, s}: the configuration's parser (PONI_PUBSUB_SUBSCRIPTION).
using core::parse_subscription;

}  // namespace poni::pubsub
