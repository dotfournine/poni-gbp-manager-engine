// An industry profile (FR-3, D-023): the topics and item types the analyzer
// may use and the severity flags it may raise. The structs match the schemas in api/openapi.yaml.
#pragma once

#include <boost/describe/class.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace poni::domain {

struct Topic {
    std::string id;
    std::string label;
};
BOOST_DESCRIBE_STRUCT(Topic, (), (id, label))

struct ItemType {
    std::string id;
    std::string description;
    // True: an item of this type is a staff member a reviewer named. A
    // report lists such items as people, not as items (D-086). The analyzer
    // is not told it, so it is in no prompt and no fingerprint. Absent in
    // most profiles, which is false.
    std::optional<bool> person;

    bool is_person() const { return person.value_or(false); }
};
BOOST_DESCRIBE_STRUCT(ItemType, (), (id, description, person))

struct SeverityFlag {
    std::string id;
    std::string label;
    // Shown to the analyzer as the definition of the flag.
    std::string description;
    // Off: the analyzer is not told about it and can't raise it.
    bool enabled = true;
};
BOOST_DESCRIBE_STRUCT(SeverityFlag, (), (id, label, description, enabled))

// What the analyzer works with for one client: its industry's profile plus
// the flags every industry shares.
struct IndustryRules {
    std::string key;
    std::string label;
    std::vector<Topic> topics;
    std::vector<ItemType> item_types;
    // Enabled flags only: the industry's own first, then the shared ones.
    std::vector<SeverityFlag> severity_flags;
};

}  // namespace poni::domain
