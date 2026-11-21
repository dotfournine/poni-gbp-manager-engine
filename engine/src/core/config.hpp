// Engine configuration. Every setting is a PONI_* variable that comes from
// the process environment or from an optional KEY=VALUE file; the environment
// wins. Secrets are passed the same way and never live in the repository.
// load_config is the one place a setting is checked: what it returns is
// parsed, and the engine starts on it or not at all.
#pragma once

#include "core/crypto.hpp"
#include "core/log.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace poni::core {

// PONI_LLM_<NAME>_PROTOCOL, required: "openai" (any OpenAI-compatible
// provider) or "anthropic" (Anthropic's Messages API, D-074).
enum class LlmProtocol { openai, anthropic };

// "openai" or "anthropic"; nothing for any other word.
std::optional<LlmProtocol> parse_llm_protocol(std::string_view name);

// "projects/{p}/subscriptions/{s}" -> {p, s}; nothing for any other shape.
std::optional<std::pair<std::string, std::string>> parse_subscription(std::string_view subscription);

// One model provider, as the LLM client takes it (llm::NetOptions::Provider).
struct LlmProvider {
    // The endpoint up to and including the version segment: the engine posts
    // to {base_url}/chat/completions, or to {base_url}/messages when the
    // protocol is anthropic. DashScope keys only work in their own region
    // (D-010).
    std::string base_url;
    // Comes from the environment; never logged.
    std::string api_key;
    LlmProtocol protocol = LlmProtocol::openai;
};

// Which of Apple's two push services an engine sends to (D-097). Builds of
// the app from TestFlight and the App Store have tokens of `production`,
// builds run from Xcode tokens of `sandbox`.
enum class ApnsEnvironment { production, sandbox };
std::string_view name(ApnsEnvironment environment);

// Push to the phone app (D-097): one environment per engine and one token
// key, an APNs auth key that is environment-specific and topic-specific.
struct ApnsSettings {
    ApnsEnvironment environment = ApnsEnvironment::sandbox;
    // PONI_APNS_KEY: the key, as base64 the .p8 file holds. A secret.
    Es256Key key;
    // PONI_APNS_KEY_ID, PONI_APNS_TEAM_ID: ten letters and digits each.
    std::string key_id;
    std::string team_id;
    // PONI_APNS_BUNDLE_ID: the app's bundle id, every push's topic.
    std::string bundle_id;
};

struct Config {
    // libpq connection URL. The default is the local development database
    // from deploy/docker-compose.dev.yml.
    std::string database_url = "postgresql://poni:poni@127.0.0.1:55432/poni";
    unsigned db_pool_size = 8;

    std::string http_host = "127.0.0.1";
    std::uint16_t http_port = 8080;

    log::Level log_level = log::Level::info;
    log::Format log_format = log::Format::json;

    // Loops on the one engine thread; a job mostly waits on the network, so they are cheap.
    // Eight keep up with the design point of 10,000 reviews a day, and run a go-live test eight cases at a time.
    unsigned job_workers = 8;
    std::chrono::milliseconds job_poll_interval{1000};
    // A running job whose lock is older than this is handed to another worker,
    // so it must be longer than the longest job: a go-live case can take 30
    // minutes at the defaults (3 repeats x 2 calls x 3 attempts x 60 s, and
    // the judge at 240 s x 3), and it must stay shorter than the 45 minutes
    // after which a go-live run without progress is ended (kGoLiveStalledAfter).
    std::chrono::seconds job_lock_timeout{2400};
    std::chrono::seconds heartbeat_interval{300};
    // Succeeded jobs are deleted after this many days.
    unsigned job_retention_days = 7;

    std::filesystem::path migrations_dir = PONI_DEFAULT_MIGRATIONS_DIR;

    // ---- secrets and sign-in ----

    // PONI_SECRET_KEY: base64 of 32 random bytes (`openssl rand -base64 32`).
    // Encrypts what the `credentials` table stores. Without it the engine
    // runs, but can't read or store the GBP token.
    std::optional<SecretBox> secret_box;
    // The dashboard's address as staff type it, an origin without a trailing
    // slash: https, or http on localhost in development. Decides the cookie
    // flags and the Google redirect addresses ({public_url}/api/auth/google/callback).
    std::string public_url = "http://localhost:5173";
    // Only Google Workspace accounts of these domains may sign in (D-033,
    // D-053). PONI_SIGNIN_DOMAINS is a comma-separated list.
    std::vector<std::string> signin_domains = {"xvisionmediaus.com", "xvisionus.com"};
    // OAuth web client for dashboard sign-in. Empty: Google sign-in is off.
    std::string google_signin_client_id;
    std::string google_signin_client_secret;
    // Local development only: sign in as this address without Google. Refused
    // with an https public_url, so it can't be switched on in production by accident.
    std::string dev_login_email;
    std::chrono::hours session_ttl{24 * 7};
    std::chrono::hours session_idle{24};
    // The phone app's session (D-095): it ends this long after signing in,
    // or this long after its last use.
    std::chrono::hours app_session_ttl{24 * 90};
    std::chrono::hours app_session_idle{24 * 30};
    // True behind a reverse proxy (Caddy): the client's address is then taken
    // from X-Forwarded-For.
    bool trust_forwarded_for = false;

