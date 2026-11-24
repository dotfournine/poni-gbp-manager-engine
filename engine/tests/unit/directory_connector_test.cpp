// The Workspace directory's connector against a scripted HTTP client: the
// one address it reads, page after page, what it makes of each user, and
// that a list that cannot be read whole is an error and never a shorter list.

#include "connectors/workspace/directory.hpp"
#include "http/client.hpp"
#include "unit/connector_fixture.hpp"

#include <boost/system/system_error.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <format>
#include <string>
#include <vector>

using namespace poni;
using namespace std::chrono_literals;
using google::ApiError;
using test::fixture;
using test::kTokenOk;

namespace {

constexpr std::string_view kFirstPage = "https://admin.googleapis.com/admin/directory/v1/users?customer=my_customer&maxResults=500";

class Directory : public test::ConnectorTest {
protected:
    Directory()
        : ConnectorTest({workspace::kDirectoryApi, google::kOAuthApi}),
          tokens_(clock_, google::refresh_fetch(http_, limiter_, test::stored_credentials())),
          directory_(api_) {}

    // Requests sent to the directory, without the token requests.
    std::vector<http::ClientRequest> directory_requests() const {
        std::vector<http::ClientRequest> out;
        for (const auto& request : http_.requests) {
            if (request.url.host() == "admin.googleapis.com") out.push_back(request);
        }
        return out;
    }

    google::Result<std::vector<workspace::DirectoryUser>> list() { return await(directory_.list_users()); }

    google::CachedTokenSource tokens_;
    google::Api api_{http_, tokens_, limiter_};
    workspace::HttpDirectory directory_;
};

std::string page_of(const std::string& users, std::string_view token = {}) {
    if (token.empty()) return std::format(R"({{"users": [{}]}})", users);
    return std::format(R"({{"users": [{}], "nextPageToken": "{}"}})", users, token);
}

std::string user(std::string_view id) { return std::format(R"({{"id": "{}", "primaryEmail": "{}@example.com"}})", id, id); }

}  // namespace

TEST_F(Directory, TheListFollowsItsPages) {
    http_.reply(200, std::string(kTokenOk));
    http_.reply(200, fixture("directory-users-page.json"));
    http_.reply(200, fixture("directory-users-last-page.json"));
    const auto users = list();
    ASSERT_TRUE(users) << users.error().message;

    const auto requests = directory_requests();
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_EQ(requests[0].url.buffer(), kFirstPage);
    EXPECT_EQ(requests[1].url.buffer(), std::string(kFirstPage) + "&pageToken=made-up-page-2");
    EXPECT_EQ(http_.header(1, "Authorization"), "Bearer token-1");
    EXPECT_EQ(http_.header(2, "Authorization"), "Bearer token-1");

    ASSERT_EQ(users->size(), 4u);
    const auto& manager = users->at(0);
    EXPECT_EQ(manager.id, "104000000000000000001");
    EXPECT_EQ(manager.primary_email, "riley.manager@example.com");
    EXPECT_EQ(manager.full_name, "Riley Manager");
    EXPECT_FALSE(manager.is_admin);
    EXPECT_TRUE(manager.is_delegated_admin);
    EXPECT_FALSE(manager.suspended);
    EXPECT_FALSE(manager.archived);

    const auto& suspended = users->at(1);
    EXPECT_EQ(suspended.id, "104000000000000000002");
    EXPECT_EQ(suspended.primary_email, "sam.suspended@example.com");
    EXPECT_TRUE(suspended.suspended);
    EXPECT_FALSE(suspended.archived);
    EXPECT_FALSE(suspended.is_delegated_admin);

    const auto& archived = users->at(2);
    EXPECT_EQ(archived.id, "104000000000000000003");
    EXPECT_EQ(archived.full_name, "Alex Archived");
    EXPECT_FALSE(archived.suspended);
    EXPECT_TRUE(archived.archived);

    const auto& owner = users->at(3);
    EXPECT_EQ(owner.id, "104000000000000000004");
    EXPECT_EQ(owner.primary_email, "casey.owner@example.com");
    EXPECT_TRUE(owner.is_admin);
    EXPECT_FALSE(owner.is_delegated_admin);
}

TEST_F(Directory, AnAbsentFlagIsFalseAndAnAbsentNameIsEmpty) {
    http_.reply(200, std::string(kTokenOk));
    http_.reply(200, page_of(R"({"id": "104000000000000000009", "primaryEmail": "plain@example.com"})"));
    const auto users = list();
    ASSERT_TRUE(users) << users.error().message;
    ASSERT_EQ(users->size(), 1u);
    const auto& plain = users->at(0);
    EXPECT_EQ(plain.primary_email, "plain@example.com");
    EXPECT_TRUE(plain.full_name.empty());
    EXPECT_FALSE(plain.suspended);
    EXPECT_FALSE(plain.archived);
    EXPECT_FALSE(plain.is_admin);
    EXPECT_FALSE(plain.is_delegated_admin);
}

TEST_F(Directory, APageTokenIsEncoded) {
    http_.reply(200, std::string(kTokenOk));
    http_.reply(200, page_of(user("u1"), "a/b+c=d&e"));
    http_.reply(200, page_of(user("u2")));
    ASSERT_TRUE(list());
    const auto requests = directory_requests();
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_EQ(requests[1].url.buffer(), std::string(kFirstPage) + "&pageToken=a%2Fb%2Bc%3Dd%26e");
    EXPECT_FALSE(http::why_refused(http::ClientMethod::get, requests[1].url));
}

