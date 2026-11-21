#include "core/log.hpp"

#include <boost/json/serialize.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <format>
#include <utility>

namespace poni::log {

namespace {

// Set once by init(); read by every thread that logs (the pool threads too).
std::atomic<Level> g_level{Level::info};
std::atomic<Format> g_format{Format::json};

bool reserved(std::string_view key) { return key == "ts" || key == "level" || key == "msg"; }

}  // namespace

std::expected<Level, std::string> parse_level(std::string_view s) {
    if (s == "debug") return Level::debug;
    if (s == "info") return Level::info;
    if (s == "warn") return Level::warn;
    if (s == "error") return Level::error;
    return std::unexpected(std::format("unknown log level '{}' (expected debug, info, warn or error)", s));
}

std::expected<Format, std::string> parse_format(std::string_view s) {
    if (s == "json") return Format::json;
    if (s == "text") return Format::text;
    return std::unexpected(std::format("unknown log format '{}' (expected json or text)", s));
}

std::string_view to_string(Level level) {
    switch (level) {
        case Level::debug: return "debug";
        case Level::info: return "info";
        case Level::warn: return "warn";
        case Level::error: return "error";
    }
    std::unreachable();
}

void init(Level min_level, Format format) {
    g_format = format;
    g_level = min_level;
}

std::string format_line(Format format, core::TimePoint ts, Level level, std::string_view msg,
                        const boost::json::object& fields) {
    const auto stamp =
        std::format("{:%FT%T}Z", std::chrono::time_point_cast<std::chrono::milliseconds>(ts));
    if (format == Format::json) {
        boost::json::object line;
        line.reserve(fields.size() + 3);
        line.emplace("ts", stamp);
        line.emplace("level", to_string(level));
        line.emplace("msg", msg);
        for (const auto& kv : fields) {
            if (!reserved(kv.key())) line.emplace(kv.key(), kv.value());
        }
        return boost::json::serialize(line);
    }
    std::string line = std::format("{} {:<5} {}", stamp, to_string(level), msg);
    for (const auto& kv : fields) {
        if (reserved(kv.key())) continue;
        line += std::format(" {}={}", std::string_view(kv.key()), boost::json::serialize(kv.value()));
    }
    return line;
}

void write(Level level, std::string_view msg, const boost::json::object& fields) {
    if (level < g_level.load()) return;
    auto line = format_line(g_format, core::SystemClock{}.now(), level, msg, fields);
    line += '\n';
    // One write per line, flushed at once: stdio locks the stream for the call, so lines from
    // several threads don't interleave, and a crash loses no line that was logged.
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
}

}  // namespace poni::log
