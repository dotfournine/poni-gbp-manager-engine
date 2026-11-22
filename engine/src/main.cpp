// poni-gbp-engine: the daemon, and nothing else (D-079). It serves the
// HTTP API and runs the scheduler and the job workers until it is told to
// stop. It has no subcommands:
//
//   - migrations and the knowledge base are poni-tools' (tools/); the engine
//     only checks at start that nothing is pending and that a knowledge base
//     is seeded;
//   - the two Google accounts are connected on the dashboard's status page
//     (http/connect.hpp).
//
//   poni-gbp-engine [--config FILE]
//   poni-gbp-engine --version

#include "connectors/gbp/local_posts.hpp"
#include "connectors/gbp/reader.hpp"
#include "connectors/gbp/performance.hpp"
#include "connectors/gbp/writer.hpp"
#include "connectors/google/auth.hpp"
#include "connectors/identity/provider.hpp"
#include "connectors/llm/net_client.hpp"
#include "connectors/notify/apns.hpp"
#include "connectors/notify/gmail.hpp"
#include "connectors/pubsub/subscriber.hpp"
#include "connectors/storage/store.hpp"
#include "core/config.hpp"
#include "core/log.hpp"
#include "core/rate_limiter.hpp"
#include "core/tls.hpp"
#include "core/version.hpp"
#include "db/database.hpp"
#include "db/migrations.hpp"
#include "db/repo_analysis.hpp"
#include "db/repo_google_target.hpp"
#include "db/repo_knowledge.hpp"
#include "db/repo_notifications.hpp"
#include "http/api.hpp"
#include "http/auth.hpp"
#include "http/client.hpp"
#include "http/connect.hpp"
#include "http/health.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "jobs/analysis.hpp"
#include "jobs/builtin.hpp"
#include "jobs/go_live.hpp"
#include "jobs/local_posts.hpp"
#include "jobs/notifications.hpp"
#include "jobs/post_advisor.hpp"
#include "jobs/posting.hpp"
#include "jobs/queue.hpp"
#include "jobs/reports.hpp"
#include "jobs/scheduler.hpp"
#include "jobs/sync.hpp"
#include "jobs/worker.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>

#include <csignal>
#include <cstdlib>
#include <deque>
#include <exception>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace asio = boost::asio;
using namespace poni;

namespace {

constexpr std::string_view kUsage = R"(Usage: poni-gbp-engine [--config FILE]
       poni-gbp-engine --version

Runs the engine: the HTTP API, the scheduler and the job workers, until it
is stopped (SIGINT or SIGTERM; a second one stops at once). It takes no
command.

  Migrations and the knowledge base   poni-tools migrate, poni-tools seed (tools/)
  The Google accounts                 "Connect" on the dashboard's status page

Settings are PONI_* environment variables; --config (or PONI_CONFIG) names an
optional KEY=VALUE file, which the environment overrides. Without either,
~/.config/poni-gbp/engine.env is used if it exists. See README.md.
)";

// Refuses to serve against a database that is behind the code, that has no
// knowledge base this engine can read (D-078), or that belongs to the other
// Google: the mock's or the real one (D-087). A database that can't be
// reached at start is fatal too, since which Google it belongs to can't be
// checked; in production Compose starts the engine after the database and
// restarts it.
bool database_ready(const core::Config& config) {
    std::optional<pqxx::connection> conn;
    try {
        conn.emplace(config.database_url);
    } catch (const std::exception& e) {
        log::error("start: the database can't be reached", {{"error", e.what()}});
        return false;
    }
    try {
        if (const auto problem = db::migrations_problem(*conn, config.migrations_dir)) {
            log::error("start: the database's migrations are not the engine's", {{"problem", *problem}});
            return false;
        }
        pqxx::work tx{*conn};
        auto problem = repo::knowledge_problem(tx);
        if (!problem) problem = repo::claim_google_target(tx, config.google_mock(), core::SystemClock{}.now());
        if (!problem) {
            tx.commit();
            return true;
        }
        log::error("start: " + *problem);
    } catch (const std::exception& e) {
        log::error("start: the check of the database at start failed", {{"error", e.what()}});
    }
    return false;
}

