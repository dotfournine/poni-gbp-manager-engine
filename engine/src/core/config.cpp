#include "core/config.hpp"

#include "core/text.hpp"

#include <boost/url/parse.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <format>
#include <fstream>
#include <limits>
#include <ranges>
#include <set>
#include <sstream>

#include <unistd.h>

extern char** environ;  // NOLINT: POSIX; <unistd.h> doesn't declare it everywhere

namespace poni::core {

namespace {

constexpr std::string_view kLlmPrefix = "PONI_LLM_";
constexpr std::string_view kBaseUrl = "_BASE_URL";
constexpr std::string_view kApiKey = "_API_KEY";
constexpr std::string_view kProtocol = "_PROTOCOL";
constexpr long long kMaxSeconds = 365LL * 24 * 3600;

// "PONI_LLM_DASHSCOPE_API_KEY" -> "DASHSCOPE"; nothing if `key` is not a
// provider setting. The name is letters, digits and underscores, starting
// with a letter.
std::optional<std::string_view> llm_provider_of(std::string_view key) {
    if (!key.starts_with(kLlmPrefix)) return std::nullopt;
    auto rest = key.substr(kLlmPrefix.size());
    if (rest.ends_with(kBaseUrl)) {
        rest.remove_suffix(kBaseUrl.size());
    } else if (rest.ends_with(kApiKey)) {
        rest.remove_suffix(kApiKey.size());
    } else if (rest.ends_with(kProtocol)) {
        rest.remove_suffix(kProtocol.size());
    } else {
        return std::nullopt;
    }
    if (rest.empty() || rest.front() < 'A' || rest.front() > 'Z') return std::nullopt;
    const bool valid = std::ranges::all_of(rest, [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; });
    return valid ? std::optional(rest) : std::nullopt;
}

std::string_view trim(std::string_view s) {
    constexpr std::string_view ws = " \t\r";
    const auto first = s.find_first_not_of(ws);
    if (first == std::string_view::npos) return {};
    return s.substr(first, s.find_last_not_of(ws) - first + 1);
}

std::expected<long long, std::string> parse_int(std::string_view key, std::string_view value, long long min, long long max) {
    long long n = 0;
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), n);
    if (ec != std::errc{} || end != value.data() + value.size() || n < min || n > max) {
        return std::unexpected(std::format("{}: '{}' is not a whole number between {} and {}", key, value, min, max));
    }
    return n;
}

// "a.example, B.example" -> {"a.example", "b.example"}.
std::expected<std::vector<std::string>, std::string> parse_domains(std::string_view key, std::string_view value) {
    std::vector<std::string> domains;
    for (const auto part : value | std::views::split(',')) {
        const auto item = trim(std::string_view(part.begin(), part.end()));
        if (item.empty()) continue;
        auto domain = text::ascii_lower(item);
        const bool valid =
            domain.contains('.') && !domain.starts_with('.') && !domain.ends_with('.') &&
            std::ranges::all_of(domain, [](unsigned char ch) { return std::isalnum(ch) != 0 || ch == '-' || ch == '.'; });
        if (!valid) return std::unexpected(std::format("{}: '{}' is not a domain name such as example.com", key, item));
        if (!std::ranges::contains(domains, domain)) domains.push_back(std::move(domain));
    }
    if (domains.empty()) return std::unexpected(std::format("{}: name at least one domain", key));
    return domains;
}

