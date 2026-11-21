// The strict reader for request bodies (D-092): what it takes, what it
// refuses, and the sentence it refuses with.

#include "core/body.hpp"

#include <boost/describe/class.hpp>
#include <boost/json/serialize.hpp>
#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace poni;

namespace {

struct Step {
    std::string name;
    std::optional<int> minutes;
};
BOOST_DESCRIBE_STRUCT(Step, (), (name, minutes))

// A PATCH: everything may be left out, and two members can be cleared.
struct Patch {
    std::optional<std::string> name;
    std::optional<bool> enabled;
    std::optional<std::int64_t> cap;
    std::optional<double> ratio;
    core::Nullable<std::string> zone;
    core::Nullable<std::int64_t> owner_id;
    std::optional<std::vector<Step>> steps;
    std::optional<boost::json::object> extras;
};
BOOST_DESCRIBE_STRUCT(Patch, (), (name, enabled, cap, ratio, zone, owner_id, steps, extras))

struct Create {
    std::string name;
    std::vector<std::string> recipients;
    std::optional<bool> enabled;
};
BOOST_DESCRIBE_STRUCT(Create, (), (name, recipients, enabled))

// A document that travels whole: every member is there, an empty one as null.
struct Button {
    std::string kind;
    std::optional<std::string> url;
    static constexpr bool sent_whole = true;
};
BOOST_DESCRIBE_STRUCT(Button, (), (kind, url))

struct Document {
    std::string text;
    std::optional<Button> button;
    std::optional<std::int64_t> media_id;
    static constexpr bool sent_whole = true;
};
BOOST_DESCRIBE_STRUCT(Document, (), (text, button, media_id))

// Settings as a save sends them: structs inside structs, a list and a map.
struct Range {
    std::int64_t min = 0;
    std::int64_t max = 0;
};
BOOST_DESCRIBE_STRUCT(Range, (), (min, max))

struct Emoji {
    bool allowed = false;
    std::int64_t max = 0;
};
BOOST_DESCRIBE_STRUCT(Emoji, (), (allowed, max))

struct Settings {
    std::string sign_off;
    std::vector<std::string> facts;
    Emoji emoji;
    std::map<std::string, Range> length;
};
BOOST_DESCRIBE_STRUCT(Settings, (), (sign_off, facts, emoji, length))

struct SettingsBody {
    Settings settings;
};
BOOST_DESCRIBE_STRUCT(SettingsBody, (), (settings))

template <class T>
std::string refusal(std::string_view body) {
    const auto parsed = core::parse_body<T>(body);
    return parsed ? std::string("accepted") : parsed.error();
}

}  // namespace

TEST(Body, TakesWhatTheStructDescribes) {
    const auto patch = core::parse_body<Patch>(
        R"({"name": "x", "enabled": false, "cap": 5, "ratio": 1, "steps": [{"name": "a", "minutes": 3}, {"name": "b"}],
            "extras": {"anything": null}})");
    ASSERT_TRUE(patch) << patch.error();
    EXPECT_EQ(patch->name, "x");
    EXPECT_EQ(patch->enabled, false);
    EXPECT_EQ(patch->cap, 5);
    EXPECT_EQ(patch->ratio, 1.0);
    ASSERT_EQ(patch->steps->size(), 2u);
    EXPECT_EQ(patch->steps->at(0).minutes, 3);
    EXPECT_FALSE(patch->steps->at(1).minutes);
    // A free-form object is taken as it is.
    EXPECT_TRUE(patch->extras->at("anything").is_null());

    const auto empty = core::parse_body<Patch>("{}");
    ASSERT_TRUE(empty) << empty.error();
    EXPECT_FALSE(empty->name || empty->enabled || empty->cap || empty->steps);
}

TEST(Body, RefusesAMemberTheStructDoesNotHave) {
    EXPECT_EQ(refusal<Patch>(R"({"name": "x", "colour": "red"})"), "unknown field 'colour'");
    EXPECT_EQ(refusal<Patch>(R"({"steps": [{"name": "a"}, {"name": "b", "seconds": 1}]})"), "unknown field 'steps[1].seconds'");
    // A name is the caller's text: the answer carries the start of a very long one.
    EXPECT_EQ(refusal<Patch>("{\"" + std::string(500, 'x') + "\": 1}"), "unknown field '" + std::string(80, 'x') + "…'");
    // The misspelled member is named, not the one that is then missing.
    EXPECT_EQ(refusal<Create>(R"({"nam": "x", "recipients": []})"), "unknown field 'nam'");
}

