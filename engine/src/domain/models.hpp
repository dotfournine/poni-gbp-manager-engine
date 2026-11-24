// The engine's entities as the API returns them. Each struct must match the
// schema of the same name in api/openapi.yaml: Boost.Describe turns it into
// JSON as is. Timestamps are RFC 3339 strings in UTC; a std::optional that is
// empty becomes JSON null.
#pragma once

#include "domain/industry.hpp"
#include "domain/local_post.hpp"
#include "domain/paged.hpp"
#include "domain/post_help.hpp"
#include "domain/reply_checks.hpp"
#include "domain/reply_settings.hpp"
#include "domain/review_pipeline.hpp"

#include <boost/describe/class.hpp>
#include <boost/json/value.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace poni::model {

struct User {
    std::int64_t id = 0;
    std::string email;
    std::string name;
    std::optional<std::string> picture_url;
};
BOOST_DESCRIBE_STRUCT(User, (), (id, email, name, picture_url))

struct SignInMethods {
    bool google = false;
    bool dev = false;
    // The Workspace domains whose accounts may sign in with Google.
    std::vector<std::string> domains;
};
BOOST_DESCRIBE_STRUCT(SignInMethods, (), (google, dev, domains))

struct Session {
    bool authenticated = false;
    std::optional<User> user;
    std::optional<std::string> csrf_token;
    SignInMethods sign_in;
    // The engine works against the local mock of Google, on made-up data (D-087).
    bool google_mock = false;
};
BOOST_DESCRIBE_STRUCT(Session, (), (authenticated, user, csrf_token, sign_in, google_mock))

// What the phone app gets for signing in (D-095): its bearer token, given this once.
struct AppToken {
    std::string token;
    // When the token ends at the latest; it ends earlier when it goes unused.
    std::string expires_at;
    Session session;
};
BOOST_DESCRIBE_STRUCT(AppToken, (), (token, expires_at, session))

struct Client {
    std::int64_t id = 0;
    std::string name;
    std::string notes;
    // A group of locations for bookkeeping only (D-102): archiving changes nothing about them.
    bool archived = false;
    std::int64_t location_count = 0;
    // The locations linked to it, in id order.
    std::vector<std::int64_t> location_ids;
    std::string created_at;
    std::string updated_at;
};
BOOST_DESCRIBE_STRUCT(Client, (), (id, name, notes, archived, location_count, location_ids, created_at, updated_at))

using ClientList = Paged<Client>;

struct Location {
    std::int64_t id = 0;
    std::string gbp_location_id;
    std::string gbp_account_id;
    // The client it is grouped under: bookkeeping only (D-102).
    std::optional<std::int64_t> client_id;
    std::optional<std::string> client_name;
    // Whether the engine works on the location (D-102); off at discovery, switched by a person only.
    bool enabled = false;
    // The industry whose profile its reviews are analyzed with; none, or one the
    // knowledge base does not have (Knowledge.industries): not analyzed.
    std::optional<std::string> industry_key;
    // "YYYY-MM-DD" (UTC): only reviews written on or after this day are analyzed
    // and drafted on their own; none: every review.
    std::optional<std::string> analyze_from;
    // Who the customers are, in a line written by staff. The model reads it when a person
    // asks it for ideas and texts for local posts (D-088).
    std::string audience;
    std::string title;
    std::optional<std::string> store_code;
    std::optional<std::string> primary_category;
    std::optional<std::string> address;
    std::optional<std::string> locality;
    std::optional<std::string> administrative_area;
    std::optional<std::string> postal_code;
    std::optional<std::string> region_code;
    std::optional<std::string> phone;
    std::optional<std::string> website_uri;
    std::optional<std::string> maps_uri;
    std::string access_status;  // "active" or "lost"
    std::optional<std::string> access_lost_at;
    std::string first_seen_at;
    std::string last_seen_at;
    std::int64_t review_poll_interval_s = 0;
    // The location's kill switch for posting; the global one must be on too.
    bool posting_enabled = true;
    std::int64_t review_count = 0;
    std::int64_t unreplied_count = 0;
    std::optional<double> average_rating;
    std::optional<std::string> reviews_polled_at;
    std::optional<std::string> reviews_full_synced_at;
    std::optional<std::string> last_sync_error;
    std::optional<std::string> last_sync_error_at;
    // The read of the location's local posts (D-088 point 8).
    std::optional<std::string> local_posts_synced_at;
    std::optional<std::string> local_posts_sync_error;
    std::optional<std::string> local_posts_sync_error_at;
    // The one time zone of the location, for every purpose (D-071, D-098); nothing until a person entered it.
    std::optional<std::string> time_zone;
};
BOOST_DESCRIBE_STRUCT(Location, (),
                      (id, gbp_location_id, gbp_account_id, client_id, client_name, enabled, industry_key, analyze_from, audience,
                       title, store_code, primary_category,
                       address, locality, administrative_area, postal_code, region_code, phone, website_uri, maps_uri,
                       access_status, access_lost_at, first_seen_at, last_seen_at, review_poll_interval_s,
                       posting_enabled, review_count,
                       unreplied_count, average_rating, reviews_polled_at, reviews_full_synced_at, last_sync_error,
                       last_sync_error_at, local_posts_synced_at, local_posts_sync_error, local_posts_sync_error_at,
                       time_zone))

using LocationList = Paged<Location>;

// A person's decision that a review needs no reply (D-068).
struct ReviewIgnore {
    std::string by;  // email
    std::string at;
    std::string reason;  // may be empty
};
BOOST_DESCRIBE_STRUCT(ReviewIgnore, (), (by, at, reason))

struct ReviewReply {
    std::string text;
    std::optional<std::string> updated_time;
    std::optional<std::string> state;
    std::optional<std::string> origin;  // "external" or "system"
};
BOOST_DESCRIBE_STRUCT(ReviewReply, (), (text, updated_time, state, origin))

