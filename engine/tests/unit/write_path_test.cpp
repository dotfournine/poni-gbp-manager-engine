// The engine's ways out, against a scripted HTTP client and the real
// outbound client: the four requests that change something at Google (a
// reply to a review, the create of a local post, the delete of a local
// post, the edit of a local post), each of which the client refuses to send
// anywhere else; the copy and the delete of a report's page in the public
// bucket (D-104), which go to that bucket and nowhere else; the one read
// of Google's Admin SDK, the list of users (D-106); the one read of Google
// Places, a place's time zone (D-109); and the email the Gmail channel builds.

#include "connectors/gbp/writer.hpp"
#include "connectors/notify/gmail.hpp"
#include "connectors/storage/store.hpp"
#include "core/crypto.hpp"
#include "core/tls.hpp"
#include "unit/connector_fixture.hpp"

#include <boost/json/parse.hpp>
#include <boost/system/system_error.hpp>
#include <boost/url/parse.hpp>
#include <gmock/gmock.h>

#include <array>
#include <format>
#include <string>
#include <utility>
#include <vector>

using namespace poni;
using google::ApiError;
using testing::HasSubstr;
using testing::Not;

using test::kTokenOk;

namespace {

constexpr std::string_view kReplyUrl = "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/reviews/r1/reply";

class WritePath : public test::ConnectorTest {
protected:
    WritePath()
        : ConnectorTest({gbp::kReviewsApi, google::kOAuthApi, notify::kGmailApi, storage::kStorageApi}),
          tokens_(clock_, google::refresh_fetch(http_, limiter_, test::stored_credentials())),
          writer_(api_),
          gmail_(api_, "PONI GBP Manager", [this]() -> boost::asio::awaitable<std::expected<std::string, std::string>> {
              co_return mailbox_;
          }) {}

    // What the connected mailbox's address is, as the email channel asks for it before every message (D-100).
    std::expected<std::string, std::string> mailbox_ = std::string("host@xvisionmediaus.com");
    google::CachedTokenSource tokens_;
    google::Api api_{http_, tokens_, limiter_};
    gbp::HttpReplyWriter writer_;
    notify::GmailChannel gmail_;
};

// The text between the header block and the end, base64-decoded.
std::string mime_body(const std::string& mime) {
    const auto start = mime.find("\r\n\r\n");
    std::string encoded;
    for (const char c : mime.substr(start + 4)) {
        if (c != '\r' && c != '\n') encoded.push_back(c);
    }
    return core::base64_decode(encoded).value_or("");
}

}  // namespace

// ---- the reply writer -----------------------------------------------------------------

TEST_F(WritePath, AReplyIsOnePutToTheReviewsReplyAddress) {
    http_.reply(200, std::string(kTokenOk));
    http_.reply(200, R"({"comment": "Thank you!", "updateTime": "2026-11-10T12:00:01Z"})");
    const auto result = await(writer_.put_reply({"111", "9001", "r1"}, "Thank you!"));
    ASSERT_TRUE(result);
    EXPECT_EQ(result->comment, "Thank you!");
    ASSERT_EQ(http_.requests.size(), 2u);
    const auto& request = http_.requests[1];
    EXPECT_EQ(request.method, http::ClientMethod::put);
    EXPECT_EQ(std::string_view(request.url.buffer()), kReplyUrl);
    EXPECT_EQ(boost::json::parse(request.body), boost::json::parse(R"({"comment": "Thank you!"})"));
    EXPECT_EQ(request.content_type, "application/json");
    EXPECT_EQ(http_.header(1, "Authorization"), "Bearer token-1");
}

TEST_F(WritePath, AnIdCannotChangeWhereTheReplyGoes) {
    http_.reply(200, std::string(kTokenOk));
    // The writer encodes the id as one segment, so its slashes are not followed; and the outbound client's rule
    // then refuses the address, because a review's id is letters, digits, '-' and '_'.
    for (const auto* id : {"r1/../../../locations/9002/reviews/r2", "..", "r1?updateMask=x", "r1#x", "r1%2F..", ""}) {
        EXPECT_THROW(await(writer_.put_reply({"111", "9001", id}, "Hi")), std::invalid_argument) << id;
    }
    EXPECT_THROW(await(writer_.put_reply({"111", "9001/../9002", "r1"}, "Hi")), std::invalid_argument);
    ASSERT_EQ(http_.refused.size(), 7u);
    EXPECT_EQ(http_.refused[0].url.segments().size(), 8u) << "the slashes in the id were encoded, not followed";
    EXPECT_THAT(std::string(http_.refused[0].url.encoded_path()), HasSubstr("r1%2F"));
    for (const auto& sent : http_.requests) EXPECT_NE(sent.method, http::ClientMethod::put) << "only the token request went out";
}

TEST_F(WritePath, ARefusalIsAnErrorAndNothingIsRetriedExceptAStaleToken) {
    http_.reply(200, std::string(kTokenOk));
    http_.reply(403, R"({"error": {"status": "PERMISSION_DENIED", "message": "no"}})");
    const auto refused = await(writer_.put_reply({"111", "9001", "r1"}, "Hi"));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().kind, ApiError::Kind::permission_denied);
    EXPECT_EQ(http_.requests.size(), 2u);

    // 401: Google did nothing, so one more try with a fresh token is safe.
    http_.reply(401, "{}");
    http_.reply(200, R"({"access_token": "token-2", "expires_in": 3600})");
    http_.reply(200, "{}");
    ASSERT_TRUE(await(writer_.put_reply({"111", "9001", "r1"}, "Hi")));
    EXPECT_EQ(http_.header(4, "Authorization"), "Bearer token-2");

    // A 5xx is reported as such; whether to post again is not the writer's call.
    http_.reply(503, "{}");
    const auto unavailable = await(writer_.put_reply({"111", "9001", "r1"}, "Hi"));
    ASSERT_FALSE(unavailable);
    EXPECT_EQ(unavailable.error().kind, ApiError::Kind::unavailable);
    EXPECT_EQ(http_.requests.size(), 6u) << "no second PUT after a 5xx";
}

TEST_F(WritePath, AReplyWhoseAnswerNeverComesThrowsAndIsNotSentAgain) {
    http_.reply(200, std::string(kTokenOk));
    http_.fail();
    // Not an ApiError: the caller must not read "no answer" as "not posted" (jobs/posting.cpp keeps the post open).
    EXPECT_THROW(await(writer_.put_reply({"111", "9001", "r1"}, "Thank you!")), boost::system::system_error);
    ASSERT_EQ(http_.requests.size(), 2u);
    EXPECT_EQ(http_.requests[1].method, http::ClientMethod::put);
    // The same after a refused token: the second PUT is the one whose outcome is unknown.
    http_.reply(401, "{}");
    http_.reply(200, R"({"access_token": "token-2", "expires_in": 3600})");
    http_.fail();
    EXPECT_THROW(await(writer_.put_reply({"111", "9001", "r1"}, "Thank you!")), boost::system::system_error);
    EXPECT_EQ(http_.requests.size(), 5u);
}

// ---- the outbound client: PUT exists for one address only ------------------------------

TEST(OutboundPut, OnlyTheReplyAddressIsAReplyAddress) {
    EXPECT_TRUE(http::is_reply_url(boost::urls::url(kReplyUrl)));
    for (const auto* other : {
             "http://mybusiness.googleapis.com/v4/accounts/111/locations/9001/reviews/r1/reply",          // not TLS
             "https://mybusiness.googleapis.com.evil.example/v4/accounts/111/locations/9001/reviews/r1/reply",
             "https://evil.example/v4/accounts/111/locations/9001/reviews/r1/reply",
             "https://mybusiness.googleapis.com:8443/v4/accounts/111/locations/9001/reviews/r1/reply",
             "https://user@mybusiness.googleapis.com/v4/accounts/111/locations/9001/reviews/r1/reply",
             "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/reviews/r1",              // the review itself
             "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/reviews/r1/reply/extra",
             "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001",                         // a location
             "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/media/m1/x/reply",
             "https://mybusiness.googleapis.com/v4/accounts//locations/9001/reviews/r1/reply",
             "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/reviews/r1/reply?updateMask=x",
             "https://mybusinessbusinessinformation.googleapis.com/v1/locations/9001",
             "https://gmail.googleapis.com/gmail/v1/users/me/messages/send"}) {
        EXPECT_FALSE(http::is_reply_url(boost::urls::url(other))) << other;
    }
}

// The real client for the refusal tests. It is in mock mode with nothing listening on the mock's port: the rules
// are checked on Google's address before it is exchanged for the mock's, and if a rule were ever lost, the request
// would fail on this machine instead of reaching Google. `public_bucket`: as
// the engine's client is given PONI_PUBLIC_BUCKET.
struct RefusingClient {
    explicit RefusingClient(std::string public_bucket = {}) : client(tls.ctx, boost::urls::url("http://127.0.0.1:1"), std::move(public_bucket)) {}

    boost::asio::io_context io;
    core::TlsSetup tls = core::make_tls_client_context();
    http::NetClient client;

    http::ClientResponse send(http::ClientRequest request) { return test::run(io, client.send(std::move(request))); }
};

TEST(OutboundPut, TheClientRefusesAnyOtherPutBeforeOpeningAConnection) {
    RefusingClient refusing;
    auto& io = refusing.io;
    auto& client = refusing.client;
    for (const auto* target : {"https://mybusinessbusinessinformation.googleapis.com/v1/locations/9001",
                               "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/reviews/r1",
                               "http://127.0.0.1:1/v4/accounts/111/locations/9001/reviews/r1/reply"}) {
        http::ClientRequest request;
        request.method = http::ClientMethod::put;
        request.url = boost::urls::url(target);
        // invalid_argument, not a network error: nothing was resolved or connected.
        EXPECT_THROW(test::run(io, client.send(std::move(request))), std::invalid_argument) << target;
    }
}

// ---- the outbound client: DELETE exists for one address only (D-088) ---------------------

namespace {

constexpr std::string_view kLocalPostUrl = "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/7856477975681504943";
constexpr std::string_view kLocalPostsUrl = "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts";
constexpr std::string_view kBatchGetReviewsUrl = "https://mybusiness.googleapis.com/v4/accounts/111/locations:batchGetReviews";

// Everything at Google that must never be deleted, and every way of writing
// the one address that may be which is not that address.
const auto kNotALocalPost = std::to_array<const char*>({
    // other things on a profile
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/reviews/r1",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/reviews/r1/reply",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/media/m1",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/media/localPosts/m1",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/questions/q1",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001",
    "https://mybusiness.googleapis.com/v4/accounts/111",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts",  // the collection
    "https://mybusinessbusinessinformation.googleapis.com/v1/locations/9001",
    "https://mybusinessaccountmanagement.googleapis.com/v1/accounts/111/admins/a1",
    "https://storage.googleapis.com/storage/v1/b/poni-public/o/r%2Ftoken.html",
    "https://gmail.googleapis.com/gmail/v1/users/me/messages/m1",
    // the address of a local post, written another way
    "http://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1",  // not TLS
    "https://mybusiness.googleapis.com.evil.example/v4/accounts/111/locations/9001/localPosts/1",
    "https://evil.example/v4/accounts/111/locations/9001/localPosts/1",
    "https://mybusiness.googleapis.com:8443/v4/accounts/111/locations/9001/localPosts/1",
    "https://user@mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1",
    "https://MyBusiness.GoogleAPIs.com/v4/accounts/111/locations/9001/localPosts/1",
    "https://mybusiness.googleapis.com./v4/accounts/111/locations/9001/localPosts/1",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?force=true",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1#x",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1/",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1/extra",
    "https://mybusiness.googleapis.com/v5/accounts/111/locations/9001/localPosts/1",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localposts/1",
    // ids that are no ids: a server that resolves dot segments would land on the location
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/..",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/%2E%2E",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/.",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/../localPosts/1",
    "https://mybusiness.googleapis.com/v4/accounts//locations/9001/localPosts/1",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1%2F..%2F..%2Freviews%2Fr1",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1%3Fx",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/a%20b",
});

http::ClientRequest request_to(http::ClientMethod method, std::string_view target) {
    http::ClientRequest request;
    request.method = method;
    request.url = boost::urls::url(target);
    return request;
}

}  // namespace

