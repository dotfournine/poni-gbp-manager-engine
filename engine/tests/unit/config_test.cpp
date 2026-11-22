#include "core/config.hpp"
#include "unit/test_key.hpp"
#include "jobs/go_live.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <map>

using namespace poni::core;
using namespace std::chrono_literals;
using ::testing::HasSubstr;

namespace {

Environment env_of(Environment values) { return values; }

// A key that SecretBox takes: base64 of 32 bytes, which ends in '='.
constexpr std::string_view kKey = "MDEyMzQ1Njc4OWFiY2RlZjAxMjM0NTY3ODlhYmNkZWY=";

class ConfigFile {
public:
    explicit ConfigFile(std::string_view text)
        : path_(std::filesystem::temp_directory_path() / std::format("poni-config-test-{}.env", ::getpid())) {
        std::ofstream(path_) << text;
    }
    ~ConfigFile() { std::filesystem::remove(path_); }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace

TEST(Config, DefaultsPointAtTheDevDatabase) {
    const auto c = load_config(env_of({}), std::nullopt);
    ASSERT_TRUE(c) << c.error();
    EXPECT_EQ(c->database_url, "postgresql://poni:poni@127.0.0.1:55432/poni");
    EXPECT_EQ(c->http_host, "127.0.0.1");
    EXPECT_EQ(c->http_port, 8080);
    EXPECT_EQ(c->log_format, poni::log::Format::json);
    EXPECT_EQ(c->job_workers, 8u);
    EXPECT_EQ(c->job_poll_interval, 1000ms);
    EXPECT_FALSE(c->migrations_dir.empty());
}

// X4: a job that runs longer than the lock timeout is handed to another worker and runs twice.
TEST(Config, TheJobLockTimeoutOutlastsTheLongestJob) {
    const auto c = load_config(env_of({}), std::nullopt);
    ASSERT_TRUE(c) << c.error();
    // A go-live case at the defaults: 3 repeats x 2 calls x 3 attempts x 60 s, and the judge at 240 s x 3.
    const auto longest_case = 3 * 2 * c->llm_max_attempts * c->llm_timeout + 3 * 240s;
    EXPECT_GT(c->job_lock_timeout, longest_case);
    EXPECT_LT(c->job_lock_timeout, poni::jobs::kGoLiveStalledAfter) << "a run whose case is still running is not ended as stalled";
}

TEST(Config, EnvironmentOverridesDefaults) {
    const auto c = load_config(env_of({{"PONI_HTTP_PORT", "9000"},
                                       {"PONI_DATABASE_URL", "postgresql://db/x"},
                                       {"PONI_JOB_POLL_INTERVAL_MS", "250"},
                                       {"PONI_HEARTBEAT_INTERVAL_S", "60"}}),
                               std::nullopt);
    ASSERT_TRUE(c) << c.error();
    EXPECT_EQ(c->http_port, 9000);
    EXPECT_EQ(c->database_url, "postgresql://db/x");
    EXPECT_EQ(c->job_poll_interval, 250ms);
    EXPECT_EQ(c->heartbeat_interval, 60s);
}

TEST(Config, EnvironmentWinsOverFileAndFileOverDefaults) {
    const ConfigFile file("# local settings\nPONI_HTTP_PORT=7000\nPONI_LOG_LEVEL = \"debug\"\n\n");
    const auto c = load_config(env_of({{"PONI_HTTP_PORT", "7001"}}), file.path());
    ASSERT_TRUE(c) << c.error();
    EXPECT_EQ(c->http_port, 7001);
    EXPECT_EQ(c->log_level, poni::log::Level::debug);
}

TEST(Config, EmptyEnvironmentValueCountsAsUnset) {
    const auto c = load_config(env_of({{"PONI_HTTP_HOST", ""}}), std::nullopt);
    ASSERT_TRUE(c) << c.error();
    EXPECT_EQ(c->http_host, "127.0.0.1");
}

TEST(Config, RejectsInvalidNumbersAndNamesTheSetting) {
    for (const char* bad : {"0", "65536", "80a", "-1", " 80"}) {
        const auto c = load_config(env_of({{"PONI_HTTP_PORT", bad}}), std::nullopt);
        ASSERT_FALSE(c) << bad;
        EXPECT_THAT(c.error(), HasSubstr("PONI_HTTP_PORT"));
    }
}

TEST(Config, RejectsUnknownSettingInFile) {
    const ConfigFile file("PONI_HTTP_PROT=8080\n");
    const auto c = load_config(env_of({}), file.path());
    ASSERT_FALSE(c);
    EXPECT_THAT(c.error(), HasSubstr("PONI_HTTP_PROT"));
}

TEST(Config, RejectsMissingFile) {
    const auto c = load_config(env_of({}), std::filesystem::path("/nonexistent/poni.env"));
    ASSERT_FALSE(c);
    EXPECT_THAT(c.error(), HasSubstr("/nonexistent/poni.env"));
}

TEST(EnvFile, ParsesQuotesCommentsAndWhitespace) {
    const auto parsed = parse_env_file("A=1\n  B = two words \r\n#C=3\nD='q'\nE=\"x=y\"\nF=\n");
    ASSERT_TRUE(parsed) << parsed.error();
    EXPECT_EQ(parsed->at("A"), "1");
    EXPECT_EQ(parsed->at("B"), "two words");
    EXPECT_FALSE(parsed->contains("C"));
    EXPECT_EQ(parsed->at("D"), "q");
    EXPECT_EQ(parsed->at("E"), "x=y");
    EXPECT_EQ(parsed->at("F"), "");
}

TEST(EnvFile, ReportsTheLineOfASyntaxError) {
    const auto parsed = parse_env_file("A=1\nnot a setting\n");
    ASSERT_FALSE(parsed);
    EXPECT_THAT(parsed.error(), HasSubstr("line 2"));
}

TEST(Config, Phase1DefaultsAreSafeForLocalDevelopment) {
    const auto c = load_config(env_of({}), std::nullopt);
    ASSERT_TRUE(c) << c.error();
    EXPECT_FALSE(c->secret_box);
    EXPECT_EQ(c->public_url, "http://localhost:5173");
    EXPECT_EQ(c->signin_domains, (std::vector<std::string>{"xvisionmediaus.com", "xvisionus.com"}));
    EXPECT_TRUE(c->dev_login_email.empty()) << "development sign-in is off unless asked for";
    EXPECT_TRUE(c->google_signin_client_id.empty());
    EXPECT_FALSE(c->trust_forwarded_for);
    EXPECT_FALSE(c->pubsub_subscription) << "notifications are off unless a subscription is named";
    EXPECT_EQ(c->google_credentials, "adc");
    EXPECT_EQ(c->session_ttl, 168h);
    EXPECT_EQ(c->session_idle, 24h);
    EXPECT_EQ(c->app_session_ttl, 24h * 90) << "the phone app's session (D-095)";
    EXPECT_EQ(c->app_session_idle, 24h * 30);
    EXPECT_EQ(c->review_poll_tick, 60s);
    EXPECT_EQ(c->review_full_sync_interval, 3600s);
    EXPECT_EQ(c->location_discovery_interval, 900s);
}

TEST(Config, ReadsPhase1SettingsFromAFile) {
    const ConfigFile file(std::format("PONI_SECRET_KEY={}\n", kKey) +
                          "PONI_PUBLIC_URL=https://gbp.example.com\n"
                          "PONI_TRUST_FORWARDED_FOR=true\n"
                          "PONI_GOOGLE_CREDENTIALS=metadata\n"
                          "PONI_PUBSUB_SUBSCRIPTION=projects/p/subscriptions/s\n"
                          "PONI_SESSION_IDLE_HOURS=8\n"
                          "PONI_APP_SESSION_TTL_HOURS=720\n"
                          "PONI_APP_SESSION_IDLE_HOURS=168\n");
    const auto c = load_config(env_of({}), file.path());
    ASSERT_TRUE(c) << c.error();
    ASSERT_TRUE(c->secret_box) << "a value may contain '='";
    EXPECT_EQ(c->secret_box->key_id(), SecretBox::from_base64_key(kKey)->key_id());
    EXPECT_EQ(c->public_url, "https://gbp.example.com");
    EXPECT_TRUE(c->trust_forwarded_for);
    EXPECT_EQ(c->google_credentials, "metadata");
    EXPECT_EQ(c->pubsub_subscription, (std::pair<std::string, std::string>{"p", "s"}));
    EXPECT_EQ(c->session_idle, 8h);
    EXPECT_EQ(c->app_session_ttl, 720h);
    EXPECT_EQ(c->app_session_idle, 168h);
    EXPECT_EQ(c->session_ttl, 168h) << "each kind of session has its own two settings";
}

// A setting of a command the engine no longer has is refused by name, like any unknown setting.
TEST(Config, TheOldAuthCommandsSettingIsGone) {
    const ConfigFile file("PONI_AUTH_CALLBACK_PORT=9000\n");
    EXPECT_THAT(load_config(env_of({}), file.path()).error(), HasSubstr("unknown setting PONI_AUTH_CALLBACK_PORT"));
}

// The sending mailbox is the account connected on the status page (D-100): no setting names it any more.
TEST(Config, TheSendersAddressIsNoSetting) {
    const ConfigFile file("PONI_EMAIL_FROM=host@xvisionmediaus.com\n");
    EXPECT_THAT(load_config(env_of({}), file.path()).error(), HasSubstr("unknown setting PONI_EMAIL_FROM"));
    const ConfigFile name_only("PONI_EMAIL_FROM_NAME=PONI\n");
    const auto loaded = load_config(env_of({}), name_only.path());
    ASSERT_TRUE(loaded) << loaded.error();
    EXPECT_EQ(loaded->email_from_name, "PONI") << "the name shown beside the address stays a setting";
}

TEST(Config, RejectsBadPhase1Values) {
    EXPECT_THAT(load_config(env_of({{"PONI_TRUST_FORWARDED_FOR", "yes"}}), std::nullopt).error(),
                HasSubstr("PONI_TRUST_FORWARDED_FOR"));
    EXPECT_THAT(load_config(env_of({{"PONI_GOOGLE_CREDENTIALS", "keyfile"}}), std::nullopt).error(),
                HasSubstr("PONI_GOOGLE_CREDENTIALS"));
    EXPECT_THAT(load_config(env_of({{"PONI_REVIEW_POLL_TICK_S", "1"}}), std::nullopt).error(),
                HasSubstr("PONI_REVIEW_POLL_TICK_S"));
    EXPECT_THAT(load_config(env_of({{"PONI_SECRET_KEY", "abc="}}), std::nullopt).error(), HasSubstr("PONI_SECRET_KEY"));
    EXPECT_THAT(load_config(env_of({{"PONI_PUBSUB_SUBSCRIPTION", "my-sub"}}), std::nullopt).error(),
                HasSubstr("PONI_PUBSUB_SUBSCRIPTION must look like projects/NAME/subscriptions/NAME"));
    EXPECT_THAT(load_config(env_of({{"PONI_LOG_LEVEL", "loud"}}), std::nullopt).error(), HasSubstr("unknown log level 'loud'"));
    EXPECT_THAT(load_config(env_of({{"PONI_LOG_FORMAT", "xml"}}), std::nullopt).error(), HasSubstr("unknown log format 'xml'"));
}

TEST(Config, SignInDomainsAreACommaSeparatedList) {
    const auto c = load_config(env_of({{"PONI_SIGNIN_DOMAINS", " Example.com ,second.example,, example.com "}}), std::nullopt);
    ASSERT_TRUE(c) << c.error();
    EXPECT_EQ(c->signin_domains, (std::vector<std::string>{"example.com", "second.example"}))
        << "trimmed, lower-cased, without repeats";

    const auto single = load_config(env_of({{"PONI_SIGNIN_DOMAINS", "example.com"}}), std::nullopt);
    ASSERT_TRUE(single) << single.error();
    EXPECT_EQ(single->signin_domains, std::vector<std::string>{"example.com"});

    for (const auto* bad : {" , ", "example", "staff@example.com", "example.com.", ".example.com", "exa mple.com"}) {
        const auto refused = load_config(env_of({{"PONI_SIGNIN_DOMAINS", bad}}), std::nullopt);
        ASSERT_FALSE(refused) << bad;
        EXPECT_THAT(refused.error(), HasSubstr("PONI_SIGNIN_DOMAINS")) << bad;
    }
}

TEST(Config, TheLlmIsOffUntilAnAddressAndAKeyAreGiven) {
    const auto none = load_config(env_of({}), std::nullopt);
    ASSERT_TRUE(none) << none.error();
    EXPECT_TRUE(none->llm_providers.empty());
    EXPECT_EQ(none->llm_timeout, 60s);
    EXPECT_EQ(none->llm_max_attempts, 3u);
    EXPECT_EQ(none->analysis_tick, 60s);

    const auto set = load_config(env_of({{"PONI_LLM_TIMEOUT_S", "90"},
                                         {"PONI_LLM_MAX_ATTEMPTS", "5"},
                                         {"PONI_ANALYSIS_TICK_S", "30"}}),
                                 std::nullopt);
    ASSERT_TRUE(set) << set.error();
    EXPECT_EQ(set->llm_timeout, 90s);
    EXPECT_EQ(set->llm_max_attempts, 5u);
    EXPECT_EQ(set->analysis_tick, 30s);

    EXPECT_FALSE(load_config(env_of({{"PONI_LLM_MAX_ATTEMPTS", "0"}}), std::nullopt));
}

// D-059: any number of providers, each a PONI_LLM_<NAME>_BASE_URL / _API_KEY pair.
TEST(Config, ModelProvidersAreNamedPairsFromTheFileOrTheEnvironment) {
    const ConfigFile file("PONI_LLM_DASHSCOPE_BASE_URL=https://dashscope.example/compatible-mode/v1\n"
                          "PONI_LLM_DASHSCOPE_API_KEY=file-key\n"
                          "PONI_LLM_DASHSCOPE_PROTOCOL=openai\n"
                          "PONI_LLM_OPENAI_BASE_URL=https://openai.example/v1\n"
                          "PONI_LLM_OPENAI_API_KEY=key-o\n"
                          "PONI_LLM_OPENAI_PROTOCOL=openai\n");
    const auto from_file = load_config(env_of({}), file.path());
    ASSERT_TRUE(from_file) << from_file.error();
    ASSERT_EQ(from_file->llm_providers.size(), 2u);
    EXPECT_EQ(from_file->llm_providers.at("dashscope").base_url, "https://dashscope.example/compatible-mode/v1");
    EXPECT_EQ(from_file->llm_providers.at("dashscope").api_key, "file-key");
    EXPECT_EQ(from_file->llm_providers.at("openai").api_key, "key-o");

    // The environment adds providers and overrides the file's values.
    const Environment environment{{"PONI_LLM_DASHSCOPE_API_KEY", "env-key"},
                                  {"PONI_LLM_DEEP_SEEK_BASE_URL", "http://localhost:9000/v1"},
                                  {"PONI_LLM_DEEP_SEEK_API_KEY", "key-s"},
                                  {"PONI_LLM_DEEP_SEEK_PROTOCOL", "openai"},
                                  {"UNRELATED", "x"}};
    const auto merged = load_config(environment, file.path());
    ASSERT_TRUE(merged) << merged.error();
    ASSERT_EQ(merged->llm_providers.size(), 3u);
    EXPECT_EQ(merged->llm_providers.at("dashscope").api_key, "env-key");
    EXPECT_EQ(merged->llm_providers.at("deep_seek").base_url, "http://localhost:9000/v1");
}

TEST(Config, AModelProviderNeedsBothAnAddressAndAKey) {
    const ConfigFile half("PONI_LLM_OPENAI_API_KEY=key-o\n");
    const auto missing = load_config(env_of({}), half.path());
    ASSERT_FALSE(missing);
    EXPECT_THAT(missing.error(), HasSubstr("PONI_LLM_OPENAI_API_KEY is set without PONI_LLM_OPENAI_BASE_URL"));

    const ConfigFile bad_address("PONI_LLM_OPENAI_BASE_URL=openai.example/v1\nPONI_LLM_OPENAI_API_KEY=k\nPONI_LLM_OPENAI_PROTOCOL=openai\n");
    const auto bad = load_config(env_of({}), bad_address.path());
    ASSERT_FALSE(bad);
    EXPECT_THAT(bad.error(), HasSubstr("PONI_LLM_OPENAI_BASE_URL must start with https://"));

    // A pair left empty (as in the example file) is no provider and no error.
    const ConfigFile empty("PONI_LLM_DASHSCOPE_BASE_URL=\nPONI_LLM_DASHSCOPE_API_KEY=\n");
    const auto none = load_config(env_of({}), empty.path());
    ASSERT_TRUE(none) << none.error();
    EXPECT_TRUE(none->llm_providers.empty());

    // A typo in a provider setting is still an unknown setting.
    const ConfigFile typo("PONI_LLM_OPENAI_APIKEY=k\n");
    const auto unknown = load_config(env_of({}), typo.path());
    ASSERT_FALSE(unknown);
    EXPECT_THAT(unknown.error(), HasSubstr("unknown setting PONI_LLM_OPENAI_APIKEY"));
}

// D-074: a provider that speaks Anthropic's own API says so.
TEST(Config, AProviderNamesItsProtocol) {
    const ConfigFile file("PONI_LLM_ANTHROPIC_BASE_URL=https://api.anthropic.example/v1\n"
                          "PONI_LLM_ANTHROPIC_API_KEY=key-a\n"
                          "PONI_LLM_ANTHROPIC_PROTOCOL=anthropic\n"
                          "PONI_LLM_DASHSCOPE_BASE_URL=https://dashscope.example/compatible-mode/v1\n"
                          "PONI_LLM_DASHSCOPE_API_KEY=key-d\n"
                          "PONI_LLM_DASHSCOPE_PROTOCOL=openai\n");
    const auto config = load_config(env_of({}), file.path());
    ASSERT_TRUE(config) << config.error();
    EXPECT_EQ(config->llm_providers.at("anthropic").protocol, LlmProtocol::anthropic);
    EXPECT_EQ(config->llm_providers.at("dashscope").protocol, LlmProtocol::openai);

    // There is no default: a provider without the line is refused, with the two words it may be.
    const ConfigFile bare("PONI_LLM_DASHSCOPE_BASE_URL=https://dashscope.example/compatible-mode/v1\nPONI_LLM_DASHSCOPE_API_KEY=key-d\n");
    const auto missing = load_config(env_of({}), bare.path());
    ASSERT_FALSE(missing);
    EXPECT_THAT(missing.error(), HasSubstr("PONI_LLM_DASHSCOPE_PROTOCOL is required: openai"));

    const ConfigFile typo("PONI_LLM_ANTHROPIC_BASE_URL=https://api.anthropic.example/v1\nPONI_LLM_ANTHROPIC_API_KEY=k\n"
                          "PONI_LLM_ANTHROPIC_PROTOCOL=claude\n");
    const auto unknown = load_config(env_of({}), typo.path());
    ASSERT_FALSE(unknown);
    EXPECT_THAT(unknown.error(), HasSubstr("PONI_LLM_ANTHROPIC_PROTOCOL: 'claude' is not openai or anthropic"));

    // The example file names each provider's protocol; one without an address and a key is simply not set.
    const ConfigFile alone("PONI_LLM_ANTHROPIC_PROTOCOL=anthropic\nPONI_LLM_ANTHROPIC_BASE_URL=\nPONI_LLM_ANTHROPIC_API_KEY=\n");
    const auto unset = load_config(env_of({}), alone.path());
    ASSERT_TRUE(unset) << unset.error();
    EXPECT_TRUE(unset->llm_providers.empty());
}

// ---- the Google mock (D-087) ------------------------------------------------------------

TEST(Config, TheGoogleMockIsOffUnlessSet) {
    const auto c = load_config(env_of({}), std::nullopt);
    ASSERT_TRUE(c);
    EXPECT_TRUE(c->google_mock_url.empty());
}

TEST(Config, TheGoogleMockMustBeOnThisMachine) {
    for (const auto& [given, kept] : std::map<std::string, std::string>{{"http://127.0.0.1:8090", "http://127.0.0.1:8090"},
                                                                        {"http://127.0.0.1:8090/", "http://127.0.0.1:8090"},
                                                                        {"http://localhost:9000", "http://localhost:9000"}}) {
        const auto c = load_config(env_of({{"PONI_GOOGLE_MOCK_URL", given}}), std::nullopt);
        ASSERT_TRUE(c) << given << ": " << c.error();
        EXPECT_EQ(c->google_mock_url, kept);
    }
    for (const auto* bad : {"https://127.0.0.1:8090", "http://10.0.0.5:8090", "http://mock.example:8090", "http://127.0.0.1",
                            "http://127.0.0.1:8090/mock", "http://127.0.0.1:8090?x=1", "http://user@127.0.0.1:8090",
                            "http://127.0.0.1.evil.example:8090", "http://127.0.0.1:0", "http://127.0.0.1:99999",
                            "http://[::1]:8090", "127.0.0.1:8090"}) {
        const auto c = load_config(env_of({{"PONI_GOOGLE_MOCK_URL", bad}}), std::nullopt);
        ASSERT_FALSE(c) << bad;
        EXPECT_THAT(c.error(), HasSubstr("PONI_GOOGLE_MOCK_URL")) << bad;
    }
}

TEST(Config, AgainstTheGoogleMockNothingMayReachARealGoogleService) {
    const Environment mock{{"PONI_GOOGLE_MOCK_URL", "http://127.0.0.1:8090"}};
    const auto with = [&](std::string key, std::string value) {
        auto values = mock;
        values.emplace(std::move(key), std::move(value));
        return load_config(env_of(std::move(values)), std::nullopt);
    };
    EXPECT_THAT(with("PONI_PUBSUB_SUBSCRIPTION", "projects/p/subscriptions/s").error(), HasSubstr("PONI_PUBSUB_SUBSCRIPTION"));
    EXPECT_THAT(with("PONI_GOOGLE_SIGNIN_CLIENT_ID", "id").error(), HasSubstr("PONI_GOOGLE_SIGNIN_CLIENT_ID"));
    EXPECT_THAT(with("PONI_PUBLIC_BUCKET", "poni-public").error(), HasSubstr("local folders"));
    EXPECT_TRUE(with("PONI_PUBLIC_BUCKET", "/tmp/poni-mock/public"));
    // Without the mock, the same settings are fine.
    EXPECT_TRUE(load_config(env_of({{"PONI_PUBSUB_SUBSCRIPTION", "projects/p/subscriptions/s"}}), std::nullopt));
}

// ---- the settings -------------------------------------------------------------------

namespace {

Environment apns_env(std::string environment = "sandbox") {
    return {{"PONI_APNS_ENVIRONMENT", std::move(environment)},
            {"PONI_APNS_KEY", poni::test::test_key().p8},
            {"PONI_APNS_KEY_ID", "ABC123DEFG"},
            {"PONI_APNS_TEAM_ID", "TEAM123456"},
            {"PONI_APNS_BUNDLE_ID", "com.example.gbpmanager"}};
}

}  // namespace

TEST(ApnsConfig, AllFiveOrNone) {
    const auto none = load_config({}, std::nullopt);
    ASSERT_TRUE(none);
    EXPECT_FALSE(none->apns);

    const auto all = load_config(apns_env("production"), std::nullopt);
    ASSERT_TRUE(all) << all.error();
    ASSERT_TRUE(all->apns);
    EXPECT_EQ(all->apns->environment, ApnsEnvironment::production);
    EXPECT_EQ(all->apns->key_id, "ABC123DEFG");
    EXPECT_EQ(all->apns->bundle_id, "com.example.gbpmanager");

    auto missing = apns_env();
    missing.erase("PONI_APNS_TEAM_ID");
    const auto refused = load_config(missing, std::nullopt);
    ASSERT_FALSE(refused);
    EXPECT_THAT(refused.error(), HasSubstr("PONI_APNS_TEAM_ID is missing"));
}

TEST(ApnsConfig, EachSettingIsChecked) {
    const auto with = [](std::string key, std::string value) {
        auto env = apns_env();
        env[key] = std::move(value);
        return load_config(env, std::nullopt);
    };
    EXPECT_THAT(with("PONI_APNS_ENVIRONMENT", "development").error(), HasSubstr("not production or sandbox"));
    EXPECT_THAT(with("PONI_APNS_KEY", "bm90IGEga2V5").error(), HasSubstr("PONI_APNS_KEY"));
    EXPECT_THAT(with("PONI_APNS_KEY_ID", "abc").error(), HasSubstr("PONI_APNS_KEY_ID"));
    EXPECT_THAT(with("PONI_APNS_TEAM_ID", "team123456").error(), HasSubstr("PONI_APNS_TEAM_ID"));
    EXPECT_THAT(with("PONI_APNS_BUNDLE_ID", "com.example\r\nx").error(), HasSubstr("PONI_APNS_BUNDLE_ID"));
}

TEST(ApnsConfig, AgainstTheGoogleMockPushGoesOnlyToBuildsRunFromXcode) {
    auto env = apns_env("production");
    env["PONI_GOOGLE_MOCK_URL"] = "http://127.0.0.1:8090";
    const auto refused = load_config(env, std::nullopt);
    ASSERT_FALSE(refused);
    EXPECT_THAT(refused.error(), HasSubstr("must be sandbox"));
    env["PONI_APNS_ENVIRONMENT"] = "sandbox";
    EXPECT_TRUE(load_config(env, std::nullopt));
}
