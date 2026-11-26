#include "db/repo_knowledge.hpp"

#include "db/sql.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value_from.hpp>
#include <boost/json/value_to.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <map>
#include <string_view>

namespace poni::repo {

using db::iso;

namespace {

boost::json::value json_column(const pqxx::field_ref& field) { return boost::json::parse(field.as<std::string>()); }

boost::json::object object_column(const pqxx::field_ref& field) {
    auto value = json_column(field);
    return value.is_object() ? std::move(value.get_object()) : boost::json::object{};
}

template <class T>
T typed_column(const pqxx::field_ref& field) {
    return boost::json::value_to<T>(json_column(field));
}

std::vector<std::string> expect_flags(const boost::json::object& expect) {
    const auto* flags = expect.if_contains("severity_any");
    return flags ? boost::json::value_to<std::vector<std::string>>(*flags) : std::vector<std::string>{};
}

}  // namespace

std::optional<KnowledgeMeta> knowledge_meta(pqxx::transaction_base& tx) {
    const auto rows = tx.exec(std::format("SELECT version, format, {} FROM kb_meta", iso("seeded_at")));
    if (rows.empty()) return std::nullopt;
    return KnowledgeMeta{.version = rows[0][0].as<std::string>(), .format = rows[0][1].as<int>(), .seeded_at = rows[0][2].as<std::string>()};
}

std::optional<std::string> knowledge_problem(pqxx::transaction_base& tx) {
    const auto meta = knowledge_meta(tx);
    if (!meta) return "the database has no knowledge base; run `poni-tools seed` first";
    if (meta->format != kKnowledgeFormat) {
        return std::format("the seeded knowledge base has format {}, and this engine reads format {}; seed the knowledge base "
                           "that belongs to this release, or deploy the engine that reads this one",
                           meta->format, kKnowledgeFormat);
    }
    // A migration that rebuilds a defaults table leaves it empty for the next seed (a migration writes no kb_* row):
    // without its row, no location could be discovered.
    for (const auto table : {std::string_view("kb_content_defaults"), std::string_view("kb_reply_defaults")}) {
        if (tx.query_value<std::int64_t>(std::format("SELECT count(*) FROM {}", table)) != 1) {
            return std::format("the knowledge base has no row in {}; run `poni-tools seed` to fill it", table);
        }
    }
    return std::nullopt;
}

// ---- industries ------------------------------------------------------------------------

std::optional<domain::IndustryRules> industry_rules(pqxx::transaction_base& tx, const std::string& key) {
    // One statement, so a seed that commits meanwhile cannot give the profile of one version and the shared flags of another.
    const auto rows = tx.exec("SELECT i.key, i.label, i.topics::text, i.item_types::text, i.severity_flags::text, "
                              "coalesce((SELECT c.severity_flags FROM kb_common c), '[]'::jsonb)::text "
                              "FROM kb_industries i WHERE i.key = $1",
                              pqxx::params{key});
    if (rows.empty()) return std::nullopt;
    const auto& row = rows[0];
    const auto enabled = [](std::vector<domain::SeverityFlag> flags) {
        std::erase_if(flags, [](const domain::SeverityFlag& flag) { return !flag.enabled; });
        return flags;
    };
    domain::IndustryRules rules{.key = row[0].as<std::string>(),
                                .label = row[1].as<std::string>(),
                                .topics = typed_column<std::vector<domain::Topic>>(row[2]),
                                .item_types = typed_column<std::vector<domain::ItemType>>(row[3]),
                                .severity_flags = enabled(typed_column<std::vector<domain::SeverityFlag>>(row[4]))};
    // The industry's own flags first, then the shared ones (the check refuses a flag that is in both).
    for (auto& flag : enabled(typed_column<std::vector<domain::SeverityFlag>>(row[5]))) {
        if (std::ranges::find(rules.severity_flags, flag.id, &domain::SeverityFlag::id) == rules.severity_flags.end()) {
            rules.severity_flags.push_back(std::move(flag));
        }
    }
    return rules;
}

bool industry_supported(pqxx::transaction_base& tx, const std::string& key) {
    return !tx.exec("SELECT 1 FROM kb_industries WHERE key = $1", pqxx::params{key}).empty();
}

std::optional<domain::Corpus> industry_corpus(pqxx::transaction_base& tx, const std::string& key) {
    const auto rows = tx.exec("SELECT i.corpus_version, k.case_id, k.category, k.stars, k.text "
                              "FROM kb_industries i LEFT JOIN kb_corpus_cases k ON k.industry_key = i.key "
                              "WHERE i.key = $1 ORDER BY k.position",
                              pqxx::params{key});
    if (rows.empty()) return std::nullopt;
    domain::Corpus corpus{.version = rows[0][0].as<std::string>(), .cases = {}};
    for (const auto& row : rows) {
        if (row[1].is_null()) continue;
        corpus.cases.push_back({.id = row[1].as<std::string>(),
                                .category = row[2].as<std::string>(),
                                .stars = row[3].as<int>(),
                                .text = row[4].as<std::string>()});
    }
    return corpus;
}

std::optional<domain::CorpusCase> corpus_case(pqxx::transaction_base& tx, const std::string& industry_key, const std::string& case_id) {
    const auto rows = tx.exec("SELECT case_id, category, stars, text FROM kb_corpus_cases WHERE industry_key = $1 AND case_id = $2",
                              pqxx::params{industry_key, case_id});
    if (rows.empty()) return std::nullopt;
    return domain::CorpusCase{.id = rows[0][0].as<std::string>(),
                              .category = rows[0][1].as<std::string>(),
                              .stars = rows[0][2].as<int>(),
                              .text = rows[0][3].as<std::string>()};
}

// ---- defaults and prices ---------------------------------------------------------------

KnowledgeDefaults knowledge_defaults(pqxx::transaction_base& tx) {
    // One statement, so a seed that commits meanwhile cannot give one version's content defaults and another's reply defaults.
    const auto rows = tx.exec("SELECT (SELECT (to_jsonb(c) - 'id')::text FROM kb_content_defaults c), "
                              "(SELECT (to_jsonb(r) - 'id')::text FROM kb_reply_defaults r), "
                              "(SELECT analysis::text FROM kb_defaults)");
    const auto object_or_empty = [](const pqxx::field_ref& field) {
        return field.is_null() ? boost::json::object{} : object_column(field);
    };
    return {.content = object_or_empty(rows[0][0]), .reply = object_or_empty(rows[0][1]), .analysis = object_or_empty(rows[0][2])};
}

std::optional<model::ModelPrice> model_price(pqxx::transaction_base& tx, const std::string& model) {
    const auto rows = tx.exec("SELECT input_per_mtok::float8, output_per_mtok::float8 FROM kb_model_prices WHERE model = $1",
                              pqxx::params{model});
    if (rows.empty()) return std::nullopt;
    return model::ModelPrice{.input_per_mtok = rows[0][0].as<double>(), .output_per_mtok = rows[0][1].as<double>()};
}

// ---- for the dashboard ------------------------------------------------------------------

model::Knowledge knowledge(pqxx::transaction_base& tx) {
    model::Knowledge out;
    if (const auto meta = knowledge_meta(tx)) {
        out.version = meta->version;
        out.format = meta->format;
        out.seeded_at = meta->seeded_at;
    }
    out.industries = db::rows_as<model::KnowledgeIndustry>(tx, R"sql(
        SELECT i.key, i.label, i.topics, i.item_types, i.severity_flags, i.test_client ->> 'name' AS test_client_name,
               jsonb_build_object(
                   'version', i.corpus_version,
                   'severity_cases', (SELECT count(*) FROM kb_corpus_cases k WHERE k.industry_key = i.key AND k.category = 'severity'),
                   'manipulation_cases', (SELECT count(*) FROM kb_corpus_cases k WHERE k.industry_key = i.key AND k.category = 'manipulation'),
                   'control_cases', (SELECT count(*) FROM kb_corpus_cases k WHERE k.industry_key = i.key AND k.category = 'control')) AS corpus,
               (SELECT count(*) FROM locations l WHERE l.industry_key = i.key AND l.enabled) AS location_count
          FROM kb_industries i ORDER BY i.position, i.key)sql");
    const auto common = tx.exec("SELECT severity_flags::text FROM kb_common");
    if (!common.empty()) out.common_severity_flags = typed_column<std::vector<domain::SeverityFlag>>(common[0][0]);
    out.voice_templates = db::rows_as<model::KnowledgeVoiceTemplate>(
        tx, "SELECT key, label, description, content, reply FROM kb_voice_templates ORDER BY position, key");
    auto defaults = knowledge_defaults(tx);
    out.defaults = {.content = std::move(defaults.content), .reply = std::move(defaults.reply), .analysis = std::move(defaults.analysis)};
    for (const auto& row : tx.exec("SELECT model, input_per_mtok::float8, output_per_mtok::float8 FROM kb_model_prices ORDER BY model")) {
        out.model_prices.emplace(row[0].as<std::string>(),
                                 model::ModelPrice{.input_per_mtok = row[1].as<double>(), .output_per_mtok = row[2].as<double>()});
    }
    return out;
}

std::optional<model::KnowledgeCorpus> knowledge_corpus(pqxx::transaction_base& tx, const std::string& industry_key) {
    const auto rows = tx.exec("SELECT i.corpus_version, k.case_id, k.category, k.stars, k.lang, k.text, "
                              "k.expect::text FROM kb_industries i LEFT JOIN kb_corpus_cases k ON k.industry_key = i.key "
                              "WHERE i.key = $1 ORDER BY k.position",
                              pqxx::params{industry_key});
    if (rows.empty()) return std::nullopt;
    model::KnowledgeCorpus out{.industry_key = industry_key,
                               .version = rows[0][0].as<std::string>(),
                               .cases = {}};
    for (const auto& row : rows) {
        if (row[1].is_null()) continue;
        const auto expect = object_column(row[6]);
        const auto* manipulation = expect.if_contains("manipulation");
        const auto* action = expect.if_contains("action");
        out.cases.push_back({.id = row[1].as<std::string>(),
                             .category = row[2].as<std::string>(),
                             .stars = row[3].as<std::int64_t>(),
                             .lang = row[4].as<std::string>(),
                             .text = row[5].as<std::string>(),
                             .severity_any = expect_flags(expect),
                             .manipulation = manipulation && manipulation->is_bool() && manipulation->get_bool(),
                             .action = action && action->is_string() ? std::string(action->get_string()) : std::string{}});
    }
    return out;
}

}  // namespace poni::repo