TEST(OutboundDelete, OnlyTheAddressOfALocalPostIsOne) {
    EXPECT_TRUE(http::is_local_post_url(boost::urls::url(kLocalPostUrl)));
    EXPECT_TRUE(http::is_local_post_url(boost::urls::url("https://mybusiness.googleapis.com/v4/accounts/a-1/locations/l_2/localPosts/AbC-9_x")));
    for (const auto* other : kNotALocalPost) EXPECT_FALSE(http::is_local_post_url(boost::urls::url(other))) << other;
}

TEST(OutboundDelete, EveryOtherDeleteIsRefused) {
    EXPECT_FALSE(http::why_refused(http::ClientMethod::del, boost::urls::url(kLocalPostUrl)));
    for (const auto* other : kNotALocalPost) {
        const auto refusal = http::why_refused(http::ClientMethod::del, boost::urls::url(other));
        ASSERT_TRUE(refusal) << other;
        EXPECT_THAT(std::string(*refusal), HasSubstr("DELETE"));
    }
    // The one address a PUT may go to is not one a DELETE may: a reply is never deleted.
    EXPECT_TRUE(http::why_refused(http::ClientMethod::del, boost::urls::url(kReplyUrl)));
    // And a local post is created and deleted, never replaced.
    EXPECT_TRUE(http::why_refused(http::ClientMethod::put, boost::urls::url(kLocalPostUrl)));
    EXPECT_TRUE(http::why_refused(http::ClientMethod::put, boost::urls::url(kLocalPostsUrl)));
}

TEST(OutboundDelete, TheClientRefusesAnyOtherDeleteBeforeOpeningAConnection) {
    RefusingClient refusing;
    for (const auto* target : kNotALocalPost) {
        // invalid_argument, not a network error: nothing was resolved or connected.
        EXPECT_THROW(refusing.send(request_to(http::ClientMethod::del, target)), std::invalid_argument) << target;
    }
    EXPECT_THROW(refusing.send(request_to(http::ClientMethod::del, "http://127.0.0.1:1/v4/accounts/1/locations/2/localPosts/3")),
                 std::invalid_argument)
        << "not on the engine's own machine either";
    // The address of a local post is let through to the network, which here is nothing: a transport error, not a refusal.
    EXPECT_THROW(refusing.send(request_to(http::ClientMethod::del, kLocalPostUrl)), boost::system::system_error);
    // A reply's address written with a dot segment is refused by the client itself, not only by is_reply_url.
    EXPECT_THROW(refusing.send(request_to(http::ClientMethod::put, "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/reviews/../reply")),
                 std::invalid_argument);
}

TEST(OutboundDelete, ADeleteCarriesNoBody) {
    auto request = request_to(http::ClientMethod::del, kLocalPostUrl);
    EXPECT_FALSE(http::why_refused(request));
    request.body = R"({"force": true})";
    ASSERT_TRUE(http::why_refused(request));
    EXPECT_THAT(std::string(*http::why_refused(request)), HasSubstr("without a body"));
}

// ---- what is checked is what is sent ----------------------------------------------------------

TEST(OutboundAddress, AnAddressThatChangesSomethingIsTakenOnlyAsTheEngineWritesIt) {
    // Each of these names the same resource as an allowed address to some server, or might: none is that address.
    for (const auto* written : {
             "https://mybusiness.googleapis.com/./v4/accounts/111/locations/9001/localPosts/77",       // Boost.URL hides one leading "/."
             "https://mybusiness.googleapis.com/%76%34/accounts/111/locations/9001/local%50osts/%37%37",  // letters, percent-encoded
             "https://mybusiness%2Egoogleapis.com/v4/accounts/111/locations/9001/localPosts/77",
             "https://%6Dybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/77",
             "HTTPS://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/77",
             "https://mybusiness.googleapis.com:/v4/accounts/111/locations/9001/localPosts/77",          // an empty port
             "https://@mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/77",          // an empty user
             "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/77?",          // an empty query
             "https://mybusiness.googleapis.com//v4/accounts/111/locations/9001/localPosts/77",
             "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/77:x",
             "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/77;a=b",
             "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/%C3%A9",
             "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1%252F..",
             "https://mybusiness.googleapis.com/v4/accounts/111/locations:batchGetReviews"}) {
        EXPECT_TRUE(http::why_refused(http::ClientMethod::del, boost::urls::url(written))) << written;
    }
    const std::string long_id(129, '7');
    EXPECT_TRUE(http::why_refused(http::ClientMethod::del,
                                  boost::urls::url("https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/" + long_id)));
    EXPECT_FALSE(http::why_refused(http::ClientMethod::del, boost::urls::url("https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/"
                                                                             "localPosts/" + long_id.substr(1))));

    // The same holds for the reply and for the two POSTs: ids are plain, nothing is encoded, nothing is added.
    for (const auto* written : {"https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/reviews/a%2F..%2F..%2Flocations%2F9002%2Freviews%2Fb/reply",
                                "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/reviews/r1%3F/reply",
                                "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/reviews/r1%23/reply",
                                "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/reviews/r1%00/reply",
                                "https://mybusiness.googleapis.com/./v4/accounts/111/locations/9001/reviews/r1/reply",
                                "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/reviews/r1/repl%79"}) {
        EXPECT_TRUE(http::why_refused(http::ClientMethod::put, boost::urls::url(written))) << written;
    }
    EXPECT_FALSE(http::why_refused(http::ClientMethod::put,
                                   boost::urls::url("https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/reviews/AbFvOq-x_9/reply")));
    for (const auto* written : {"https://mybusiness.googleapis.com/v4/accounts/x%2Flocations%2F9001%2Fmedia%3F/locations:batchGetReviews",
                                "https://mybusiness.googleapis.com/v4/accounts/111/locations%3AbatchGetReviews",
                                "https://mybusiness.googleapis.com/v4/accounts/111/locations:batchGetReviews#x",
                                "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts#x",
                                "https://mybusiness.googleapis.com/./v4/accounts/111/locations/9001/localPosts",
                                "https://mybusiness.googleapis.com/batch",
                                "https://mybusiness.googleapis.com/upload/v4/accounts/111/locations/9001/media"}) {
        EXPECT_TRUE(http::why_refused(http::ClientMethod::post, boost::urls::url(written))) << written;
    }
}

TEST(OutboundAddress, AHostIsNamedWithLettersDigitsDotsAndHyphensForEveryMethod) {
    // A name that ends in an encoded NUL is not "mybusiness.googleapis.com" to a comparison, and is to the resolver.
    for (const auto* written : {"https://mybusiness.googleapis.com%00/v4/accounts/111/locations/9001/media",
                                "https://mybusiness.googleapis.com%09/v4/accounts/111/locations/9001/media",
                                "https://mybusiness.googleapis.com%20/v4/accounts/111/locations/9001/media",
                                "https://mybusiness.googleapis.com%00.example/v4/accounts/111/locations/9001/media",
                                "https://mybusiness%EF%BC%8Egoogleapis.com/v4/accounts/111/locations/9001/media",  // a full-width dot
                                "https://my_business.googleapis.com/v4/accounts/111/locations/9001/media"}) {
        for (const auto method : {http::ClientMethod::get, http::ClientMethod::post, http::ClientMethod::put, http::ClientMethod::del,
                                  http::ClientMethod::patch}) {
            EXPECT_TRUE(http::why_refused(method, boost::urls::url(written))) << written;
        }
    }
    // Ordinary names and addresses are what they were.
    for (const auto* ordinary : {"https://oauth2.googleapis.com/token", "http://127.0.0.1:8090/x", "http://[::1]:8090/x",
                                 "http://metadata.google.internal/computeMetadata/v1/x", "https://dashscope-intl.aliyuncs.com/v1/chat",
                                 "http://localhost:8080/api/health", "https://example.com./x"}) {
        EXPECT_FALSE(http::why_refused(http::ClientMethod::get, boost::urls::url(ordinary))) << ordinary;
    }
    // In mock mode such a name would otherwise pass for "not Google" and be called for real.
    RefusingClient refusing;
    EXPECT_THROW(refusing.send(request_to(http::ClientMethod::get, "https://mybusiness.googleapis.com%00/v4/accounts/111/locations/9001/reviews")),
                 std::invalid_argument);
}

TEST(OutboundAddress, AMethodTheClientDoesNotKnowIsRefused) {
    EXPECT_TRUE(http::why_refused(static_cast<http::ClientMethod>(5), boost::urls::url("https://example.com/x")));
}

// ---- what a request may not carry ---------------------------------------------------------------

TEST(OutboundHeaders, NoHeaderChangesTheMethodTheHostOrTheLengthOfARequest) {
    // Google's front ends take a method from X-HTTP-Method-Override on a POST: with it, a POST to an address the
    // client allows would be a DELETE or a PATCH of that address.
    for (const auto* name : {"X-HTTP-Method-Override", "x-http-method-override", "X-HTTP-Method", "X-Method-Override", "Host", "host",
                             "Content-Length", "Transfer-Encoding"}) {
        auto request = request_to(http::ClientMethod::post, "https://oauth2.googleapis.com/token");
        request.headers.emplace_back(name, "DELETE");
        EXPECT_TRUE(http::why_refused(request)) << name;
    }
    // A value that holds a line break would add a header of its own.
    auto smuggled = request_to(http::ClientMethod::post, kLocalPostsUrl);
    smuggled.headers.emplace_back("Authorization", "Bearer x\r\nX-HTTP-Method-Override: DELETE");
    EXPECT_TRUE(http::why_refused(smuggled));
    auto in_name = request_to(http::ClientMethod::get, "https://example.com/x");
    in_name.headers.emplace_back("X-Test\nHost", "evil.example");
    EXPECT_TRUE(http::why_refused(in_name));
    auto in_type = request_to(http::ClientMethod::post, "https://example.com/x");
    in_type.content_type = "application/json\r\nX-HTTP-Method-Override: PATCH";
    EXPECT_TRUE(http::why_refused(in_type));
    auto nul = request_to(http::ClientMethod::get, "https://example.com/x");
    nul.headers.emplace_back("X-Test", std::string("a\0b", 3));
    EXPECT_TRUE(http::why_refused(nul));

    // What the engine's callers send is untouched.
    auto ordinary = request_to(http::ClientMethod::post, kLocalPostsUrl);
    ordinary.headers.emplace_back("Authorization", "Bearer token-1");
    ordinary.headers.emplace_back("x-goog-user-project", "poni-gbp");
    ordinary.content_type = "application/json";
    ordinary.body = "{}";
    EXPECT_FALSE(http::why_refused(ordinary));

    RefusingClient refusing;
    auto through_the_client = request_to(http::ClientMethod::post, kLocalPostsUrl);
    through_the_client.headers.emplace_back("X-HTTP-Method-Override", "PATCH");
    EXPECT_THROW(refusing.send(std::move(through_the_client)), std::invalid_argument);
}