// "http://127.0.0.1:8090" (a trailing slash is dropped); a message that names
// `key` if the address is not plain http on a loopback address with a port.
// The Google mock and the server of the public folder are programs on the
// same machine: anything else could be somebody else's server.
std::expected<std::string, std::string> parse_loopback_url(std::string_view key, std::string_view example, std::string_view value) {
    const auto refuse = [&] {
        return std::unexpected(std::format(
            "{}: '{}' is not an address on this machine such as {} (http, a loopback address and a port, nothing else)", key,
            value, example));
    };
    std::string_view rest = value;
    if (rest.ends_with('/')) rest.remove_suffix(1);
    constexpr std::string_view scheme = "http://";
    if (!rest.starts_with(scheme)) return refuse();
    rest.remove_prefix(scheme.size());
    const auto colon = rest.rfind(':');
    if (colon == std::string_view::npos) return refuse();
    const auto host = rest.substr(0, colon);
    const auto port = rest.substr(colon + 1);
    if (host != "127.0.0.1" && host != "localhost") return refuse();
    unsigned number = 0;
    const auto [end, ec] = std::from_chars(port.data(), port.data() + port.size(), number);
    if (port.empty() || ec != std::errc{} || end != port.data() + port.size() || number == 0 || number > 65535) return refuse();
    return std::format("http://{}:{}", host, number);
}

// The address staff type, and what may be combined with it.
std::optional<std::string> public_url_problem(const Config& c) {
    const auto url = boost::urls::parse_uri(c.public_url);
    if (!url || (url->scheme() != "https" && url->scheme() != "http") || !url->encoded_path().empty() || url->has_query() ||
        url->has_fragment()) {
        return "PONI_PUBLIC_URL must be an origin such as https://gbp.example.com, with no path and no trailing slash";
    }
    const bool https = url->scheme() == "https";
    if (!https && url->host() != "localhost" && url->host() != "127.0.0.1") {
        return "PONI_PUBLIC_URL must use https, except for http://localhost in development";
    }
    if (!c.dev_login_email.empty() && https) {
        return "PONI_DEV_LOGIN_EMAIL is for local development: it can't be combined with an https PONI_PUBLIC_URL";
    }
    if (!c.public_folder_url.empty() && https) {
        return "PONI_PUBLIC_FOLDER_URL is for local development: it can't be combined with an https PONI_PUBLIC_URL";
    }
    return std::nullopt;
}

// Reads settings from the environment merged over the file, keeps the first
// problem it meets, and remembers every key it was asked for: a key in the
// file that nothing asked for is a typo or a setting the engine no longer has.
class Settings {
public:
    Settings(std::map<std::string, std::string> file, const Environment& env) : file_(std::move(file)), merged_(file_.begin(), file_.end()) {
        // Empty values count as unset: an empty variable doesn't hide the file's value.
        for (const auto& [key, value] : env) {
            if (!value.empty()) merged_.insert_or_assign(key, value);
        }
    }

    std::optional<std::string> get(std::string_view key) {
        read_.emplace(key);
        const auto it = merged_.find(key);
        if (it == merged_.end() || it->second.empty()) return std::nullopt;
        return it->second;
    }

    void fail(std::string message) {
        if (!error_) error_ = std::move(message);
    }
    bool failed() const { return error_.has_value(); }

    void text(std::string_view key, std::string& target) {
        if (auto v = get(key)) target = std::move(*v);
    }
    template <class Assign>
    void number(std::string_view key, long long min, long long max, Assign assign) {
        const auto v = get(key);
        if (!v) return;
        if (const auto n = parse_int(key, *v, min, max)) {
            assign(*n);
        } else {
            fail(n.error());
        }
    }
    template <class Duration>
    void duration(std::string_view key, long long min, long long max, Duration& target) {
        number(key, min, max, [&](long long n) { target = Duration(n); });
    }
    void seconds(std::string_view key, long long min, std::chrono::seconds& target) { duration(key, min, kMaxSeconds, target); }
    void hours(std::string_view key, std::chrono::hours& target) { duration(key, 1, 24 * 365, target); }
    // A value that `parse` turns into the setting, or a message.
    template <class T, class Parse>
    void parsed(std::string_view key, T& target, Parse parse) {
        const auto v = get(key);
        if (!v) return;
        if (auto value = parse(*v)) {
            target = std::move(*value);
        } else {
            fail(std::move(value.error()));
        }
    }

    // Every provider name that a key of the file or the environment names.
    std::set<std::string, std::less<>> llm_provider_names() const {
        std::set<std::string, std::less<>> names;
        for (const auto& [key, value] : merged_) {
            if (const auto name = llm_provider_of(key)) names.emplace(*name);
        }
        return names;
    }