// What the analyzer found in one version of a review.
struct ReviewAnalysis {
    std::int64_t id = 0;
    std::string status;  // "ok" or "failed"
    std::optional<std::string> error;
    std::string language;
    std::string script;
    std::string sentiment;
    bool contains_complaint = false;
    std::vector<domain::TopicMention> topics;
    std::vector<domain::ItemMention> items;
    std::vector<domain::RaisedFlag> severity_flags;
    bool manipulation_suspected = false;
    std::string manipulation_evidence;
    std::string summary_en;
    std::string industry_key;
    std::optional<std::string> model;
    std::string prompt_version;
    // Whether the review is still the version that was analyzed.
    bool is_current = false;
    std::string created_at;
};
BOOST_DESCRIBE_STRUCT(ReviewAnalysis, (),
                      (id, status, error, language, script, sentiment, contains_complaint, topics, items, severity_flags,
                       manipulation_suspected, manipulation_evidence, summary_en, industry_key, model,
                       prompt_version, is_current, created_at))

// A drafted reply and what the policy decided to do with it.
// One attempt of the engine to put a reply on Google.
struct ReplyPost {
    std::int64_t id = 0;
    std::string status;  // "posting", "posted", "published", "failed", "unconfirmed" or "superseded"
    std::string started_at;
    std::optional<std::string> posted_at;
    std::optional<std::string> published_at;
    std::optional<std::string> error;
};
BOOST_DESCRIBE_STRUCT(ReplyPost, (), (id, status, started_at, posted_at, published_at, error))

struct ReplyDraft {
    std::int64_t id = 0;
    std::int64_t review_id = 0;
    // "draft", "scheduled", "approved", "posting", "posted", "published", "rejected", "cancelled" or "superseded"
    std::string status;
    std::optional<std::string> status_reason;
    std::string route;  // "auto_post", "approval_queue" or "escalate"
    std::vector<std::string> route_reasons;
    std::string mode;
    std::string text;
    std::string language;
    std::optional<std::string> script;
    bool generator_manipulation_suspected = false;
    std::string notes_for_staff;
    std::vector<domain::CheckResult> checks;
    std::string model;
    std::string prompt_version;
    // What shaped the prompt. Left out (null) where drafts are listed with reviews.
    boost::json::value inputs;
    // The reply as the model wrote it; `text` may be a person's edit of it.
    std::string generated_text;
    std::optional<std::string> instructions;
    // Emails: who asked for the draft, who edited it, who approved or rejected it.
    std::optional<std::string> created_by;
    std::optional<std::string> edited_by;
    std::optional<std::string> edited_at;
    std::optional<std::string> decided_by;
    std::optional<std::string> decided_at;
    // The newest attempt to post it, if it was approved or auto-posted.
    std::optional<ReplyPost> post;
    // A draft the policy scheduled for posting without a person (D-069):
    // when it goes out, and the version of the rules that decided it.
    std::optional<std::string> post_at;
    std::optional<std::string> policy_version;
    std::string created_at;
    std::string updated_at;
};
BOOST_DESCRIBE_STRUCT(ReplyDraft, (),
                      (id, review_id, status, status_reason, route, route_reasons, mode, text,
                       language, script, generator_manipulation_suspected, notes_for_staff,
                       checks, model, prompt_version, inputs, generated_text, instructions, created_by, edited_by,
                       edited_at, decided_by, decided_at, post, post_at, policy_version, created_at, updated_at))

struct ReplyDraftList {
    std::vector<ReplyDraft> items;
};
BOOST_DESCRIBE_STRUCT(ReplyDraftList, (), (items))

struct Review {
    std::int64_t id = 0;
    std::int64_t location_id = 0;
    std::string location_title;
    std::optional<std::int64_t> client_id;
    std::optional<std::string> client_name;
    std::optional<std::int64_t> rating;
    std::optional<std::string> comment;
    std::optional<std::string> original_text;
    std::optional<std::string> translated_text;
    std::optional<std::string> language;
    std::optional<std::string> reviewer_name;
    std::optional<std::string> reviewer_photo_url;
    bool reviewer_anonymous = false;
    std::string created_time;
    std::string updated_time;
    bool has_reply = false;
    std::optional<ReviewReply> reply;
    std::string status;  // "active" or "removed"
    std::optional<std::string> removed_at;
    std::string first_seen_at;
    std::string last_seen_at;
    // Set while a person's "no reply needed" stands.
    std::optional<ReviewIgnore> ignored;
    // The newest analysis, if the review has been analyzed.
    std::optional<ReviewAnalysis> analysis;
    // The newest draft, whatever its status, if one was written.
    std::optional<ReplyDraft> draft;
};
BOOST_DESCRIBE_STRUCT(Review, (),
                      (id, location_id, location_title, client_id, client_name, rating, comment,
                       original_text, translated_text, language, reviewer_name, reviewer_photo_url, reviewer_anonymous,
                       created_time, updated_time, has_reply, reply, status, removed_at, first_seen_at, last_seen_at,
                       ignored, analysis, draft))

using ReviewList = Paged<Review>;

struct ReviewEvent {
    std::int64_t id = 0;
    std::string kind;
    std::string source;
    std::string occurred_at;
    boost::json::value detail;
};
BOOST_DESCRIBE_STRUCT(ReviewEvent, (), (id, kind, source, occurred_at, detail))

struct ReviewEventList {
    std::vector<ReviewEvent> items;
};
BOOST_DESCRIBE_STRUCT(ReviewEventList, (), (items))

struct AuditEntry {
    std::int64_t id = 0;
    std::string occurred_at;
    std::string actor_type;
    std::string actor;
    std::string action;
    std::string object_type;
    std::string object_id;
    std::optional<std::int64_t> location_id;  // D-103
    std::optional<std::string> location_title;
    boost::json::value before;
    boost::json::value after;
    std::optional<std::string> ip;
};
BOOST_DESCRIBE_STRUCT(AuditEntry, (),
                      (id, occurred_at, actor_type, actor, action, object_type, object_id, location_id, location_title, before,
                       after, ip))

using AuditList = Paged<AuditEntry>;

