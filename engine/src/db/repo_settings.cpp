#include "db/repo_settings.hpp"

#include "core/etag.hpp"
#include "core/json.hpp"
#include "core/text.hpp"
#include "db/database.hpp"
#include "db/sql.hpp"
#include "domain/prompts.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value_from.hpp>

#include <algorithm>
#include <format>
#include <stdexcept>

namespace poni::repo {

using db::iso;
using db::to_timestamptz;

namespace {

boost::json::value json_column(const pqxx::field_ref& field) { return boost::json::parse(field.as<std::string>()); }

// The three parts as one JSON object, for an audit entry.
constexpr std::string_view kPartsAsJson = "jsonb_build_object('voice', voice, 'localization', localization, 'policy', policy)::text";

constexpr std::string_view kNoRow = "the location has no reply settings";

}  // namespace

// ---- reply settings ------------------------------------------------------------

std::expected<domain::ResolvedSettings, std::string> stored_reply_settings(pqxx::transaction_base& tx, std::int64_t location_id) {
    const auto rows =
        tx.exec("SELECT voice::text, localization::text, policy::text FROM reply_settings WHERE location_id = $1", pqxx::params{location_id});
    if (rows.empty()) return std::unexpected(std::string(kNoRow));
    return domain::read_settings(json_column(rows[0][0]), json_column(rows[0][1]), json_column(rows[0][2]));
}

std::string unreadable_reply_settings(std::string_view why) {
    return std::format("The location's reply settings can't be read: {}.", why);
}

Result<model::ReplySettings> reply_settings(pqxx::transaction_base& tx, std::int64_t location_id) {
    // Whose settings they are; the three parts are read and checked by the one reader.
    auto answer = db::row_as<model::ReplySettings>(
        tx,
        std::format("SELECT l.id AS location_id, l.title AS location_title, l.client_id, c.name AS client_name, l.industry_key, "
                    "{} AS updated_at FROM locations l LEFT JOIN clients c ON c.id = l.client_id "
                    "JOIN reply_settings r ON r.location_id = l.id WHERE l.id = $1",
                    iso("r.updated_at")),
        pqxx::params{location_id}, db::FilledLater{"voice", "localization", "policy"});
    if (!answer) {
        // No such location, or one without a row.
        if (tx.exec("SELECT 1 FROM locations WHERE id = $1", pqxx::params{location_id}).empty()) return not_found("location");
        return conflict("reply_settings_incomplete", unreadable_reply_settings(kNoRow));
    }
    auto settings = stored_reply_settings(tx, location_id);
    if (!settings) return conflict("reply_settings_incomplete", unreadable_reply_settings(settings.error()));
    answer->voice = std::move(settings->voice);
    answer->localization = std::move(settings->localization);
    answer->policy = settings->policy;
    return std::move(*answer);
}

Result<model::ReplySettings> replace_reply_settings(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t location_id,
                                                    const domain::ResolvedSettings& settings) {
    if (tx.exec("SELECT 1 FROM locations WHERE id = $1 FOR UPDATE", pqxx::params{location_id}).empty()) return not_found("location");
    if (auto problem = domain::check_settings(settings)) return invalid(std::move(*problem));

    const auto after = boost::json::value_from(settings);
    const pqxx::params parts{location_id, boost::json::serialize(after.at("voice")), boost::json::serialize(after.at("localization")),
                             boost::json::serialize(after.at("policy"))};
    // Compared as values, in the database: the same settings written another
    // way are no change, and a member the code no longer knows is one, so a save removes it.
    const auto stored = tx.exec(std::format("SELECT voice = $2::jsonb AND localization = $3::jsonb AND policy = $4::jsonb, {} "
                                            "FROM reply_settings WHERE location_id = $1",
                                            kPartsAsJson),
                                parts);
    if (!stored.empty() && stored[0][0].as<bool>()) {
        audit.nothing_changed();
        return reply_settings(tx, location_id);
    }
    auto written = parts;
    written.append(to_timestamptz(audit.now()));
    tx.exec("INSERT INTO reply_settings (location_id, voice, localization, policy, created_at, updated_at) "
            "VALUES ($1, $2::jsonb, $3::jsonb, $4::jsonb, $5::timestamptz, $5::timestamptz) "
            "ON CONFLICT (location_id) DO UPDATE SET voice = EXCLUDED.voice, localization = EXCLUDED.localization, "
            "policy = EXCLUDED.policy, updated_at = EXCLUDED.updated_at",
            written)
        .no_rows();
    audit.record("reply_settings.update", "location", location_id,
                 {.before = stored.empty() ? boost::json::value(nullptr) : json_column(stored[0][1]), .after = after});
    return reply_settings(tx, location_id);
}

void write_default_reply_settings(pqxx::transaction_base& tx, std::int64_t location_id, core::TimePoint now) {
    const auto written = tx.exec("INSERT INTO reply_settings (location_id, voice, localization, policy, created_at, updated_at) "
                                 "SELECT $1, voice, localization, policy, $2::timestamptz, $2::timestamptz FROM kb_defaults",
                                 pqxx::params{location_id, to_timestamptz(now)});
    // Never a location without settings.
    if (written.affected_rows() != 1) throw std::runtime_error("no knowledge base is seeded: there are no default reply settings to copy");
}

// ---- pipeline settings ---------------------------------------------------------

boost::json::object extras_for(const model::AiSettings& settings, std::string_view provider) {
    const auto* all = settings.request_extras.if_object();
    const auto* own = all ? all->if_contains(provider) : nullptr;
    return own && own->is_object() ? own->get_object() : boost::json::object{};
}

model::AiSettings get_ai_settings(pqxx::transaction_base& tx) {
    const auto row = tx.exec(std::format(
                                 "SELECT enabled, analyzer_model, generator_model, analyzer_temperature, analyzer_send_temperature, "
                                 "max_output_tokens, request_extras::text, batch_size, {}, "
                                 "analyzer_provider, generator_provider, generator_send_temperature, judge_provider, judge_model, "
                                 "summary_provider, summary_model, posts_provider, posts_model "
                                 "FROM ai_settings",
                                 iso("updated_at")))
                         .one_row();
    return {.enabled = row[0].as<bool>(),
            .analyzer_provider = row[9].as<std::string>(),
            .analyzer_model = row[1].as<std::string>(),
            .generator_provider = row[10].as<std::string>(),
            .generator_model = row[2].as<std::string>(),
            .judge_provider = row[12].as<std::string>(),
            .judge_model = row[13].as<std::string>(),
            .summary_provider = row[14].as<std::string>(),
            .summary_model = row[15].as<std::string>(),
            .posts_provider = row[16].as<std::string>(),
            .posts_model = row[17].as<std::string>(),
            .analyzer_temperature = row[3].as<double>(),
            .analyzer_send_temperature = row[4].as<bool>(),
            .generator_send_temperature = row[11].as<bool>(),
            .max_output_tokens = row[5].as<std::int64_t>(),
            .request_extras = json_column(row[6]),
            .batch_size = row[7].as<std::int64_t>(),
            .prompt_version = std::string(domain::prompt_version()),
            .providers = {},
            .updated_at = row[8].as<std::string>()};
}

boost::json::object editable(const model::AiSettings& settings) {
    return {{"enabled", settings.enabled},
            {"analyzer_model", settings.analyzer_model},
            {"generator_model", settings.generator_model},
            {"analyzer_provider", settings.analyzer_provider},
            {"generator_provider", settings.generator_provider},
            {"judge_provider", settings.judge_provider},
            {"judge_model", settings.judge_model},
            {"summary_provider", settings.summary_provider},
            {"summary_model", settings.summary_model},
            {"posts_provider", settings.posts_provider},
            {"posts_model", settings.posts_model},
            {"analyzer_temperature", settings.analyzer_temperature},
            {"analyzer_send_temperature", settings.analyzer_send_temperature},
            {"generator_send_temperature", settings.generator_send_temperature},
            {"max_output_tokens", settings.max_output_tokens},
            {"request_extras", settings.request_extras},
            {"batch_size", settings.batch_size}};
}

Result<std::string> current_ai_settings_etag(pqxx::transaction_base& tx) {
    tx.exec("SELECT 1 FROM ai_settings FOR UPDATE").one_row();
    return core::etag(editable(get_ai_settings(tx)));
}

Result<model::AiSettings> update_ai_settings(pqxx::transaction_base& tx, audit::Writer& audit, const AiSettingsPatch& patch,
                                             const std::vector<std::string>& configured_providers) {
    tx.exec("SELECT 1 FROM ai_settings FOR UPDATE").one_row();
    const auto before = get_ai_settings(tx);
    auto after = before;
    if (patch.enabled) after.enabled = *patch.enabled;
    if (patch.analyzer_provider) after.analyzer_provider = *patch.analyzer_provider;
    if (patch.generator_provider) after.generator_provider = *patch.generator_provider;
    if (patch.analyzer_model) after.analyzer_model = std::string(text::trim(*patch.analyzer_model));
    if (patch.generator_model) after.generator_model = std::string(text::trim(*patch.generator_model));
    if (patch.judge_provider) after.judge_provider = *patch.judge_provider;
    if (patch.judge_model) after.judge_model = std::string(text::trim(*patch.judge_model));
    if (patch.summary_provider) after.summary_provider = *patch.summary_provider;
    if (patch.summary_model) after.summary_model = std::string(text::trim(*patch.summary_model));
    if (patch.posts_provider) after.posts_provider = *patch.posts_provider;
    if (patch.posts_model) after.posts_model = std::string(text::trim(*patch.posts_model));
    if (patch.analyzer_temperature) after.analyzer_temperature = *patch.analyzer_temperature;
    if (patch.analyzer_send_temperature) after.analyzer_send_temperature = *patch.analyzer_send_temperature;
    if (patch.generator_send_temperature) after.generator_send_temperature = *patch.generator_send_temperature;
    if (patch.max_output_tokens) after.max_output_tokens = *patch.max_output_tokens;
    if (patch.request_extras) after.request_extras = *patch.request_extras;
    if (patch.batch_size) after.batch_size = *patch.batch_size;

    const auto model_ok = [](const std::string& name) { return !name.empty() && name.size() <= 100; };
    if (!model_ok(after.analyzer_model) || !model_ok(after.generator_model) || !model_ok(after.judge_model) ||
        !model_ok(after.summary_model) || !model_ok(after.posts_model)) {
        return invalid("a model name must have 1 to 100 characters");
    }
    // A provider can only be chosen while it has an address and a key; one that was chosen earlier may stay.
    for (const auto& [name, old] : {std::pair<const std::string&, const std::string&>{after.analyzer_provider, before.analyzer_provider},
                                    {after.generator_provider, before.generator_provider},
                                    {after.judge_provider, before.judge_provider},
                                    {after.summary_provider, before.summary_provider},
                                    {after.posts_provider, before.posts_provider}}) {
        if (name != old && !std::ranges::contains(configured_providers, name)) {
            return invalid(std::format("'{}' is not a configured model provider", name));
        }
    }
    if (after.request_extras.is_object()) {
        for (const auto& member : after.request_extras.get_object()) {
            if (!member.value().is_object()) return invalid("'request_extras' holds one object per provider name");
        }
    }
    if (!(after.analyzer_temperature >= 0 && after.analyzer_temperature <= 2)) return invalid("a temperature must be between 0 and 2");
    if (after.max_output_tokens < 100 || after.max_output_tokens > 8000) return invalid("'max_output_tokens' must be between 100 and 8000");
    if (after.batch_size < 0 || after.batch_size > 100) return invalid("'batch_size' must be between 0 and 100");
    if (boost::json::serialize(after.request_extras).size() > 2000) return invalid("'request_extras' is too large");

    const auto before_json = boost::json::value_from(before);
    if (boost::json::value_from(after) == before_json) {
        audit.nothing_changed();
        return before;
    }
    tx.exec("UPDATE ai_settings SET enabled = $1, analyzer_model = $2, generator_model = $3, analyzer_temperature = $4, "
            "analyzer_send_temperature = $5, max_output_tokens = $6, request_extras = $7::jsonb, batch_size = $8, "
            "updated_at = $9::timestamptz, analyzer_provider = $10, generator_provider = $11, "
            "generator_send_temperature = $12, judge_provider = $13, judge_model = $14, summary_provider = $15, summary_model = $16, "
            "posts_provider = $17, posts_model = $18",
            pqxx::params{after.enabled, after.analyzer_model, after.generator_model, after.analyzer_temperature,
                         after.analyzer_send_temperature, after.max_output_tokens, boost::json::serialize(after.request_extras),
                         after.batch_size, to_timestamptz(audit.now()), after.analyzer_provider, after.generator_provider,
                         after.generator_send_temperature, after.judge_provider, after.judge_model, after.summary_provider,
                         after.summary_model, after.posts_provider, after.posts_model})
        .no_rows();
    auto stored = get_ai_settings(tx);
    audit.record({.action = "ai_settings.update",
                  .object_type = "ai_settings",
                  .object_id = "1",
                  .before = before_json,
                  .after = boost::json::value_from(stored)});
    return stored;
}

}  // namespace poni::repo