// Every outbound API's share of its quota, by limiter key. The Google ones
// are half of each project quota (spike Step A), which leaves room for
// anything else that uses the same Google Cloud project.
void configure_limits(core::RateLimiter& limiter) {
    const std::pair<std::string_view, core::RateLimiter::Limit> limits[] = {
        {gbp::kAccountsApi, {.per_minute = 150, .burst = 10}},
        {gbp::kLocationsApi, {.per_minute = 150, .burst = 10}},
        {gbp::kReviewsApi, {.per_minute = 300, .burst = 20}},
        // Google publishes no quota for v4 posts. Its limit for edits of one profile is 10 a minute.
        {gbp::kLocalPostsApi, {.per_minute = 60, .burst = 5}},
        {gbp::kPerformanceApi, {.per_minute = 150, .burst = 10}},
        // The refreshes of the engine's own tokens, connecting an account, and the metadata server.
        {google::kOAuthApi, {.per_minute = 30, .burst = 5}},
        // Sign-in's code exchange, apart from the refreshes (each attempt is also throttled per address).
        {identity::kSignInApi, {.per_minute = 30, .burst = 5}},
        {pubsub::kPubsubApi, {.per_minute = 120, .burst = 10}},
        {storage::kStorageApi, {.per_minute = 120, .burst = 10}},
        {notify::kGmailApi, {.per_minute = 60, .burst = 5}},
        // One slot per connection to Apple's push service, which carries one delivery's pushes.
        {notify::kApnsApi, {.per_minute = 120, .burst = 10}},
        {llm::kLlmApi, {.per_minute = 120, .burst = 10}},
    };
    for (const auto& [api, limit] : limits) limiter.configure(std::string(api), limit);
}