struct DatabaseStatus {
    bool reachable = false;
};
BOOST_DESCRIBE_STRUCT(DatabaseStatus, (), (reachable))

struct GbpStatus {
    // A refresh token is stored. Whether Google still accepts it shows in the other fields.
    bool connected = false;
    // The connected account's address, as its connection named it (D-100); none before it was kept.
    std::optional<std::string> account_email;
    std::optional<std::string> last_ok_at;
    std::optional<std::string> last_error;
    std::optional<std::string> last_error_at;
};
BOOST_DESCRIBE_STRUCT(GbpStatus, (), (connected, account_email, last_ok_at, last_error, last_error_at))

struct IntegrationStatus {
    std::optional<std::string> last_ok_at;
    std::optional<std::string> last_error;
    std::optional<std::string> last_error_at;
};
BOOST_DESCRIBE_STRUCT(IntegrationStatus, (), (last_ok_at, last_error, last_error_at))

struct PubsubStatus {
    bool enabled = false;
    std::optional<std::string> last_ok_at;
    std::optional<std::string> last_error;
    std::optional<std::string> last_error_at;
    std::optional<std::string> last_message_at;
};
BOOST_DESCRIBE_STRUCT(PubsubStatus, (), (enabled, last_ok_at, last_error, last_error_at, last_message_at))

struct LocationSyncStatus {
    std::int64_t id = 0;
    std::string title;
    std::optional<std::string> client_name;
    std::string access_status;
    // Enabled and accessible, so its reviews are being synced.
    bool syncing = false;
    std::int64_t review_count = 0;
    std::optional<std::string> reviews_polled_at;
    std::optional<std::string> reviews_full_synced_at;
    std::optional<std::string> last_sync_error;
    std::optional<std::string> last_sync_error_at;
};
BOOST_DESCRIBE_STRUCT(LocationSyncStatus, (),
                      (id, title, client_name, access_status, syncing, review_count, reviews_polled_at,
                       reviews_full_synced_at, last_sync_error, last_sync_error_at))

struct JobFailure {
    std::int64_t id = 0;
    std::string kind;
    std::int64_t attempts = 0;
    std::optional<std::string> last_error;
    std::optional<std::string> failed_at;
};
BOOST_DESCRIBE_STRUCT(JobFailure, (), (id, kind, attempts, last_error, failed_at))

struct JobsStatus {
    std::int64_t queued = 0;
    std::int64_t running = 0;
    std::int64_t failed = 0;
    // The most recent permanently failed jobs, newest first.
    std::vector<JobFailure> failures;
};
BOOST_DESCRIBE_STRUCT(JobsStatus, (), (queued, running, failed, failures))

// The analysis pipeline on the status page.
struct AiStatus {
    // The providers both tasks name have an address and a key.
    bool llm_configured = false;
    // The providers that have an address and a key (D-059).
    std::vector<std::string> providers;
    bool enabled = false;
    std::string analyzer_provider;
    std::string analyzer_model;
    std::string generator_provider;
    std::string generator_model;
    std::optional<std::string> last_ok_at;
    std::optional<std::string> last_error;
    std::optional<std::string> last_error_at;
    // Reviews of analyzed locations that still lack an analysis, or a draft when they have no reply.
    std::int64_t waiting_reviews = 0;
    std::int64_t analyzed_reviews = 0;
};
BOOST_DESCRIBE_STRUCT(AiStatus, (),
                      (llm_configured, providers, enabled, analyzer_provider, analyzer_model, generator_provider, generator_model, last_ok_at,
                       last_error, last_error_at,
                       waiting_reviews, analyzed_reviews))

// Reply posting, for the status page.
struct PostingStatus {
    bool enabled = false;
    std::int64_t paused_locations = 0;
    std::int64_t drafts_waiting = 0;
    // Approved or sent, and not yet seen on Google.
    std::int64_t in_flight = 0;
    std::int64_t published = 0;
    std::optional<std::string> last_posted_at;
    std::int64_t open_escalations = 0;
};
BOOST_DESCRIBE_STRUCT(PostingStatus, (),
                      (enabled, paused_locations, drafts_waiting, in_flight, published, last_posted_at, open_escalations))

// The email channel, for the status page.
struct EmailStatus {
    bool sending_enabled = false;
    // The sending account has signed in (a credential is stored).
    bool connected = false;
    // The mailbox's address, as its connection named it (D-100); email is sent from it.
    std::optional<std::string> account_email;
    std::optional<std::string> last_ok_at;
    std::optional<std::string> last_error;
    std::optional<std::string> last_error_at;
    std::int64_t pending = 0;
    std::int64_t failed = 0;  // in the last 7 days
};
BOOST_DESCRIBE_STRUCT(EmailStatus, (),
                      (sending_enabled, connected, account_email, last_ok_at, last_error, last_error_at, pending, failed))

// Which knowledge base is seeded (FR-13); empty strings when none is.
struct KnowledgeStatus {
    std::string version;
    std::string seeded_at;
};
BOOST_DESCRIBE_STRUCT(KnowledgeStatus, (), (version, seeded_at))

struct SystemStatus {
    std::string version;
    DatabaseStatus database;
    KnowledgeStatus knowledge;
    // The engine can connect a Google account from the dashboard (D-079); filled in by the API.
    bool connect_configured = false;
    GbpStatus gbp;
    IntegrationStatus location_discovery;
    PubsubStatus pubsub;
    AiStatus ai;
    PostingStatus posting;
    EmailStatus email;
    std::vector<LocationSyncStatus> locations;
    JobsStatus jobs;
};
BOOST_DESCRIBE_STRUCT(SystemStatus, (),
                      (version, database, knowledge, connect_configured, gbp, location_discovery, pubsub, ai, posting, email, locations, jobs))

// ---- analysis settings -----------------------------------------------------------