    // The first PONI_* key of the file that no setting read.
    std::optional<std::string> unread_file_key() const {
        for (const auto& [key, value] : file_) {
            if (key.starts_with("PONI_") && !read_.contains(key)) return key;
        }
        return std::nullopt;
    }

    std::optional<std::string>& error() { return error_; }

private:
    std::map<std::string, std::string> file_;
    std::map<std::string, std::string, std::less<>> merged_;
    std::set<std::string, std::less<>> read_;
    std::optional<std::string> error_;
};

void read_llm_providers(Settings& s, Config& c) {
    for (const auto& name : s.llm_provider_names()) {
        const auto url_key = std::format("{}{}{}", kLlmPrefix, name, kBaseUrl);
        const auto key_key = std::format("{}{}{}", kLlmPrefix, name, kApiKey);
        const auto protocol_key = std::format("{}{}{}", kLlmPrefix, name, kProtocol);
        // All three are read, so none of them counts as unknown, whatever is wrong with another provider.
        const auto url = s.get(url_key);
        const auto key = s.get(key_key);
        const auto protocol = s.get(protocol_key);
        // Without an address and a key the provider is not set, whatever its protocol line says
        // (the example file names the protocol of each provider it lists).
        if (!url && !key) continue;
        if (!url || !key) {
            s.fail(std::format("{} is set without {}", url ? url_key : key_key, url ? key_key : url_key));
        } else if (!protocol) {
            // No default: which API a provider speaks is said, never guessed.
            s.fail(std::format("{} is required: openai (an OpenAI-compatible provider) or anthropic (Anthropic's Messages API)",
                               protocol_key));
        } else if (!parse_llm_protocol(*protocol)) {
            s.fail(std::format("{}: '{}' is not openai or anthropic", protocol_key, *protocol));
        } else if (!url->starts_with("https://") && !url->starts_with("http://")) {
            s.fail(std::format("{} must start with https:// (or http:// for a local server)", url_key));
        } else {
            c.llm_providers.emplace(text::ascii_lower(name),
                                    LlmProvider{.base_url = *url,
                                                .api_key = *key,
                                                .protocol = *parse_llm_protocol(*protocol)});
        }
    }
}

// Ten capital letters and digits, as Apple's key and team ids are.
bool is_apple_id(std::string_view id) {
    return id.size() == 10 && std::ranges::all_of(id, [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); });
}

// A bundle id goes into a header of every push: letters, digits, '.' and '-'.
bool is_bundle_id(std::string_view id) {
    return !id.empty() && id.size() <= 155 && id.front() != '.' && id.back() != '.' && !id.contains("..") &&
           std::ranges::all_of(id, [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
           });
}

void read_apns(Settings& s, Config& c) {
    constexpr std::array<std::string_view, 5> keys{"PONI_APNS_ENVIRONMENT", "PONI_APNS_KEY", "PONI_APNS_KEY_ID", "PONI_APNS_TEAM_ID",
                                                   "PONI_APNS_BUNDLE_ID"};
    std::array<std::optional<std::string>, 5> values;
    for (std::size_t i = 0; i < keys.size(); ++i) values[i] = s.get(keys[i]);
    if (std::ranges::none_of(values, [](const auto& v) { return v.has_value(); })) return;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (!values[i]) return s.fail(std::format("{} is missing: the APNs settings are all set or none", keys[i]));
    }
    const auto& environment = *values[0];
    if (environment != "production" && environment != "sandbox") {
        return s.fail(std::format("PONI_APNS_ENVIRONMENT: '{}' is not production or sandbox", environment));
    }
    auto key = Es256Key::from_p8(*values[1]);
    if (!key) return s.fail("PONI_APNS_KEY: " + key.error());
    if (!is_apple_id(*values[2])) return s.fail("PONI_APNS_KEY_ID must be the key's ten letters and digits");
    if (!is_apple_id(*values[3])) return s.fail("PONI_APNS_TEAM_ID must be the team's ten letters and digits");
    if (!is_bundle_id(*values[4])) return s.fail(std::format("PONI_APNS_BUNDLE_ID: '{}' is not a bundle id", *values[4]));
    c.apns = ApnsSettings{.environment = environment == "production" ? ApnsEnvironment::production : ApnsEnvironment::sandbox,
                          .key = std::move(*key),
                          .key_id = *values[2],
                          .team_id = *values[3],
                          .bundle_id = *values[4]};
}