TEST(Body, RefusesNullForAMemberThatCannotBeEmpty) {
    EXPECT_EQ(refusal<Patch>(R"({"enabled": null})"), "field 'enabled' must not be null");
    EXPECT_EQ(refusal<Patch>(R"({"steps": null})"), "field 'steps' must not be null");
    EXPECT_EQ(refusal<Patch>(R"({"steps": [{"name": "a", "minutes": null}]})"), "field 'steps[0].minutes' must not be null");
    EXPECT_EQ(refusal<Patch>(R"({"steps": [null]})"), "field 'steps[0]' must not be null");
    EXPECT_EQ(refusal<Create>(R"({"name": null, "recipients": []})"), "field 'name' must not be null");
    EXPECT_EQ(refusal<Create>(R"({"name": "x", "recipients": ["a", null]})"), "field 'recipients[1]' must not be null");
}

TEST(Body, RefusesAMissingRequiredMember) {
    EXPECT_EQ(refusal<Create>(R"({"recipients": []})"), "missing field 'name'");
    EXPECT_EQ(refusal<Create>(R"({"name": "x"})"), "missing field 'recipients'");
    EXPECT_EQ(refusal<Patch>(R"({"steps": [{"minutes": 3}]})"), "missing field 'steps[0].name'");
}

TEST(Body, AWholeNumberMayHaveAFractionOfZero) {
    const auto patch = core::parse_body<Patch>(R"({"cap": 100.0, "owner_id": 7.0, "steps": [{"name": "a", "minutes": 1e1}]})");
    ASSERT_TRUE(patch) << patch.error();
    EXPECT_EQ(patch->cap, 100);
    EXPECT_EQ(patch->owner_id.value(), 7);
    EXPECT_EQ(patch->steps->at(0).minutes, 10);
    EXPECT_EQ(refusal<Patch>(R"({"cap": 100.5})"), "field 'cap' has the wrong type");
    EXPECT_EQ(refusal<Patch>(R"({"cap": 1e30})"), "field 'cap' has the wrong type");
    // Too large for the member's own type.
    EXPECT_EQ(refusal<Patch>(R"({"steps": [{"name": "a", "minutes": 5000000000}]})"), "field 'steps[0].minutes' has the wrong type");
}

TEST(Body, RefusesAValueOfTheWrongType) {
    EXPECT_EQ(refusal<Patch>(R"({"name": 5})"), "field 'name' has the wrong type");
    EXPECT_EQ(refusal<Patch>(R"({"enabled": "yes"})"), "field 'enabled' has the wrong type");
    EXPECT_EQ(refusal<Patch>(R"({"enabled": 1})"), "field 'enabled' has the wrong type");
    EXPECT_EQ(refusal<Patch>(R"({"cap": "5"})"), "field 'cap' has the wrong type");
    EXPECT_EQ(refusal<Patch>(R"({"cap": true})"), "field 'cap' has the wrong type");
    EXPECT_EQ(refusal<Patch>(R"({"ratio": "high"})"), "field 'ratio' has the wrong type");
    EXPECT_EQ(refusal<Patch>(R"({"zone": 5})"), "field 'zone' has the wrong type");
    EXPECT_EQ(refusal<Patch>(R"({"steps": {}})"), "field 'steps' must be a list");
    EXPECT_EQ(refusal<Patch>(R"({"steps": ["a"]})"), "field 'steps[0]' must be an object");
    EXPECT_EQ(refusal<Patch>(R"({"extras": []})"), "field 'extras' has the wrong type");
}

TEST(Body, MustBeAJsonObject) {
    for (const auto* body : {"", "{not json", "[]", "null", "5", "\"text\""}) {
        EXPECT_EQ(refusal<Patch>(body), "The body must be a JSON object.") << body;
    }
}

TEST(Body, ANullableMemberHasThreeStates) {
    const auto left_out = core::parse_body<Patch>("{}").value();
    EXPECT_FALSE(left_out.zone.given());
    EXPECT_FALSE(left_out.zone.value());
    EXPECT_EQ(left_out.zone.over("UTC"), "UTC");

    const auto null = core::parse_body<Patch>(R"({"zone": null, "owner_id": null})").value();
    EXPECT_TRUE(null.zone.given());
    EXPECT_FALSE(null.zone.value());
    EXPECT_EQ(null.zone.over("UTC"), std::nullopt);
    EXPECT_TRUE(null.owner_id.given());

    const auto value = core::parse_body<Patch>(R"({"zone": "America/Toronto"})").value();
    EXPECT_TRUE(value.zone.given());
    EXPECT_EQ(value.zone.value(), "America/Toronto");
    EXPECT_EQ(value.zone.over("UTC"), "America/Toronto");
    EXPECT_FALSE(value.owner_id.given());
}

TEST(Body, ANullableMemberIsBuiltFromEachState) {
    const core::Nullable<std::string> left_out;
    const core::Nullable<std::string> null = std::nullopt;
    const core::Nullable<std::string> value = "x";
    const core::Nullable<std::string> from_optional = std::optional<std::string>{};
    EXPECT_FALSE(left_out.given());
    EXPECT_TRUE(null.given());
    EXPECT_EQ(value.value(), "x");
    EXPECT_EQ(from_optional, null);
    EXPECT_NE(left_out, null);
}