int run(const core::Config& config) {
    if (!database_ready(config)) return 1;

    // Encrypts and decrypts the stored tokens. Without the key the engine
    // still serves the dashboard; it just can't reach Google.
    const core::SecretBox* box = config.secret_box ? &*config.secret_box : nullptr;
    if (!box) log::warn("PONI_SECRET_KEY is not set: the stored GBP token can't be read, so nothing will sync");

    auto tls = core::make_tls_client_context();

    asio::io_context io;  // one thread: handlers and coroutines need no locking
    db::Database database(config.database_url, config.db_pool_size);
    const core::SystemClock clock;
    jobs::JobQueue queue(database, clock);

    // ---- outbound: Google ------------------------------------------------------
    std::optional<boost::urls::url> google_mock;
    if (config.google_mock()) {
        google_mock.emplace(config.google_mock_url);
        log::warn("PONI_GOOGLE_MOCK_URL is set: Google Business Profile, its tokens, the Performance API and Gmail are the local "
                  "mock, and every other Google service is refused",
                  {{"mock", config.google_mock_url}});
    }
    http::NetClient http_client(tls.ctx, google_mock);
    core::RateLimiter limiter(io.get_executor(), clock);
    configure_limits(limiter);

    // The GBP managing account: reviews, replies (D-061), local posts (D-088) and the Performance API.
    google::CachedTokenSource gbp_tokens(
        clock, google::refresh_fetch(http_client, limiter, http::stored_credentials(database, box, http::kConnectGbp),
                                     // Token health for the status page: every refresh, about once an hour.
                                     [&](std::optional<std::string> error) -> asio::awaitable<void> {
                                         co_await database.tx([error = std::move(error), now = clock.now()](pqxx::work& tx) {
                                             repo::record_gbp_token_health(tx, error, now);
                                         });
                                     }));
    const google::Api gbp_api{http_client, gbp_tokens, limiter};
    gbp::HttpReader gbp_reader(gbp_api);
    gbp::HttpReplyWriter gbp_writer(gbp_api);
    gbp::HttpLocalPostReader local_post_reader(gbp_api);
    gbp::HttpLocalPostWriter local_post_writer(gbp_api);
    gbp::HttpPerformanceReader performance_reader(gbp_api);

    // The sending mailbox has its own token (gmail.send only). Until someone
    // connects it on the status page and switches sending on in the
    // dashboard, every notification is logged as not sent.
    google::CachedTokenSource gmail_tokens(
        clock, google::refresh_fetch(http_client, limiter, http::stored_credentials(database, box, http::kConnectGmail)));
    notify::GmailChannel email_channel({http_client, gmail_tokens, limiter}, config.email_from_name, http::stored_mailbox_address(database));

    // Push to the phone app (D-097), with the APNs settings only.
    std::optional<notify::ApnsSender> apns;
    if (config.apns) {
        apns.emplace(http_client, limiter, clock, *config.apns);
        log::info("push is on", {{"environment", std::string(core::name(config.apns->environment))}});
    } else {
        log::info("push is off: no PONI_APNS_* settings");
    }

    // The engine's own identity, for Pub/Sub and the public bucket: the VM's
    // service account in production, or gcloud's credentials. Asked only once
    // either is used.
    const bool from_metadata = config.google_credentials == "metadata";
    google::CachedTokenSource service_tokens(clock, google::service_account_fetch(http_client, limiter, from_metadata));

    // Pub/Sub notifications: a second source next to polling, off unless a
    // subscription is configured.
    std::optional<pubsub::HttpSubscriber> subscriber;
    if (const auto& subscription = config.pubsub_subscription) {
        // User credentials: Pub/Sub wants to know which project to bill.
        std::optional<std::string> quota_project;
        if (!from_metadata) quota_project = subscription->first;
        subscriber.emplace(google::Api{http_client, service_tokens, limiter}, *subscription, quota_project);
    }

    // The public bucket takes shared report pages (D-085) and the media of
    // local posts. A folder stands in for it on a developer's machine.
    std::unique_ptr<storage::ObjectStore> public_store;
    if (storage::is_folder_setting(config.public_bucket)) {
        public_store = std::make_unique<storage::FolderStore>(config.public_bucket);
    } else if (!config.public_bucket.empty()) {
        public_store = std::make_unique<storage::GcsStore>(google::Api{http_client, service_tokens, limiter}, config.public_bucket);
    } else {
        log::warn("PONI_PUBLIC_BUCKET is not set: reports can't be shared or sent");
    }

    // ---- outbound: the LLM -----------------------------------------------------
    // Without a provider the engine runs and analyzes nothing.
    llm::NetClient llm_client(http_client, clock, limiter,
                              {.providers = config.llm_providers,
                               .timeout = config.llm_timeout,
                               .max_attempts = static_cast<int>(config.llm_max_attempts)},
                              // Every request to the model becomes a row in llm_calls, failed attempts included.
                              [&](llm::CallRecord call) -> asio::awaitable<void> {
                                  co_await database.tx([call = std::move(call)](pqxx::work& tx) { repo::record_llm_call(tx, call); });
                              });
    if (llm_client.providers().empty()) {
        log::warn("no model provider is configured (PONI_LLM_<NAME>_BASE_URL and PONI_LLM_<NAME>_API_KEY): reviews are not analyzed");
    }

    // ---- jobs ------------------------------------------------------------------
    jobs::Sync sync(database, clock, gbp_reader, queue,
                    {.discovery_interval = config.location_discovery_interval,
                     .poll_tick = config.review_poll_tick,
                     .full_sync_interval = config.review_full_sync_interval,
                     .notification_pull_interval = config.pubsub_pull_interval});
    jobs::ReviewPipeline pipeline(database, clock, llm_client, queue, {.tick = config.analysis_tick});
    jobs::ReplyPoster poster(database, clock, gbp_reader, gbp_writer, {.confirm_window = config.post_confirm_window});
    jobs::LocalPosts local_posts(database, clock, local_post_reader, local_post_writer, queue,
                                 {.media_base = public_store ? public_store->base_url() : std::string{}});
    jobs::GoLiveTester go_live_tester(database, clock, llm_client);
    jobs::Notifier notifier(database, clock, {.public_url = config.public_url, .lifetimes = http::session_lifetimes(config)});
    notifier.add_channel(email_channel);
    if (apns) notifier.set_push(*apns);
    jobs::Reporter reporter(database, clock, performance_reader, llm_client, public_store.get());
    // Ideas and texts for local posts, when a person asks (D-088 point 11). Nothing schedules it.
    jobs::PostAdvisor post_advisor(database, clock, llm_client, performance_reader);

    jobs::Registry registry;
    jobs::install_builtin(registry, database, queue,
                          {.heartbeat_interval = config.heartbeat_interval,
                           .job_retention = std::chrono::hours(24) * config.job_retention_days,
                           .sessions = http::session_lifetimes(config)});
    jobs::install(registry, sync, subscriber ? &*subscriber : nullptr);
    jobs::install(registry, pipeline);
    jobs::install(registry, poster);
    jobs::install(registry, local_posts, config.google_mock());
    jobs::install(registry, go_live_tester);
    jobs::install(registry, notifier);
    jobs::install(registry, reporter);

    // ---- inbound: the dashboard's API ------------------------------------------
    std::optional<identity::GoogleProvider> google_signin;
    if (!config.google_signin_client_id.empty() && !config.google_signin_client_secret.empty()) {
        google_signin.emplace(http_client, clock, limiter, config.google_signin_client_id, config.google_signin_client_secret,
                              config.signin_domains);
    } else {
        log::warn("Google sign-in is not configured (PONI_GOOGLE_SIGNIN_CLIENT_ID / _SECRET): staff can't sign in");
    }
    if (!config.dev_login_email.empty()) {
        log::warn("development sign-in is ON: anyone who can reach this engine can sign in without Google",
                  {{"email", config.dev_login_email}});
    }
    http::AuthService auth(database, clock, google_signin ? &*google_signin : nullptr, config);

    // Connecting the GBP managing account and the sending mailbox, from the status page.
    // The token sources cache an access token for about an hour: without the invalidate the
    // engine would go on as the account that was replaced until it ran out.
    http::ConnectService connect(database, clock, http_client, limiter, config, [&](std::string_view account) {
        (account == http::kConnectGmail ? gmail_tokens : gbp_tokens).invalidate();
    });
    if (!connect.configured()) {
        log::warn("Google accounts can't be connected from the dashboard: set PONI_GBP_OAUTH_CLIENT_ID, PONI_GBP_OAUTH_CLIENT_SECRET "
                  "and PONI_SECRET_KEY");
    }

    http::Router router;
    router.set_auth_hook(auth.hook());
    router.trust_forwarded_for(config.trust_forwarded_for);
    router.add(boost::beast::http::verb::get, "/api/health", {http::Access::anonymous, "system.health"},
               http::make_health_handler(database));
    auth.add_routes(router);
    connect.add_routes(router);
    http::add_api_routes(router, database, clock,
                         {.pubsub_enabled = subscriber.has_value(),
                          .llm_providers = llm_client.providers(),
                          .job_workers = static_cast<int>(std::max(config.job_workers, 1u)),
                          .connect_configured = connect.configured(),
                          .google_mock = config.google_mock(),
                          .push_environment = config.apns ? std::optional(config.apns->environment) : std::nullopt},
                         pipeline, reporter, public_store.get(), post_advisor);

    std::optional<http::Server> server;
    try {
        server.emplace(io.get_executor(), router, config.http_host, config.http_port);
    } catch (const boost::system::system_error& e) {
        // code().message(), not what(): what() adds a source location that is noise in a log.
        log::error("start: cannot listen",
                   {{"host", config.http_host}, {"port", config.http_port}, {"error", e.code().message()}});
        return 1;
    }
    server->start();

    // The signal wait would keep io.run() going for ever, so it is cancelled
    // once the last worker or scheduler loop has ended.
    asio::signal_set signals(io, SIGINT, SIGTERM);
    std::size_t loops_running = config.job_workers + 1;
    const auto loop_done = [&](std::exception_ptr) {
        if (--loops_running == 0) signals.cancel();
    };

    // A job given up on, by a worker or because its lock expired on its last attempt (FR-7).
    const jobs::FailureHook job_failed = [&](jobs::Job job, std::string error) -> asio::awaitable<void> {
        co_await database.tx([job = std::move(job), error = std::move(error), now = clock.now()](pqxx::work& tx) {
            repo::emit_job_failed(tx, job, error, now);
        });
    };
    std::deque<jobs::Worker> workers;  // deque: Worker is not movable
    for (unsigned i = 0; i < config.job_workers; ++i) {
        workers.emplace_back(io.get_executor(), queue, registry,
                             jobs::Worker::Options{.id = std::format("worker-{}", i),
                                                   .poll_interval = config.job_poll_interval,
                                                   .on_permanent_failure = job_failed});
        asio::co_spawn(io, workers.back().run(), loop_done);
    }
    jobs::Scheduler scheduler(io.get_executor(), queue, registry.recurring(),
                              {.job_lock_timeout = config.job_lock_timeout, .on_permanent_failure = job_failed});
    asio::co_spawn(io, scheduler.run(), loop_done);

    // First signal: stop accepting work and let what's running finish.
    // Second signal: stop now.
    bool stopping = false;
    std::function<void(const boost::system::error_code&, int)> on_signal =
        [&](const boost::system::error_code& ec, int signal) {
            if (ec) return;
            if (stopping) {
                log::warn("second signal: stopping immediately", {{"signal", signal}});
                io.stop();
                return;
            }
            stopping = true;
            log::info("shutting down", {{"signal", signal}});
            server->stop();
            scheduler.stop();
            for (auto& worker : workers) worker.stop();
            signals.async_wait(on_signal);
        };
    signals.async_wait(on_signal);

    log::info("engine started", {{"version", core::kVersion},
                                 {"listen", std::format("{}:{}", config.http_host, server->port())},
                                 {"job_workers", config.job_workers},
                                 {"public_url", config.public_url},
                                 {"google_sign_in", google_signin.has_value()},
                                 {"pubsub", subscriber.has_value()},
                                 {"llm_providers", static_cast<std::int64_t>(llm_client.providers().size())},
                                 {"ca_bundle", tls.ca_source}});

    io.run();
    database.join();
    log::info("engine stopped");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::optional<std::filesystem::path> config_file;
    const std::span<char*> args(argv, static_cast<std::size_t>(argc));
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--config") {
            if (++i >= args.size()) {
                std::println(stderr, "--config needs a file name\n\n{}", kUsage);
                return 2;
            }
            config_file = args[i];
        } else if (arg == "--help" || arg == "-h") {
            std::print("{}", kUsage);
            return 0;
        } else if (arg == "--version") {
            std::println("poni-gbp-engine {}", core::kVersion);
            return 0;
        } else {
            std::println(stderr, "poni-gbp-engine takes no command ('{}'): it is the daemon.\n\n{}", arg, kUsage);
            return 2;
        }
    }

    const auto env = core::process_environment();
    if (!config_file) config_file = core::default_config_file(env);
    const auto config = core::load_config(env, config_file);
    if (!config) {
        std::println(stderr, "configuration error: {}", config.error());
        return 2;
    }
    log::init(config->log_level, config->log_format);

    // libpq waits for a dead host for minutes by default.
    ::setenv("PGCONNECT_TIMEOUT", "5", 0);

    try {
        return run(*config);
    } catch (const std::exception& e) {
        log::error("fatal", {{"error", e.what()}});
        return 1;
    }
}
