// The outbound HTTP client against the engine's own server on loopback:
// what it sends, what it returns, and that transport failures throw.

#include "core/tls.hpp"
#include "http/client.hpp"
#include "loopback.hpp"

#include <boost/json/parse.hpp>
#include <format>
#include <gtest/gtest.h>

using namespace poni;
namespace asio = boost::asio;
using boost::beast::http::verb;

namespace {

class OutboundClient : public ::testing::Test {
protected:
    OutboundClient() : tls_(core::make_tls_client_context()), client_(tls_.ctx) {
        const http::RouteInfo open{http::Access::anonymous, "test.echo"};
        const auto echo = [](const http::Request& request) -> asio::awaitable<http::Response> {
            co_return http::Response::json(200, boost::json::object{{"method", boost::beast::http::to_string(request.method)},
                                                                     {"query", request.query},
                                                                     {"body", request.body},
                                                                     {"content_type", request.header("content-type")},
                                                                     {"x_test", request.header("x-test")},
                                                                     {"host", request.header("host")},
                                                                     {"user_agent", request.header("user-agent")}});
        };
        router_.add(verb::get, "/echo", open, echo);
        router_.add(verb::post, "/echo", open, echo);
        router_.add(verb::get, "/missing", open, [](const http::Request&) -> asio::awaitable<http::Response> {
            co_return http::Response::error(404, "not_found", "No such thing.");
        });
    }

    // Sends one request to a server that lives for the duration of the call.
    http::ClientResponse send(http::ClientRequest request) {
        return test::with_server(io_, router_, [&](std::uint16_t port) {
            request.url.set_scheme("http").set_host("127.0.0.1").set_port_number(port);
            return client_.send(std::move(request));
        });
    }

    asio::io_context io_;
    core::TlsSetup tls_;
    http::NetClient client_;
    http::Router router_;
};

}  // namespace

TEST_F(OutboundClient, SendsAGetWithHeadersAndQuery) {
    http::ClientRequest request;
    request.url = boost::urls::url("http://placeholder/echo");
    request.url.params().append({"orderBy", "updateTime desc"});
    request.headers.emplace_back("X-Test", "yes");
    const auto response = send(std::move(request));
    ASSERT_TRUE(response.ok());
    const auto body = boost::json::parse(response.body);
    EXPECT_EQ(body.at("method").as_string(), "GET");
    EXPECT_EQ(body.at("query").as_string(), "orderBy=updateTime+desc");
    EXPECT_EQ(body.at("x_test").as_string(), "yes");
    EXPECT_TRUE(body.at("host").as_string().starts_with("127.0.0.1:"));
    EXPECT_TRUE(body.at("user_agent").as_string().starts_with("poni-gbp-engine/"));
}

TEST_F(OutboundClient, SendsAPostBody) {
    http::ClientRequest request;
    request.method = http::ClientMethod::post;
    request.url = boost::urls::url("http://placeholder/echo");
    request.content_type = "application/json";
    request.body = R"({"maxMessages": 10})";
    const auto body = boost::json::parse(send(std::move(request)).body);
    EXPECT_EQ(body.at("method").as_string(), "POST");
    EXPECT_EQ(body.at("body").as_string(), R"({"maxMessages": 10})");
    EXPECT_EQ(body.at("content_type").as_string(), "application/json");
}

TEST_F(OutboundClient, ReturnsErrorStatusesInsteadOfThrowing) {
    http::ClientRequest request;
    request.url = boost::urls::url("http://placeholder/missing");
    const auto response = send(std::move(request));
    EXPECT_EQ(response.status, 404u);
    EXPECT_FALSE(response.ok());
    EXPECT_TRUE(response.body.contains("not_found"));
}

TEST_F(OutboundClient, ThrowsWhenNothingAnswers) {
    http::ClientRequest request;
    request.url = boost::urls::url("http://127.0.0.1:1/");  // nothing listens on port 1
    EXPECT_THROW(test::run(io_, client_.send(std::move(request))), boost::system::system_error);

    http::ClientRequest odd;
    odd.url = boost::urls::url("ftp://127.0.0.1/file");
    EXPECT_THROW(test::run(io_, client_.send(std::move(odd))), std::invalid_argument);
}

// ---- against the Google mock (D-087) ------------------------------------------------------