// What a location's replies are written with: its own complete settings (D-093).
struct ReplySettings {
    std::int64_t location_id = 0;
    std::string location_title;
    // The client the location is grouped under, for display only.
    std::optional<std::int64_t> client_id;
    std::optional<std::string> client_name;
    std::optional<std::string> industry_key;
    domain::Voice voice;
    domain::Localization localization;
    domain::PolicyRules policy;
    std::string updated_at;
};
BOOST_DESCRIBE_STRUCT(ReplySettings, (),
                      (location_id, location_title, client_id, client_name, industry_key, voice, localization, policy, updated_at))

// USD per million tokens.
struct ModelPrice {
    double input_per_mtok = 0;
    double output_per_mtok = 0;
};
BOOST_DESCRIBE_STRUCT(ModelPrice, (), (input_per_mtok, output_per_mtok))

// ---- the knowledge base, read-only (FR-13) ---------------------------------------

// An industry's test corpus in numbers.
struct KnowledgeCorpusSummary {
    std::string version;
    std::int64_t severity_cases = 0;
    std::int64_t manipulation_cases = 0;
    std::int64_t control_cases = 0;
};
BOOST_DESCRIBE_STRUCT(KnowledgeCorpusSummary, (), (version, severity_cases, manipulation_cases, control_cases))

// A supported industry: its pack was complete when it was seeded.
struct KnowledgeIndustry {
    std::string key;
    std::string label;
    std::vector<domain::Topic> topics;
    std::vector<domain::ItemType> item_types;
    // The industry's own flags, switched-off ones included.
    std::vector<domain::SeverityFlag> severity_flags;
    // The made-up business the test suites run as.
    std::string test_client_name;
    KnowledgeCorpusSummary corpus;
    // Enabled locations that name this industry.
    std::int64_t location_count = 0;
};
BOOST_DESCRIBE_STRUCT(KnowledgeIndustry, (),
                      (key, label, topics, item_types, severity_flags, test_client_name, corpus, location_count))

struct KnowledgeVoiceTemplate {
    std::string key;
    std::string label;
    std::string description;
    // The fields the template sets (a VoiceLayer).
    boost::json::value voice;
};
BOOST_DESCRIBE_STRUCT(KnowledgeVoiceTemplate, (), (key, label, description, voice))

// What the first location of a client starts with, and the default of every analysis setting.
struct KnowledgeDefaults {
    boost::json::value voice;
    boost::json::value localization;
    boost::json::value policy;
    boost::json::value analysis;
};
BOOST_DESCRIBE_STRUCT(KnowledgeDefaults, (), (voice, localization, policy, analysis))

struct Knowledge {
    // A hash of the files' content, and when it was seeded.
    std::string version;
    std::int64_t format = 0;
    std::string seeded_at;
    std::vector<KnowledgeIndustry> industries;
    // The severity flags of every industry.
    std::vector<domain::SeverityFlag> common_severity_flags;
    std::vector<KnowledgeVoiceTemplate> voice_templates;
    KnowledgeDefaults defaults;
    std::map<std::string, ModelPrice> model_prices;
};
BOOST_DESCRIBE_STRUCT(Knowledge, (),
                      (version, format, seeded_at, industries, common_severity_flags, voice_templates, defaults, model_prices))

// One synthetic review of a corpus and what is expected of it.
struct KnowledgeCorpusCase {
    std::string id;
    std::string category;  // "severity", "manipulation" or "control"
    std::int64_t stars = 0;
    std::string lang;
    std::string text;
    // At least one of these flags must be raised.
    std::vector<std::string> severity_any;
    bool manipulation = false;
    // The route the gate must give when the models behave.
    std::string action;
};
BOOST_DESCRIBE_STRUCT(KnowledgeCorpusCase, (), (id, category, stars, lang, text, severity_any, manipulation, action))

struct KnowledgeCorpus {
    std::string industry_key;
    std::string version;
    std::vector<KnowledgeCorpusCase> cases;
};
BOOST_DESCRIBE_STRUCT(KnowledgeCorpus, (), (industry_key, version, cases))

struct AiSettings {
    bool enabled = false;
    // The configured provider each task uses (D-059), and its model there.
    std::string analyzer_provider;
    std::string analyzer_model;
    std::string generator_provider;
    std::string generator_model;
    // The judge of a go-live test (D-074): a third task with its own provider and model.
    std::string judge_provider;
    std::string judge_model;
    // The summary of a report (D-085): a fourth task.
    std::string summary_provider;
    std::string summary_model;
    // Ideas and texts for local posts (D-088): a fifth task, asked only when a person presses a button.
    std::string posts_provider;
    std::string posts_model;
    // The writer's temperature is in a location's voice (Voice::temperature).
    double analyzer_temperature = 0;
    // Off: requests of that task carry no temperature, for a model that refuses one.
    bool analyzer_send_temperature = true;
    bool generator_send_temperature = true;
    std::int64_t max_output_tokens = 0;
    // By provider name: extra members for every request to that provider.
    boost::json::value request_extras;
    std::int64_t batch_size = 0;
    // Of the prompts built into this engine; read-only.
    std::string prompt_version;
    // The providers that have an address and a key; read-only, filled in by the API.
    std::vector<std::string> providers;
    std::string updated_at;
};
BOOST_DESCRIBE_STRUCT(AiSettings, (),
                      (enabled, analyzer_provider, analyzer_model, generator_provider, generator_model, judge_provider, judge_model,
                       summary_provider, summary_model, posts_provider, posts_model,
                       analyzer_temperature, analyzer_send_temperature, generator_send_temperature, max_output_tokens,
                       request_extras, batch_size, prompt_version, providers, updated_at))

// ---- LLM usage -------------------------------------------------------------------

struct LlmUsageRow {
    // The day ("2026-11-11"), the model, the task, the provider or the location's id, by grouping.
    std::string key;
    std::string label;
    std::int64_t calls = 0;
    std::int64_t errors = 0;
    std::int64_t prompt_tokens = 0;
    std::int64_t completion_tokens = 0;
    // Estimated from the knowledge base's model prices at the time of each call.
    double cost_usd = 0;
    // Calls of a model without a price: not part of cost_usd.
    std::int64_t unpriced_calls = 0;
    std::int64_t average_latency_ms = 0;
};
BOOST_DESCRIBE_STRUCT(LlmUsageRow, (),
                      (key, label, calls, errors, prompt_tokens, completion_tokens, cost_usd, unpriced_calls,
                       average_latency_ms))

