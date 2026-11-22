#include "http/api.hpp"

#include "http/api_support.hpp"
#include "core/version.hpp"
#include "db/audit.hpp"
#include "db/repo_catalog.hpp"
#include "db/repo_reviews.hpp"

#include <algorithm>
#include <format>

namespace poni::http {

namespace asio = boost::asio;
using boost::beast::http::verb;

using namespace api_support;

void add_api_routes(Router& router, db::Database& db, const core::Clock& clock, ApiOptions options, jobs::ReviewPipeline& pipeline,
                    jobs::Reporter& reporter, storage::ObjectStore* public_store, jobs::PostAdvisor& advisor) {
    const Api api{db, clock, std::move(options)};

    // ---- clients -----------------------------------------------------------------

    router.add(verb::get, "/api/clients", {Access::user, "clients.list"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const QueryParams query(request.query);
                   repo::ClientFilter filter;
                   bool bad = false;
                   filter.archived = query.boolean("archived", bad);
                   filter.q = query.get("q").value_or("");
                   if (!read_page(query, filter.page, bad)) co_return bad_query();
                   co_return ok(co_await api.query([filter](pqxx::work& tx) { return repo::list_clients(tx, filter); }));
               });

    router.add(verb::post, "/api/clients", {Access::user, "clients.create"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const auto client = body_as<repo::NewClient>(request);
                   if (!client) co_return client.error();
                   co_return respond(co_await api.change(request, [client = *client](pqxx::work& tx, audit::Writer& audit) {
                       return repo::create_client(tx, audit, client);
                   }), 201);
               });

    add_get_by_id(router, api, "/api/clients/{id}", "clients.read", "client", repo::get_client);

    router.add(verb::patch, "/api/clients/{id}", {Access::user, "clients.update"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("client");
                   const auto patch = body_as<repo::ClientPatch>(request);
                   if (!patch) co_return patch.error();
                   co_return respond(co_await api.change(request, [id = *id, patch = *patch](pqxx::work& tx, audit::Writer& audit) {
                       return repo::update_client(tx, audit, id, patch);
                   }));
               });

    // ---- locations ---------------------------------------------------------------

    router.add(verb::get, "/api/locations", {Access::user, "locations.list"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const QueryParams query(request.query);
                   repo::LocationFilter filter;
                   bool bad = false;
                   filter.client_id = query.integer("client_id", bad);
                   filter.linked = query.boolean("linked", bad);
                   read_choice(query, "access_status", repo::kAccessStatuses, filter.access_status, bad);
                   filter.q = query.get("q").value_or("");
                   if (!read_page(query, filter.page, bad)) co_return bad_query();
                   co_return ok(co_await api.query([filter](pqxx::work& tx) { return repo::list_locations(tx, filter); }));
               });

    // Before "/api/locations/{id}" would matter: the two differ in method.
    router.add(verb::post, "/api/locations/discover", {Access::user, "locations.discover"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   co_await api.change(request, [](pqxx::work& tx, audit::Writer& audit) { repo::request_discovery(tx, audit); });
                   co_return enqueued();
               });

    add_get_by_id(router, api, "/api/locations/{id}", "locations.read", "location", repo::get_location);

    router.add(verb::patch, "/api/locations/{id}", {Access::user, "locations.update"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("location");
                   const auto patch = body_as<repo::LocationPatch>(request);
                   if (!patch) co_return patch.error();
                   co_return respond(co_await api.change(request, [id = *id, patch = *patch](pqxx::work& tx, audit::Writer& audit) {
                       return repo::update_location(tx, audit, id, patch);
                   }));
               });

    // ---- reviews -----------------------------------------------------------------

    router.add(verb::get, "/api/reviews", {Access::user, "reviews.list"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const QueryParams query(request.query);
                   repo::ReviewFilter filter;
                   bool bad = false;
                   filter.client_id = query.integer("client_id", bad);
                   filter.location_id = query.integer("location_id", bad);
                   for (const auto& rating : query.all("rating")) {
                       if (rating.size() == 1 && rating[0] >= '1' && rating[0] <= '5') {
                           filter.ratings.push_back(rating[0] - '0');
                       } else {
                           bad = true;
                       }
                   }
                   read_choice(query, "status", repo::kReviewStatuses, filter.status, bad);
                   filter.language = query.get("language").value_or("");
                   filter.has_reply = query.boolean("has_reply", bad);
                   filter.q = query.get("q").value_or("");
                   filter.analyzed = query.boolean("analyzed", bad);
                   filter.flagged = query.boolean("flagged", bad);
                   filter.manipulation = query.boolean("manipulation", bad);
                   filter.ignored = query.boolean("ignored", bad);
                   filter.has_draft = query.boolean("has_draft", bad);
                   read_choice(query, "sentiment", domain::kSentiments, filter.sentiment, bad);
                   read_choice(query, "route", repo::kDraftRoutes, filter.route, bad);
                   read_choice(query, "draft_status", repo::kDraftStatuses, filter.draft_status, bad);
                   read_choice(query, "sort", repo::kReviewSorts, filter.sort, bad);
                   if (const auto order = query.get("order")) {
                       if (*order != "asc" && *order != "desc") bad = true;
                       filter.descending = *order == "desc";
                   }
                   if (!read_page(query, filter.page, bad)) co_return bad_query();
                   co_return ok(co_await api.query([filter](pqxx::work& tx) { return repo::list_reviews(tx, filter); }));
               });

    add_get_by_id(router, api, "/api/reviews/{id}", "reviews.read", "review", repo::get_review);

    add_get_by_id(router, api, "/api/reviews/{id}/events", "reviews.read", "review",
                  [](pqxx::work& tx, std::int64_t id) -> std::optional<model::ReviewEventList> {
                      if (!repo::get_review(tx, id)) return std::nullopt;
                      return repo::list_review_events(tx, id);
                  });

    // ---- audit log and status ----------------------------------------------------

    router.add(verb::get, "/api/audit-log", {Access::user, "audit.list"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const QueryParams query(request.query);
                   repo::AuditFilter filter;
                   bool bad = false;
                   filter.action = query.get("action").value_or("");
                   filter.object_type = query.get("object_type").value_or("");
                   filter.object_id = query.get("object_id").value_or("");
                   filter.actor = query.get("actor").value_or("");
                   if (!read_page(query, filter.page, bad)) co_return bad_query();
                   co_return ok(co_await api.query([filter](pqxx::work& tx) { return repo::list_audit(tx, filter); }));
               });

    router.add(verb::get, "/api/system/status", {Access::user, "system.status"},
               [api](const Request&) -> asio::awaitable<Response> {
                   const auto now = api.clock.now();
                   auto status = co_await api.query([now](pqxx::work& tx) { return repo::system_status(tx, now); });
                   status.version = std::string(core::kVersion);
                   status.pubsub.enabled = api.options.pubsub_enabled;
                   status.connect_configured = api.options.connect_configured;
                   status.ai.providers = api.options.llm_providers;
                   status.ai.llm_configured = std::ranges::contains(api.options.llm_providers, status.ai.analyzer_provider) &&
                                              std::ranges::contains(api.options.llm_providers, status.ai.generator_provider);
                   co_return ok(status);
               });

    add_analysis_routes(router, api);
    add_reply_routes(router, api, pipeline);
    add_auto_posting_routes(router, api);
    add_go_live_routes(router, api);
    add_local_post_routes(router, api, public_store, advisor);
    add_report_routes(router, api, reporter);
}

}  // namespace poni::http
