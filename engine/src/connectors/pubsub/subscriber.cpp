#include "connectors/pubsub/subscriber.hpp"

#include "core/crypto.hpp"
#include "core/json.hpp"

#include <boost/json/serialize.hpp>

#include <format>

namespace poni::pubsub {

namespace asio = boost::asio;

namespace {

struct WireMessage {
    std::optional<std::string> data;  // base64
    std::string messageId;
    std::optional<std::string> publishTime;
};
BOOST_DESCRIBE_STRUCT(WireMessage, (), (data, messageId, publishTime))

struct ReceivedMessage {
    std::string ackId;
    WireMessage message;
};
BOOST_DESCRIBE_STRUCT(ReceivedMessage, (), (ackId, message))

struct PullResponse {
    std::optional<std::vector<ReceivedMessage>> receivedMessages;
};
BOOST_DESCRIBE_STRUCT(PullResponse, (), (receivedMessages))

}  // namespace

asio::awaitable<Result<std::string>> HttpSubscriber::post(std::string_view action, std::string body) {
    http::ClientRequest request;
    request.method = http::ClientMethod::post;
    request.url = google::url("https://pubsub.googleapis.com", {"v1", "projects", project_, "subscriptions", std::format("{}:{}", name_, action)});
    if (quota_project_ && !quota_project_->empty()) request.headers.emplace_back("x-goog-user-project", *quota_project_);
    request.content_type = "application/json";
    request.body = std::move(body);
    auto response = co_await google::send(api_, kPubsubApi, std::move(request));
    if (!response) co_return std::unexpected(response.error());
    co_return std::move(response->body);
}

asio::awaitable<Result<std::vector<Message>>> HttpSubscriber::pull(int max_messages) {
    // returnImmediately is deprecated but still honoured; without it the call
    // would hold a connection open while the subscription is empty.
    const auto body = co_await post(
        "pull", boost::json::serialize(boost::json::object{{"returnImmediately", true}, {"maxMessages", max_messages}}));
    if (!body) co_return std::unexpected(body.error());
    const auto parsed = core::parse_json<PullResponse>(*body);
    if (!parsed) {
        co_return std::unexpected(google::invalid_response(std::format("pull: {}", parsed.error())));
    }
    std::vector<Message> out;
    for (const auto& received : parsed->receivedMessages.value_or(std::vector<ReceivedMessage>{})) {
        out.push_back({.ack_id = received.ackId,
                       .message_id = received.message.messageId,
                       .publish_time = received.message.publishTime.value_or(""),
                       .data = core::base64_decode(received.message.data.value_or("")).value_or("")});
    }
    co_return out;
}

asio::awaitable<Result<void>> HttpSubscriber::acknowledge(std::vector<std::string> ack_ids) {
    if (ack_ids.empty()) co_return Result<void>{};
    boost::json::array ids;
    for (auto& id : ack_ids) ids.emplace_back(std::move(id));
    const auto body = co_await post("acknowledge", boost::json::serialize(boost::json::object{{"ackIds", std::move(ids)}}));
    if (!body) co_return std::unexpected(body.error());
    co_return Result<void>{};
}

}  // namespace poni::pubsub
