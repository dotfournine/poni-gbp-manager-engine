// What the analysis pipeline is configured with, as the dashboard reads and
// edits it: a location's reply settings (its voice, localization and policy,
// D-093) and the pipeline's own settings. What these start from (industry
// profiles, voice templates, the defaults, model prices) is the knowledge
// base, which nobody edits (db/repo_knowledge.hpp, D-078). Blocking calls on
// a transaction the caller owns: run them through Database::run.
#pragma once

#include "core/clock.hpp"
#include "db/audit.hpp"
#include "db/failure.hpp"
#include "domain/models.hpp"
#include "domain/reply_settings.hpp"

#include <boost/describe/class.hpp>
#include <boost/json/object.hpp>
#include <pqxx/pqxx>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace poni::repo {

// ---- reply settings (D-093) ---------------------------------------------------
//
// One row per location, complete: what the location's replies are written
// with is that row and nothing else. A row is written from the knowledge
// base's defaults when the location is discovered, enabled or not (D-102),
// and replaced whole by a person's save. Nothing copies settings between
// locations, and a location's client has nothing to do with them.

// The location's settings as they are stored. Nothing is merged in and
// nothing is filled in: the error says that there is no row, or which part
// lacks which field.
std::expected<domain::ResolvedSettings, std::string> stored_reply_settings(pqxx::transaction_base& tx, std::int64_t location_id);
// That error as a sentence for a person.
std::string unreadable_reply_settings(std::string_view why);

// The same for the API; settings that can't be read are `reply_settings_incomplete`, with the reason.
Result<model::ReplySettings> reply_settings(pqxx::transaction_base& tx, std::int64_t location_id);

// Replaces the location's settings with these, whole. Settings equal to the stored ones change nothing.
Result<model::ReplySettings> replace_reply_settings(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t location_id,
                                                    const domain::ResolvedSettings& settings);

// Part of discovering a location: it starts with the knowledge base's defaults, which the engine does not start without.
void write_default_reply_settings(pqxx::transaction_base& tx, std::int64_t location_id, core::TimePoint now);

// ---- pipeline settings ---------------------------------------------------------

// The body of PATCH /api/ai/settings: what is given is changed.
struct AiSettingsPatch {
    std::optional<bool> enabled;
    std::optional<std::string> analyzer_model;
    std::optional<std::string> generator_model;
    std::optional<std::string> analyzer_provider;
    std::optional<std::string> generator_provider;
    std::optional<std::string> judge_provider;
    std::optional<std::string> judge_model;
    std::optional<std::string> summary_provider;
    std::optional<std::string> summary_model;
    std::optional<std::string> posts_provider;
    std::optional<std::string> posts_model;
    std::optional<double> analyzer_temperature;
    std::optional<bool> analyzer_send_temperature;
    std::optional<bool> generator_send_temperature;
    std::optional<std::int64_t> max_output_tokens;
    std::optional<boost::json::object> request_extras;
    std::optional<std::int64_t> batch_size;
};
BOOST_DESCRIBE_STRUCT(AiSettingsPatch, (),
                      (enabled, analyzer_model, generator_model, analyzer_provider, generator_provider, judge_provider, judge_model,
                       summary_provider, summary_model, posts_provider, posts_model, analyzer_temperature, analyzer_send_temperature,
                       generator_send_temperature, max_output_tokens, request_extras, batch_size))

model::AiSettings get_ai_settings(pqxx::transaction_base& tx);
// The extra request members of one provider, as `settings` holds them ("enable_thinking" is DashScope's;
// another provider may refuse it). Empty if it has none.
boost::json::object extras_for(const model::AiSettings& settings, std::string_view provider);
// `configured_providers`: the providers that have an address and a key, which is what a provider may be changed to.
Result<model::AiSettings> update_ai_settings(pqxx::transaction_base& tx, audit::Writer& audit, const AiSettingsPatch& patch,
                                             const std::vector<std::string>& configured_providers);
// What PATCH /api/ai/settings can set, in AiSettingsPatch's order, with the stored values: what their ETag covers
// (D-108). Not `providers` (the engine's configuration), `prompt_version` or `updated_at`.
boost::json::object editable(const model::AiSettings& settings);
// Locks the row (FOR UPDATE, the lock update_ai_settings takes) and gives the ETag of what it holds.
Result<std::string> current_ai_settings_etag(pqxx::transaction_base& tx);

}  // namespace poni::repo