// ---- the outbound client: a POST to the GBP host is one of two (D-088) --------------------

TEST(OutboundPost, TheCreateOfALocalPostAndBatchGetReviewsAreTheOnlyPostsToTheGbpHost) {
    EXPECT_TRUE(http::is_local_post_collection_url(boost::urls::url(kLocalPostsUrl)));
    EXPECT_TRUE(http::is_batch_get_reviews_url(boost::urls::url(kBatchGetReviewsUrl)));
    EXPECT_FALSE(http::why_refused(http::ClientMethod::post, boost::urls::url(kLocalPostsUrl)));
    EXPECT_FALSE(http::why_refused(http::ClientMethod::post, boost::urls::url(kBatchGetReviewsUrl)));

    for (const auto* other : {
             // other things a POST could create or change on a profile
             "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/media",
             "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/media:startUpload",
             "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/questions",
             "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts:reportInsights",
             "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1",  // a post itself
             "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/reviews/r1/reply",
             "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001:associate",
             "https://mybusiness.googleapis.com/v4/accounts/111/locations:batchGet",
             "https://mybusiness.googleapis.com/v4/accounts/111/locations",
             "https://mybusiness.googleapis.com/v4/accounts",
             // the two allowed addresses, written another way: the host is recognised, the address is not
             "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts?validateOnly=false",
             "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/",
             "https://mybusiness.googleapis.com/v4/accounts/111/locations/../localPosts",
             "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001%2F..%2F9002/localPosts",
             "https://mybusiness.googleapis.com/v4/accounts/111/locations:batchGetReviews?alt=json",
             "https://mybusiness.googleapis.com/v4/accounts/../locations:batchGetReviews",
             "https://MyBusiness.GoogleAPIs.com/v4/accounts/111/locations/9001/localPosts",
             "https://mybusiness.googleapis.com./v4/accounts/111/locations/9001/localPosts",
             "https://mybusiness.googleapis.com:443/v4/accounts/111/locations/9001/localPosts",
             "https://user@mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts",
             "http://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts"}) {
        const auto refusal = http::why_refused(http::ClientMethod::post, boost::urls::url(other));
        ASSERT_TRUE(refusal) << other;
        EXPECT_THAT(std::string(*refusal), HasSubstr("POST"));
    }
}

TEST(OutboundPost, PostsToOtherHostsAreTheEnginesOrdinaryCalls) {
    for (const auto* ordinary : {"https://oauth2.googleapis.com/token",
                                 "https://pubsub.googleapis.com/v1/projects/p/subscriptions/s:pull",
                                 "https://gmail.googleapis.com/gmail/v1/users/me/messages/send",
                                 "https://storage.googleapis.com/upload/storage/v1/b/poni-public/o?uploadType=multipart",
                                 "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions",
                                 "https://api.anthropic.com/v1/messages",
                                 // Not Google's host: a name that only starts or ends like it.
                                 "https://mybusiness.googleapis.com.example/v4/accounts/1/locations/2/media",
                                 "https://notmybusiness.googleapis.com/v4/accounts/1/locations/2/media"}) {
        EXPECT_FALSE(http::why_refused(http::ClientMethod::post, boost::urls::url(ordinary))) << ordinary;
    }
    // A GET changes nothing, wherever it goes.
    EXPECT_FALSE(http::why_refused(http::ClientMethod::get, boost::urls::url("https://mybusiness.googleapis.com/v4/accounts/1/locations/2/media")));
    EXPECT_FALSE(http::why_refused(http::ClientMethod::get, boost::urls::url(kLocalPostUrl)));
}

TEST(OutboundPost, TheClientRefusesAnyOtherPostToTheGbpHostBeforeOpeningAConnection) {
    RefusingClient refusing;
    for (const auto* target : {"https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/media",
                               "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1",
                               "https://MYBUSINESS.googleapis.com/v4/accounts/111/locations/9001/localPosts",
                               "http://mybusiness.googleapis.com:1/v4/accounts/111/locations/9001/localPosts"}) {
        EXPECT_THROW(refusing.send(request_to(http::ClientMethod::post, target)), std::invalid_argument) << target;
    }
}

TEST(OutboundPut, TheReplyAddressTakesNoDotSegmentForAnId) {
    for (const auto* other : {"https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/reviews/../reply",
                              "https://mybusiness.googleapis.com/v4/accounts/111/locations/../reviews/r1/reply",
                              "https://mybusiness.googleapis.com/v4/accounts/./locations/9001/reviews/r1/reply",
                              "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/reviews/%2e%2e/reply"}) {
        EXPECT_FALSE(http::is_reply_url(boost::urls::url(other))) << other;
    }
}

TEST_F(WritePath, TheScriptedClientRefusesWhatTheRealOneRefuses) {
    // So every connector test runs under the same rules as the engine.
    EXPECT_THROW(await(http_.send(request_to(http::ClientMethod::del, "https://mybusiness.googleapis.com/v4/accounts/1/locations/2"))),
                 std::invalid_argument);
    EXPECT_THROW(await(http_.send(request_to(http::ClientMethod::post, "https://mybusiness.googleapis.com/v4/accounts/1/locations/2/media"))),
                 std::invalid_argument);
    EXPECT_THROW(await(http_.send(request_to(http::ClientMethod::put, "https://mybusiness.googleapis.com/v4/accounts/1/locations/2"))),
                 std::invalid_argument);
    EXPECT_THROW(await(http_.send(request_to(http::ClientMethod::patch, "https://mybusiness.googleapis.com/v4/accounts/1/locations/2?updateMask=summary"))),
                 std::invalid_argument);
    EXPECT_THROW(await(http_.send(request_to(http::ClientMethod::patch, kLocalPostUrl))), std::invalid_argument) << "a PATCH without its mask";
    EXPECT_TRUE(http_.requests.empty()) << "a refused request is not a sent one";
    EXPECT_EQ(http_.refused.size(), 5u);
}

// ---- the outbound client: PATCH exists for one address only (D-099) ------------------------

namespace {

constexpr std::string_view kLocalPostUpdateUrl =
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/7856477975681504943?updateMask=summary";

// Every way of writing the one address a PATCH may go to that is not that address, and every mask
// that is not one or more of the six field names, each once, joined by commas.
const auto kNotALocalPostUpdate = std::to_array<const char*>({
    // without a mask, or with an empty one
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?updateMask=",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?updateMask",
    // a field that is not one of the six: the kind, the time, what only Google writes, everything at once
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?updateMask=topicType",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?updateMask=scheduledTime",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?updateMask=summary,scheduledTime",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?updateMask=state",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?updateMask=name",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?updateMask=alertType",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?updateMask=*",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?updateMask=event.title",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?updateMask=Summary",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?updateMask=call_to_action",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?updateMask=summar",
    // a name twice, a stray comma, a space
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?updateMask=summary,summary",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?updateMask=summary,event,summary",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?updateMask=summary,",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?updateMask=,summary",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?updateMask=summary,,event",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?updateMask=summary,%20event",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?updateMask=summary+event",
    // anything percent-encoded, which a server would read as one of the above or as something else
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?updateMask=summary%2Cevent",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?updateMask=%73ummary",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?update%4Dask=summary",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?updateMask%3Dsummary",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?updateMask=summary%26validateOnly=true",
    // a second parameter, before or after, and the parameter under another name
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?updateMask=summary&validateOnly=true",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?updateMask=summary&updateMask=topicType",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?alt=json&updateMask=summary",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?updateMask=summary&",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?updateMask=summary;x=1",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?update_mask=summary",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?updatemask=summary",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?fieldMask=summary",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?updateMask=summary#x",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?updateMask=summary?x",
    // other things on a profile, with a mask that would be allowed on a post
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/reviews/r1?updateMask=summary",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/reviews/r1/reply?updateMask=summary",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001?updateMask=summary",                // a location
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001?updateMask=media",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts?updateMask=summary",     // the list
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/media/m1?updateMask=media",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/media/localPosts/m1?updateMask=media",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/questions/q1?updateMask=summary",
    "https://mybusiness.googleapis.com/v4/accounts/111?updateMask=summary",
    "https://mybusinessbusinessinformation.googleapis.com/v1/locations/9001?updateMask=title",
    "https://mybusinessaccountmanagement.googleapis.com/v1/accounts/111/admins/a1?updateMask=summary",
    "https://storage.googleapis.com/storage/v1/b/poni-public/o/r%2Ftoken.html?updateMask=summary",
    "https://gmail.googleapis.com/gmail/v1/users/me/messages/m1?updateMask=summary",
    // the address of a post, written another way
    "http://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?updateMask=summary",   // not TLS
    "https://mybusiness.googleapis.com.evil.example/v4/accounts/111/locations/9001/localPosts/1?updateMask=summary",
    "https://evil.example/v4/accounts/111/locations/9001/localPosts/1?updateMask=summary",
    "https://mybusiness.googleapis.com:8443/v4/accounts/111/locations/9001/localPosts/1?updateMask=summary",
    "https://mybusiness.googleapis.com:443/v4/accounts/111/locations/9001/localPosts/1?updateMask=summary",
    "https://user@mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?updateMask=summary",
    "https://MyBusiness.GoogleAPIs.com/v4/accounts/111/locations/9001/localPosts/1?updateMask=summary",
    "https://mybusiness.googleapis.com./v4/accounts/111/locations/9001/localPosts/1?updateMask=summary",
    "HTTPS://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1?updateMask=summary",
    "https://mybusiness.googleapis.com/./v4/accounts/111/locations/9001/localPosts/1?updateMask=summary",
    "https://mybusiness.googleapis.com//v4/accounts/111/locations/9001/localPosts/1?updateMask=summary",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1/?updateMask=summary",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1/extra?updateMask=summary",
    "https://mybusiness.googleapis.com/v5/accounts/111/locations/9001/localPosts/1?updateMask=summary",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localposts/1?updateMask=summary",
    "https://mybusiness.googleapis.com/%76%34/accounts/111/locations/9001/local%50osts/1?updateMask=summary",
    // ids that are no ids: a server that resolves dot segments would land on the location
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/..?updateMask=summary",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/%2E%2E?updateMask=summary",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/.?updateMask=summary",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/../localPosts/1?updateMask=summary",
    "https://mybusiness.googleapis.com/v4/accounts//locations/9001/localPosts/1?updateMask=summary",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1%2F..%2F..%2Freviews%2Fr1?updateMask=summary",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1%3FupdateMask=summary",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/1%3Fx?updateMask=summary",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001%2F..%2F9002/localPosts/1?updateMask=summary",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/a%20b?updateMask=summary",
    "https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/7:x?updateMask=summary",
});

}  // namespace

TEST(OutboundPatch, OnlyTheAddressOfALocalPostWithItsMaskIsOne) {
    EXPECT_TRUE(http::is_local_post_update_url(boost::urls::url(kLocalPostUpdateUrl)));
    EXPECT_FALSE(http::why_refused(http::ClientMethod::patch, boost::urls::url(kLocalPostUpdateUrl)));
    // Each of the six fields alone, several in any order, and all of them.
    for (const auto* mask : {"summary", "callToAction", "event", "offer", "media", "languageCode", "summary,callToAction", "event,summary",
                             "media,languageCode,offer", "summary,callToAction,event,offer,media,languageCode"}) {
        const boost::urls::url address(std::format("https://mybusiness.googleapis.com/v4/accounts/a-1/locations/l_2/localPosts/AbC-9_x?updateMask={}", mask));
        EXPECT_TRUE(http::is_local_post_update_url(address)) << mask;
        EXPECT_FALSE(http::why_refused(http::ClientMethod::patch, address)) << mask;
    }
    for (const auto* other : kNotALocalPostUpdate) EXPECT_FALSE(http::is_local_post_update_url(boost::urls::url(other))) << other;
}

