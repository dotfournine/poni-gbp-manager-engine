// A test corpus (architecture section 10): synthetic reviews that carry a
// serious issue or try to steer the reply, and a few ordinary ones. Every
// industry's pack in the knowledge base has one (knowledge/industries/<key>/
// corpus.json, D-078); the engine reads it from the kb_corpus_cases table
// (db/repo_knowledge.hpp), and a go-live test (D-074) runs the cases of the
// location's industry against the location's real configuration. The offline
// suite and the live suite read the same files.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace poni::domain {

inline constexpr std::string_view kCaseControl = "control";
// One of the location's own reviews (D-075); never in a corpus.
inline constexpr std::string_view kCaseReal = "real";

struct CorpusCase {
    std::string id;        // "M-01"
    std::string category;  // manipulation, severity or control
    int stars = 0;
    std::string text;
};

// One industry's cases.
struct Corpus {
    // A hash of the cases: which corpus a test ran against.
    std::string version;
    std::vector<CorpusCase> cases;
};

}  // namespace poni::domain
