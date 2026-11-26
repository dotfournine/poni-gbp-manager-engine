// The ETag of an editable thing and the comparison with If-Match (D-108).
#include "core/etag.hpp"
#include "http/api_support.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace poni;
using http::api_support::if_match_accepts;

TEST(Etag, IsTheQuotedSha256OfTheMembersInTheirOrder) {
    const boost::json::object client{{"name", "Willow Moon Head Spa"}, {"notes", ""}, {"archived", false}};
    const auto tag = core::etag(client);
    ASSERT_EQ(tag.size(), 66u);
    EXPECT_TRUE(tag.starts_with('"') && tag.ends_with('"'));
    EXPECT_EQ(tag.find_first_not_of("0123456789abcdef", 1), 65u) << "64 lower-case hex digits between the quotes";
    // The hash of {"name":"x"} as Boost.JSON writes it.
    EXPECT_EQ(core::etag({{"name", "x"}}), "\"0229d37e33daae149bf40543a5ce1db4459d10f830d5139279aa2bfd5f6485a1\"");

    EXPECT_EQ(core::etag({{"name", "Willow Moon Head Spa"}, {"notes", ""}, {"archived", false}}), tag) << "equal objects, equal tags";
    EXPECT_NE(core::etag({{"name", "Willow Moon Head Spa"}, {"notes", ""}, {"archived", true}}), tag) << "a changed value";
    EXPECT_NE(core::etag({{"name", "Willow Moon Head Spa"}, {"notes", " "}, {"archived", false}}), tag) << "a changed value";
    // Another order is another tag: which is why every caller builds the object in one fixed order.
    EXPECT_NE(core::etag({{"archived", false}, {"name", "Willow Moon Head Spa"}, {"notes", ""}}), tag);
}

TEST(Etag, IfMatchTakesTheOneStrongTagAndWhatCaddysCompressionMakesOfIt) {
    const auto tag = core::etag({{"name", "x"}});
    const auto hex = tag.substr(1, 64);
    EXPECT_TRUE(if_match_accepts(tag, tag));
    EXPECT_TRUE(if_match_accepts("  " + tag + " \t", tag)) << "spaces around it";
    // Caddy's `encode zstd gzip` (v2.11.9) answers "<hex>-gzip" or "<hex>-zstd" for a response it compresses.
    EXPECT_TRUE(if_match_accepts("\"" + hex + "-gzip\"", tag));
    EXPECT_TRUE(if_match_accepts("\"" + hex + "-zstd\"", tag));

    for (const std::string& refused : {
             core::etag({{"name", "y"}}),            // another tag
             std::string("*"),                       // anything at all
             tag + ", " + tag,                       // a list, even of the same tag
             core::etag({{"name", "y"}}) + ", " + tag,  // a list that holds it
             std::string(""),                        // empty
             std::string("   "),                     // only spaces
             "W/" + tag,                             // a weak tag
             "W/\"" + hex + "-gzip\"",               // a weak compressed one
             hex,                                    // without the quotes
             "\"" + hex + "-br\"",                   // an encoding the site does not use
             "\"" + hex + "-gzip-gzip\"",            // twice
             "\"" + hex + "gzip\"",                  // without the dash
             "\"" + hex.substr(1) + "-gzip\"",       // another hex of the same length
             "\"" + hex + "\"x",                     // something after it
         }) {
        EXPECT_FALSE(if_match_accepts(refused, tag)) << "[" << refused << "]";
    }
}