TEST(OutboundPatch, EveryOtherPatchIsRefused) {
    for (const auto* other : kNotALocalPostUpdate) {
        const auto refusal = http::why_refused(http::ClientMethod::patch, boost::urls::url(other));
        ASSERT_TRUE(refusal) << other;
        EXPECT_THAT(std::string(*refusal), HasSubstr("PATCH"));
    }
    // Everything a DELETE may not go to, a PATCH may not go to either, with or without a mask.
    for (const auto* other : kNotALocalPost) {
        EXPECT_TRUE(http::why_refused(http::ClientMethod::patch, boost::urls::url(other))) << other;
        if (std::string_view(other).find_first_of("?#") != std::string_view::npos) continue;
        EXPECT_TRUE(http::why_refused(http::ClientMethod::patch, boost::urls::url(std::string(other) + "?updateMask=summary"))) << other;
    }
    // Nothing else of the engine's is a PATCH: not its ordinary calls to other hosts, a model or its own machine.
    for (const auto* elsewhere : {"https://oauth2.googleapis.com/token", "https://gmail.googleapis.com/gmail/v1/users/me/settings",
                                  "https://storage.googleapis.com/storage/v1/b/poni-public/o/x", "https://api.anthropic.com/v1/messages",
                                  "https://example.com/x?updateMask=summary", "http://127.0.0.1:8090/x", "http://localhost:8080/api/clients/1"}) {
        EXPECT_TRUE(http::why_refused(http::ClientMethod::patch, boost::urls::url(elsewhere))) << elsewhere;
    }
    // The address with its mask belongs to the PATCH alone: nothing is deleted, replaced or created under it,
    // and the one address a PUT may go to takes no PATCH.
    const boost::urls::url update(kLocalPostUpdateUrl);
    EXPECT_TRUE(http::why_refused(http::ClientMethod::del, update));
    EXPECT_TRUE(http::why_refused(http::ClientMethod::put, update));
    EXPECT_TRUE(http::why_refused(http::ClientMethod::post, update));
    EXPECT_TRUE(http::why_refused(http::ClientMethod::patch, boost::urls::url(kReplyUrl)));
    EXPECT_TRUE(http::why_refused(http::ClientMethod::patch, boost::urls::url(std::string(kReplyUrl) + "?updateMask=summary")));
    EXPECT_TRUE(http::why_refused(http::ClientMethod::patch, boost::urls::url(kLocalPostsUrl)));
    EXPECT_TRUE(http::why_refused(http::ClientMethod::patch, boost::urls::url(kBatchGetReviewsUrl)));
    // Reading a post is not limited, whatever follows its address.
    EXPECT_FALSE(http::why_refused(http::ClientMethod::get, update));

    // An id of 128 characters is one; one more is not.
    const std::string long_id(129, '7');
    EXPECT_TRUE(http::why_refused(http::ClientMethod::patch,
                                  boost::urls::url("https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/" + long_id +
                                                   "?updateMask=summary")));
    EXPECT_FALSE(http::why_refused(http::ClientMethod::patch,
                                   boost::urls::url("https://mybusiness.googleapis.com/v4/accounts/111/locations/9001/localPosts/" +
                                                    long_id.substr(1) + "?updateMask=summary")));
}

TEST(OutboundPatch, TheClientRefusesAnyOtherPatchBeforeOpeningAConnection) {
    RefusingClient refusing;
    for (const auto* target : kNotALocalPostUpdate) {
        // invalid_argument, not a network error: nothing was resolved or connected.
        EXPECT_THROW(refusing.send(request_to(http::ClientMethod::patch, target)), std::invalid_argument) << target;
    }
    EXPECT_THROW(refusing.send(request_to(http::ClientMethod::patch, "http://127.0.0.1:1/v4/accounts/1/locations/2/localPosts/3?updateMask=summary")),
                 std::invalid_argument)
        << "not on the engine's own machine either";
    // The address of a local post with its mask is let through to the network, which here is nothing: a transport error, not a refusal.
    auto allowed = request_to(http::ClientMethod::patch, kLocalPostUpdateUrl);
    allowed.content_type = "application/json";
    allowed.body = R"({"summary": "Open late on Fridays."})";
    EXPECT_THROW(refusing.send(std::move(allowed)), boost::system::system_error);
}

TEST(OutboundPatch, NoHeaderTurnsAnotherRequestIntoAPatchOrThePatchIntoAnotherRequest) {
    // Google's front ends take a method from X-HTTP-Method-Override on a POST: neither may a POST become a PATCH
    // that way, nor the PATCH something else.
    for (const auto* name : {"X-HTTP-Method-Override", "x-http-method-override", "X-HTTP-Method", "X-Method-Override", "Host",
                             "Content-Length", "Transfer-Encoding"}) {
        auto patch = request_to(http::ClientMethod::patch, kLocalPostUpdateUrl);
        patch.headers.emplace_back(name, "DELETE");
        EXPECT_TRUE(http::why_refused(patch)) << name;
        auto post = request_to(http::ClientMethod::post, kLocalPostsUrl);
        post.headers.emplace_back(name, "PATCH");
        EXPECT_TRUE(http::why_refused(post)) << name;
    }
    auto smuggled = request_to(http::ClientMethod::patch, kLocalPostUpdateUrl);
    smuggled.headers.emplace_back("Authorization", "Bearer x\r\nX-HTTP-Method-Override: DELETE");
    EXPECT_TRUE(http::why_refused(smuggled));

    // What the connector sends is untouched.
    auto ordinary = request_to(http::ClientMethod::patch, kLocalPostUpdateUrl);
    ordinary.headers.emplace_back("Authorization", "Bearer token-1");
    ordinary.content_type = "application/json";
    ordinary.body = R"({"summary": "Open late on Fridays."})";
    EXPECT_FALSE(http::why_refused(ordinary));

    RefusingClient refusing;
    auto through_the_client = request_to(http::ClientMethod::patch, kLocalPostUpdateUrl);
    through_the_client.headers.emplace_back("X-HTTP-Method-Override", "DELETE");
    EXPECT_THROW(refusing.send(std::move(through_the_client)), std::invalid_argument);
}

// ---- email ----------------------------------------------------------------------------

TEST_F(WritePath, AnEmailIsPlainTextAndNothingInItCanAddAHeader) {
    const auto mime = notify::build_mime({.address = "host@xvisionmediaus.com", .name = "PONI GBP Manager"}, "owner@xvisionmediaus.com",
                                         "Escalation: 路梦养生馆\r\nBcc: evil@example.com",
                                         "Review:\nGreat place.\r\nBcc: evil@example.com\n\n<script>alert(1)</script> 很好");
    const auto headers = mime.substr(0, mime.find("\r\n\r\n"));
    EXPECT_THAT(headers, HasSubstr("To: <owner@xvisionmediaus.com>\r\n"));
    EXPECT_THAT(headers, HasSubstr("<host@xvisionmediaus.com>\r\n"));
    EXPECT_THAT(headers, HasSubstr("Content-Type: text/plain; charset=UTF-8"));
    EXPECT_THAT(headers, Not(HasSubstr("Bcc"))) << "the subject is encoded, so its line break starts no header";
    EXPECT_THAT(headers, Not(HasSubstr("evil@example.com")));
    for (std::size_t start = 0; start < headers.size();) {
        const auto end = headers.find("\r\n", start);
        const auto line = headers.substr(start, end - start);
        EXPECT_LE(line.size(), 78u) << line;
        if (end == std::string::npos) break;
        start = end + 2;
    }
    EXPECT_EQ(mime_body(mime), "Review:\nGreat place.\r\nBcc: evil@example.com\n\n<script>alert(1)</script> 很好");
}

TEST_F(WritePath, GmailSendIsAPostWithTheMessageBase64UrlEncoded) {
    http_.reply(200, std::string(kTokenOk));
    http_.reply(200, R"({"id": "m1"})");
    const auto sent = await(gmail_.send({.recipient = "owner@xvisionmediaus.com", .subject = "Test", .body = "Hello."}));
    ASSERT_TRUE(sent);
    const auto& request = http_.requests[1];
    EXPECT_EQ(request.method, http::ClientMethod::post);
    EXPECT_EQ(std::string_view(request.url.buffer()), "https://gmail.googleapis.com/gmail/v1/users/me/messages/send");
    const auto raw = core::base64_decode(std::string(boost::json::parse(request.body).at("raw").as_string()));
    ASSERT_TRUE(raw);
    EXPECT_THAT(*raw, HasSubstr("To: <owner@xvisionmediaus.com>"));
    EXPECT_THAT(*raw, HasSubstr("<host@xvisionmediaus.com>\r\n"));
    EXPECT_EQ(mime_body(*raw), "Hello.");
}

// The sender is the mailbox connected on the status page, asked for on every message (D-100).
TEST_F(WritePath, GmailSendsFromTheConnectedMailboxAndFromNothingElse) {
    using Kind = notify::SendError::Kind;
    const auto from_of = [&](const http::ClientRequest& request) {
        const auto raw = core::base64_decode(std::string(boost::json::parse(request.body).at("raw").as_string())).value_or("");
        const auto at = raw.find("From: ");
        return raw.substr(at, raw.find("\r\n", at) - at);
    };
    mailbox_ = std::string("alerts@xvisionus.com");
    http_.reply(200, std::string(kTokenOk));
    http_.reply(200, R"({"id": "m1"})");
    ASSERT_TRUE(await(gmail_.send({"a@example.com", "s", "b"})));
    EXPECT_THAT(from_of(http_.requests.back()), testing::EndsWith(" <alerts@xvisionus.com>"));

    // Another mailbox was connected: the next message is sent from it, without a restart.
    mailbox_ = std::string("host@xvisionmediaus.com");
    http_.reply(200, R"({"id": "m2"})");
    ASSERT_TRUE(await(gmail_.send({"a@example.com", "s", "b"})));
    EXPECT_THAT(from_of(http_.requests.back()), testing::EndsWith(" <host@xvisionmediaus.com>"));

    // No address known: nothing is sent, not even a token is asked for, and the reason goes to the delivery log.
    mailbox_ = std::unexpected(std::string("the sending mailbox was connected before the engine kept its address"));
    const auto before = http_.requests.size();
    const auto refused = await(gmail_.send({"a@example.com", "s", "b"}));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().kind, Kind::not_configured);
    EXPECT_THAT(refused.error().message, HasSubstr("before the engine kept its address"));
    EXPECT_EQ(http_.requests.size(), before);
}

TEST_F(WritePath, GmailFailuresAreSortedIntoRetryRefusedAndNotSignedIn) {
    using Kind = notify::SendError::Kind;
    http_.reply(200, std::string(kTokenOk));
    http_.reply(503, "{}");
    EXPECT_EQ(await(gmail_.send({"a@example.com", "s", "b"})).error().kind, Kind::transient);
    http_.reply(403, R"({"error": {"status": "PERMISSION_DENIED", "message": "Request had insufficient authentication scopes."}})");
    const auto refused = await(gmail_.send({"a@example.com", "s", "b"}));
    EXPECT_EQ(refused.error().kind, Kind::permanent);
    EXPECT_THAT(refused.error().message, HasSubstr("insufficient authentication scopes"));

    google::CachedTokenSource nobody(clock_, google::refresh_fetch(http_, limiter_, []() -> boost::asio::awaitable<google::Result<google::OAuthCredentials>> {
        co_return std::unexpected(google::not_configured("the sending mailbox is not signed in"));
    }));
    notify::GmailChannel unsigned_channel({http_, nobody, limiter_}, "PONI", []() -> boost::asio::awaitable<std::expected<std::string, std::string>> {
        co_return std::string("host@xvisionmediaus.com");
    });
    const auto before = http_.requests.size();
    EXPECT_EQ(await(unsigned_channel.send({"a@example.com", "s", "b"})).error().kind, Kind::not_configured);
    EXPECT_EQ(http_.requests.size(), before) << "nothing was sent anywhere";
}

