// Structured logging. One line per event on stdout: JSON by default, or a
// readable text form for local work (PONI_LOG_FORMAT).
#pragma once

#include "core/clock.hpp"

#include <boost/json/object.hpp>

#include <expected>
#include <string>
#include <string_view>

namespace poni::log {

enum class Level { debug, info, warn, error };
enum class Format { json, text };

std::expected<Level, std::string> parse_level(std::string_view s);
std::expected<Format, std::string> parse_format(std::string_view s);
std::string_view to_string(Level level);

// Call once at start-up. Before that, events at `info` and above go to stdout as JSON.
void init(Level min_level, Format format);

// The line that would be written, without the trailing newline. Reserved keys
// `ts`, `level` and `msg` come first; a field with one of those names is dropped.
std::string format_line(Format format, core::TimePoint ts, Level level, std::string_view msg,
                        const boost::json::object& fields);

void write(Level level, std::string_view msg, const boost::json::object& fields = {});

inline void debug(std::string_view msg, const boost::json::object& fields = {}) { write(Level::debug, msg, fields); }
inline void info(std::string_view msg, const boost::json::object& fields = {}) { write(Level::info, msg, fields); }
inline void warn(std::string_view msg, const boost::json::object& fields = {}) { write(Level::warn, msg, fields); }
inline void error(std::string_view msg, const boost::json::object& fields = {}) { write(Level::error, msg, fields); }

}  // namespace poni::log
