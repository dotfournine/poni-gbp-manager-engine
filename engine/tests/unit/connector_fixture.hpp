// What the unit tests of a connector start from: an io_context to run its
// coroutines on, a clock the test controls, a scripted HTTP client, and a
// rate limiter that lets the named APIs through. Each test fixture builds
// its own connectors from these, in its own constructor.
#pragma once

#include "connectors/google/auth.hpp"
#include "core/clock.hpp"
#include "core/rate_limiter.hpp"
#include "fakes.hpp"
#include "support.hpp"

#include <boost/asio/io_context.hpp>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <string_view>

namespace poni::test {

// Google's answer to a token request.
inline constexpr std::string_view kTokenOk = R"({"access_token": "token-1", "expires_in": 3600, "token_type": "Bearer"})";

// A file in tests/fixtures, as text.
inline std::string fixture(const std::string& name) {
    std::ifstream in(std::filesystem::path(PONI_TEST_FIXTURES_DIR) / name);
    EXPECT_TRUE(in) << "missing fixture " << name;
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// A stored refresh token, for google::refresh_fetch: the token
// request then goes through the scripted client like any other.
inline google::CredentialsLoader stored_credentials() {
    return []() -> boost::asio::awaitable<google::Result<google::OAuthCredentials>> {
        co_return google::OAuthCredentials{"client", "secret", "refresh"};
    };
}

// A token source that needs no network: every call gets "token-1".
struct StaticTokens final : google::TokenSource {
    boost::asio::awaitable<google::Result<std::string>> access_token() override { co_return std::string("token-1"); }
    void invalidate() override { ++invalidated; }
    int invalidated = 0;
};

class ConnectorTest : public ::testing::Test {
protected:
    // `apis` are the rate limiter's names of the APIs the tests call.
    explicit ConnectorTest(std::initializer_list<std::string_view> apis, double per_minute = 6000) {
        for (const auto api : apis) limiter_.configure(std::string(api), {.per_minute = per_minute, .burst = 100});
    }

    template <class T>
    T await(boost::asio::awaitable<T> task) {
        return run(io_, std::move(task));
    }

    boost::asio::io_context io_;
    core::ManualClock clock_{t0()};
    FakeHttp http_;
    core::RateLimiter limiter_{io_.get_executor(), clock_};
};

}  // namespace poni::test