    // ---- Google Business Profile ----

    // The Google OAuth client (a web client) through which the GBP managing
    // account and the sending mailbox are connected on the dashboard's status
    // page (D-079). Its redirect addresses must include
    // <PONI_PUBLIC_URL>/api/connections/gbp/callback and .../gmail/callback.
    std::string gbp_oauth_client_id;
    std::string gbp_oauth_client_secret;
    std::chrono::seconds location_discovery_interval{900};
    // How often the review poll job runs; each location has its own interval on top.
    std::chrono::seconds review_poll_tick{60};
    std::chrono::seconds review_full_sync_interval{3600};
    // Development only (D-087): the address of `poni-tools mock-google` on
    // this machine, "http://127.0.0.1:8090". Set, the engine sends every call
    // it makes to Google Business Profile, Google's token endpoint, the
    // Performance API and Gmail to the mock instead, refuses to talk to any
    // other Google service, and only starts against a database marked as a
    // mock database. Empty: Google, as in production.
    std::string google_mock_url;
    bool google_mock() const { return !google_mock_url.empty(); }

    // ---- Pub/Sub notifications ----

    // PONI_PUBSUB_SUBSCRIPTION, "projects/{project}/subscriptions/{name}", as
    // the project and the name. Nothing: notifications are off and polling is
    // the only source.
    std::optional<std::pair<std::string, std::string>> pubsub_subscription;
    // Where Pub/Sub credentials come from: "adc" (gcloud application-default
    // credentials, for development) or "metadata" (the VM's service account).
    std::string google_credentials = "adc";
    std::chrono::seconds pubsub_pull_interval{30};

    // ---- review analysis and drafts (the LLM) ----

    // The model providers the engine may use, by name (D-059). A provider is
    // PONI_LLM_<NAME>_BASE_URL, PONI_LLM_<NAME>_API_KEY and _PROTOCOL; its name here is
    // <NAME> in lower case ("dashscope", "openai"). Which one each task uses is
    // a setting in the dashboard. None: nothing is analyzed.
    std::map<std::string, LlmProvider> llm_providers;
    // Applied to each stage of one request (connect, TLS, write, read).
    std::chrono::seconds llm_timeout{60};
    // Requests per call before the job gives up and is retried later.
    unsigned llm_max_attempts = 3;
    // How often reviews that still need an analysis or a draft are picked up.
    std::chrono::seconds analysis_tick{60};

    // ---- posting and notifications ----

    // How long a reply that was sent may stay invisible on Google before the
    // engine stops waiting and returns the draft to the approval queue.
    std::chrono::seconds post_confirm_window{30 * 60};
    // The mailbox notifications are sent from (D-026), and the name shown
    // with it. Connecting the mailbox on the status page only accepts a sign-in of this address.
    std::string email_from = "host@xvisionmediaus.com";
    std::string email_from_name = "PONI GBP Manager";

    // ---- reports (D-085) ----

    // The Cloud Storage bucket shared report pages are put in, readable by
    // anyone who has an object's address and listable by nobody. A value
    // with a slash in it is a local folder instead, for development. Empty:
    // no report can be shared or sent.
    std::string public_bucket;

    // ---- push to the phone app (D-097) ----

    // PONI_APNS_ENVIRONMENT, PONI_APNS_KEY, PONI_APNS_KEY_ID, PONI_APNS_TEAM_ID
    // and PONI_APNS_BUNDLE_ID, all or none. None: nothing is pushed, and a
    // push rule is refused.
    std::optional<ApnsSettings> apns;
};

// The environment, by variable name.
using Environment = std::map<std::string, std::string, std::less<>>;

// A copy of the process environment, taken once at start.
Environment process_environment();

// The config file to read when none is named: PONI_CONFIG, or else
// ~/.config/poni-gbp/engine.env if it exists (local development: secrets
// live outside the repository). tools/src/poni_tools/settings.py finds it
// the same way; keep the two in step.
std::optional<std::filesystem::path> default_config_file(const Environment& env);

// Parses KEY=VALUE lines. Blank lines and lines starting with '#' are skipped;
// a value may be wrapped in single or double quotes.
std::expected<std::map<std::string, std::string>, std::string> parse_env_file(std::string_view text);

// Builds the configuration: defaults, then `file` (if given), then `env`.
// Returns a message naming the offending setting on any invalid value, and
// rejects PONI_* keys in the file that no setting reads, so a typo isn't
// silently ignored.
std::expected<Config, std::string> load_config(const Environment& env, const std::optional<std::filesystem::path>& file);

}  // namespace poni::core