TEST_F(Directory, AFailedPageGivesNoList) {
    // The second page fails: what the first held is not a list.
    http_.reply(200, std::string(kTokenOk));
    http_.reply(200, page_of(user("u1"), "p2"));
    http_.reply(500, R"({"error": {"code": 500, "message": "Backend Error"}})");
    const auto failed = list();
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().kind, ApiError::Kind::unavailable);

    // So does every other answer outside 2xx, a 401 after its one retry included.
    for (const unsigned status : {404u, 429u}) {
        http_.reply(status, R"({"error": {"code": 1, "message": "no"}})");
        const auto refused = list();
        ASSERT_FALSE(refused) << status;
        EXPECT_EQ(refused.error().http_status, status);
        // A 429 leaves the API alone for a minute; the test has no minute.
        limiter_.configure(std::string(workspace::kDirectoryApi), {.per_minute = 6000, .burst = 100});
        clock_.advance(61s);
    }
    http_.reply(401, "{}");
    http_.reply(200, R"({"access_token": "token-2", "expires_in": 3600})");
    http_.reply(401, "{}");
    const auto unauthenticated = list();
    ASSERT_FALSE(unauthenticated);
    EXPECT_EQ(unauthenticated.error().kind, ApiError::Kind::unauthenticated);
}

TEST_F(Directory, AForbiddenListIsAnError) {
    http_.reply(200, std::string(kTokenOk));
    http_.reply(403, R"({"error": {"code": 403, "message": "Not Authorized to access this resource/api",
                         "errors": [{"reason": "forbidden"}]}})");
    const auto forbidden = list();
    ASSERT_FALSE(forbidden);
    EXPECT_EQ(forbidden.error().kind, ApiError::Kind::permission_denied);
    EXPECT_EQ(directory_requests().size(), 1u);
}

TEST_F(Directory, ATokenThatCouldNotBeHadIsAnError) {
    http_.reply(404, R"({"error": "not_found", "error_description": "Not Found"})");
    const auto users = list();
    ASSERT_FALSE(users);
    EXPECT_EQ(users.error().kind, ApiError::Kind::not_configured);
    EXPECT_TRUE(directory_requests().empty());
}

TEST_F(Directory, AnEmptyDirectoryIsAnEmptyList) {
    http_.reply(200, std::string(kTokenOk));
    http_.reply(200, R"({"kind": "admin#directory#users"})");
    const auto users = list();
    ASSERT_TRUE(users) << users.error().message;
    EXPECT_TRUE(users->empty());
}

TEST_F(Directory, AnEntryThatIsNoUserGivesNoList) {
    http_.reply(200, std::string(kTokenOk));
    for (const std::string entry : {R"({})", R"({"id": "104000000000000000009"})", R"({"id": "", "primaryEmail": "x@example.com"})",
                                    R"({"id": "dev:someone@example.com", "primaryEmail": "someone@example.com"})"}) {
        http_.reply(200, page_of(user("u1") + "," + entry));
        const auto users = list();
        ASSERT_FALSE(users) << entry;
        EXPECT_EQ(users.error().kind, ApiError::Kind::invalid_response) << entry;
        EXPECT_EQ(users.error().message, "the directory's list could not be read");
    }
}

TEST_F(Directory, AUserNamedTwiceGivesNoList) {
    http_.reply(200, std::string(kTokenOk));
    http_.reply(200, page_of(user("u1"), "p2"));
    http_.reply(200, page_of(user("u2") + "," + user("u1")));
    const auto users = list();
    ASSERT_FALSE(users);
    EXPECT_EQ(users.error().kind, ApiError::Kind::invalid_response);
    EXPECT_EQ(users.error().message, "the directory's list names a user twice");
}

TEST_F(Directory, AListThatDoesNotEndIsRefused) {
    http_.reply(200, std::string(kTokenOk));
    for (int page = 0; page < 21; ++page) http_.reply(200, page_of(user(std::format("u{}", page)), std::format("p{}", page + 1)));
    const auto users = list();
    ASSERT_FALSE(users);
    EXPECT_EQ(users.error().kind, ApiError::Kind::invalid_response);
    EXPECT_EQ(users.error().message, "the directory's list does not end");
    EXPECT_EQ(directory_requests().size(), 20u) << "the read stops at its bound";
}

TEST_F(Directory, ATransportFailureThrows) {
    http_.reply(200, std::string(kTokenOk));
    http_.fail();
    EXPECT_THROW(list(), boost::system::system_error);
    EXPECT_EQ(directory_requests().size(), 1u) << "it went out once and was not repeated";
}

TEST_F(Directory, EveryCallIsAGet) {
    // Every list this suite's cases read, in one fixture: the pages, an error and a failure.
    http_.reply(200, std::string(kTokenOk));
    http_.reply(200, fixture("directory-users-page.json"));
    http_.reply(200, fixture("directory-users-last-page.json"));
    ASSERT_TRUE(list());
    http_.reply(403, "{}");
    ASSERT_FALSE(list());
    http_.reply(200, page_of(user("u1"), "a/b+c=d&e"));
    http_.reply(500, "{}");
    ASSERT_FALSE(list());
    http_.fail();
    EXPECT_THROW(list(), boost::system::system_error);

    const auto requests = directory_requests();
    ASSERT_EQ(requests.size(), 6u);
    for (const auto& request : requests) {
        EXPECT_EQ(request.method, http::ClientMethod::get) << request.url.buffer();
        EXPECT_EQ(request.url.path(), "/admin/directory/v1/users");
        EXPECT_TRUE(request.body.empty());
    }
    EXPECT_TRUE(http_.refused.empty());
}
