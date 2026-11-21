// The core helpers: hashing, encoding and secrets at rest, JSON mapping errors, and the rate limiter.

#include "unit/test_key.hpp"
#include "core/crypto.hpp"
#include "core/json.hpp"
#include "core/rate_limiter.hpp"
#include "support.hpp"

#include <boost/describe/class.hpp>
#include <gtest/gtest.h>

#include <stdexcept>

using namespace poni;
using namespace std::chrono_literals;

namespace {

const std::string kKey = core::base64_encode(std::string(32, 'k'));

struct Inner {
    std::string id;
    std::optional<std::string> note;
};
BOOST_DESCRIBE_STRUCT(Inner, (), (id, note))

struct Outer {
    std::string name;
    std::optional<std::vector<Inner>> items;
    std::optional<int> count;
};
BOOST_DESCRIBE_STRUCT(Outer, (), (name, items, count))

}  // namespace

TEST(Base64, RoundTripsBothAlphabets) {
    const std::string bytes("\x00\xff\xfe\x10hello", 9);
    EXPECT_EQ(core::base64_decode(core::base64_encode(bytes)), bytes);
    EXPECT_EQ(core::base64_decode(core::base64url_encode(bytes)), bytes);
    EXPECT_EQ(core::base64_encode("ab"), "YWI=");
    EXPECT_EQ(core::base64url_encode("ab"), "YWI");
    EXPECT_FALSE(core::base64_decode("not base64!"));
}

TEST(RandomToken, IsLongAndDifferentEachTime) {
    const auto a = core::random_token();
    EXPECT_GE(a.size(), 43u);
    EXPECT_NE(a, core::random_token());
}

TEST(SecretBox, RoundTripsAndNeverStoresPlaintext) {
    const auto box = core::SecretBox::from_base64_key(kKey);
    ASSERT_TRUE(box);
    const auto sealed = box->seal("refresh-token-value", "gbp");
    EXPECT_EQ(sealed.find("refresh-token-value"), std::string::npos);
    EXPECT_NE(sealed, box->seal("refresh-token-value", "gbp")) << "a fresh nonce every time";
    EXPECT_EQ(box->open(sealed, "gbp"), "refresh-token-value");
}

TEST(SecretBox, RejectsWrongKeyContextAndTampering) {
    const auto box = core::SecretBox::from_base64_key(kKey);
    const auto other = core::SecretBox::from_base64_key(core::base64_encode(std::string(32, 'x')));
    ASSERT_TRUE(box && other);
    const auto sealed = box->seal("secret", "gbp");

    EXPECT_FALSE(other->open(sealed, "gbp"));
    EXPECT_FALSE(box->open(sealed, "another-row")) << "a value can't be moved to another row";
    auto raw = *core::base64_decode(sealed);
    raw[raw.size() / 2] ^= 0x01;
    EXPECT_FALSE(box->open(core::base64_encode(raw), "gbp"));
    EXPECT_FALSE(box->open("AAAA", "gbp"));
    EXPECT_NE(box->key_id(), other->key_id());
}

TEST(SecretBox, NeedsA32ByteKey) {
    EXPECT_FALSE(core::SecretBox::from_base64_key(""));
    EXPECT_FALSE(core::SecretBox::from_base64_key(core::base64_encode("too short")));
}

TEST(Json, IgnoresUnknownKeysAndAllowsMissingOptionals) {
    const auto parsed = core::parse_json<Outer>(R"({"name": "x", "extra": [1, 2], "items": [{"id": "a", "more": true}]})");
    ASSERT_TRUE(parsed) << parsed.error();
    EXPECT_EQ(parsed->name, "x");
    ASSERT_TRUE(parsed->items);
    EXPECT_EQ(parsed->items->at(0).id, "a");
    EXPECT_FALSE(parsed->count);
}

TEST(Json, NamesTheFieldThatIsMissingOrWrong) {
    EXPECT_EQ(core::parse_json<Outer>(R"({"items": []})").error(), "missing field 'name'");
    EXPECT_EQ(core::parse_json<Outer>(R"({"name": "x", "items": [{"id": "a"}, {"note": "n"}]})").error(),
              "missing field 'items[1].id'");
    EXPECT_EQ(core::parse_json<Outer>(R"({"name": "x", "count": "three"})").error(), "field 'count' has the wrong type");
    EXPECT_EQ(core::parse_json<Outer>(R"({"name": "x", "items": {}})").error(), "field 'items' must be a list");
    EXPECT_TRUE(core::parse_json<Outer>("{not json").error().starts_with("not valid JSON"));
}

