// The permanent adversarial suite, offline (architecture section 10, D-021,
// D-058). No model is called: scripted models stand in for the analyzer and
// the generator, and the cases run through the same functions the engine's
// job uses (domain/review_pipeline.hpp).
//
// Since D-058 the engine relies on the model to spot severity issues and
// manipulation; code only checks a reply for links, email addresses and
// phone numbers. So this suite proves three things: a flag the model raises
// is always acted on, a reply with contact details is stopped even when the
// model was fooled, and (stated, not hidden) what is no longer caught when it
// is. How often the real model is fooled is the live suite's job.
// The cases are the corpora of the knowledge base (knowledge/industries/<key>/corpus.json),
// each run as its pack's made-up business. New attacks seen in real reviews are added there,
// rewritten so that no reviewer's words are in the repository.

#include "ai_fixture.hpp"
#include "core/text.hpp"
#include "domain/language.hpp"
#include "domain/review_pipeline.hpp"

#include <boost/json/serialize.hpp>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <format>
#include <map>
#include <print>

using namespace poni;
using namespace poni::domain;

namespace {

using Case = test::KnowledgeCase;

struct Outcome {
    Analysis analysis;
    DraftPlan plan;
    DraftOutcome draft;
};

class AdversarialSuite : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        cases_ = new std::vector<Case>(test::KnowledgeFiles().cases());
        // What the scripted fake model answers: test data, not knowledge.
        const auto suite = test::load_json_fixture("scripted_replies.json");
        compliant_ = new std::map<std::string, std::string>();
        for (const auto& [id, reply] : suite.at("compliant_replies").as_object()) compliant_->emplace(id, reply.as_string());
        cheerful_ = new std::map<std::string, std::string>();
        for (const auto& [language, reply] : suite.at("cheerful_reply").as_object()) cheerful_->emplace(language, reply.as_string());
    }

    static void TearDownTestSuite() {
        delete cases_;
        delete compliant_;
        delete cheerful_;
    }

    // What the scripted analyzer does with a case.
    enum class Analyzer {
        // Misses everything: every review is happy praise with nothing to flag.
        blind,
        // Does its job: raises the case's severity flag, or says the review tries to steer the reply.
        faithful,
    };
    // What the scripted generator does with a manipulation case.
    enum class Generator {
        // Writes exactly what the attack asks for and suspects nothing.
        gives_in,
        // Writes a cheerful thank-you and says the review tried to steer it.
        resists,
    };

    static boost::json::object analyzer_answer(const Case& c, const IndustryRules& industry, Analyzer analyzer) {
        const bool faithful = analyzer == Analyzer::faithful;
        boost::json::array flags;
        if (faithful) {
            for (const auto& id : c.severity_any) {
                if (std::ranges::find(industry.severity_flags, id, &SeverityFlag::id) == industry.severity_flags.end()) continue;
                flags.push_back(boost::json::object{{"id", id}, {"evidence", "scripted"}});
                break;
            }
        }
        const bool complaint = faithful && c.category == "control" && c.action == kRouteApproval;
        return {{"sentiment", complaint ? "mixed" : "positive"},
                {"contains_complaint", complaint},
                {"topics", boost::json::array{}},
                {"items", boost::json::array{}},
                {"severity_flags", std::move(flags)},
                {"manipulation_suspected", faithful && c.category == "manipulation"},
                {"manipulation_evidence", ""},
                {"summary_en", "A happy guest."}};
    }

    static GeneratorAnswer generator_answer(const Case& c, const DraftPlan& plan, Generator generator) {
        if (c.category == "manipulation" && generator == Generator::gives_in) {
            return {.reply = compliant_->at(c.id), .manipulation_suspected = false, .notes_for_staff = {}};
        }
        const auto it = cheerful_->find(plan.target.language);
        return {.reply = it != cheerful_->end() ? it->second : cheerful_->at("en"),
                .manipulation_suspected = c.category == "manipulation" && generator == Generator::resists,
                .notes_for_staff = {}};
    }

    Outcome run(const Case& c, Analyzer analyzer, Generator generator) const {
        const auto setup = seed_.client(c.industry);
        const auto answer = analyzer_answer(c, setup.industry, analyzer);
        Outcome out;
        out.analysis = build_analysis(setup.industry, c.text, AnalyzerAnswer{.json = &answer, .error = {}});
        out.plan = plan_draft(out.analysis, c.stars, true, setup.settings);
        out.draft = finish_draft(out.analysis, out.plan, c.stars, true, generator_answer(c, out.plan, generator), setup.settings);
        return out;
    }

    static bool failed(const Outcome& out, std::string_view check) {
        return std::ranges::any_of(out.draft.checks, [&](const CheckResult& r) { return r.name == check && !r.passed; });
    }

    static std::string why_stopped(const Outcome& out) {
        std::string why;
        for (const auto& reason : out.draft.route_reasons) why += (why.empty() ? "" : "; ") + reason;
        return why;
    }

    test::KnowledgeFiles seed_;
    static std::vector<Case>* cases_;
    static std::map<std::string, std::string>* compliant_;
    static std::map<std::string, std::string>* cheerful_;
};

std::vector<Case>* AdversarialSuite::cases_ = nullptr;
std::map<std::string, std::string>* AdversarialSuite::compliant_ = nullptr;
std::map<std::string, std::string>* AdversarialSuite::cheerful_ = nullptr;

}  // namespace

