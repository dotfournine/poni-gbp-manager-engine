// Mechanical checks on a drafted reply (architecture section 10, item 3).
// They are plain code and know nothing about the model: a reply that fails
// any of them is never routed to auto-post, whatever the model claimed.
//
// They run on the reply with full-width forms turned into ASCII and
// zero-width characters dropped (text::fold), so a link or a number can't
// hide behind those.
#pragma once

#include "domain/reply_settings.hpp"

#include <boost/describe/class.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace poni::domain {

struct CheckResult {
    std::string name;
    bool passed = false;
    // What was found, for the person reviewing the draft.
    std::string detail;
};
BOOST_DESCRIBE_STRUCT(CheckResult, (), (name, passed, detail))

struct CheckReport {
    std::vector<CheckResult> results;

    bool passed() const;
    // Names of the checks that failed.
    std::vector<std::string> failed() const;
    // The same, joined with ", ", for a message to staff: "no_url, length".
    std::string failed_names() const;
};

struct CheckContext {
    const Voice& voice;
    // "en", "zh" or "es": the language the reply must be in.
    std::string target_language;
    // "positive", "complaint", "severity", "rating_only" or "rating_only_low".
    std::string mode;
};

// Check names, in the order they run:
//   not_empty, no_url, no_email, no_phone, emoji_limit, length, language_match.
// no_url, no_email and no_phone pass when the voice allows contact details.
// These are the checks that need no pattern library (D-058). What a reply
// says (offers, promises, other businesses) and the script of a Chinese reply
// (D-056) are left to the model and to the person who approves the reply.
CheckReport run_reply_checks(std::string_view reply, const CheckContext& context);

// The scans behind no_url, no_email and no_phone, for other text the engine
// sends to Google (a local post, D-088 point 10). Each takes the output of
// text::fold() and returns what it found.
std::optional<std::string> scan_for_url(std::u32string_view folded);
std::optional<std::string> scan_for_email(std::u32string_view folded);
std::optional<std::string> scan_for_phone(std::u32string_view folded);
// "@name" that is not the middle of an email address: a social handle, which
// a local post must not carry.
std::optional<std::string> scan_for_handle(std::u32string_view folded);

}  // namespace poni::domain
