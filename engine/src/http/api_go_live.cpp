// The go-live test's part of the dashboard's API (D-074): where every
// client's configurations stand, starting a test, stopping one, and reading
// a run with its cases. Paths, parameters and bodies are defined in
// api/openapi.yaml; change that file first.
//
// A test starts only here, by a signed-in person. Starting one queues jobs
// that call the models, which costs money; nothing here posts, and a pass
// switches nothing on.

#include "db/audit.hpp"
#include "db/database.hpp"
#include "db/repo_go_live.hpp"
#include "http/api.hpp"
#include "http/api_support.hpp"

namespace poni::http {

namespace asio = boost::asio;
using boost::beast::http::verb;
using namespace api_support;

void add_go_live_routes(Router& router, const Api& api) {
    router.add(verb::get, "/api/go-live", {Access::user, "go_live.read"},
               [api](const Request&) -> asio::awaitable<Response> {
                   co_return ok(co_await api.query([providers = api.options.llm_providers, parallel = api.options.job_workers](pqxx::work& tx) {
                       return repo::go_live_overview(tx, providers, parallel);
                   }));
               });

    // The request that spends money on model calls. Its own action name, so
    // a role can later allow reading and not this.
    router.add(verb::post, "/api/clients/{id}/go-live-test", {Access::user, "go_live.start"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("client");
                   const auto started = co_await api.change(request, [id = *id, providers = api.options.llm_providers,
                                                                      parallel = api.options.job_workers](pqxx::work& tx, audit::Writer& audit) {
                       return repo::start_go_live(tx, audit, id, providers, parallel);
                   });
                   if (!started) co_return respond(started.error());
                   co_return ok(model::GoLiveRunList{.items = *started, .total = static_cast<std::int64_t>(started->size())}, 201);
               });

    add_get_by_id(router, api, "/api/go-live-runs/{id}", "go_live.read", "go-live test", repo::get_go_live_run);

    router.add(verb::post, "/api/go-live-runs/{id}/cancel", {Access::user, "go_live.cancel"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("go-live test");
                   co_return respond(co_await api.change(request, [id = *id](pqxx::work& tx, audit::Writer& audit) {
                       return repo::cancel_go_live(tx, audit, id);
                   }));
               });
}

}  // namespace poni::http
