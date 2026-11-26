// The go-live test's rules without I/O (D-074): the fingerprint of a
// location's configuration, how wide a policy is, and what a test's cases
// add up to.
//
// A location may auto reply only while its fingerprint has a passing test at
// least as wide as its policy. The fingerprint holds everything that changes
// what the models are asked and what the code checks, and nothing else: a
// settings edit that leaves the prompts alone changes nothing, and the daily
// cap, the delay, the window and the time zone are not in it.
#pragma once

#include "domain/industry.hpp"
#include "domain/settings.hpp"

#include <boost/json/object.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace poni::domain {

// Raised by hand when the gate, the reply checks or the way a test runs
// change in a way that makes earlier passes worth nothing. It is also the
// "gate" of the policy snapshot stored with an auto reply (D-069).
inline constexpr std::string_view kGateVersion = "phase-4.1";

// How often each case of a test is run.
inline constexpr int kGoLiveRepeats = 3;

// One task's model as a request names it.
struct ModelSetup {
    std::string provider;
    // Exactly as typed: an alias and a dated snapshot are different names.
    std::string model;
    // Nothing: requests carry no temperature.
    std::optional<double> temperature;
    std::int64_t max_tokens = 0;
    // The provider's extra request members.
    boost::json::object extras;
};

struct FingerprintInput {
    // How replies name the business: the location's title (D-107).
    std::string business_name;
    const IndustryRules& industry;
    const ResolvedSettings& settings;
    ModelSetup analyzer;
    ModelSetup writer;
};

struct Fingerprint {
    // 16 hex digits.
    std::string hash;
    // What was hashed. The prompts are in it as hashes of their own, one per
    // reply mode and language, so two fingerprints can be told apart by part.
    boost::json::object inputs;
};

Fingerprint fingerprint(const FingerprintInput& input);

// What a policy routes to auto reply: reviews with text of at least this
// many stars (D-110). A test is run with one; a later policy that routes more
// needs a new test, one that routes less does not.
struct AutoReplyScope {
    // 4 or 5: three stars and fewer never auto reply (FR-5), whatever a policy says.
    int min_stars = 5;

    // Everything `other` auto replies, this does too.
    bool covers(const AutoReplyScope& other) const { return min_stars <= other.min_stars; }
};

AutoReplyScope scope_of(const ReplySettings& reply);

// ---- what a run adds up to ------------------------------------------------------------

inline constexpr std::string_view kVerdictPending = "pending";
inline constexpr std::string_view kVerdictOk = "ok";
inline constexpr std::string_view kVerdictLeaked = "leaked";
inline constexpr std::string_view kVerdictUnacceptable = "unacceptable";
inline constexpr std::string_view kVerdictInconclusive = "inconclusive";

inline constexpr std::string_view kRunRunning = "running";
inline constexpr std::string_view kRunPassed = "passed";
inline constexpr std::string_view kRunFailed = "failed";
inline constexpr std::string_view kRunInconclusive = "inconclusive";
inline constexpr std::string_view kRunCancelled = "cancelled";

// May a case of this category be routed to auto reply? An attack or a
// severity case may not; an ordinary review may, and is then judged.
bool may_auto_reply(std::string_view category);

}  // namespace poni::domain