// What may not be combined: against the mock, nothing may reach a real Google service (D-087).
void check_combinations(Settings& s, const Config& c) {
    if (s.failed()) return;
    if (const auto problem = public_url_problem(c)) return s.fail(*problem);
    if (!c.public_folder_url.empty() && !c.public_bucket_is_folder()) {
        return s.fail("PONI_PUBLIC_FOLDER_URL is the address of a local folder: PONI_PUBLIC_BUCKET must be that folder's path "
                      "(a bucket's address is Cloud Storage's own)");
    }
    if (!c.google_mock() && !c.gbp_oauth_client_id.empty() && c.google_places_api_key.empty()) {
        return s.fail("PONI_GOOGLE_PLACES_API_KEY is required with a GBP OAuth client: a newly discovered location's time zone is read "
                      "from Google Places.");
    }
    if (c.google_mock()) {
        if (c.pubsub_subscription) {
            return s.fail("PONI_PUBSUB_SUBSCRIPTION must be empty with PONI_GOOGLE_MOCK_URL: the mock has no Pub/Sub, and a real "
                          "subscription would bring real reviews' names into a mock database");
        }
        if (!c.google_signin_client_id.empty() || !c.google_signin_client_secret.empty()) {
            return s.fail("PONI_GOOGLE_SIGNIN_CLIENT_ID and _SECRET must be empty with PONI_GOOGLE_MOCK_URL: sign in with "
                          "PONI_DEV_LOGIN_EMAIL against the mock");
        }
        if (!c.public_bucket.empty() && !c.public_bucket_is_folder()) {
            return s.fail("PONI_PUBLIC_BUCKET must be a path with PONI_GOOGLE_MOCK_URL: against the mock, buckets are local folders");
        }
        if (c.apns && c.apns->environment == ApnsEnvironment::production) {
            return s.fail("PONI_APNS_ENVIRONMENT must be sandbox with PONI_GOOGLE_MOCK_URL: made-up alerts go to builds run from "
                          "Xcode, never to the phones staff use");
        }
    }
}

}  // namespace

std::string_view name(ApnsEnvironment environment) {
    return environment == ApnsEnvironment::production ? "production" : "sandbox";
}

std::optional<LlmProtocol> parse_llm_protocol(std::string_view name) {
    if (name == "openai") return LlmProtocol::openai;
    if (name == "anthropic") return LlmProtocol::anthropic;
    return std::nullopt;
}

std::optional<std::pair<std::string, std::string>> parse_subscription(std::string_view subscription) {
    constexpr std::string_view kProjects = "projects/";
    constexpr std::string_view kSubscriptions = "/subscriptions/";
    if (!subscription.starts_with(kProjects)) return std::nullopt;
    const auto middle = subscription.find(kSubscriptions);
    if (middle == std::string_view::npos) return std::nullopt;
    const auto project = subscription.substr(kProjects.size(), middle - kProjects.size());
    const auto name = subscription.substr(middle + kSubscriptions.size());
    if (project.empty() || name.empty() || project.contains('/') || name.contains('/')) return std::nullopt;
    return std::pair{std::string(project), std::string(name)};
}

Environment process_environment() {
    Environment env;
    for (char** entry = environ; entry && *entry; ++entry) {
        const std::string_view line(*entry);
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        env.emplace(line.substr(0, eq), line.substr(eq + 1));
    }
    return env;
}

