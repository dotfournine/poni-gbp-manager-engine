// The knowledge base as the engine reads it (FR-13, D-078): industry
// profiles, the test corpus, voice templates, defaults and model prices, in
// the kb_* tables that `poni-tools seed` fills from the files in knowledge/.
//
// The tables are static. Nothing here writes them, and nothing in the
// engine may: an update is a new set of files, seeded again, which replaces
// all of the tables in one transaction. So every function reads what it
// needs in one statement, and the engine reads on each use and keeps
// nothing in memory: a new version is in effect with the next read, with no
// restart and nothing that watches for it (D-084).
//
// Blocking calls on a transaction the caller owns: run them through Database::run.
#pragma once

#include "domain/corpus.hpp"
#include "domain/industry.hpp"
#include "domain/models.hpp"

#include <boost/json/object.hpp>
#include <pqxx/pqxx>

#include <optional>
#include <string>
#include <vector>

namespace poni::repo {

// The shape of the knowledge files this engine reads (knowledge/manifest.json).
inline constexpr int kKnowledgeFormat = 2;

struct KnowledgeMeta {
    std::string version;
    int format = 0;
    std::string seeded_at;
};

// Which knowledge base is seeded; nothing if none is.
std::optional<KnowledgeMeta> knowledge_meta(pqxx::transaction_base& tx);

// Why the engine cannot work with this database's knowledge base, as a
// sentence; nothing when it can. Checked once at start: the engine refuses
// to serve without a seeded knowledge base or with a format it doesn't know.
std::optional<std::string> knowledge_problem(pqxx::transaction_base& tx);

// ---- industries ------------------------------------------------------------------------

// The profile `key` with the flags every industry shares added, enabled
// flags only: what a review of that industry is analyzed against. Nothing
// if the knowledge base has no such industry: its locations are not analyzed.
std::optional<domain::IndustryRules> industry_rules(pqxx::transaction_base& tx, const std::string& key);

// Is this an industry a location can be given?
bool industry_supported(pqxx::transaction_base& tx, const std::string& key);

// The industry's test corpus: what a go-live test runs besides the location's own reviews.
std::optional<domain::Corpus> industry_corpus(pqxx::transaction_base& tx, const std::string& key);

// One case by its id, as a running test reads it; nothing if a reseed took it away.
std::optional<domain::CorpusCase> corpus_case(pqxx::transaction_base& tx, const std::string& industry_key, const std::string& case_id);

// ---- defaults and prices ---------------------------------------------------------------

// What every location starts with when it is discovered (its content and reply settings, D-102, D-107),
// and the default of every analysis setting, each as one JSON object. Empty objects when nothing is seeded.
struct KnowledgeDefaults {
    boost::json::object content;
    boost::json::object reply;
    boost::json::object analysis;
};

KnowledgeDefaults knowledge_defaults(pqxx::transaction_base& tx);

// USD per million tokens; nothing for a model without a price.
std::optional<model::ModelPrice> model_price(pqxx::transaction_base& tx, const std::string& model);

// ---- for the dashboard ------------------------------------------------------------------

// The whole knowledge base, read-only, without the corpus texts.
model::Knowledge knowledge(pqxx::transaction_base& tx);

// One industry's corpus cases; nothing if there is no such industry.
std::optional<model::KnowledgeCorpus> knowledge_corpus(pqxx::transaction_base& tx, const std::string& industry_key);

}  // namespace poni::repo
