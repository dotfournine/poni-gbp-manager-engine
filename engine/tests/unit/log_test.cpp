#include "core/log.hpp"

#include "support.hpp"

#include <boost/json/parse.hpp>
#include <gtest/gtest.h>

using namespace poni;
using namespace std::chrono_literals;

TEST(Log, JsonLineHasTimestampLevelMessageThenFields) {
    const auto line = log::format_line(log::Format::json, test::t0() + 123ms, log::Level::warn, "job failed",
                                       {{"job_id", 42}, {"error", "boom"}});
    EXPECT_EQ(line,
              R"({"ts":"2026-11-10T12:00:00.123Z","level":"warn","msg":"job failed","job_id":42,"error":"boom"})");
}

TEST(Log, JsonLineEscapesAndStaysOneLine) {
    const auto line = log::format_line(log::Format::json, test::t0(), log::Level::info, "quote \" newline \n 路梦",
                                       {{"k", "tab\t"}});
    EXPECT_EQ(line.find('\n'), std::string::npos);
    const auto parsed = boost::json::parse(line).as_object();
    EXPECT_EQ(parsed.at("msg").as_string(), "quote \" newline \n 路梦");
    EXPECT_EQ(parsed.at("k").as_string(), "tab\t");
}

TEST(Log, FieldsCannotOverwriteReservedKeys) {
    const auto line = log::format_line(log::Format::json, test::t0(), log::Level::info, "real",
                                       {{"msg", "fake"}, {"level", "error"}, {"ts", "never"}, {"kept", true}});
    const auto parsed = boost::json::parse(line).as_object();
    EXPECT_EQ(parsed.size(), 4u);
    EXPECT_EQ(parsed.at("msg").as_string(), "real");
    EXPECT_EQ(parsed.at("level").as_string(), "info");
    EXPECT_TRUE(parsed.at("kept").as_bool());
}

TEST(Log, TextLineIsReadable) {
    const auto line =
        log::format_line(log::Format::text, test::t0(), log::Level::info, "engine started", {{"port", 8080}});
    EXPECT_EQ(line, "2026-11-10T12:00:00.000Z info  engine started port=8080");
}

TEST(Log, ParsesLevelsAndFormats) {
    EXPECT_EQ(log::parse_level("debug"), log::Level::debug);
    EXPECT_EQ(log::parse_level("error"), log::Level::error);
    EXPECT_FALSE(log::parse_level("verbose"));
    EXPECT_EQ(log::parse_format("text"), log::Format::text);
    EXPECT_FALSE(log::parse_format("xml"));
}
