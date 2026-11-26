// The dashboard's API for drafts and re-analysis of a
// review, the knowledge base (read-only, D-078), a location's reply settings
// (D-093), the pipeline settings, and LLM usage. Paths, parameters and bodies are
// defined in api/openapi.yaml; change that file first.

#include "core/etag.hpp"
#include "core/json.hpp"
#include "db/audit.hpp"
#include "db/repo_analysis.hpp"
#include "db/repo_knowledge.hpp"
#include "db/repo_reviews.hpp"
#include "db/repo_settings.hpp"
#include "domain/report_schedule.hpp"
#include "http/api.hpp"
#include "http/api_support.hpp"

#include <chrono>
#include <utility>

namespace poni::http {

namespace asio = boost::asio;
using boost::beast::http::verb;
using namespace api_support;

void add_analysis_routes(Router& router, const Api& api) {
    // ---- a review's drafts and re-analysis ---------------------------------------

    add_get_by_id(router, api, "/api/reviews/{id}/drafts", "reviews.read", "review",
                  [](pqxx::work& tx, std::int64_t id) -> std::optional<model::ReplyDraftList> {
                      if (!repo::get_review(tx, id)) return std::nullopt;
                      return repo::list_review_drafts(tx, id);
                  });

    router.add(verb::post, "/api/reviews/{id}/analyze", {Access::user, "reviews.analyze"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("review");
                   co_return respond(co_await api.change(request, [id = *id](pqxx::work& tx, audit::Writer& audit) {
                       return repo::request_analysis(tx, audit, id);
                   }), enqueued());
               });

    router.add(verb::get, "/api/locations/{id}/reply-settings", {Access::user, "locations.read"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("location");
                   co_return respond(co_await api.query([id = *id](pqxx::work& tx) { return repo::reply_settings(tx, id); }));
               });

    // A save sends every field and replaces the settings whole (D-093).
    router.add(verb::put, "/api/locations/{id}/reply-settings", {Access::user, "reply_settings.update"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("location");
                   const auto body = body_as<domain::ResolvedSettings>(request);
                   if (!body) co_return body.error();
                   co_return respond(co_await api.change(request, [id = *id, settings = *body](pqxx::work& tx, audit::Writer& audit) {
                       return repo::replace_reply_settings(tx, audit, id, settings);
                   }));
               });

    // ---- the knowledge base, read-only (FR-13) ------------------------------------
    // No route writes it: an update is a new set of files, seeded by poni-tools.

    router.add(verb::get, "/api/knowledge", {Access::user, "knowledge.read"}, [api](const Request&) -> asio::awaitable<Response> {
        co_return ok(co_await api.query([](pqxx::work& tx) { return repo::knowledge(tx); }));
    });

    router.add(verb::get, "/api/knowledge/industries/{key}/corpus", {Access::user, "knowledge.read"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const auto key = request.params.find("key");
                   if (key == request.params.end()) co_return not_found("industry");
                   co_return respond(co_await api.query([key = key->second](pqxx::work& tx) { return repo::knowledge_corpus(tx, key); }), "industry");
               });

    // ---- pipeline settings -------------------------------------------------------

    // The ETag covers what the PATCH can set, not the providers the engine is configured with (D-108).
    const auto ai_etag = [](const model::AiSettings& settings) { return core::etag(repo::editable(settings)); };

    router.add(verb::get, "/api/ai/settings", {Access::user, "ai_settings.read"},
               [api, ai_etag](const Request&) -> asio::awaitable<Response> {
                   auto settings = co_await api.query([](pqxx::work& tx) { return repo::get_ai_settings(tx); });
                   settings.providers = api.options.llm_providers;
                   co_return respond_tagged(repo::Result<model::AiSettings>(std::move(settings)), ai_etag);
               });

    router.add(verb::patch, "/api/ai/settings", {Access::user, "ai_settings.update"},
               [api, ai_etag](const Request& request) -> asio::awaitable<Response> {
                   const auto patch = body_as<repo::AiSettingsPatch>(request);
                   if (!patch) co_return patch.error();
                   auto result = co_await api.change_if_match(
                       request, [](pqxx::work& tx) { return repo::current_ai_settings_etag(tx); },
                       [patch = *patch, providers = api.options.llm_providers](pqxx::work& tx, audit::Writer& audit) {
                           return repo::update_ai_settings(tx, audit, patch, providers);
                       });
                   if (result) result->providers = api.options.llm_providers;
                   co_return respond_tagged(result, ai_etag);
               });

    // ---- LLM usage ---------------------------------------------------------------

    router.add(verb::get, "/api/llm/usage", {Access::user, "llm.usage"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const QueryParams query(request.query);
                   using std::chrono::days;
                   // Whole days in UTC: `to` is included, so the range ends at the next midnight.
                   auto last_day = core::TimePoint(std::chrono::floor<days>(api.clock.now()));
                   if (const auto to = query.get("to")) {
                       const auto parsed = domain::parse_report_day(*to);
                       if (!parsed) co_return invalid("\"to\" must be a date, YYYY-MM-DD.");
                       last_day = *parsed;
                   }
                   auto first_day = last_day - days(29);
                   if (const auto from = query.get("from")) {
                       const auto parsed = domain::parse_report_day(*from);
                       if (!parsed) co_return invalid("\"from\" must be a date, YYYY-MM-DD.");
                       first_day = *parsed;
                   }
                   if (first_day > last_day || last_day - first_day > days(366)) {
                       co_return invalid("\"from\" must not be after \"to\", and the range may span at most a year.");
                   }
                   co_return ok(co_await api.query([first_day, end = last_day + days(1)](pqxx::work& tx) {
                       return repo::llm_usage(tx, first_day, end);
                   }));
               });

    router.add(verb::get, "/api/llm/calls", {Access::user, "llm.calls"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const QueryParams query(request.query);
                   db::Page page;
                   bool bad = false;
                   if (!read_page(query, page, bad)) co_return bad_query();
                   co_return ok(co_await api.query([page](pqxx::work& tx) { return repo::list_llm_calls(tx, page); }));
               });
}

}  // namespace poni::http