TEST_F(WritePath, AGmailFailureSaysWhenItWasTheToken) {
    using Kind = notify::SendError::Kind;
    // The refresh token was revoked: Google's token endpoint refuses it, and nothing is sent to Gmail.
    http_.reply(400, R"({"error": "invalid_grant", "error_description": "Token has been expired or revoked."})");
    const auto revoked = await(gmail_.send({"a@example.com", "s", "b"}));
    ASSERT_FALSE(revoked);
    EXPECT_EQ(revoked.error().kind, Kind::permanent);
    EXPECT_EQ(revoked.error().message, "the sending account's token: HTTP 400 invalid_grant: Token has been expired or revoked.");
    EXPECT_EQ(http_.requests.size(), 1u);

    // The token endpoint is down: worth another attempt, and still said to be the token.
    http_.reply(503, "{}");
    const auto down = await(gmail_.send({"a@example.com", "s", "b"}));
    EXPECT_EQ(down.error().kind, Kind::transient);
    EXPECT_THAT(down.error().message, testing::StartsWith("the sending account's token: HTTP 503"));

    // The token endpoint answers 404: the token's failure, not a mailbox that was never connected.
    http_.reply(404, "{}");
    const auto missing = await(gmail_.send({"a@example.com", "s", "b"}));
    EXPECT_EQ(missing.error().kind, Kind::permanent);
    EXPECT_THAT(missing.error().message, testing::StartsWith("the sending account's token: HTTP 404"));

    // Gmail refuses the token it was given, and no new one can be had: that is the token's failure too.
    http_.reply(200, std::string(kTokenOk));
    http_.reply(401, "{}");
    http_.reply(400, R"({"error": "invalid_grant", "error_description": "Token has been expired or revoked."})");
    const auto after_a_401 = await(gmail_.send({"a@example.com", "s", "b"}));
    EXPECT_EQ(after_a_401.error().kind, Kind::permanent);
    EXPECT_THAT(after_a_401.error().message, testing::StartsWith("the sending account's token: HTTP 400 invalid_grant"));

    // What Gmail itself answers is not the token's failure, also when a token had to be fetched for it.
    http_.reply(200, std::string(kTokenOk));
    http_.reply(403, R"({"error": {"status": "PERMISSION_DENIED", "message": "Request had insufficient authentication scopes."}})");
    const auto refused = await(gmail_.send({"a@example.com", "s", "b"}));
    EXPECT_EQ(refused.error().kind, Kind::permanent);
    EXPECT_EQ(refused.error().message, "HTTP 403 PERMISSION_DENIED Request had insufficient authentication scopes.");

    // No answer from the token endpoint is a network error like any other.
    tokens_.invalidate();
    http_.fail();
    const auto lost = await(gmail_.send({"a@example.com", "s", "b"}));
    EXPECT_EQ(lost.error().kind, Kind::transient);
    EXPECT_THAT(lost.error().message, testing::StartsWith("network error: "));
}

// ---- Apple's push service: one POST, to one device (D-097) --------------------------------

namespace {

const std::string kDeviceToken(64, 'a');

}  // namespace

TEST(OutboundApns, OnlyAPostToOneDeviceIsSentToApple) {
    for (const auto* host : {"api.push.apple.com", "api.sandbox.push.apple.com"}) {
        const auto url = std::format("https://{}/3/device/{}", host, kDeviceToken);
        EXPECT_TRUE(http::is_apns_device_url(boost::urls::url(url))) << url;
        EXPECT_FALSE(http::why_refused(http::ClientMethod::post, boost::urls::url(url))) << url;
        // Not even a read: the engine has nothing to ask Apple.
        EXPECT_TRUE(http::why_refused(http::ClientMethod::get, boost::urls::url(url))) << url;
        EXPECT_TRUE(http::why_refused(http::ClientMethod::put, boost::urls::url(url))) << url;
        EXPECT_TRUE(http::why_refused(http::ClientMethod::del, boost::urls::url(url))) << url;
    }
}

TEST(OutboundApns, TheAddressIsTakenOnlyAsTheEngineWritesIt) {
    for (const auto& other : {std::format("https://api.push.apple.com/3/device/{}", std::string(64, 'A')),  // capitals
                              std::format("https://api.push.apple.com/3/device/{}", std::string(63, 'a')),  // odd length
                              std::format("https://api.push.apple.com/3/device/{}", std::string(62, 'a')),  // too short
                              std::format("https://api.push.apple.com/3/device/{}g", std::string(63, 'a')),
                              std::format("https://api.push.apple.com:443/3/device/{}", kDeviceToken),
                              std::format("https://api.push.apple.com/3/device/{}?x=1", kDeviceToken),
                              std::format("https://api.push.apple.com/3/device/{}/", kDeviceToken),
                              std::format("https://api.push.apple.com/3/device/{}#x", kDeviceToken),
                              std::format("https://API.push.apple.com/3/device/{}", kDeviceToken),
                              std::format("https://api.push.apple.com./3/device/{}", kDeviceToken),
                              std::format("https://user@api.push.apple.com/3/device/{}", kDeviceToken),
                              std::format("http://api.push.apple.com/3/device/{}", kDeviceToken),
                              std::format("https://api.push.apple.com/4/device/{}", kDeviceToken),
                              std::format("https://api.push.apple.com/3/devices/{}", kDeviceToken),
                              std::format("https://api.push.apple.com/3/device/%61{}", std::string(62, 'a')),
                              std::format("https://api.push.apple.com/3/device/../device/{}", kDeviceToken),
                              std::string("https://api.push.apple.com/3/broadcasts/apps/com.example")}) {
        EXPECT_FALSE(http::is_apns_device_url(boost::urls::url(other))) << other;
        EXPECT_TRUE(http::why_refused(http::ClientMethod::post, boost::urls::url(other))) << other;
    }
}

TEST(OutboundApns, HttpTwoCarriesOnlyThatPostAndRefusesTheWholeBeforeOpeningAConnection) {
    RefusingClient refusing;
    const auto good = request_to(http::ClientMethod::post, std::format("https://api.sandbox.push.apple.com/3/device/{}", kDeviceToken));
    const auto run = [&](std::vector<http::ClientRequest> requests) { return test::run(refusing.io, refusing.client.send_h2(std::move(requests))); };
    // One bad request refuses them all.
    EXPECT_THROW(run({good, request_to(http::ClientMethod::post, "https://api.sandbox.push.apple.com/3/device/XYZ")}), std::invalid_argument);
    // Two hosts on one connection.
    EXPECT_THROW(run({good, request_to(http::ClientMethod::post, std::format("https://api.push.apple.com/3/device/{}", kDeviceToken))}),
                 std::invalid_argument);
    // HTTP/2 to Google, whose rules are for HTTP/1.1 and the mock: not this way.
    EXPECT_THROW(run({request_to(http::ClientMethod::post, "https://gmail.googleapis.com/gmail/v1/users/me/messages/send")}),
                 std::invalid_argument);
    // A GET has no business on this path either.
    EXPECT_THROW(run({request_to(http::ClientMethod::get, "https://example.com/")}), std::invalid_argument);
    auto with_header = good;
    with_header.headers.emplace_back("X-HTTP-Method-Override", "DELETE");
    EXPECT_THROW(run({with_header}), std::invalid_argument);
    EXPECT_TRUE(run({}).empty());
}

// ---- Cloud Storage: an upload, and a report's page copied to the archive and deleted (D-104) ----

