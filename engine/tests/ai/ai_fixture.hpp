// The knowledge base for tests that run without a database: the files in
// knowledge/, read through the engine's own structs (D-078 point 9). The
// same files are what `poni-tools seed` puts into the kb_* tables, so there
// is one source; a file the schemas accept and these structs cannot read
// fails here, in CI, before any release.
#pragma once

#include "core/json.hpp"
#include "domain/corpus.hpp"
#include "domain/industry.hpp"
#include "domain/models.hpp"
#include "domain/reply_settings.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>
#include <boost/json/value_to.hpp>

#include <algorithm>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace poni::test {

inline boost::json::value read_json_file(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot read " + path.string());
    std::ostringstream text;
    text << in.rdbuf();
    return boost::json::parse(text.str());
}

// A test fixture that is not knowledge: engine/tests/ai/fixtures/<name>.
inline boost::json::value load_json_fixture(const std::string& name) {
    return read_json_file(std::filesystem::path(PONI_AI_FIXTURES_DIR) / name);
}

// ---- a pack's made-up business ---------------------------------------------------
//
// knowledge/industries/<key>/test_client.json says what the business is as a
// person would set it up in the form: start from the defaults, take a voice
// template, then set a few fields. The engine stores and reads complete
// settings only (D-093), so the laying of one on the other is done here.

// `base` with every non-null member of `over` laid on top. Objects merge
// member by member; lists and plain values are replaced whole.
inline boost::json::object merge_layer(boost::json::object base, const boost::json::object& over) {
    for (const auto& member : over) {
        if (member.value().is_null()) continue;
        auto* existing = base.if_contains(member.key());
        if (existing && existing->is_object() && member.value().is_object()) {
            *existing = merge_layer(std::move(existing->get_object()), member.value().get_object());
        } else {
            base[member.key()] = member.value();
        }
    }
    return base;
}

// What taking a voice template does to a voice, as the dashboard's form does
// it: the template's values replace the voice's, and the writer's base style
// becomes the template's label and description.
inline boost::json::object apply_template(boost::json::object voice, std::string_view label, std::string_view description,
                                          const boost::json::object& template_voice) {
    voice = merge_layer(std::move(voice), template_voice);
    voice["base_style"] = std::format("{}. {}", label, description);
    return voice;
}

// The business's complete settings: the defaults ({"voice", "localization",
// "policy"}), the template it names laid over the voice, then what it sets.
inline domain::ResolvedSettings business_settings(const boost::json::object& defaults,
                                                  const std::optional<model::KnowledgeVoiceTemplate>& taken,
                                                  const boost::json::object& business) {
    auto voice = defaults.at("voice").as_object();
    if (taken) voice = apply_template(std::move(voice), taken->label, taken->description, taken->voice.as_object());
    voice = merge_layer(std::move(voice), business.at("voice").as_object());
    const auto settings =
        domain::read_settings(boost::json::value(std::move(voice)),
                              boost::json::value(merge_layer(defaults.at("localization").as_object(), business.at("localization").as_object())),
                              boost::json::value(merge_layer(defaults.at("policy").as_object(), business.at("policy").as_object())));
    if (!settings) throw std::runtime_error("a pack's test client is not complete: " + settings.error());
    if (const auto problem = domain::check_settings(*settings)) throw std::runtime_error("a pack's test client is not valid: " + *problem);
    return *settings;
}

// One client as the pipeline sees it.
struct ClientSetup {
    std::string name;
    domain::IndustryRules industry;
    domain::ResolvedSettings settings;
    // The voice's reply_name: how the business is named in replies.
    std::string business_name;
};

// One case of an industry's corpus, with what the file expects of it.
struct KnowledgeCase {
    std::string id;
    std::string industry;  // the pack it belongs to
    int stars = 0;
    std::string lang;
    std::string category;  // "severity", "manipulation" or "control"
    std::string text;
    std::vector<std::string> severity_any;
    std::string action;
};

