// What the fingerprinted prompts (prompts.cpp) and the others
// (report_post_prompts.cpp) share, and how the domain reads a model's
// answer. Nothing outside domain/ uses it.
#pragma once

#include "domain/reply_settings.hpp"

#include <boost/json/object.hpp>

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace poni::domain::prompt_parts {

// Replaces each ${name} with its value. A name without a value is a bug in the prompt's file.
std::string fill(std::string_view text, const std::map<std::string_view, std::string>& values);

// The voice's lines as the writer is given them: base style, emoji, sign-off,
// words to avoid, style notes, and with `with_examples` the example replies.
std::string voice_text(const ResolvedSettings& settings, bool with_examples);

// A member of a model's answer, if it is there with the right type.
inline std::optional<std::string> string_of(const boost::json::object& object, std::string_view key) {
    const auto* value = object.if_contains(key);
    if (!value || !value->is_string()) return std::nullopt;
    return std::string(value->get_string());
}

inline std::optional<bool> bool_of(const boost::json::object& object, std::string_view key) {
    const auto* value = object.if_contains(key);
    if (!value || !value->is_bool()) return std::nullopt;
    return value->get_bool();
}

}  // namespace poni::domain::prompt_parts