namespace {

constexpr std::string_view kToken = "AbCdEfGhIjKlMnOpQrStUvWxYz0123456789-_abcde";
constexpr std::string_view kOtherToken = "zYxWvUtSrQpOnMlKjIhGfEdCbA9876543210_-edcba";
static_assert(kToken.size() == 43 && kOtherToken.size() == 43);
const std::string kPage = std::format("r/{}.html", kToken);
const std::string kArchive = std::format("a/{}.html", kOtherToken);

// A copy's and a delete's address, from segments written as they go out (an object's '/' as %2F).
std::string copy_address(std::string_view from, std::string_view to, std::string_view from_bucket = "poni-public",
                         std::string_view to_bucket = "poni-public") {
    return std::format("https://storage.googleapis.com/storage/v1/b/{}/o/{}/rewriteTo/b/{}/o/{}", from_bucket, from, to_bucket, to);
}
std::string delete_address(std::string_view object, std::string_view bucket = "poni-public") {
    return std::format("https://storage.googleapis.com/storage/v1/b/{}/o/{}", bucket, object);
}

const std::string kPageSegment = std::format("r%2F{}.html", kToken);
const std::string kArchiveSegment = std::format("a%2F{}.html", kOtherToken);
const std::string kCopyUrl = copy_address(kPageSegment, kArchiveSegment);
const std::string kDeleteUrl = delete_address(kPageSegment);

// Every way of writing the copy and the delete that is not them, and every other POST and DELETE at Cloud
// Storage, refused with `poni-public` as the configured bucket.
std::vector<std::pair<http::ClientMethod, std::string>> storage_neighbours() {
    using enum http::ClientMethod;
    const auto page = [](std::string_view token, std::string_view ext = "html") { return std::format("r%2F{}.{}", token, ext); };
    const auto archive = [](std::string_view token, std::string_view ext = "html") { return std::format("a%2F{}.{}", token, ext); };
    const auto with_a = std::format("%41{}", kToken.substr(1));  // 'A', percent-encoded
    std::vector<std::pair<http::ClientMethod, std::string>> out{
        // another bucket, in either place of the copy, or in the delete
        {post, copy_address(kPageSegment, kArchiveSegment, "other-bucket")},
        {post, copy_address(kPageSegment, kArchiveSegment, "poni-public", "other-bucket")},
        {post, copy_address(kPageSegment, kArchiveSegment, "other-bucket", "other-bucket")},
        {del, delete_address(kPageSegment, "other-bucket")},
        {del, delete_address(kPageSegment, "poni-publi")},
        // from anything but a page, to anything but an archive
        {post, copy_address(archive(kToken), kArchiveSegment)},
        {post, copy_address(std::format("p%2F{}.jpg", kToken), archive(kOtherToken, "jpg"))},
        {post, copy_address(kPageSegment, page(kOtherToken))},
        {post, copy_address(kArchiveSegment, kPageSegment)},
        {post, copy_address(kPageSegment, std::format("p%2F{}.html", kOtherToken))},
        // a DELETE of an archive, of a post's photo, of anything else
        {del, delete_address(kArchiveSegment)},
        {del, delete_address(std::format("p%2F{}.jpg", kToken))},
        {del, delete_address("x")},
        {del, "https://storage.googleapis.com/storage/v1/b/poni-public"},
        {del, "https://storage.googleapis.com/storage/v1/b/poni-public/o"},
        // a query
        {del, kDeleteUrl + "?generation=1"},
        {del, kDeleteUrl + "?"},
        {post, kCopyUrl + "?rewriteToken=x"},
        {post, kCopyUrl + "?"},
        {post, kCopyUrl + "#x"},
        // percent-encoding: a token's letter, a lower-case %2f, a second %2F
        {del, delete_address(page(with_a))},
        {post, copy_address(page(with_a), kArchiveSegment)},
        {del, delete_address(std::format("r%2f{}.html", kToken))},
        {post, copy_address(kPageSegment, std::format("a%2f{}.html", kOtherToken))},
        {del, delete_address(std::format("r%2F%2F{}.html", kToken.substr(1)))},
        {del, delete_address(std::format("r%2F{}.html%2F", kToken))},
        {post, copy_address(std::format("r%2F{}%2Ehtml", kToken), kArchiveSegment)},
        // dot segments, an extra '/' or segment
        {del, delete_address("..")},
        {del, delete_address(std::format("r%2F..%2F{}.html", kToken.substr(3)))},
        {del, std::format("https://storage.googleapis.com/storage/v1/b/poni-public/o/r/{}.html", kToken)},
        {del, kDeleteUrl + "/"},
        {del, kDeleteUrl + "/x"},
        {del, std::format("https://storage.googleapis.com/storage/v1/b/poni-public/o//{}", kPageSegment)},
        {del, std::format("https://storage.googleapis.com/./storage/v1/b/poni-public/o/{}", kPageSegment)},
        {post, copy_address("..", kArchiveSegment)},
        {post, kCopyUrl + "/"},
        {post, kCopyUrl + "/x"},
        {post, std::format("https://storage.googleapis.com/storage/v1/b/poni-public/o/{}/rewriteTo//b/poni-public/o/{}", kPageSegment,
                           kArchiveSegment)},
        // two formats; a token of the wrong length
        {post, copy_address(kPageSegment, archive(kOtherToken, "pdf"))},
        {post, copy_address(page(kToken, "pdf"), kArchiveSegment)},
        {del, delete_address(page(kToken.substr(1)))},
        {del, delete_address(page(std::format("{}x", kToken)))},
        {post, copy_address(page(kToken.substr(1)), kArchiveSegment)},
        {post, copy_address(kPageSegment, archive(std::format("{}x", kOtherToken)))},
        {del, delete_address(page(kToken, "abcdefghi"))},
        {del, delete_address(page(kToken, "HTML"))},
        // the host however it is written, the scheme
        {del, std::format("https://storage.googleapis.com.example/storage/v1/b/poni-public/o/{}", kPageSegment)},
        {del, std::format("https://storage.googleapis.com./storage/v1/b/poni-public/o/{}", kPageSegment)},
        {post, std::format("https://storage.googleapis.com./storage/v1/b/poni-public/o/{}/rewriteTo/b/poni-public/o/{}", kPageSegment,
                           kArchiveSegment)},
        {del, std::format("https://STORAGE.googleapis.com/storage/v1/b/poni-public/o/{}", kPageSegment)},
        {post, std::format("https://Storage.GoogleAPIs.com/storage/v1/b/poni-public/o/{}/rewriteTo/b/poni-public/o/{}", kPageSegment,
                           kArchiveSegment)},
        {del, std::format("https://storage.googleapis.com:443/storage/v1/b/poni-public/o/{}", kPageSegment)},
        {post, std::format("https://storage.googleapis.com:443/storage/v1/b/poni-public/o/{}/rewriteTo/b/poni-public/o/{}", kPageSegment,
                           kArchiveSegment)},
        {del, std::format("https://user@storage.googleapis.com/storage/v1/b/poni-public/o/{}", kPageSegment)},
        {post, std::format("https://user@storage.googleapis.com/storage/v1/b/poni-public/o/{}/rewriteTo/b/poni-public/o/{}", kPageSegment,
                           kArchiveSegment)},
        {del, std::format("http://storage.googleapis.com/storage/v1/b/poni-public/o/{}", kPageSegment)},
        {post, std::format("http://storage.googleapis.com/storage/v1/b/poni-public/o/{}/rewriteTo/b/poni-public/o/{}", kPageSegment,
                           kArchiveSegment)},
        {post, std::format("https://storage.googleapis.com/storage/v2/b/poni-public/o/{}/rewriteTo/b/poni-public/o/{}", kPageSegment,
                           kArchiveSegment)},
        // a POST to the JSON API that is not the copy
        {post, "https://storage.googleapis.com/storage/v1/b/poni-public/o"},
        {post, "https://storage.googleapis.com/storage/v1/b"},
        {post, std::format("https://storage.googleapis.com/storage/v1/b/poni-public/o/{}/compose", kPageSegment)},
        {post, std::format("https://storage.googleapis.com/storage/v1/b/poni-public/o/{}/copyTo/b/poni-public/o/{}", kPageSegment, kArchiveSegment)},
        {post, std::format("https://storage.googleapis.com/storage/v1/b/poni-public/o/{}/moveTo/o/{}", kPageSegment, kArchiveSegment)},
        {post, "https://storage.googleapis.com/batch/storage/v1"},
        // an upload under another path, with an extra segment, or with no bucket's name as the bucket
        {post, "https://storage.googleapis.com/upload/storage/v1/b/poni-public/o/x?uploadType=media&name=r%2Fx.html"},
        {post, "https://storage.googleapis.com/upload/storage/v1/b/poni-public?uploadType=multipart"},
        {post, "https://storage.googleapis.com/upload/storage/v1/b/../o?uploadType=multipart"},
        {post, "https://storage.googleapis.com/upload/storage/v1/b/./o?uploadType=multipart"},
        {post, "https://storage.googleapis.com/upload/storage/v1/b//o?uploadType=multipart"},
        {post, "https://storage.googleapis.com/upload/storage/v1/b/%70oni-public/o?uploadType=multipart"},
        {post, "https://storage.googleapis.com:443/upload/storage/v1/b/poni-public/o?uploadType=multipart"},
        {post, "http://storage.googleapis.com/upload/storage/v1/b/poni-public/o?uploadType=multipart"},
        // the copy and the delete with ".." as the bucket in the address
        {post, copy_address(kPageSegment, kArchiveSegment, "..", "..")},
        {post, copy_address(kPageSegment, kArchiveSegment, "poni-public", "..")},
        {del, delete_address(kPageSegment, "..")},
        {del, delete_address(kPageSegment, ".")},
        // what is not a POST or a DELETE is refused as before
        {http::ClientMethod::put, kDeleteUrl},
        {patch, kDeleteUrl},
    };
    return out;
}

}  // namespace

TEST(OutboundStorage, TheCopyAndTheDeleteGoOnlyToTheConfiguredBucket) {
    // Exactly as storage::GcsStore builds them.
    EXPECT_EQ(kCopyUrl, std::string(google::url("https://storage.googleapis.com",
                                                {"storage", "v1", "b", "poni-public", "o", kPage, "rewriteTo", "b", "poni-public", "o", kArchive})
                                        .buffer()));
    EXPECT_EQ(kDeleteUrl, std::string(google::url("https://storage.googleapis.com", {"storage", "v1", "b", "poni-public", "o", kPage}).buffer()));
    const boost::urls::url copy_url(kCopyUrl);
    const boost::urls::url delete_url(kDeleteUrl);
    EXPECT_TRUE(http::is_storage_host(copy_url));
    EXPECT_TRUE(http::is_report_page_copy_url(copy_url, "poni-public"));
    EXPECT_TRUE(http::is_report_page_delete_url(delete_url, "poni-public"));
    EXPECT_FALSE(http::why_refused(http::ClientMethod::post, copy_url, "poni-public"));
    EXPECT_FALSE(http::why_refused(http::ClientMethod::del, delete_url, "poni-public"));
    // Any extension, the same on both sides.
    EXPECT_FALSE(http::why_refused(http::ClientMethod::post, boost::urls::url(copy_address(std::format("r%2F{}.pdf", kToken),
                                                                                          std::format("a%2F{}.pdf", kOtherToken))),
                                   "poni-public"));

    // Without a configured bucket (a folder, or nothing), or with another, neither is sent.
    for (const auto* bucket : {"", "other-bucket", "poni-publi", "..", ".", "PONI-PUBLIC"}) {
        EXPECT_TRUE(http::why_refused(http::ClientMethod::post, copy_url, bucket)) << bucket;
        EXPECT_TRUE(http::why_refused(http::ClientMethod::del, delete_url, bucket)) << bucket;
    }
    EXPECT_THAT(std::string(*http::why_refused(http::ClientMethod::post, copy_url)), HasSubstr("Cloud Storage"));
    EXPECT_THAT(std::string(*http::why_refused(http::ClientMethod::del, delete_url)), HasSubstr("Cloud Storage"));
    // A configured bucket that is no bucket's name allows nothing, even when the address names it.
    for (const auto* bucket : {"..", ".", "ab", "Poni-Public", "-poni", "poni-"}) {
        EXPECT_TRUE(http::why_refused(http::ClientMethod::post, boost::urls::url(copy_address(kPageSegment, kArchiveSegment, bucket, bucket)), bucket))
            << bucket;
        EXPECT_TRUE(http::why_refused(http::ClientMethod::del, boost::urls::url(delete_address(kPageSegment, bucket)), bucket)) << bucket;
    }
    // The upload is not tied to the configured bucket, and its query is not looked at.
    for (const auto* upload : {"https://storage.googleapis.com/upload/storage/v1/b/poni-public/o?uploadType=multipart",
                               "https://storage.googleapis.com/upload/storage/v1/b/another.bucket_name/o?uploadType=media&name=p%2Fabc.jpg"}) {
        EXPECT_TRUE(http::is_storage_upload_url(boost::urls::url(upload))) << upload;
        EXPECT_FALSE(http::why_refused(http::ClientMethod::post, boost::urls::url(upload))) << upload;
    }
}

TEST(OutboundStorage, EveryOtherPostAndDeleteAtCloudStorageIsRefused) {
    for (const auto& [method, address] : storage_neighbours()) {
        EXPECT_TRUE(http::why_refused(method, boost::urls::url(address), "poni-public")) << address;
    }
    // A DELETE carries no body, also to the one address it may go to.
    auto with_body = request_to(http::ClientMethod::del, kDeleteUrl);
    EXPECT_FALSE(http::why_refused(with_body, "poni-public"));
    with_body.body = "{}";
    EXPECT_TRUE(http::why_refused(with_body, "poni-public"));
    // Nor does a header turn the copy into something else.
    auto overridden = request_to(http::ClientMethod::post, kCopyUrl);
    overridden.headers.emplace_back("X-HTTP-Method-Override", "DELETE");
    EXPECT_TRUE(http::why_refused(overridden, "poni-public"));
}

TEST(OutboundStorage, TheClientRefusesThemWithTheRulesOwnWordsBeforeOpeningAConnection) {
    // In mock mode an allowed Cloud Storage address is refused as well, by the mock's rule ("no other Google
    // service"); so what is compared is the refusal's text, which for a request the rule refuses is the rule's.
    RefusingClient refusing("poni-public");
    const auto refusal_of = [&](http::ClientRequest request) -> std::string {
        try {
            refusing.send(std::move(request));
        } catch (const std::invalid_argument& e) {
            return e.what();
        }
        return "sent";
    };
    auto requests = std::vector<http::ClientRequest>{};
    for (const auto& [method, address] : storage_neighbours()) requests.push_back(request_to(method, address));
    requests.push_back(request_to(http::ClientMethod::del, kDeleteUrl));
    requests.back().body = "{}";
    for (auto& request : requests) {
        const auto rule = http::why_refused(request, "poni-public");
        ASSERT_TRUE(rule) << request.url.buffer();
        const std::string address(request.url.buffer());
        EXPECT_EQ(refusal_of(std::move(request)), *rule) << address;
    }
    // The two allowed requests pass the rule and stop at the mock's.
    constexpr std::string_view kMockRefusal = "against the Google mock the engine calls no other Google service";
    EXPECT_EQ(refusal_of(request_to(http::ClientMethod::post, kCopyUrl)), kMockRefusal);
    EXPECT_EQ(refusal_of(request_to(http::ClientMethod::del, kDeleteUrl)), kMockRefusal);
    // A client without a bucket refuses both with the rule's words.
    RefusingClient without_bucket;
    for (auto request : {request_to(http::ClientMethod::post, kCopyUrl), request_to(http::ClientMethod::del, kDeleteUrl)}) {
        const auto rule = http::why_refused(request);
        ASSERT_TRUE(rule);
        try {
            without_bucket.send(std::move(request));
            ADD_FAILURE() << "sent";
        } catch (const std::invalid_argument& e) {
            EXPECT_EQ(std::string_view(e.what()), *rule);
        }
    }
}

