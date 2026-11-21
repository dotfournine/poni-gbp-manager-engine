#include "http/router.hpp"

#include "support.hpp"

#include <boost/json/parse.hpp>
#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

using namespace poni;
using boost::beast::http::verb;

namespace {

http::Response dispatch(const http::Router& router, verb method, std::string path) {
    boost::asio::io_context io;
    http::Request request;
    request.method = method;
    request.path = std::move(path);
    return test::run(io, router.dispatch(request));
}

const http::RouteInfo kOpen{http::Access::anonymous, "test.open"};

http::Router make_router() {
    http::Router router;
    router.add(verb::get, "/api/thing", kOpen, [](const http::Request&) -> boost::asio::awaitable<http::Response> {
        co_return http::Response::json(200, boost::json::object{{"ok", true}});
    });
    router.add(verb::post, "/api/thing", kOpen, [](const http::Request& r) -> boost::asio::awaitable<http::Response> {
        co_return http::Response::json(201, boost::json::object{{"echo", r.body}});
    });
    router.add(verb::get, "/api/broken", kOpen, [](const http::Request&) -> boost::asio::awaitable<http::Response> {
        throw std::runtime_error("secret detail");
        co_return http::Response{};
    });
    router.add(verb::get, "/api/things/{id}/parts/{part}", kOpen,
               [](const http::Request& r) -> boost::asio::awaitable<http::Response> {
                   co_return http::Response::json(200, boost::json::object{{"id", r.params.at("id")}, {"part", r.params.at("part")}});
               });
    router.add(verb::get, "/api/private", {http::Access::user, "test.private"},
               [](const http::Request& r) -> boost::asio::awaitable<http::Response> {
                   co_return http::Response::json(200, boost::json::object{{"user", r.principal ? r.principal->email : "nobody"}});
               });
    return router;
}

std::string error_code(const http::Response& response) {
    return std::string(boost::json::parse(response.body).at("error").at("code").as_string());
}

}  // namespace

TEST(Router, DispatchesByMethodAndPath) {
    const auto router = make_router();
    const auto got = dispatch(router, verb::get, "/api/thing");
    EXPECT_EQ(got.status, 200u);
    EXPECT_EQ(got.content_type, "application/json");
    EXPECT_EQ(got.body, R"({"ok":true})");
    EXPECT_EQ(dispatch(router, verb::post, "/api/thing").status, 201u);
}

TEST(Router, UnknownPathIs404) {
    const auto response = dispatch(make_router(), verb::get, "/api/nothing");
    EXPECT_EQ(response.status, 404u);
    EXPECT_EQ(error_code(response), "not_found");
}

TEST(Router, PathsMatchExactly) {
    EXPECT_EQ(dispatch(make_router(), verb::get, "/api/thing/").status, 404u);
    EXPECT_EQ(dispatch(make_router(), verb::get, "/api/thin").status, 404u);
}

TEST(Router, WrongMethodIs405WithAllowHeader) {
    const auto response = dispatch(make_router(), verb::delete_, "/api/thing");
    EXPECT_EQ(response.status, 405u);
    EXPECT_EQ(error_code(response), "method_not_allowed");
    ASSERT_EQ(response.headers.size(), 1u);
    EXPECT_EQ(response.headers[0].first, "Allow");
    EXPECT_EQ(response.headers[0].second, "GET, POST");
}

TEST(Router, HandlerExceptionIs500WithoutDetails) {
    const auto response = dispatch(make_router(), verb::get, "/api/broken");
    EXPECT_EQ(response.status, 500u);
    EXPECT_EQ(error_code(response), "internal_error");
    EXPECT_EQ(response.body.find("secret detail"), std::string::npos);
}

TEST(Router, PathParametersAreHandedToTheHandler) {
    const auto router = make_router();
    EXPECT_EQ(dispatch(router, verb::get, "/api/things/42/parts/lid").body, R"({"id":"42","part":"lid"})");
    EXPECT_EQ(dispatch(router, verb::get, "/api/things//parts/lid").status, 404u) << "a parameter is never empty";
    EXPECT_EQ(dispatch(router, verb::get, "/api/things/42/parts").status, 404u);
}

TEST(Router, RoutesThatNeedAUserAreClosedWithoutAnAuthorizationHook) {
    const auto response = dispatch(make_router(), verb::get, "/api/private");
    EXPECT_EQ(response.status, 401u);
    EXPECT_EQ(error_code(response), "unauthenticated");
}

TEST(Router, TheHookRunsBeforeEveryHandlerAndCanRefuse) {
    auto router = make_router();
    std::vector<std::string> asked;
    router.set_auth_hook([&](http::Request& request, const http::RouteInfo& route) -> boost::asio::awaitable<std::optional<http::Response>> {
        asked.push_back(route.action);
        if (request.header("x-test-user").empty()) {
            if (route.access == http::Access::user) co_return http::Response::error(401, "unauthenticated", "no");
            co_return std::nullopt;
        }
        request.principal = http::Principal{.user_id = 7, .email = std::string(request.header("x-test-user"))};
        co_return std::nullopt;
    });

    EXPECT_EQ(dispatch(router, verb::get, "/api/thing").status, 200u);
    EXPECT_EQ(dispatch(router, verb::get, "/api/private").status, 401u);
    EXPECT_EQ(asked, (std::vector<std::string>{"test.open", "test.private"}));

    boost::asio::io_context io;
    http::Request request;
    request.path = "/api/private";
    request.headers["x-test-user"] = "staff@example.com";
    EXPECT_EQ(test::run(io, router.dispatch(request)).body, R"({"user":"staff@example.com"})");
}

TEST(Router, AFailingHookIsA500NotAnOpenDoor) {
    auto router = make_router();
    router.set_auth_hook([](http::Request&, const http::RouteInfo&) -> boost::asio::awaitable<std::optional<http::Response>> {
        throw std::runtime_error("database down");
        co_return std::nullopt;
    });
    EXPECT_EQ(dispatch(router, verb::get, "/api/private").status, 500u);
}

TEST(QueryParams, DecodesAndSplitsValues) {
    const http::QueryParams query("q=hello+world%21&rating=1,2&rating=5&empty=&page=3&flag=true");
    EXPECT_EQ(query.get("q"), "hello world!");
    EXPECT_EQ(query.all("rating"), (std::vector<std::string>{"1", "2", "5"}));
    EXPECT_FALSE(query.get("empty")) << "an empty value counts as absent";
    EXPECT_FALSE(query.get("missing"));
    bool bad = false;
    EXPECT_EQ(query.integer("page", bad), 3);
    EXPECT_EQ(query.boolean("flag", bad), true);
    EXPECT_FALSE(bad);
    EXPECT_FALSE(query.integer("q", bad));
    EXPECT_TRUE(bad);
}

TEST(Cookies, AreFoundByExactName) {
    constexpr std::string_view header = "theme=dark; poni_session=abc123; other_poni_session=zzz";
    EXPECT_EQ(http::cookie_value(header, "poni_session"), "abc123");
    EXPECT_EQ(http::cookie_value(header, "theme"), "dark");
    EXPECT_FALSE(http::cookie_value(header, "session"));
    EXPECT_FALSE(http::cookie_value("", "poni_session"));
}