TEST(Body, ADocumentSentWholeNeedsEveryMemberAndTakesNull) {
    const auto document = core::parse_body<Document>(R"({"text": "x", "button": {"kind": "call", "url": null}, "media_id": null})");
    ASSERT_TRUE(document) << document.error();
    EXPECT_EQ(document->button->kind, "call");
    EXPECT_FALSE(document->button->url);
    EXPECT_FALSE(document->media_id);

    EXPECT_EQ(refusal<Document>(R"({"text": "x", "button": null})"), "missing field 'media_id'");
    // The struct inside says for itself that it is sent whole.
    EXPECT_EQ(refusal<Document>(R"({"text": "x", "button": {"kind": "call"}, "media_id": 3})"), "missing field 'button.url'");
    EXPECT_EQ(refusal<Document>(R"({"text": null, "button": null, "media_id": null})"), "field 'text' must not be null");
    EXPECT_EQ(refusal<Document>(R"({"text": "x", "button": null, "media_id": null, "id": 9})"), "unknown field 'id'");
    EXPECT_EQ(refusal<Document>(R"({"text": "x", "button": {"kind": "call", "url": null, "colour": "red"}, "media_id": null})"),
              "unknown field 'button.colour'");
}

TEST(Body, StructsListsAndMapsAreReadAtEveryDepth) {
    const auto body = core::parse_body<SettingsBody>(
        R"({"settings": {"sign_off": "Thanks", "facts": ["a"], "emoji": {"allowed": true, "max": 1}, "length": {"en": {"min": 1, "max": 9}}}})");
    ASSERT_TRUE(body) << body.error();
    EXPECT_EQ(body->settings.emoji.max, 1);
    EXPECT_EQ(body->settings.length.at("en").max, 9);
    // A map may be empty: which keys it needs is its owner's to say.
    EXPECT_TRUE(core::parse_body<SettingsBody>(R"({"settings": {"sign_off": "", "facts": [], "emoji": {"allowed": false, "max": 0}, "length": {}}})"));

    // A body with these parts, each as JSON text; the others as an empty Settings has them.
    struct Sent {
        std::string sign_off = R"("")";
        std::string facts = "[]";
        std::string emoji = R"({"allowed": false, "max": 0})";
        std::string length = "{}";
    };
    const auto with = [](const Sent& s) {
        return std::format(R"({{"settings": {{"sign_off": {}, "facts": {}, "emoji": {}, "length": {}}}}})", s.sign_off, s.facts, s.emoji, s.length);
    };
    EXPECT_EQ(refusal<SettingsBody>("{}"), "missing field 'settings'");
    EXPECT_EQ(refusal<SettingsBody>(R"({"settings": {"sign_off": ""}})"), "missing field 'settings.facts'");
    EXPECT_EQ(refusal<SettingsBody>(with({.emoji = R"({"allowed": true})"})), "missing field 'settings.emoji.max'");
    EXPECT_EQ(refusal<SettingsBody>(with({.emoji = R"({"allowed": true, "max": 1, "size": 2})"})), "unknown field 'settings.emoji.size'");
    EXPECT_EQ(refusal<SettingsBody>(with({.length = R"({"en": {"min": 1, "max": 9, "unit": "words"}})"})),
              "unknown field 'settings.length.en.unit'");
    EXPECT_EQ(refusal<SettingsBody>(with({.length = R"({"en": {"max": 9}})"})), "missing field 'settings.length.en.min'");
    EXPECT_EQ(refusal<SettingsBody>(with({.sign_off = R"(null)"})), "field 'settings.sign_off' must not be null");
    EXPECT_EQ(refusal<SettingsBody>(with({.emoji = R"({"allowed": true, "max": null})"})), "field 'settings.emoji.max' must not be null");
    EXPECT_EQ(refusal<SettingsBody>(with({.facts = R"(["a", null])"})), "field 'settings.facts[1]' must not be null");
    EXPECT_EQ(refusal<SettingsBody>(with({.emoji = R"({"allowed": true, "max": "two"})"})), "field 'settings.emoji.max' has the wrong type");
    EXPECT_EQ(refusal<SettingsBody>(with({.facts = R"("one")"})), "field 'settings.facts' must be a list");
    EXPECT_EQ(refusal<SettingsBody>(with({.length = R"([])"})), "field 'settings.length' must be an object");
    EXPECT_EQ(refusal<SettingsBody>(R"({"settings": []})"), "field 'settings' must be an object");
    EXPECT_EQ(refusal<SettingsBody>(R"({"settings": null})"), "field 'settings' must not be null");
}