TEST(OutboundStorage, TheRuleAndTheStoreTakeTheSameNames) {
    // The shape is written twice on purpose (client.hpp); this holds the two equal.
    const std::vector<std::string> names{
        std::format("{}.html", kToken),
        std::format("{}.pdf", kToken),
        std::format("{}.abcdefg8", kToken),
        std::format("{}.abcdefghi", kToken),
        std::format("{}.", kToken),
        std::format("{}", kToken),
        std::format("{}.HTML", kToken),
        std::format("{}.ht-l", kToken),
        std::format("{}.html", kToken.substr(1)),
        std::format("{}x.html", kToken),
        std::format("{}.{}.html", kToken.substr(0, 20), kToken.substr(21)),
        std::format("{}%{}.html", kToken.substr(0, 20), kToken.substr(21)),
        std::format("{}~{}.html", kToken.substr(0, 20), kToken.substr(21)),
        std::format("{}/{}.html", kToken.substr(0, 20), kToken.substr(21)),
        std::format("{}\xc3\xa9{}.html", kToken.substr(0, 20), kToken.substr(22)),
        std::format("{}.html/x", kToken),
        "",
    };
    for (const auto& rest : names) {
        for (const auto* prefix : {"r/", "a/", "p/", "R/", "", "../r/"}) {
            const auto name = prefix + rest;
            const auto as_page = google::url("https://storage.googleapis.com", {"storage", "v1", "b", "poni-public", "o", name});
            EXPECT_EQ(storage::is_report_page(name), http::is_report_page_delete_url(as_page, "poni-public")) << name;
            // As the copy's destination, from a page in the same format.
            const auto dot = name.rfind('.');
            const auto source = std::format("r/{}{}", kOtherToken, dot == std::string::npos ? std::string(".html") : name.substr(dot));
            const auto as_archive = google::url("https://storage.googleapis.com",
                                                {"storage", "v1", "b", "poni-public", "o", source, "rewriteTo", "b", "poni-public", "o", name});
            EXPECT_EQ(storage::is_report_archive(name) && storage::is_report_page(source),
                      http::is_report_page_copy_url(as_archive, "poni-public"))
                << name;
        }
    }
    EXPECT_TRUE(storage::is_report_page(kPage));
    EXPECT_TRUE(storage::is_report_archive(kArchive));
}

