// The engine's ways out, against a scripted HTTP client and the real
// outbound client: the four requests that change something at Google (a
// reply to a review, the create of a local post, the delete of a local
// post, the edit of a local post), each of which the client refuses to send
// anywhere else, and the email the Gmail channel builds.

#include "connectors/gbp/writer.hpp"
#include "connectors/notify/gmail.hpp"
#include "core/crypto.hpp"
#include "core/tls.hpp"
#include "unit/connector_fixture.hpp"

#include <boost/json/parse.hpp>
#include <boost/system/system_error.hpp>
#include <gmock/gmock.h>

#include <array>
#include <format>

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
        : ConnectorTest({gbp::kReviewsApi, google::kOAuthApi, notify::kGmailApi}),
          tokens_(clock_, google::refresh_fetch(http_, limiter_, test::stored_credentials())),
          writer_(api_),
          gmail_(api_, {.address = "host@xvisionmediaus.com", .name = "PONI GBP Manager"}) {}

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
// would fail on this machine instead of reaching Google.
struct RefusingClient {
    boost::asio::io_context io;
    core::TlsSetup tls = core::make_tls_client_context();
    http::NetClient client{tls.ctx, boost::urls::url("http://127.0.0.1:1")};

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
    EXPECT_EQ(mime_body(*raw), "Hello.");
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
    notify::GmailChannel unsigned_channel({http_, nobody, limiter_}, {.address = "host@xvisionmediaus.com", .name = "PONI"});
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
