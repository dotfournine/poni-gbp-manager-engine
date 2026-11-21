// GET /api/health over a real socket, against a real PostgreSQL and against
// a database that is down.

#include "core/version.hpp"
#include "db/database.hpp"
#include "db_fixture.hpp"
#include "http/health.hpp"
#include "loopback.hpp"

#include <boost/asio/detached.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/json/parse.hpp>

using namespace poni;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = beast::http;

namespace {

using HttpResponse = bhttp::response<bhttp::string_body>;

class HealthEndpoint : public test::DbTest {
protected:
    // Starts a server for `database_url`, sends the requests on one
    // connection, and stops the server.
    std::vector<HttpResponse> exchange(const std::string& database_url, std::vector<std::pair<bhttp::verb, std::string>> requests) {
        db::Database database(database_url, 2);
        http::Router router;
        router.add(bhttp::verb::get, "/api/health", {http::Access::anonymous, "system.health"},
                   http::make_health_handler(database));
        return test::with_server(io_, router, [&](std::uint16_t port) -> asio::awaitable<std::vector<HttpResponse>> {
            beast::tcp_stream stream(io_);
            co_await stream.async_connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);
            std::vector<HttpResponse> out;
            beast::flat_buffer buffer;
            for (const auto& [method, target] : requests) {
                bhttp::request<bhttp::empty_body> request{method, target, 11};
                request.set(bhttp::field::host, "127.0.0.1");
                co_await bhttp::async_write(stream, request, asio::use_awaitable);
                bhttp::response_parser<bhttp::string_body> parser;
                if (method == bhttp::verb::head) parser.skip(true);
                co_await bhttp::async_read(stream, buffer, parser, asio::use_awaitable);
                out.push_back(parser.release());
            }
            co_return out;
        });
    }

    asio::io_context io_;
};

}  // namespace

TEST_F(HealthEndpoint, ReportsOkVersionAndReachableDatabase) {
    const auto responses = exchange(url(), {{bhttp::verb::get, "/api/health"}});
    const auto& response = responses.at(0);
    EXPECT_EQ(response.result_int(), 200u);
    EXPECT_EQ(response[bhttp::field::content_type], "application/json");

    const auto body = boost::json::parse(response.body()).as_object();
    EXPECT_EQ(body.size(), 3u);
    EXPECT_EQ(body.at("status").as_string(), "ok");
    EXPECT_EQ(body.at("version").as_string(), core::kVersion);
    EXPECT_EQ(body.at("database").as_object().size(), 1u);
    EXPECT_TRUE(body.at("database").at("reachable").as_bool());
}

TEST_F(HealthEndpoint, Reports503DegradedWhenTheDatabaseIsDown) {
    // Port 1 on loopback: nothing listens there, so the connection is refused at once.
    const auto responses = exchange("postgresql://poni:poni@127.0.0.1:1/poni?connect_timeout=2",
                                    {{bhttp::verb::get, "/api/health"}});
    const auto& response = responses.at(0);
    EXPECT_EQ(response.result_int(), 503u);

    const auto body = boost::json::parse(response.body()).as_object();
    EXPECT_EQ(body.at("status").as_string(), "degraded");
    EXPECT_EQ(body.at("version").as_string(), core::kVersion);
    EXPECT_FALSE(body.at("database").at("reachable").as_bool());
    EXPECT_EQ(response.body().find("127.0.0.1"), std::string::npos) << "connection details must not leak";
}

TEST_F(HealthEndpoint, ServesSeveralRequestsOnOneConnection) {
    const auto responses = exchange(url(), {{bhttp::verb::get, "/api/health?verbose=1"},
                                            {bhttp::verb::head, "/api/health"},
                                            {bhttp::verb::post, "/api/health"},
                                            {bhttp::verb::get, "/api/nope"},
                                            {bhttp::verb::get, "/api/health"}});
    ASSERT_EQ(responses.size(), 5u);
    EXPECT_EQ(responses[0].result_int(), 200u) << "the query string is not part of the path";
    EXPECT_EQ(responses[1].result_int(), 200u);
    EXPECT_TRUE(responses[1].body().empty()) << "HEAD has no body";
    EXPECT_EQ(responses[2].result_int(), 405u);
    EXPECT_EQ(responses[2][bhttp::field::allow], "GET");
    EXPECT_EQ(responses[3].result_int(), 404u);
    EXPECT_EQ(responses[4].result_int(), 200u);
}
