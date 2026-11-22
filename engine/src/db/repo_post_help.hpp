// What the help with local posts reads about one location (D-088 point 11):
// its reviews of the last 90 days that have a current analysis without a
// severity flag and without suspected manipulation, the posts its profile
// has, and the settings a model call needs. Reads only; nothing here writes.
//
// A reviewer's name or photo is not selected.
#pragma once

#include "domain/industry.hpp"
#include "domain/models.hpp"
#include "domain/post_help.hpp"
#include "domain/reply_settings.hpp"

#include <pqxx/pqxx>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "core/clock.hpp"

namespace poni::repo {

struct PostHelp {
    std::int64_t location_id = 0;
    std::string location_title;
    std::string gbp_location_id;
    // A person switched the location on (D-102): the help is for such a location only.
    bool enabled = false;
    // Access to the location is not lost, so Google's figures can be read.
    bool accessible = false;
    // The location's own: who the customers are, and its industry.
    std::string audience;
    std::optional<domain::IndustryRules> industry;
    // The location's reply settings, or why there are none to read.
    std::expected<domain::ResolvedSettings, std::string> reply_settings = std::unexpected(std::string("not read"));
    // The period, in UTC: the last domain::kPostHelpDays days up to today.
    domain::Day period_start;
    domain::Day period_end;
    // Reviews written in the period that Google still has.
    std::int64_t reviews = 0;
    // Of those, the ones the help may use, newest first.
    std::vector<domain::HelpReview> usable;
    // The newest posts the profile has or had, at most 10.
    std::vector<domain::PastPost> posts;
    model::AiSettings settings;
};

// Nothing: no such location.
std::optional<PostHelp> load_post_help(pqxx::transaction_base& tx, std::int64_t location_id, core::TimePoint now);

// The panel the dashboard shows, from the stored analysis alone.
model::ReviewHighlights review_highlights(const PostHelp& help);

}  // namespace poni::repo