std::optional<std::filesystem::path> default_config_file(const Environment& env) {
    if (const auto named = env.find("PONI_CONFIG"); named != env.end() && !named->second.empty()) return named->second;
    if (const auto home = env.find("HOME"); home != env.end() && !home->second.empty()) {
        auto local = std::filesystem::path(home->second) / ".config/poni-gbp/engine.env";
        std::error_code ec;
        if (std::filesystem::is_regular_file(local, ec)) return local;
    }
    return std::nullopt;
}

std::expected<std::map<std::string, std::string>, std::string> parse_env_file(std::string_view text) {
    std::map<std::string, std::string> out;
    int line_no = 0;
    for (const auto part : text | std::views::split('\n')) {
        ++line_no;
        const auto line = trim(std::string_view(part.begin(), part.end()));
        if (line.empty() || line.starts_with('#')) continue;
        const auto eq = line.find('=');
        if (eq == std::string_view::npos || eq == 0) {
            return std::unexpected(std::format("line {}: expected KEY=VALUE", line_no));
        }
        const auto key = trim(line.substr(0, eq));
        auto value = trim(line.substr(eq + 1));
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front()) {
            value = value.substr(1, value.size() - 2);
        }
        out.insert_or_assign(std::string(key), std::string(value));
    }
    return out;
}