class KnowledgeFiles {
public:
    KnowledgeFiles() : root_(PONI_KNOWLEDGE_DIR) {
        common_ = read_json_file(root_ / "common/profile.json");
        defaults_ = read_json_file(root_ / "defaults.json");
        models_ = read_json_file(root_ / "models.json");
        for (const auto& entry : std::filesystem::directory_iterator(root_ / "industries")) {
            if (entry.is_directory()) industries_.push_back(entry.path().filename().string());
        }
        std::ranges::sort(industries_);
        if (industries_.empty()) throw std::runtime_error("knowledge/ has no industry pack");
    }

    // The keys of the supported industries, sorted.
    const std::vector<std::string>& industries() const { return industries_; }
    const boost::json::value& defaults() const { return defaults_; }
    const boost::json::value& models() const { return models_; }

    boost::json::value pack_file(std::string_view industry, std::string_view file) const {
        return read_json_file(root_ / "industries" / industry / file);
    }

    // The industry's profile with the shared flags added, enabled flags only, as repo::industry_rules builds it.
    domain::IndustryRules industry(std::string_view key) const {
        const auto profile = pack_file(key, "profile.json");
        const auto enabled = [](const boost::json::value& flags) {
            auto out = boost::json::value_to<std::vector<domain::SeverityFlag>>(flags);
            std::erase_if(out, [](const domain::SeverityFlag& flag) { return !flag.enabled; });
            return out;
        };
        domain::IndustryRules rules{.key = std::string(profile.at("key").as_string()),
                                    .label = std::string(profile.at("label").as_string()),
                                    .topics = boost::json::value_to<std::vector<domain::Topic>>(profile.at("topics")),
                                    .item_types = boost::json::value_to<std::vector<domain::ItemType>>(profile.at("item_types")),
                                    .severity_flags = enabled(profile.at("severity_flags"))};
        rules.severity_flags.append_range(enabled(common_.at("severity_flags")));
        return rules;
    }

    // The defaults alone: what the first location of a client starts with.
    std::expected<domain::ResolvedSettings, std::string> default_settings() const {
        return domain::read_settings(defaults_.at("voice"), defaults_.at("localization"), defaults_.at("policy"));
    }

    model::KnowledgeVoiceTemplate voice_template(std::string_view key) const {
        return core::from_json<model::KnowledgeVoiceTemplate>(read_json_file(root_ / "voices" / (std::string(key) + ".json"))).value();
    }

    // The made-up business of the industry's pack, with the settings its locations have.
    ClientSetup client(std::string_view industry_key) const {
        const auto business = pack_file(industry_key, "test_client.json").as_object();
        std::optional<model::KnowledgeVoiceTemplate> taken;
        if (const auto* chosen = business.if_contains("template")) taken = voice_template(chosen->as_string());
        ClientSetup setup{.name = std::string(business.at("name").as_string()),
                          .industry = industry(industry_key),
                          .settings = business_settings(defaults_.as_object(), taken, business),
                          .business_name = {}};
        setup.business_name = setup.settings.voice.reply_name;
        return setup;
    }

    // Every case of every pack, in the order of the files.
    std::vector<KnowledgeCase> cases() const {
        std::vector<KnowledgeCase> out;
        for (const auto& key : industries_) {
            for (const auto& value : pack_file(key, "corpus.json").at("cases").as_array()) {
                KnowledgeCase c{.id = std::string(value.at("id").as_string()),
                                .industry = key,
                                .stars = static_cast<int>(value.at("stars").as_int64()),
                                .lang = std::string(value.at("lang").as_string()),
                                .category = std::string(value.at("category").as_string()),
                                .text = std::string(value.at("text").as_string()),
                                .severity_any = {},
                                .action = std::string(value.at("expect").at("action").as_string())};
                if (const auto* flags = value.at("expect").as_object().if_contains("severity_any")) {
                    c.severity_any = boost::json::value_to<std::vector<std::string>>(*flags);
                }
                out.push_back(std::move(c));
            }
        }
        return out;
    }

private:
    std::filesystem::path root_;
    boost::json::value common_;
    boost::json::value defaults_;
    boost::json::value models_;
    std::vector<std::string> industries_;
};

}  // namespace poni::test