struct LlmUsage {
    std::string from;
    std::string to;
    LlmUsageRow total;
    std::vector<LlmUsageRow> by_day;
    std::vector<LlmUsageRow> by_model;
    std::vector<LlmUsageRow> by_task;
    std::vector<LlmUsageRow> by_location;
    std::vector<LlmUsageRow> by_provider;
};
BOOST_DESCRIBE_STRUCT(LlmUsage, (), (from, to, total, by_day, by_model, by_task, by_location, by_provider))

struct LlmCall {
    std::int64_t id = 0;
    std::string occurred_at;
    std::string task;
    // Null for calls made before providers were named.
    std::optional<std::string> provider;
    std::string model;
    std::optional<std::int64_t> location_id;
    std::optional<std::int64_t> review_id;
    std::int64_t attempt = 0;
    std::string status;  // "ok" or "error"
    std::optional<std::string> error;
    std::optional<std::int64_t> http_status;
    std::int64_t prompt_tokens = 0;
    std::int64_t completion_tokens = 0;
    std::int64_t latency_ms = 0;
    std::optional<double> estimated_cost_usd;
};
BOOST_DESCRIBE_STRUCT(LlmCall, (),
                      (id, occurred_at, task, provider, model, location_id, review_id, attempt, status, error,
                       http_status, prompt_tokens, completion_tokens, latency_ms, estimated_cost_usd))

using LlmCallList = Paged<LlmCall>;

// ---- escalations, notifications, the switches ----------------------------

struct UserList {
    std::vector<User> items;
};
BOOST_DESCRIBE_STRUCT(UserList, (), (items))

// A review that needs a person: the simplest form of a case (D-054).
struct Escalation {
    std::int64_t id = 0;
    std::int64_t review_id = 0;
    std::string status;  // "open" or "closed"
    std::vector<domain::RaisedFlag> severity_flags;
    std::optional<User> assignee;
    std::optional<std::string> assigned_at;
    std::optional<std::string> close_reason;
    std::string close_note;
    std::optional<std::string> closed_by;  // email
    std::optional<std::string> closed_at;
    std::string opened_at;
    std::string updated_at;
    Review review;
};
BOOST_DESCRIBE_STRUCT(Escalation, (),
                      (id, review_id, status, severity_flags, assignee, assigned_at, close_reason, close_note,
                       closed_by, closed_at, opened_at, updated_at, review))

using EscalationList = Paged<Escalation>;

struct NotificationRule {
    std::int64_t id = 0;
    std::string event_type;  // "escalation", "approval_needed", "system_problem" or "location_report"
    std::optional<std::int64_t> location_id;  // nothing: every location
    std::optional<std::string> location_title;
    std::string channel;
    std::vector<std::string> recipients;
    bool enabled = true;
    std::string created_at;
    std::string updated_at;
};
BOOST_DESCRIBE_STRUCT(NotificationRule, (),
                      (id, event_type, location_id, location_title, channel, recipients, enabled, created_at, updated_at))

using NotificationRuleList = Paged<NotificationRule>;

// A phone that gets pushes (D-097): the app session that registered it.
struct Device {
    std::int64_t id = 0;
    std::string environment;  // the engine's APNs environment
    std::string app_version;
    std::string created_at;
    std::string last_seen_at;  // the session's last request
};
BOOST_DESCRIBE_STRUCT(Device, (), (id, environment, app_version, created_at, last_seen_at))

// One message to one recipient, as the delivery log shows it.
struct NotificationDelivery {
    std::int64_t id = 0;
    std::string event_type;
    std::optional<std::int64_t> location_id;
    std::optional<std::int64_t> review_id;
    std::optional<std::int64_t> escalation_id;
    // The report the email carried, and the email of the person who asked for that send (nothing: its schedule) (D-104).
    std::optional<std::int64_t> report_id;
    std::optional<std::string> requested_by;
    std::string channel;
    std::string recipient;
    std::string subject;
    std::string body;
    std::string status;  // "pending", "sent", "failed" or "not_sent"
    std::optional<std::string> status_reason;
    std::int64_t attempts = 0;
    std::string created_at;
    std::optional<std::string> sent_at;
};
BOOST_DESCRIBE_STRUCT(NotificationDelivery, (),
                      (id, event_type, location_id, review_id, escalation_id, report_id, requested_by, channel, recipient, subject,
                       body, status, status_reason, attempts, created_at, sent_at))

using NotificationDeliveryList = Paged<NotificationDelivery>;

struct SystemSettings {
    // The global kill switch for posting replies to Google.
    bool posting_enabled = false;
    bool email_sending_enabled = false;
    std::string updated_at;
};
BOOST_DESCRIBE_STRUCT(SystemSettings, (), (posting_enabled, email_sending_enabled, updated_at))

// Auto-posting at one location (D-070 to D-072): the switches a person
// flips, the timing its reply policy asks for, and the pause the engine
// may have put on it.
struct AutoPosting {
    std::int64_t location_id = 0;
    std::string location_title;
    std::optional<std::int64_t> client_id;
    std::optional<std::string> client_name;
    bool enabled = false;
    bool five_star = false;
    bool four_star = false;
    std::int64_t daily_cap = 0;
    std::optional<std::string> time_zone;
    // From the location's reply policy; changed there.
    std::int64_t delay_min_minutes = 0;
    std::int64_t delay_max_minutes = 0;
    std::string window_start;
    std::string window_end;
    // Why the timing can't be used (a window without a time zone, a range
    // that is upside down). While this is set nothing is auto-posted here.
    std::optional<std::string> timing_problem;
    // The location's posting switch (D-063), which auto-posting needs too.
    bool posting_enabled = false;
    bool paused = false;
    std::optional<std::string> paused_at;
    // "checks_failing", "post_unconfirmed" or "post_refused"
    std::optional<std::string> pause_reason;
    std::optional<std::string> pause_detail;
    std::int64_t posted_last_24h = 0;
    std::int64_t scheduled = 0;
    // This many drafts in a row that fail a reply check pause auto-posting here (D-076).
    std::int64_t pause_after_failures = 0;
    // The go-live test of the location's configuration as it is now (D-074):
    // "passed", "not_tested", "running", "failed", "inconclusive" or
    // "cancelled". Nothing is auto-posted here unless it is "passed".
    std::string go_live_state;
};
BOOST_DESCRIBE_STRUCT(AutoPosting, (),
                      (location_id, location_title, client_id, client_name, enabled, five_star, four_star, daily_cap, time_zone,
                       delay_min_minutes, delay_max_minutes, window_start, window_end, timing_problem, posting_enabled, paused,
                       paused_at, pause_reason, pause_detail, posted_last_24h, scheduled, pause_after_failures, go_live_state))