namespace {

// A loopback server standing in for `poni-tools mock-google`: it answers every path it is given with what it received.
class AgainstTheMock : public ::testing::Test {
protected:
    AgainstTheMock() : tls_(core::make_tls_client_context()) {
        const http::RouteInfo open{http::Access::anonymous, "test.mock"};
        const auto echo = [](const http::Request& request) -> asio::awaitable<http::Response> {
            co_return http::Response::json(200, boost::json::object{{"method", boost::beast::http::to_string(request.method)},
                                                                     {"path", request.path},
                                                                     {"query", request.query},
                                                                     {"body", request.body}});
        };
        router_.add(verb::get, "/mybusiness.googleapis.com/v4/accounts/{a}/locations/{l}/reviews", open, echo);
        router_.add(verb::put, "/mybusiness.googleapis.com/v4/accounts/{a}/locations/{l}/reviews/{r}/reply", open, echo);
        router_.add(verb::put, "/v4/accounts/{a}/locations/{l}/reviews/{r}/reply", open, echo);
        router_.add(verb::post, "/oauth2.googleapis.com/token", open, echo);
        router_.add(verb::post, "/mybusiness.googleapis.com/v4/accounts/{a}/locations/{l}/localPosts", open, echo);
        router_.add(verb::delete_, "/mybusiness.googleapis.com/v4/accounts/{a}/locations/{l}/localPosts/{p}", open, echo);
        router_.add(verb::delete_, "/mybusiness.googleapis.com/v4/accounts/{a}/locations/{l}", open, echo);
        router_.add(verb::post, "/mybusiness.googleapis.com/v4/accounts/{a}/locations/{l}/media", open, echo);
    }

    // Sends one request through a client in mock mode, with the server as the mock.
    http::ClientResponse send(http::ClientRequest request) {
        std::optional<http::NetClient> client;
        return test::with_server(io_, router_, [&](std::uint16_t port) {
            client.emplace(tls_.ctx, boost::urls::url(std::format("http://127.0.0.1:{}", port)));
            return client->send(std::move(request));
        });
    }

    asio::io_context io_;
    core::TlsSetup tls_;
    http::Router router_;
};

}  // namespace

TEST_F(AgainstTheMock, AGoogleReadGoesToTheMockUnderItsHost) {
    http::ClientRequest request;
    request.url = boost::urls::url("https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/reviews");
    request.url.params().append({"orderBy", "updateTime desc"});
    const auto response = send(std::move(request));
    ASSERT_TRUE(response.ok()) << response.body;
    const auto body = boost::json::parse(response.body);
    EXPECT_EQ(body.at("path").as_string(), "/mybusiness.googleapis.com/v4/accounts/111/locations/9001/reviews");
    EXPECT_EQ(body.at("query").as_string(), "orderBy=updateTime+desc");
}

TEST_F(AgainstTheMock, TheOneWriteIsTheSameWriteOnTheMock) {
    http::ClientRequest request;
    request.method = http::ClientMethod::put;
    request.url = boost::urls::url("https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/reviews/r1/reply");
    request.content_type = "application/json";
    request.body = R"({"comment": "Thank you!"})";
    const auto response = send(std::move(request));
    ASSERT_TRUE(response.ok()) << response.body;
    const auto body = boost::json::parse(response.body);
    EXPECT_EQ(body.at("method").as_string(), "PUT");
    EXPECT_EQ(body.at("path").as_string(), "/mybusiness.googleapis.com/v4/accounts/111/locations/9001/reviews/r1/reply");
    EXPECT_EQ(body.at("body").as_string(), R"({"comment": "Thank you!"})");
}

TEST_F(AgainstTheMock, AnyOtherPutIsRefusedAsBeforeEvenOnTheMocksOwnAddress) {
    for (const auto* target : {"https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/reviews/r1",
                               "https://mybusinessbusinessinformation.googleapis.com/v1/locations/9001",
                               // The mock's address is not a reply address: only Google's is, rewritten by the client.
                               "http://127.0.0.1:1/mybusiness.googleapis.com/v4/accounts/111/locations/9001/reviews/r1/reply",
                               "http://127.0.0.1:1/v4/accounts/111/locations/9001/reviews/r1/reply"}) {
        http::ClientRequest request;
        request.method = http::ClientMethod::put;
        request.url = boost::urls::url(target);
        EXPECT_THROW(send(std::move(request)), std::invalid_argument) << target;
    }
}

