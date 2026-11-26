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
#include <ranges>
#include <tuple>
#include <format>
#include <stdexcept>

namespace poni::repo {

using db::iso;
using db::to_timestamptz;

namespace {

boost::json::value json_column(const pqxx::field_ref& field) { return boost::json::parse(field.as<std::string>()); }

// A list column written from one JSON parameter, in the list's order.
std::string text_array(std::string_view parameter) {
    return std::format("ARRAY(SELECT e FROM jsonb_array_elements_text({}::jsonb) WITH ORDINALITY AS t(e, ord) ORDER BY ord)", parameter);
}

// Each bundle's columns, in member order: what a row is written with and the defaults are copied from.
constexpr std::string_view kContentColumns =
    "business_facts, base_style, emoji_allowed, emoji_max, emoji_preferred, words_to_avoid, style_notes, chinese_variant, "
    "english_variant, audience";
constexpr std::string_view kReplyColumns =
    "reply_languages, fallback_language, signature, sign_off_name, length, example_replies, promos_allowed, contact_allowed, "
    "temperature, auto_reply_positive, auto_reply_rating_only, auto_reply_min_stars, auto_reply_delay_min_minutes, "
    "auto_reply_delay_max_minutes, auto_reply_window_start, auto_reply_window_end";

struct Bundle {
    std::string_view table;
    std::string_view name;  // as a person reads it
};
constexpr Bundle kContent{"content_settings", "content settings"};
constexpr Bundle kReply{"reply_settings", "reply settings"};

// One bundle of the location as it is stored, read by column name; nothing if it has no row, and what is wrong
// if the row does not read as the bundle.
template <class T>
std::expected<std::optional<T>, std::string> read_bundle(pqxx::transaction_base& tx, const Bundle& bundle, std::int64_t location_id) {
    const auto rows =
        tx.exec(std::format("SELECT to_jsonb(s)::text FROM {} s WHERE location_id = $1", bundle.table), pqxx::params{location_id});
    if (rows.empty()) return std::optional<T>{};
    // Precise: a float8 must come out as the same double the column holds.
    boost::json::parse_options options;
    options.numbers = boost::json::number_precision::precise;
    auto parsed = core::from_json<T>(boost::json::parse(rows[0][0].view(), {}, options));
    if (!parsed) return std::unexpected(std::format("{}: {}", bundle.name, parsed.error()));
    return std::optional<T>(std::move(*parsed));
}

template <class T>
Result<T> bundle_for_api(pqxx::transaction_base& tx, const Bundle& bundle, std::int64_t location_id) {
    auto read = read_bundle<T>(tx, bundle, location_id);
    if (!read) return conflict("settings_incomplete", unreadable_settings(read.error()));
    if (!*read) return not_found("location");
    return std::move(**read);
}

bool lock_location(pqxx::transaction_base& tx, std::int64_t location_id) {
    return !tx.exec("SELECT 1 FROM locations WHERE id = $1 FOR UPDATE", pqxx::params{location_id}).empty();
}

template <class T>
Result<std::string> current_etag(pqxx::transaction_base& tx, const Bundle& bundle, std::int64_t location_id) {
    if (!lock_location(tx, location_id)) return not_found("location");
    auto stored = bundle_for_api<T>(tx, bundle, location_id);
    if (!stored) return std::unexpected(stored.error());
    return core::etag(editable(*stored));
}

// Writes the bundle's row: `values` are the members' SQL values in member order, from $2 on.
template <class T>
Result<T> replace_bundle(pqxx::transaction_base& tx, audit::Writer& audit, const Bundle& bundle, std::string_view columns,
                         std::string_view values, std::int64_t location_id, const T& settings, pqxx::params params) {
    auto stored = read_bundle<T>(tx, bundle, location_id);
    // An unreadable row is replaced: that is how a person repairs it.
    const bool unchanged = stored && *stored && editable(**stored) == editable(settings);
    if (unchanged) {
        audit.nothing_changed();
        return std::move(**stored);
    }
    const auto at = std::format("${}::timestamptz", params.size() + 1);
    params.append(to_timestamptz(audit.now()));
    std::string updates;
    for (const auto column : std::views::split(columns, std::string_view(", "))) {
        const std::string_view name(column.begin(), column.end());
        updates += std::format("{} = EXCLUDED.{}, ", name, name);
    }
    tx.exec(std::format("INSERT INTO {0} (location_id, {1}, created_at, updated_at) VALUES ($1, {2}, {3}, {3}) "
                        "ON CONFLICT (location_id) DO UPDATE SET {4}updated_at = EXCLUDED.updated_at",
                        bundle.table, columns, values, at, updates),
            params)
        .no_rows();
    const auto before = stored && *stored ? boost::json::value(editable(**stored)) : boost::json::value(nullptr);
    audit.record(std::format("{}.update", bundle.table), "location", location_id, {.before = before, .after = editable(settings)});
    auto written = read_bundle<T>(tx, bundle, location_id);
    if (!written || !*written) throw std::logic_error(std::format("the {} just written can't be read back", bundle.name));
    return std::move(**written);
}

}  // namespace

// ---- content and reply settings ------------------------------------------------

std::expected<domain::ResolvedSettings, std::string> stored_settings(pqxx::transaction_base& tx, std::int64_t location_id) {
    auto content = read_bundle<domain::ContentSettings>(tx, kContent, location_id);
    if (!content) return std::unexpected(content.error());
    if (!*content) return std::unexpected(std::string("the location has no content settings"));
    auto reply = read_bundle<domain::ReplySettings>(tx, kReply, location_id);
    if (!reply) return std::unexpected(reply.error());
    if (!*reply) return std::unexpected(std::string("the location has no reply settings"));
    return domain::ResolvedSettings{.content = std::move(**content), .reply = std::move(**reply)};
}

std::string unreadable_settings(std::string_view why) { return std::format("The location's settings can't be read: {}.", why); }

Result<domain::ContentSettings> content_settings(pqxx::transaction_base& tx, std::int64_t location_id) {
    return bundle_for_api<domain::ContentSettings>(tx, kContent, location_id);
}

Result<domain::ReplySettings> reply_settings(pqxx::transaction_base& tx, std::int64_t location_id) {
    return bundle_for_api<domain::ReplySettings>(tx, kReply, location_id);
}

boost::json::object editable(const domain::ContentSettings& settings) { return boost::json::value_from(settings).as_object(); }

boost::json::object editable(const domain::ReplySettings& settings) { return boost::json::value_from(settings).as_object(); }

Result<std::string> current_content_settings_etag(pqxx::transaction_base& tx, std::int64_t location_id) {
    return current_etag<domain::ContentSettings>(tx, kContent, location_id);
}

Result<std::string> current_reply_settings_etag(pqxx::transaction_base& tx, std::int64_t location_id) {
    return current_etag<domain::ReplySettings>(tx, kReply, location_id);
}

Result<domain::ContentSettings> replace_content_settings(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t location_id,
                                                         const domain::ContentSettings& body) {
    // The location's row is the lock, as auto_verdict's is: a save and a verdict don't interleave.
    if (!lock_location(tx, location_id)) return not_found("location");
    auto settings = body;
    settings.audience = std::string(text::trim(settings.audience));
    if (auto problem = domain::check_content_settings(settings)) return invalid(std::move(*problem));
    const auto values = std::format("$2, $3, $4, $5, {}, {}, {}, $9, $10, $11", text_array("$6"), text_array("$7"), text_array("$8"));
    return replace_bundle(tx, audit, kContent, kContentColumns, values, location_id, settings,
                          pqxx::params{location_id, settings.business_facts, settings.base_style, settings.emoji_allowed, settings.emoji_max,
                                       db::json_list(settings.emoji_preferred), db::json_list(settings.words_to_avoid),
                                       db::json_list(settings.style_notes), settings.chinese_variant, settings.english_variant,
                                       settings.audience});
}

Result<domain::ReplySettings> replace_reply_settings(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t location_id,
                                                     const domain::ReplySettings& settings) {
    if (!lock_location(tx, location_id)) return not_found("location");
    if (auto problem = domain::check_reply_settings(settings)) return invalid(std::move(*problem));
    const auto values = std::format("{}, $3, $4, $5, $6::jsonb, {}, $8, $9, $10, $11, $12, $13, $14, $15, $16, $17", text_array("$2"),
                                    text_array("$7"));
    return replace_bundle(tx, audit, kReply, kReplyColumns, values, location_id, settings,
                          pqxx::params{location_id, db::json_list(settings.reply_languages), settings.fallback_language, settings.signature,
                                       settings.sign_off_name, boost::json::serialize(boost::json::value_from(settings.length)),
                                       db::json_list(settings.example_replies), settings.promos_allowed, settings.contact_allowed,
                                       settings.temperature, settings.auto_reply_positive, settings.auto_reply_rating_only,
                                       settings.auto_reply_min_stars, settings.auto_reply_delay_min_minutes,
                                       settings.auto_reply_delay_max_minutes, settings.auto_reply_window_start,
                                       settings.auto_reply_window_end});
}

void write_default_settings(pqxx::transaction_base& tx, std::int64_t location_id, core::TimePoint now) {
    for (const auto& [bundle, columns, defaults] : {std::tuple{kContent, kContentColumns, std::string_view("kb_content_defaults")},
                                                    std::tuple{kReply, kReplyColumns, std::string_view("kb_reply_defaults")}}) {
        const auto written = tx.exec(std::format("INSERT INTO {0} (location_id, {1}, created_at, updated_at) "
                                                 "SELECT $1, {1}, $2::timestamptz, $2::timestamptz FROM {2}",
                                                 bundle.table, columns, defaults),
                                     pqxx::params{location_id, to_timestamptz(now)});
        // Never a location without settings.
        if (written.affected_rows() != 1) {
            throw std::runtime_error(std::format("no knowledge base is seeded: there are no default {} to copy", bundle.name));
        }
    }
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