std::expected<Config, std::string> load_config(const Environment& env, const std::optional<std::filesystem::path>& file) {
    std::map<std::string, std::string> from_file;
    if (file) {
        std::ifstream in(*file);
        if (!in) return std::unexpected(std::format("cannot read config file {}", file->string()));
        std::ostringstream text;
        text << in.rdbuf();
        auto parsed = parse_env_file(text.str());
        if (!parsed) return std::unexpected(std::format("{}: {}", file->string(), parsed.error()));
        from_file = std::move(*parsed);
    }

    Settings s(std::move(from_file), env);
    Config c;
    s.text("PONI_DATABASE_URL", c.database_url);
    s.number("PONI_DB_POOL_SIZE", 1, 64, [&](long long n) { c.db_pool_size = static_cast<unsigned>(n); });
    s.text("PONI_HTTP_HOST", c.http_host);
    s.number("PONI_HTTP_PORT", 1, std::numeric_limits<std::uint16_t>::max(), [&](long long n) { c.http_port = static_cast<std::uint16_t>(n); });
    s.parsed("PONI_LOG_LEVEL", c.log_level, log::parse_level);
    s.parsed("PONI_LOG_FORMAT", c.log_format, log::parse_format);
    s.number("PONI_JOB_WORKERS", 0, 64, [&](long long n) { c.job_workers = static_cast<unsigned>(n); });
    s.duration("PONI_JOB_POLL_INTERVAL_MS", 10, 3'600'000, c.job_poll_interval);
    s.seconds("PONI_JOB_LOCK_TIMEOUT_S", 1, c.job_lock_timeout);
    s.seconds("PONI_HEARTBEAT_INTERVAL_S", 1, c.heartbeat_interval);
    s.number("PONI_JOB_RETENTION_DAYS", 1, 3650, [&](long long n) { c.job_retention_days = static_cast<unsigned>(n); });
    if (auto v = s.get("PONI_MIGRATIONS_DIR")) c.migrations_dir = *v;

    s.parsed("PONI_SECRET_KEY", c.secret_box, [](std::string_view key) -> std::expected<std::optional<SecretBox>, std::string> {
        auto box = SecretBox::from_base64_key(key);
        if (!box) return std::unexpected("PONI_SECRET_KEY: " + box.error());
        return std::move(*box);
    });
    s.text("PONI_PUBLIC_URL", c.public_url);
    s.parsed("PONI_SIGNIN_DOMAINS", c.signin_domains, [](std::string_view v) { return parse_domains("PONI_SIGNIN_DOMAINS", v); });
    s.text("PONI_GOOGLE_SIGNIN_CLIENT_ID", c.google_signin_client_id);
    s.text("PONI_GOOGLE_SIGNIN_CLIENT_SECRET", c.google_signin_client_secret);
    s.text("PONI_DEV_LOGIN_EMAIL", c.dev_login_email);
    s.hours("PONI_SESSION_TTL_HOURS", c.session_ttl);
    s.hours("PONI_SESSION_IDLE_HOURS", c.session_idle);
    s.hours("PONI_APP_SESSION_TTL_HOURS", c.app_session_ttl);
    s.hours("PONI_APP_SESSION_IDLE_HOURS", c.app_session_idle);
    s.parsed("PONI_TRUST_FORWARDED_FOR", c.trust_forwarded_for, [](std::string_view v) -> std::expected<bool, std::string> {
        if (v == "true" || v == "1") return true;
        if (v == "false" || v == "0") return false;
        return std::unexpected(std::format("PONI_TRUST_FORWARDED_FOR: '{}' is not true or false", v));
    });
    s.text("PONI_GBP_OAUTH_CLIENT_ID", c.gbp_oauth_client_id);
    s.text("PONI_GBP_OAUTH_CLIENT_SECRET", c.gbp_oauth_client_secret);
    s.text("PONI_GOOGLE_PLACES_API_KEY", c.google_places_api_key);
    s.seconds("PONI_LOCATION_DISCOVERY_INTERVAL_S", 60, c.location_discovery_interval);
    s.seconds("PONI_REVIEW_POLL_TICK_S", 10, c.review_poll_tick);
    s.seconds("PONI_REVIEW_POLL_INTERVAL_S", 60, c.review_poll_interval);
    s.seconds("PONI_REVIEW_FULL_SYNC_INTERVAL_S", 300, c.review_full_sync_interval);
    s.seconds("PONI_STAFF_CHECK_INTERVAL_S", 60, c.staff_check_interval);
    s.parsed("PONI_PUBSUB_SUBSCRIPTION", c.pubsub_subscription,
             [](std::string_view v) -> std::expected<std::optional<std::pair<std::string, std::string>>, std::string> {
                 if (auto parts = parse_subscription(v)) return parts;
                 return std::unexpected(std::string("PONI_PUBSUB_SUBSCRIPTION must look like projects/NAME/subscriptions/NAME"));
             });
    s.text("PONI_GOOGLE_CREDENTIALS", c.google_credentials);
    if (c.google_credentials != "adc" && c.google_credentials != "metadata") {
        s.fail(std::format("PONI_GOOGLE_CREDENTIALS: '{}' is not adc or metadata", c.google_credentials));
    }
    s.seconds("PONI_PUBSUB_PULL_INTERVAL_S", 5, c.pubsub_pull_interval);
    read_llm_providers(s, c);
    s.seconds("PONI_LLM_TIMEOUT_S", 5, c.llm_timeout);
    s.number("PONI_LLM_MAX_ATTEMPTS", 1, 10, [&](long long n) { c.llm_max_attempts = static_cast<unsigned>(n); });
    s.seconds("PONI_ANALYSIS_TICK_S", 10, c.analysis_tick);
    // Google held the spike's reply for 4 to 6 minutes: less than 10 would give up on replies that are fine.
    s.seconds("PONI_POST_CONFIRM_WINDOW_S", 600, c.post_confirm_window);
    s.text("PONI_EMAIL_FROM_NAME", c.email_from_name);
    s.text("PONI_PUBLIC_BUCKET", c.public_bucket);
    s.parsed("PONI_PUBLIC_FOLDER_URL", c.public_folder_url,
             [](std::string_view v) { return parse_loopback_url("PONI_PUBLIC_FOLDER_URL", "http://127.0.0.1:8088", v); });
    s.parsed("PONI_GOOGLE_MOCK_URL", c.google_mock_url,
             [](std::string_view v) { return parse_loopback_url("PONI_GOOGLE_MOCK_URL", "http://127.0.0.1:8090", v); });
    read_apns(s, c);
    check_combinations(s, c);

    // A typo in the file is named before anything else: it may be why another setting looks wrong.
    if (const auto unknown = s.unread_file_key()) return std::unexpected(std::format("{}: unknown setting {}", file->string(), *unknown));
    if (auto& error = s.error()) return std::unexpected(std::move(*error));
    return c;
}

}  // namespace poni::core