struct AutoPostingOverview {
    // The global posting switch (D-063).
    bool posting_enabled = false;
    // The pause on every location at once.
    bool paused = false;
    std::optional<std::string> paused_at;
    std::optional<std::string> pause_reason;
    std::optional<std::string> pause_detail;
    std::vector<AutoPosting> locations;
};
BOOST_DESCRIBE_STRUCT(AutoPostingOverview, (),
                      (posting_enabled, paused, paused_at, pause_reason, pause_detail, locations))

// One reply the engine posted without a person.
struct AutoPost {
    std::int64_t id = 0;
    std::int64_t review_id = 0;
    std::int64_t location_id = 0;
    std::string location_title;
    std::optional<std::int64_t> rating;
    std::optional<std::string> review_text;
    std::string reply_text;
    std::string status;  // as ReplyPost::status
    std::string policy_version;
    std::string started_at;
    std::optional<std::string> posted_at;
    std::optional<std::string> published_at;
    // From Google returning the review to Google accepting the reply.
    std::optional<std::int64_t> seconds_to_post;
};
BOOST_DESCRIBE_STRUCT(AutoPost, (),
                      (id, review_id, location_id, location_title, rating, review_text, reply_text, status, policy_version,
                       started_at, posted_at, published_at, seconds_to_post))

using AutoPostList = Paged<AutoPost>;

// ---- the go-live test (D-074, D-075) ---------------------------------------------

// One of the repeats of a case: what the pipeline's decisions made of it.
struct GoLiveAttempt {
    // "auto_post", "approval_queue" or "escalate".
    std::string route;
    std::vector<std::string> reasons;
    // As the model wrote it; empty when it wrote none.
    std::string reply;
    std::vector<std::string> failed_checks;
    // The judge's rating, for a reply that would have been posted without a person.
    std::optional<std::string> rating;
    std::optional<std::string> rating_reason;
    // The model that rated, when it was not the judge named in the settings:
    // the provider's fallback, after the judge's model declined.
    std::optional<std::string> rated_by;
};
BOOST_DESCRIBE_STRUCT(GoLiveAttempt, (), (route, reasons, reply, failed_checks, rating, rating_reason, rated_by))

struct GoLiveCase {
    std::int64_t id = 0;
    // The corpus case's id, or "review:<id>" for one of the location's reviews.
    std::string case_key;
    // "manipulation", "severity", "control" or "real".
    std::string category;
    std::optional<std::int64_t> review_id;
    std::int64_t stars = 0;
    // The review; nothing for a rating without text.
    std::optional<std::string> text;
    // "pending", "ok", "leaked", "unacceptable" or "inconclusive".
    std::string verdict;
    std::optional<std::string> detail;
    std::vector<GoLiveAttempt> attempts;
    std::optional<std::string> finished_at;
};
BOOST_DESCRIBE_STRUCT(GoLiveCase, (), (id, case_key, category, review_id, stars, text, verdict, detail, attempts, finished_at))

struct GoLiveRun {
    std::int64_t id = 0;
    // The location the run was submitted for (D-102).
    std::int64_t location_id = 0;
    std::string location_title;
    std::string fingerprint;
    // "running", "passed", "failed", "inconclusive" or "cancelled".
    std::string status;
    std::optional<std::string> summary;
    // How wide the policy was that the cases were routed with.
    bool auto_post_positive = false;
    bool auto_post_rating_only = false;
    std::int64_t auto_post_min_stars = 0;
    // The industry's corpus the run used, and whether it is the one seeded now.
    std::string corpus_version;
    bool corpus_current = false;
    // The knowledge base the run started against; nothing for a run from before there was one.
    std::optional<std::string> kb_version;
    std::int64_t repeats = 0;
    // How many of the location's own reviews were among the cases (D-075).
    std::int64_t real_reviews = 0;
    std::int64_t cases_total = 0;
    std::int64_t cases_done = 0;
    // Requests to the models so far, and what they cost by the price settings.
    std::int64_t model_calls = 0;
    std::optional<double> spent_usd;
    std::optional<std::string> started_by;
    std::string started_at;
    std::optional<std::string> finished_at;
    // Only when one run is read.
    std::optional<std::vector<GoLiveCase>> cases;
};
BOOST_DESCRIBE_STRUCT(GoLiveRun, (),
                      (id, location_id, location_title, fingerprint, status, summary, auto_post_positive, auto_post_rating_only,
                       auto_post_min_stars, corpus_version, corpus_current, kb_version, repeats, real_reviews, cases_total, cases_done,
                       model_calls, spent_usd, started_by, started_at, finished_at, cases))

// What a test of one configuration would take, from the number of cases and
// repeats, the price settings and the recent model calls.
struct GoLiveEstimate {
    std::int64_t cases = 0;
    std::int64_t real_reviews = 0;
    // Requests to the analyzer and the writer, and at most this many to the judge.
    std::int64_t model_calls = 0;
    std::int64_t judge_calls = 0;
    // Nothing when there are no recent calls to measure.
    std::optional<std::int64_t> seconds;
    std::optional<double> cost_usd;
    // What the numbers leave out ("no price is set for ...").
    std::vector<std::string> notes;
};
BOOST_DESCRIBE_STRUCT(GoLiveEstimate, (), (cases, real_reviews, model_calls, judge_calls, seconds, cost_usd, notes))

