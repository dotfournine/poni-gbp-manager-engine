#include "connectors/gbp/local_posts.hpp"

#include "connectors/gbp/reader.hpp"
#include "core/json.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>

namespace poni::gbp {

namespace asio = boost::asio;
using google::invalid_response;

namespace {

constexpr int kPageSize = 100;  // the API's maximum

// Maps one post and keeps the payload. Nothing if it has no name the engine can use.
std::optional<LocalPost> to_post(const boost::json::value& value) {
    auto post = core::from_json<LocalPost>(value);
    if (!post || !parse_local_post_name(post->name)) return std::nullopt;
    post->raw = value;
    return std::move(*post);
}

}  // namespace

asio::awaitable<Result<LocalPostsPage>> HttpLocalPostReader::list_local_posts(std::string account_id, std::string location_id,
                                                                              std::optional<std::string> page_token) {
    http::ClientRequest request;
    request.url = google::url(kV4Origin, {"v4", "accounts", account_id, "locations", location_id, "localPosts"});
    request.url.params().append({"pageSize", std::to_string(kPageSize)});
    if (page_token) request.url.params().append({"pageToken", *page_token});
    const auto response = co_await google::send(api_, kLocalPostsApi, std::move(request));
    if (!response) co_return std::unexpected(response.error());

    boost::system::error_code ec;
    const auto body = boost::json::parse(response->body, ec);
    if (ec || !body.is_object()) co_return std::unexpected(invalid_response("localPosts: the response is not a JSON object"));
    LocalPostsPage page;
    if (const auto* posts = google::array_member(body, "localPosts")) {
        for (const auto& value : *posts) {
            if (auto post = to_post(value)) {
                page.posts.push_back(std::move(*post));
            } else {
                ++page.skipped;
            }
        }
    }
    page.next_page_token = google::next_page_token(body);
    co_return page;
}

asio::awaitable<Result<LocalPost>> HttpLocalPostReader::get_local_post(LocalPostName name) {
    http::ClientRequest request;
    request.url = google::url(kV4Origin, {"v4", "accounts", name.account_id, "locations", name.location_id, "localPosts", name.post_id});
    const auto response = co_await google::send(api_, kLocalPostsApi, std::move(request));
    if (!response) co_return std::unexpected(response.error());
    boost::system::error_code ec;
    const auto body = boost::json::parse(response->body, ec);
    if (ec) co_return std::unexpected(invalid_response("localPosts: the response is not JSON"));
    auto post = to_post(body);
    if (!post) co_return std::unexpected(invalid_response("localPosts: the post in the response can't be read"));
    co_return std::move(*post);
}

asio::awaitable<Result<LocalPost>> HttpLocalPostWriter::create_local_post(std::string account_id, std::string location_id,
                                                                          boost::json::object body) {
    http::ClientRequest request;
    request.method = http::ClientMethod::post;
    request.url = google::url(kV4Origin, {"v4", "accounts", account_id, "locations", location_id, "localPosts"});
    request.content_type = "application/json";
    request.body = boost::json::serialize(body);
    const auto response = co_await google::send(api_, kLocalPostsApi, std::move(request));
    if (!response) co_return std::unexpected(response.error());

    // Google accepted the create. A body that can't be read does not undo that:
    // the caller is told so (invalid_response) and must not send it again.
    boost::system::error_code ec;
    const auto answer = boost::json::parse(response->body, ec);
    if (ec) co_return std::unexpected(invalid_response("Google accepted the post, but its answer is not JSON"));
    auto post = to_post(answer);
    if (!post) co_return std::unexpected(invalid_response("Google accepted the post, but its answer names no post"));
    co_return std::move(*post);
}

asio::awaitable<Result<void>> HttpLocalPostWriter::delete_local_post(LocalPostName name) {
    http::ClientRequest request;
    request.method = http::ClientMethod::del;
    request.url = google::url(kV4Origin, {"v4", "accounts", name.account_id, "locations", name.location_id, "localPosts", name.post_id});
    const auto response = co_await google::send(api_, kLocalPostsApi, std::move(request));
    if (!response) co_return std::unexpected(response.error());
    co_return Result<void>{};
}

asio::awaitable<Result<LocalPost>> HttpLocalPostWriter::update_local_post(LocalPostName name, std::vector<std::string_view> mask,
                                                                          boost::json::object body) {
    http::ClientRequest request;
    request.method = http::ClientMethod::patch;
    request.url = google::url(kV4Origin, {"v4", "accounts", name.account_id, "locations", name.location_id, "localPosts", name.post_id});
    // The one query a write carries: the names joined by commas, as one parameter. Whatever a caller hands
    // over is written as that parameter's value and never as more of the address; the outbound client then
    // takes the address only if the value is field names an edit may change, each once
    // (http::is_local_post_update_url), and refuses it otherwise before anything is sent.
    std::string names;
    for (const auto field : mask) {
        if (!names.empty()) names.push_back(',');
        names.append(field);
    }
    request.url.params().append({"updateMask", names});
    request.content_type = "application/json";
    request.body = boost::json::serialize(body);
    const auto response = co_await google::send(api_, kLocalPostsApi, std::move(request));
    if (!response) co_return std::unexpected(response.error());

    // Google accepted the edit. A body that can't be read does not undo that: the caller is told so
    // (invalid_response) and must not send it again.
    boost::system::error_code ec;
    const auto answer = boost::json::parse(response->body, ec);
    if (ec) co_return std::unexpected(invalid_response("Google accepted the edit, but its answer is not JSON"));
    auto post = to_post(answer);
    if (!post) co_return std::unexpected(invalid_response("Google accepted the edit, but its answer names no post"));
    co_return std::move(*post);
}

}  // namespace poni::gbp
