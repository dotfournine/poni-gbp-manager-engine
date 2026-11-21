#include "connectors/gbp/writer.hpp"
#include "core/json.hpp"

#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>

namespace poni::gbp {

namespace asio = boost::asio;

asio::awaitable<Result<ReviewReply>> HttpReplyWriter::put_reply(ReviewName name, std::string comment) {
    http::ClientRequest request;
    request.method = http::ClientMethod::put;
    request.url = google::url(kV4Origin, {"v4", "accounts", name.account_id, "locations", name.location_id, "reviews", name.review_id, "reply"});
    request.content_type = "application/json";
    request.body = boost::json::serialize(boost::json::object{{"comment", comment}});
    const auto response = co_await google::send(api_, kReviewsApi, std::move(request));
    if (!response) co_return std::unexpected(response.error());
    // Google accepted the reply. What it echoes back is informative only.
    auto accepted = core::parse_json<ReviewReply>(response->body).value_or(ReviewReply{});
    co_return accepted;
}

}  // namespace poni::gbp