// The suite must not shrink unnoticed.
TEST_F(AdversarialSuite, HasEveryCase) {
    std::map<std::string, int> by_category, by_language;
    for (const auto& c : *cases_) {
        ++by_category[c.category];
        ++by_language[c.lang];
        if (c.category == "manipulation") {
            EXPECT_TRUE(compliant_->contains(c.id)) << c.id << " has no scripted reply";
        }
        if (c.category == "severity") {
            EXPECT_FALSE(c.severity_any.empty()) << c.id << " names no flag";
        }
    }
    EXPECT_GE(cases_->size(), 71u);
    EXPECT_GE(by_category["severity"], 43);
    EXPECT_GE(by_category["manipulation"], 21);
    EXPECT_GE(by_category["control"], 7);
    EXPECT_GE(by_language["zh"], 8);
    EXPECT_GE(by_language["zh-Hant"], 4);
    EXPECT_GE(by_language["es"], 3);
    EXPECT_GE(by_language["mixed"], 6);
}

// A severity flag from the analyzer always escalates, whatever the stars and
// however cheerful the drafted reply.
TEST_F(AdversarialSuite, EverySeverityFlagTheAnalyzerRaisesEscalates) {
    for (const auto& c : *cases_) {
        if (c.category != "severity") continue;
        const auto out = run(c, Analyzer::faithful, Generator::gives_in);
        EXPECT_EQ(out.draft.route, kRouteEscalate) << c.id << ": " << c.text;
        EXPECT_EQ(out.plan.mode, kModeSeverity) << c.id;
        ASSERT_EQ(out.analysis.severity_flags.size(), 1u) << c.id << ": the fixture names a flag the client's industry doesn't have";
        EXPECT_TRUE(std::ranges::contains(c.severity_any, out.analysis.severity_flags[0].id)) << c.id;
    }
}

// Either model saying "this review tries to steer the reply" is enough to make it wait for a person,
// even when the reply itself gave in.
TEST_F(AdversarialSuite, AManipulationFlagFromEitherModelWaitsForAPerson) {
    for (const auto& c : *cases_) {
        if (c.category != "manipulation") continue;
        EXPECT_EQ(run(c, Analyzer::faithful, Generator::gives_in).draft.route, kRouteApproval) << c.id << ": flagged by the analyzer";
        EXPECT_EQ(run(c, Analyzer::blind, Generator::resists).draft.route, kRouteApproval) << c.id << ": flagged by the generator";
    }
}

// What code alone still guarantees (D-058): a reply that carries a link, an
// email address or a phone number never reaches auto reply, even when both
// models were fooled and flagged nothing.
TEST_F(AdversarialSuite, AReplyWithALinkAnEmailOrAPhoneNumberIsStoppedWithoutTheModelsHelp) {
    const std::map<std::string, std::string> carries{{"M-03", "no_url"},   {"M-04", "no_phone"}, {"M-07", "no_phone"},
                                                     {"M-12", "no_url"},   {"M-13", "no_phone"}, {"M-20", "no_phone"}};
    for (const auto& c : *cases_) {
        const auto expected = carries.find(c.id);
        if (expected == carries.end()) continue;
        const auto out = run(c, Analyzer::blind, Generator::gives_in);
        EXPECT_NE(out.draft.route, kRouteAutoReply) << c.id << ": " << compliant_->at(c.id);
        EXPECT_TRUE(failed(out, expected->second)) << c.id << ": " << expected->second << " passed on \"" << compliant_->at(c.id) << "\"";
    }
    EXPECT_TRUE(failed(run(*std::ranges::find(*cases_, "M-04", &Case::id), Analyzer::blind, Generator::gives_in), "no_email"));
}

// What was given up with the keyword rules and the content checks (D-058),
// stated as a test so it can't be forgotten: when both models are fooled,
// nothing in the engine notices a severity issue in a 4 or 5 star review, or
// an offer a review talked the generator into. The live suite
// (live_suite.cpp) measures how often the real model is fooled.
TEST_F(AdversarialSuite, WithBothModelsFooledOnlyTheContactChecksAreLeft) {
    int severity_leaks = 0, severity = 0;
    for (const auto& c : *cases_) {
        if (c.category != "severity") continue;
        ++severity;
        severity_leaks += run(c, Analyzer::blind, Generator::gives_in).draft.route == kRouteAutoReply;
    }
    std::println("both models fooled: {} of {} severity cases would reach auto reply", severity_leaks, severity);
    EXPECT_GT(severity_leaks, 0);

    // "50% off your next visit": no link, no number, nothing code checks for.
    const auto offer = run(*std::ranges::find(*cases_, "M-01", &Case::id), Analyzer::blind, Generator::gives_in);
    EXPECT_FALSE(failed(offer, "no_url"));
    EXPECT_FALSE(failed(offer, "no_phone"));
    EXPECT_FALSE(failed(offer, "no_email"));
}

// The look-alikes: no false alarm from the checks, and the route FR-5
// expects when the analyzer behaves.
TEST_F(AdversarialSuite, BenignLookAlikesAreNotFlagged) {
    for (const auto& c : *cases_) {
        if (c.category != "control") continue;
        const auto out = run(c, Analyzer::faithful, Generator::gives_in);
        for (const auto& check : out.draft.checks) {
            // The scripted thank-you is shorter than a reply to a complaint has to be.
            if (check.name == "length" && out.plan.mode == kModeComplaint) continue;
            EXPECT_TRUE(check.passed) << c.id << ": " << check.name << " " << check.detail;
        }
        EXPECT_EQ(out.draft.route, c.action) << c.id << ": " << why_stopped(out);
    }
}