// Where one location's configuration stands (D-102). Locations with the same
// fingerprint share their runs and their pass, but each is submitted on its own.
struct GoLiveLocation {
    std::int64_t location_id = 0;
    std::string location_title;
    // The client it is grouped under, for display only.
    std::optional<std::int64_t> client_id;
    std::optional<std::string> client_name;
    // Why this location can't be tested (no industry profile); nothing when it can.
    std::optional<std::string> problem;
    // Nothing when the location can't be tested.
    std::optional<std::string> fingerprint;
    // "not_tested", "running", "passed", "failed", "inconclusive" or "cancelled".
    std::string state;
    // Why, where the state alone doesn't say ("the policy auto-posts more than the last pass covered").
    std::optional<std::string> state_detail;
    // The pass that lets this location auto-post, if there is one.
    std::optional<GoLiveRun> pass;
    // The newest run of its fingerprint.
    std::optional<GoLiveRun> latest_run;
    // Nothing when the location can't be tested.
    std::optional<GoLiveEstimate> estimate;
};
BOOST_DESCRIBE_STRUCT(GoLiveLocation, (),
                      (location_id, location_title, client_id, client_name, problem, fingerprint, state, state_detail, pass, latest_run,
                       estimate))

struct GoLiveOverview {
    std::int64_t repeats = 0;
    std::string judge_provider;
    std::string judge_model;
    // Every provider the three tasks name has an address and a key; filled in by the API.
    bool providers_configured = false;
    // Every enabled location, by title.
    std::vector<GoLiveLocation> locations;
};
BOOST_DESCRIBE_STRUCT(GoLiveOverview, (), (repeats, judge_provider, judge_model, providers_configured, locations))

// ---- reports (FR-8, D-085, D-104) --------------------------------------------------

struct Report {
    std::int64_t id = 0;
    // The location the report covers (D-102).
    std::int64_t location_id = 0;
    std::string location_title;
    // Both days are part of the period, in `time_zone`.
    std::string period_start;
    std::string period_end;
    std::string time_zone;
    std::optional<std::int64_t> schedule_id;
    std::optional<std::string> requested_by;  // nothing: a schedule
    // "building", "failed", "ready", "discarding", "discarded" or "expired" (D-104).
    std::string status;
    std::optional<std::string> error;
    // The page in the public bucket while the report is ready: whoever has the address can read it.
    std::optional<std::string> url;
    // The archived page once the report is discarded or expired, if an upload of its page was begun. For staff only.
    std::optional<std::string> archive_url;
    // "off" (a person's report, or a schedule that does not send by itself), "pending" or "done".
    std::string auto_send_status;
    // From the built report; nothing while it is building.
    std::optional<std::int64_t> new_reviews;
    // The two headline figures of the built report, as its data holds them (D-085).
    std::optional<double> average_rating;
    std::optional<double> response_rate;
    std::optional<std::string> summary;
    std::optional<std::string> summary_note;
    std::string created_at;
    // The addresses a send can go to now (the location's enabled location_report rule), sorted, each once,
    // and the report's rows of the delivery log, newest first. Both empty in a list.
    std::vector<std::string> recipients;
    std::vector<NotificationDelivery> deliveries;
};
BOOST_DESCRIBE_STRUCT(Report, (),
                      (id, location_id, location_title, period_start, period_end, time_zone, schedule_id, requested_by, status, error,
                       url, archive_url, auto_send_status, new_reviews, average_rating, response_rate, summary, summary_note,
                       created_at, recipients, deliveries))

using ReportList = Paged<Report>;

struct ReportSchedule {
    std::int64_t id = 0;
    std::int64_t location_id = 0;
    std::string location_title;
    std::string frequency;  // "daily", "weekly" or "monthly"
    std::optional<std::int64_t> weekday;    // 1 (Monday) to 7; weekly
    std::optional<std::int64_t> month_day;  // 1 to 28; monthly
    std::string time;                       // "HH:MM", local
    std::string time_zone;
    bool auto_send = false;
    bool enabled = true;
    std::optional<std::string> last_run_on;
    std::string created_at;
    std::string updated_at;
};
BOOST_DESCRIBE_STRUCT(ReportSchedule, (),
                      (id, location_id, location_title, frequency, weekday, month_day, time, time_zone, auto_send, enabled, last_run_on,
                       created_at, updated_at))

using ReportScheduleList = Paged<ReportSchedule>;

// ---- local posts (FR-14, D-088) ------------------------------------------------------

// A photo or video of a post, for showing it.
struct LocalPostMediaItem {
    std::string format;  // "photo" or "video"
    // Google's address of it once Google has the post, the uploaded file's before.
    std::string url;
};
BOOST_DESCRIBE_STRUCT(LocalPostMediaItem, (), (format, url))

// A post copied from another, as that post lists it (D-098).
struct LocalPostCopy {
    std::int64_t id = 0;
    std::string status;
};
BOOST_DESCRIBE_STRUCT(LocalPostCopy, (), (id, status))

// The open edit of a post that is at Google (D-099): the content the post is
// to have, beside the post until a person approved it and Google accepted it.
struct LocalPostEdit {
    domain::LocalPostContent content;
    // What the edit changes, and so all it ever writes at Google: of "summary", "call_to_action", "event",
    // "offer", "media" and "language_code", those in which the content differs from the post's as it was
    // when the edit was written.
    std::vector<std::string> fields;
    // What the post would show with the edit applied: the edit's uploads, in order, where it changes the
    // media, what the post shows now where it does not.
    std::vector<LocalPostMediaItem> media;
    // The checks on that content as of its last change.
    std::vector<domain::CheckResult> checks;
    std::string status;  // "draft", "approved" or "sending"
    // Why the edit is a draft again, when it came back.
    std::optional<std::string> status_reason;
    std::string written_by;  // emails
    std::string written_at;
    std::optional<std::string> decided_by;  // who approved it last
    std::optional<std::string> decided_at;
};
BOOST_DESCRIBE_STRUCT(LocalPostEdit, (),
                      (content, fields, media, checks, status, status_reason, written_by, written_at, decided_by, decided_at))