TEST(RateLimiter, AllowsABurstThenSpacesRequests) {
    boost::asio::io_context io;
    core::ManualClock clock(test::t0());
    core::RateLimiter limiter(io.get_executor(), clock);
    limiter.configure("api", {.per_minute = 60, .burst = 3});

    EXPECT_EQ(limiter.reserve("api"), 0s);
    EXPECT_EQ(limiter.reserve("api"), 0s);
    EXPECT_EQ(limiter.reserve("api"), 0s);
    EXPECT_EQ(limiter.reserve("api"), 1s) << "the burst is used up: one request a second from here";
    EXPECT_EQ(limiter.reserve("api"), 2s);

    clock.advance(10s);
    EXPECT_EQ(limiter.reserve("api"), 0s) << "a quiet spell refills the burst";
}

TEST(RateLimiter, PauseHoldsEveryRequestBack) {
    boost::asio::io_context io;
    core::ManualClock clock(test::t0());
    core::RateLimiter limiter(io.get_executor(), clock);
    limiter.configure("api", {.per_minute = 600, .burst = 10});

    limiter.pause("api", 60s);
    EXPECT_GE(limiter.reserve("api"), 60s);
    clock.advance(61s);
    EXPECT_EQ(limiter.reserve("api"), 0s);
}

TEST(RateLimiter, RefusesAnApiWithoutALimit) {
    boost::asio::io_context io;
    core::ManualClock clock(test::t0());
    core::RateLimiter limiter(io.get_executor(), clock);
    EXPECT_THROW(limiter.reserve("unknown"), std::logic_error);
}

TEST(RateLimiter, AcquireReturnsAtOnceWithinTheBurst) {
    boost::asio::io_context io;
    core::ManualClock clock(test::t0());
    core::RateLimiter limiter(io.get_executor(), clock);
    limiter.configure("api", {.per_minute = 60, .burst = 2});
    test::run(io, limiter.acquire("api"));
    test::run(io, limiter.acquire("api"));
    EXPECT_EQ(limiter.reserve("api"), 1s);
}

// Test vectors from FIPS 180-4.
TEST(Sha256, MatchesKnownVectors) {
    EXPECT_EQ(core::sha256_hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(core::sha256_hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

// ---- the APNs auth key (D-097) ----------------------------------------------------------

TEST(Es256Key, TakesThePkcs8KeyAsADotP8FileHoldsItAndSigns) {
    const auto& key = poni::test::test_key();
    const auto plain = core::Es256Key::from_p8(key.p8);
    ASSERT_TRUE(plain) << plain.error();
    const auto signature = plain->sign("header.claims");
    EXPECT_EQ(signature.size(), 64u);
    EXPECT_TRUE(key.verifies("header.claims", signature));
    EXPECT_FALSE(key.verifies("header.claimz", signature));

    // The whole file on one line, as an env file can only hold it, and the file as it is.
    std::string multi = "-----BEGIN PRIVATE KEY-----\n";
    for (std::size_t i = 0; i < key.p8.size(); i += 64) multi += key.p8.substr(i, 64) + "\n";
    multi += "-----END PRIVATE KEY-----\n";
    EXPECT_TRUE(core::Es256Key::from_p8(multi));
    EXPECT_TRUE(core::Es256Key::from_p8("-----BEGIN PRIVATE KEY-----" + key.p8 + "-----END PRIVATE KEY-----"));
}

TEST(Es256Key, RefusesWhatIsNotAP256Key) {
    EXPECT_FALSE(core::Es256Key::from_p8(""));
    EXPECT_FALSE(core::Es256Key::from_p8("not base64 !!"));
    EXPECT_FALSE(core::Es256Key::from_p8(core::base64_encode("not a key")));
    // An RSA key is a private key, but not one Apple gives.
    EVP_PKEY* rsa = EVP_RSA_gen(2048);
    PKCS8_PRIV_KEY_INFO* info = EVP_PKEY2PKCS8(rsa);
    unsigned char* der = nullptr;
    const int n = i2d_PKCS8_PRIV_KEY_INFO(info, &der);
    const auto refused = core::Es256Key::from_p8(core::base64_encode(std::string_view(reinterpret_cast<const char*>(der), static_cast<std::size_t>(n))));
    OPENSSL_free(der);
    PKCS8_PRIV_KEY_INFO_free(info);
    EVP_PKEY_free(rsa);
    ASSERT_FALSE(refused);
    EXPECT_NE(refused.error().find("P-256"), std::string::npos) << refused.error();
}