TEST_F(WritePath, AReportsPageIsCopiedAndDeletedOnlyInTheConfiguredBucket) {
    storage::GcsStore store(api_, "poni-public");
    http_.public_bucket = "poni-public";
    http_.reply(200, std::string(kTokenOk));
    http_.reply(200, R"({"kind": "storage#rewriteResponse", "done": true})");
    http_.reply(204, "");
    const auto copied = await(store.copy(kPage, kArchive));
    ASSERT_TRUE(copied) << copied.error().message;
    const auto removed = await(store.remove(kPage));
    ASSERT_TRUE(removed) << removed.error().message;
    ASSERT_EQ(http_.requests.size(), 3u);
    EXPECT_EQ(http_.requests[1].method, http::ClientMethod::post);
    EXPECT_EQ(std::string(http_.requests[1].url.buffer()), kCopyUrl);
    EXPECT_EQ(http_.requests[2].method, http::ClientMethod::del);
    EXPECT_EQ(std::string(http_.requests[2].url.buffer()), kDeleteUrl);
    EXPECT_TRUE(http_.refused.empty());

    // The engine configured with another bucket: the store's own bucket is not one the client sends these to.
    http_.public_bucket = "another-bucket";
    EXPECT_THROW(await(store.copy(kPage, kArchive)), std::invalid_argument);
    EXPECT_THROW(await(store.remove(kPage)), std::invalid_argument);
    ASSERT_EQ(http_.refused.size(), 2u);
    EXPECT_EQ(std::string(http_.refused[0].url.buffer()), kCopyUrl);
    EXPECT_EQ(std::string(http_.refused[1].url.buffer()), kDeleteUrl);
    EXPECT_EQ(http_.requests.size(), 3u);
}

// ---- Google's Admin SDK: one GET, the list of the account's users (D-106 point 3) -------------

namespace {

constexpr std::string_view kUsers = "https://admin.googleapis.com/admin/directory/v1/users";
constexpr std::string_view kUserListUrl = "https://admin.googleapis.com/admin/directory/v1/users?customer=my_customer&maxResults=500";
constexpr std::string_view kUserListPageUrl =
    "https://admin.googleapis.com/admin/directory/v1/users?customer=my_customer&maxResults=500&pageToken=abc-DEF_123.~%2F%3D";

// Every other address on the host: what the engine never asks about, and the list's address written another way.
std::vector<std::string> other_directory_addresses() {
    return {
        // one person, and the rest of a Workspace
        std::format("{}/103456789012345678901", kUsers),
        std::format("{}/someone@example.com", kUsers),
        std::format("{}/123/aliases", kUsers),
        std::format("{}/123/photos/thumbnail", kUsers),
        "https://admin.googleapis.com/admin/directory/v1/groups/abc",
        "https://admin.googleapis.com/admin/directory/v1/customer/my_customer/domains",
        // the list's path, written another way
        std::format("{}/..?customer=my_customer&maxResults=500", kUsers),
        std::format("{}/%2e%2e?customer=my_customer&maxResults=500", kUsers),
        std::format("{}/?customer=my_customer&maxResults=500", kUsers),
        "https://admin.googleapis.com/admin/directory/v1/Users?customer=my_customer&maxResults=500",
        "https://admin.googleapis.com/admin/directory/v2/users?customer=my_customer&maxResults=500",
        "http://admin.googleapis.com/admin/directory/v1/users?customer=my_customer&maxResults=500",
        "https://admin.googleapis.com:443/admin/directory/v1/users?customer=my_customer&maxResults=500",
        "https://x@admin.googleapis.com/admin/directory/v1/users?customer=my_customer&maxResults=500",
        "https://ADMIN.googleapis.com/admin/directory/v1/users?customer=my_customer&maxResults=500",
        "https://admin.googleapis.com./admin/directory/v1/users?customer=my_customer&maxResults=500",
    };
}

// The list's address with any other query than the engine's.
std::vector<std::string> other_user_list_queries() {
    const std::string list(kUserListUrl);
    return {
        std::string(kUsers),
        std::format("{}?customer=my_customer", kUsers),
        std::format("{}?maxResults=500&customer=my_customer", kUsers),
        std::format("{}?customer=C0123&maxResults=500", kUsers),
        std::format("{}?domain=example.com&maxResults=500", kUsers),
        std::format("{}?customer=my_customer&maxResults=100", kUsers),
        std::format("{}?customer=my_customer&maxResults=5000", kUsers),
        std::format("{}?customer=my_customer&customer=my_customer&maxResults=500", kUsers),
        list + "&query=name:x",
        list + "&showDeleted=true",
        list + "&viewType=domain_public",
        list + "&projection=full",
        list + "&orderBy=email",
        list + "&customer=my_customer",
        list + "&pageToken=",
        list + "&pageTokens=abc",
        list + "&pageToken=a&b",
        list + "&pageToken=a=b",
        list + "&pageToken=a/b",
        list + "&pageToken=a+b",  // a space as a form writes it
        list + "&pageToken=a?b",
        list + "&pageToken=" + std::string(2049, 'a'),
        list + "&pageToken=a&pageToken=b",
        list + "&",
        list + "&pageToken=a&",
        list + "#x",
        list + "&pageToken=a#x",
    };
}

void expect_refused_for_every_method(const std::vector<std::string>& addresses) {
    for (const auto& address : addresses) {
        const boost::urls::url url(address);
        EXPECT_TRUE(http::is_directory_host(url)) << address;
        EXPECT_FALSE(http::is_directory_user_list_url(url)) << address;
        for (const auto method : {http::ClientMethod::get, http::ClientMethod::post, http::ClientMethod::put, http::ClientMethod::patch,
                                  http::ClientMethod::del}) {
            const auto refusal = http::why_refused(method, url);
            ASSERT_TRUE(refusal) << address;
            EXPECT_THAT(std::string(*refusal), HasSubstr("Admin SDK")) << address;
        }
    }
}

}  // namespace

TEST(OutboundDirectory, TheOneRequestIsAGetOfTheList) {
    EXPECT_FALSE(http::why_refused(http::ClientMethod::get, boost::urls::url(kUserListUrl)));
    EXPECT_FALSE(http::why_refused(http::ClientMethod::get, boost::urls::url(kUserListPageUrl)));
    EXPECT_FALSE(http::why_refused(http::ClientMethod::get, boost::urls::url(std::format("{}&pageToken={}", kUserListUrl, std::string(2048, 'a')))))
        << "a token of 2,048 characters is the longest";
    EXPECT_TRUE(http::is_directory_user_list_url(boost::urls::url(kUserListUrl)));
    EXPECT_TRUE(http::is_directory_user_list_url(boost::urls::url(kUserListPageUrl)));
    // A token as Google hands it out, percent-encoded as the connector writes it, is one the rule takes.
    const auto encoded = std::format("{}&pageToken={}", kUserListUrl, http::form_encode("a b&c=d/e+f%g?h#i"));
    EXPECT_FALSE(http::why_refused(http::ClientMethod::get, boost::urls::url(encoded))) << encoded;
}

TEST(OutboundDirectory, EveryOtherMethodIsRefused) {
    for (const auto address : {kUserListUrl, kUserListPageUrl}) {
        for (const auto method : {http::ClientMethod::post, http::ClientMethod::put, http::ClientMethod::patch, http::ClientMethod::del}) {
            const auto refusal = http::why_refused(method, boost::urls::url(address));
            ASSERT_TRUE(refusal) << address;
            EXPECT_THAT(std::string(*refusal), HasSubstr("Admin SDK")) << address;
        }
    }
}

TEST(OutboundDirectory, EveryOtherAddressIsRefused) { expect_refused_for_every_method(other_directory_addresses()); }

TEST(OutboundDirectory, TheListIsOnlyAsTheEngineWritesIt) {
    expect_refused_for_every_method(other_user_list_queries());
    // A raw space and a broken escape are no address at all, so no request can carry one to the rule.
    for (const auto* rest : {"&pageToken=a b", "&pageToken=a%2", "&pageToken=a%ZZ", "&pageToken=%"}) {
        const auto address = std::format("{}{}", kUserListUrl, rest);
        EXPECT_FALSE(boost::urls::parse_uri(address)) << address;
    }
    // Encoded, as the connector writes a token, they are escapes: the rule takes them.
    for (const auto* token : {"a%20b", "a%252", "a%25ZZ", "%2F%3D%26%3F%23"}) {
        const auto address = std::format("{}&pageToken={}", kUserListUrl, token);
        EXPECT_FALSE(http::why_refused(http::ClientMethod::get, boost::urls::url(address))) << address;
    }
    // A host that only looks like it is another host.
    EXPECT_FALSE(http::is_directory_host(boost::urls::url("https://admin.googleapis.com.example/admin/directory/v1/users")));
    EXPECT_FALSE(http::is_directory_host(boost::urls::url("https://xadmin.googleapis.com/admin/directory/v1/users")));
}

TEST(OutboundDirectory, TheClientRefusesThemBeforeOpeningAConnection) {
    RefusingClient refusing;
    const auto rule = std::string(*http::why_refused(http::ClientMethod::post, boost::urls::url(kUserListUrl)));
    for (auto request : {request_to(http::ClientMethod::post, kUserListUrl),
                         request_to(http::ClientMethod::get, std::format("{}/103456789012345678901", kUsers)),
                         request_to(http::ClientMethod::get, std::format("{}/someone@example.com", kUsers)),
                         request_to(http::ClientMethod::get, std::format("{}?customer=my_customer&maxResults=500&query=name:x", kUsers))}) {
        const std::string address(request.url.buffer());
        // invalid_argument, not a network error: nothing was resolved or connected.
        try {
            refusing.send(std::move(request));
            ADD_FAILURE() << "sent: " << address;
        } catch (const std::invalid_argument& e) {
            EXPECT_EQ(std::string(e.what()), rule) << address;
        }
    }
}

TEST(OutboundDirectory, TheClientLetsTheListThrough) {
    // The list passes the rule and is sent to the mock's address, where nothing listens: a transport error, not a refusal.
    RefusingClient refusing;
    EXPECT_THROW(refusing.send(request_to(http::ClientMethod::get, kUserListUrl)), boost::system::system_error);
    EXPECT_THROW(refusing.send(request_to(http::ClientMethod::get, kUserListPageUrl)), boost::system::system_error);
}

TEST(OutboundDirectory, TheScriptedClientRefusesThemToo) {
    // So a connector test of the directory runs under the same rule as the engine.
    boost::asio::io_context io;
    test::FakeHttp http;
    EXPECT_THROW(test::run(io, http.send(request_to(http::ClientMethod::post, kUserListUrl))), std::invalid_argument);
    EXPECT_THROW(test::run(io, http.send(request_to(http::ClientMethod::get, std::format("{}/103456789012345678901", kUsers)))),
                 std::invalid_argument);
    EXPECT_TRUE(http.requests.empty()) << "a refused request is not a sent one";
    ASSERT_EQ(http.refused.size(), 2u);
    http.reply(200, R"({"users": []})");
    EXPECT_EQ(test::run(io, http.send(request_to(http::ClientMethod::get, kUserListUrl))).status, 200u);
    EXPECT_EQ(http.requests.size(), 1u);
}

// ---- the outbound client: Google Places takes one GET, a place's time zone (D-109) ----------

namespace {

constexpr std::string_view kPlaceUrl = "https://places.googleapis.com/v1/places/ChIJ-made_up-0123";

http::ClientRequest place_request(http::ClientMethod method, std::string_view address, std::vector<std::string> masks = {"timeZone"}) {
    auto request = request_to(method, address);
    request.headers.emplace_back("X-Goog-Api-Key", "test-places-key");
    for (auto& mask : masks) request.headers.emplace_back("X-Goog-FieldMask", std::move(mask));
    return request;
}

std::vector<std::string> other_place_addresses() {
    const std::string place(kPlaceUrl);
    return {
        "https://places.googleapis.com/v1/places",
        "https://places.googleapis.com/v1/places/",
        place + "/photos",
        place + "/photos/p1/media",
        "https://places.googleapis.com/v1/places:searchText",
        "https://places.googleapis.com/v1/places:searchNearby",
        "https://places.googleapis.com/v1/places:autocomplete",
        "https://places.googleapis.com/v2/places/ChIJ-made_up-0123",
        "https://places.googleapis.com/places/ChIJ-made_up-0123",
        place + "?fields=timeZone",
        place + "?key=test-places-key",
        place + "?",
        place + "#x",
        "https://places.googleapis.com/v1/places/%2E%2E",
        "https://places.googleapis.com/v1/places/..",
        "https://places.googleapis.com/v1/places/a%2Fb",
        "https://places.googleapis.com/v1/places/ChIJ%2Dx",
        "https://places.googleapis.com/v1/places/" + std::string(1025, 'a'),
        "https://places.googleapis.com:443/v1/places/ChIJ-made_up-0123",
        "https://user@places.googleapis.com/v1/places/ChIJ-made_up-0123",
        "http://places.googleapis.com/v1/places/ChIJ-made_up-0123",
        "https://PLACES.googleapis.com/v1/places/ChIJ-made_up-0123",
        "https://places.googleapis.com./v1/places/ChIJ-made_up-0123",
    };
}

}  // namespace

TEST(OutboundPlaces, TheOneRequestIsAGetOfAPlacesTimeZone) {
    EXPECT_FALSE(http::why_refused(place_request(http::ClientMethod::get, kPlaceUrl)));
    EXPECT_TRUE(http::is_place_time_zone_request(place_request(http::ClientMethod::get, kPlaceUrl)));
    EXPECT_TRUE(http::is_places_host(boost::urls::url(kPlaceUrl)));
    // A place id may be longer than GBP's ids (Google documents no maximum); 1,024 characters is the cap.
    for (const std::size_t length : {200u, 1024u}) {
        const auto address = "https://places.googleapis.com/v1/places/" + std::string(length, 'a');
        EXPECT_FALSE(http::why_refused(place_request(http::ClientMethod::get, address))) << length;
    }
    EXPECT_TRUE(http::is_place_id(std::string(1024, 'a')));
    EXPECT_FALSE(http::is_place_id(std::string(1025, 'a')));
    EXPECT_FALSE(http::is_place_id(""));
    EXPECT_FALSE(http::is_plain_id(std::string(200, 'a'))) << "GBP's own ids keep their cap";
    // The header's name in any case, as HTTP takes it.
    auto lower = request_to(http::ClientMethod::get, kPlaceUrl);
    lower.headers.emplace_back("x-goog-fieldmask", "timeZone");
    EXPECT_FALSE(http::why_refused(lower));
}

TEST(OutboundPlaces, EveryOtherMethodIsRefused) {
    for (const auto method : {http::ClientMethod::post, http::ClientMethod::put, http::ClientMethod::patch, http::ClientMethod::del}) {
        const auto refusal = http::why_refused(place_request(method, kPlaceUrl));
        ASSERT_TRUE(refusal);
        EXPECT_THAT(std::string(*refusal), HasSubstr("Places"));
        // The address alone says so too.
        EXPECT_TRUE(http::why_refused(method, boost::urls::url(kPlaceUrl)));
    }
}

TEST(OutboundPlaces, EveryOtherAddressIsRefused) {
    for (const auto& address : other_place_addresses()) {
        const boost::urls::url url(address);
        EXPECT_TRUE(http::is_places_host(url)) << address;
        for (const auto method : {http::ClientMethod::get, http::ClientMethod::post, http::ClientMethod::put, http::ClientMethod::patch,
                                  http::ClientMethod::del}) {
            const auto request = place_request(method, address);
            EXPECT_FALSE(http::is_place_time_zone_request(request)) << address;
            const auto refusal = http::why_refused(request);
            ASSERT_TRUE(refusal) << address;
            EXPECT_THAT(std::string(*refusal), HasSubstr("Places")) << address;
            EXPECT_TRUE(http::why_refused(method, url)) << address;
        }
    }
}

TEST(OutboundPlaces, TheFieldMaskIsTheTimeZoneOnceAndNothingElse) {
    const std::vector<std::vector<std::string>> masks{
        {}, {""}, {"timeZone", "timeZone"}, {"timeZone", "location"}, {"timeZone,location"}, {"*"}, {"timezone"}, {" timeZone"},
        {"timeZone "}, {"location"}, {"utcOffsetMinutes"}, {"places.timeZone"},
    };
    for (const auto& mask : masks) {
        const auto request = place_request(http::ClientMethod::get, kPlaceUrl, mask);
        EXPECT_FALSE(http::is_place_time_zone_request(request)) << mask.size();
        const auto refusal = http::why_refused(request);
        ASSERT_TRUE(refusal) << mask.size();
        EXPECT_THAT(std::string(*refusal), HasSubstr("Places"));
    }
}

TEST(OutboundPlaces, AHostThatOnlyLooksLikeItIsAnotherHost) {
    EXPECT_FALSE(http::is_places_host(boost::urls::url("https://places.googleapis.com.example/v1/places/x")));
    EXPECT_FALSE(http::is_places_host(boost::urls::url("https://xplaces.googleapis.com/v1/places/x")));
    EXPECT_FALSE(http::is_place_time_zone_request(place_request(http::ClientMethod::get, "https://places.googleapis.com.example/v1/places/x")));
}

TEST(OutboundPlaces, TheClientRefusesThemBeforeOpeningAConnection) {
    RefusingClient refusing;
    const auto rule = std::string(*http::why_refused(place_request(http::ClientMethod::post, kPlaceUrl)));
    std::vector<http::ClientRequest> requests;
    for (const auto method : {http::ClientMethod::post, http::ClientMethod::put, http::ClientMethod::patch, http::ClientMethod::del}) {
        requests.push_back(place_request(method, kPlaceUrl));
    }
    for (const auto& address : other_place_addresses()) requests.push_back(place_request(http::ClientMethod::get, address));
    requests.push_back(place_request(http::ClientMethod::get, kPlaceUrl, {}));
    requests.push_back(place_request(http::ClientMethod::get, kPlaceUrl, {"timeZone", "timeZone"}));
    requests.push_back(place_request(http::ClientMethod::get, kPlaceUrl, {"*"}));
    for (auto& request : requests) {
        const std::string address(request.url.buffer());
        // invalid_argument, not a network error: nothing was resolved or connected.
        try {
            refusing.send(std::move(request));
            ADD_FAILURE() << "sent: " << address;
        } catch (const std::invalid_argument& e) {
            EXPECT_THAT(std::string(e.what()), HasSubstr("Places")) << address;
        }
    }
    EXPECT_THAT(rule, HasSubstr("Places"));
}

TEST(OutboundPlaces, TheClientLetsTheTimeZoneThrough) {
    // It passes the rule and is sent to the mock's address, where nothing listens: a transport error, not a refusal.
    RefusingClient refusing;
    EXPECT_THROW(refusing.send(place_request(http::ClientMethod::get, kPlaceUrl)), boost::system::system_error);
}

TEST(OutboundPlaces, TheScriptedClientRefusesThemToo) {
    boost::asio::io_context io;
    test::FakeHttp http;
    EXPECT_THROW(test::run(io, http.send(place_request(http::ClientMethod::post, kPlaceUrl))), std::invalid_argument);
    EXPECT_THROW(test::run(io, http.send(place_request(http::ClientMethod::get, "https://places.googleapis.com/v1/places:searchText"))),
                 std::invalid_argument);
    EXPECT_THROW(test::run(io, http.send(place_request(http::ClientMethod::get, kPlaceUrl, {"*"}))), std::invalid_argument);
    EXPECT_TRUE(http.requests.empty()) << "a refused request is not a sent one";
    ASSERT_EQ(http.refused.size(), 3u);
    http.reply(200, R"({"timeZone": {"id": "America/New_York"}})");
    EXPECT_EQ(test::run(io, http.send(place_request(http::ClientMethod::get, kPlaceUrl))).status, 200u);
    EXPECT_EQ(http.requests.size(), 1u);
}