// A post on a location's profile: a draft, a post the engine sent, or one
// made in Google's own interface. The content fields are those of
// domain::LocalPostContent, flat, as in the schema.
struct LocalPost {
    std::int64_t id = 0;
    std::int64_t location_id = 0;
    std::string location_title;
    std::optional<std::int64_t> client_id;  // for display only; nothing: the location has no client
    std::optional<std::string> client_name;
    std::string origin;  // "system" or "google"
    // Ours: "draft", "approved", "sending", "sent", "unknown", "failed", "rejected" or "removed".
    std::string status;
    std::optional<std::string> status_reason;
    // Google's, as last read: "processing", "scheduled", "live", "recurring" or "rejected".
    std::optional<std::string> google_state;
    std::optional<std::string> google_name;
    std::optional<std::string> search_url;

    std::string topic_type;
    std::string summary;
    std::string language_code;
    std::optional<domain::LocalPostCallToAction> call_to_action;
    std::optional<domain::LocalPostEvent> event;
    std::optional<domain::LocalPostOffer> offer;
    std::vector<std::int64_t> media_ids;
    std::optional<std::string> scheduled_time;

    // What the post shows, in order: Google's copies once Google has the post, the uploads before.
    std::vector<LocalPostMediaItem> media;
    // The checks on the content as of its last change; empty for a post made at Google.
    std::vector<domain::CheckResult> checks;
    // The post this post was copied from (kept whatever becomes of the copy), and the posts copied
    // from this one, newest first. Not content: nothing approves, sends or removes by them (D-098).
    std::optional<std::int64_t> copied_from_id;
    std::vector<LocalPostCopy> copies;
    // The open edit of a post that is at Google, if it has one (D-099). The content above stays Google's.
    std::optional<LocalPostEdit> edit;

    std::optional<std::string> created_by;  // emails
    // Who last edited the draft or, once Google accepted an edit of the post, who wrote that edit.
    std::optional<std::string> edited_by;
    std::optional<std::string> edited_at;
    std::optional<std::string> decided_by;  // who approved or rejected the draft
    std::optional<std::string> decided_at;
    std::optional<std::string> sent_at;
    std::optional<std::string> removal_requested_by;
    std::optional<std::string> removal_requested_at;
    std::optional<std::string> removed_at;
    std::optional<std::string> google_created_at;
    std::optional<std::string> synced_at;
    std::string created_at;
    std::string updated_at;

    // The part a person approves.
    domain::LocalPostContent content() const {
        return {.topic_type = topic_type,
                .summary = summary,
                .language_code = language_code,
                .call_to_action = call_to_action,
                .event = event,
                .offer = offer,
                .media_ids = media_ids,
                .scheduled_time = scheduled_time};
    }
};
BOOST_DESCRIBE_STRUCT(LocalPost, (),
                      (id, location_id, location_title, client_id, client_name, origin, status, status_reason, google_state,
                       google_name, search_url, topic_type, summary, language_code, call_to_action, event, offer, media_ids,
                       scheduled_time, media, checks, copied_from_id, copies, edit, created_by, edited_by, edited_at, decided_by, decided_at, sent_at,
                       removal_requested_by, removal_requested_at, removed_at, google_created_at, synced_at,
                       created_at, updated_at))

using LocalPostList = Paged<LocalPost>;

// An uploaded photo or video for local posts, in the public bucket as it was uploaded.
struct LocalPostMedia {
    std::int64_t id = 0;
    std::string format;  // "photo" or "video"
    std::int64_t bytes = 0;
    std::optional<std::int64_t> width;  // pixels, as the file stores them
    std::optional<std::int64_t> height;
    std::string url;  // its public address, which Google reads it from
    std::optional<std::string> created_by;
    std::string created_at;
};
BOOST_DESCRIBE_STRUCT(LocalPostMedia, (), (id, format, bytes, width, height, url, created_by, created_at))

// ---- help with local posts (D-088 point 11) ---------------------------------------------

// {label, count}: a topic, an item or a language with how many reviews named it.
using HighlightCount = domain::Ranked;

// A recent review that praised something. Without the reviewer's name.
struct HighlightReview {
    std::int64_t review_id = 0;
    int rating = 0;
    std::string day;
    std::string language;
    std::string text;
};
BOOST_DESCRIBE_STRUCT(HighlightReview, (), (review_id, rating, day, language, text))

// What a location's customers praised lately, from the stored analysis.
struct ReviewHighlights {
    std::int64_t location_id = 0;
    std::string period_start;
    std::string period_end;
    std::int64_t reviews = 0;
    std::int64_t analysed_reviews = 0;
    bool enough_for_ideas = false;
    std::vector<HighlightCount> topics;
    std::vector<HighlightCount> items;
    std::vector<HighlightCount> languages;
    std::vector<HighlightReview> recent;
};
BOOST_DESCRIBE_STRUCT(ReviewHighlights, (),
                      (location_id, period_start, period_end, reviews, analysed_reviews, enough_for_ideas, topics, items, languages,
                       recent))

// {topic_type, headline, brief, reason}, as the model's answer was read.
using LocalPostIdea = domain::PostIdea;

struct LocalPostIdeas {
    std::vector<LocalPostIdea> ideas;
    std::string model;
};
BOOST_DESCRIBE_STRUCT(LocalPostIdeas, (), (ideas, model))

// A text the model wrote for a post. Nothing of it is stored.
struct LocalPostSuggestion {
    std::string summary;
    std::optional<std::string> title;
    std::string notes_for_staff;
    std::string model;
};
BOOST_DESCRIBE_STRUCT(LocalPostSuggestion, (), (summary, title, notes_for_staff, model))

}  // namespace poni::model