TEST_F(AgainstTheMock, ALocalPostIsCreatedAndDeletedOnTheSameAddressesAsAtGoogle) {
    // The mock has no posts (D-088 point 13); the client's rules are the same in both modes all the same.
    http::ClientRequest create;
    create.method = http::ClientMethod::post;
    create.url = boost::urls::url("https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts");
    create.content_type = "application/json";
    create.body = R"({"summary": "Hello"})";
    const auto created = boost::json::parse(send(std::move(create)).body);
    EXPECT_EQ(created.at("method").as_string(), "POST");
    EXPECT_EQ(created.at("path").as_string(), "/mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts");

    http::ClientRequest remove;
    remove.method = http::ClientMethod::del;
    remove.url = boost::urls::url("https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/77");
    const auto removed = boost::json::parse(send(std::move(remove)).body);
    EXPECT_EQ(removed.at("method").as_string(), "DELETE");
    EXPECT_EQ(removed.at("path").as_string(), "/mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/77");
    EXPECT_EQ(removed.at("body").as_string(), "") << "a delete carries nothing";
}

TEST_F(AgainstTheMock, AnyOtherDeleteOrPostToTheGbpHostIsRefusedEvenWhereTheMockWouldAnswer) {
    for (const auto* target : {"https://mybusiness.googleapis.com/v4/accounts/111/locations/9001",
                               // The mock's own address is not Google's: only Google's is a local post's, rewritten by the client.
                               "http://127.0.0.1:1/mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/77"}) {
        http::ClientRequest request;
        request.method = http::ClientMethod::del;
        request.url = boost::urls::url(target);
        EXPECT_THROW(send(std::move(request)), std::invalid_argument) << target;
    }
    http::ClientRequest media;
    media.method = http::ClientMethod::post;
    media.url = boost::urls::url("https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/media");
    EXPECT_THROW(send(std::move(media)), std::invalid_argument);
}

TEST_F(AgainstTheMock, EveryOtherGoogleServiceIsRefused) {
    for (const auto* target : {"https://pubsub.googleapis.com/v1/projects/p/subscriptions/s:pull",
                               "https://storage.googleapis.com/upload/storage/v1/b/poni-public/o",
                               "https://www.googleapis.com/oauth2/v3/certs",
                               "http://metadata.google.internal/computeMetadata/v1/instance/service-accounts/default/token",
                               "https://MyBusiness.GoogleAPIs.com./v4/accounts/1/locations/2/reviews",
                               "https://mybusiness.googleapis.com:443/v4/accounts/1/locations/2/reviews"}) {
        http::ClientRequest request;
        request.url = boost::urls::url(target);
        EXPECT_THROW(send(std::move(request)), std::invalid_argument) << target;
    }
}

TEST(GoogleMockAddress, KeepsThePathAndQueryUnderTheGoogleHost) {
    const boost::urls::url mock("http://127.0.0.1:8090");
    const auto mapped = http::google_mock_address(
        mock, boost::urls::url("https://accounts.google.com/o/oauth2/v2/auth?client_id=c&redirect_uri=http%3A%2F%2Flocalhost%3A5173%2Fx"));
    ASSERT_TRUE(mapped);
    EXPECT_EQ(mapped->buffer(), "http://127.0.0.1:8090/accounts.google.com/o/oauth2/v2/auth?client_id=c&redirect_uri=http%3A%2F%2Flocalhost%3A5173%2Fx");

    // An id with an encoded slash stays one segment.
    const auto reply = http::google_mock_address(
        mock, boost::urls::url("https://mybusiness.googleapis.com/v4/accounts/1/locations/2/reviews/r1%2F..%2Fx/reply"));
    ASSERT_TRUE(reply);
    EXPECT_EQ(reply->segments().size(), 9u);

    for (const auto* other : {"https://api.openai.com/v1/chat/completions", "https://pubsub.googleapis.com/v1/x",
                              "http://mybusiness.googleapis.com/v4/accounts", "https://mybusiness.googleapis.com.evil.example/v4"}) {
        EXPECT_FALSE(http::google_mock_address(mock, boost::urls::url(other))) << other;
    }
    EXPECT_TRUE(http::is_google_address(boost::urls::url("https://pubsub.googleapis.com/v1")));
    EXPECT_TRUE(http::is_google_address(boost::urls::url("http://metadata.google.internal/x")));
    EXPECT_FALSE(http::is_google_address(boost::urls::url("https://notgoogleapis.com/x")));
    EXPECT_FALSE(http::is_google_address(boost::urls::url("https://dashscope-intl.aliyuncs.com/compatible-mode/v1")));
}
