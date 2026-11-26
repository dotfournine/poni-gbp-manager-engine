// The dashboard's API for auto-posting per location (the
// switches, the star ratings, the cap, the time zone), the pauses and how a
// person lifts them, the list of what was posted without a person, and
// taking a scheduled reply back. Paths, parameters and bodies are defined in
// api/openapi.yaml; change that file first.
//
// Nothing here posts. Switching auto-posting on lets the pipeline schedule
// drafts from then on (repo::schedule_auto_post); these routes are the only
// way it is switched on, and each needs a signed-in person.

#include "db/audit.hpp"
#include "db/database.hpp"
#include "db/repo_auto_posting.hpp"
#include "db/repo_posting.hpp"
#include "http/api.hpp"
#include "http/api_support.hpp"

namespace poni::http {

namespace asio = boost::asio;
using boost::beast::http::verb;
using namespace api_support;

void add_auto_posting_routes(Router& router, const Api& api) {
    router.add(verb::get, "/api/auto-posting", {Access::user, "auto_posting.read"},
               [api](const Request&) -> asio::awaitable<Response> {
                   co_return ok(co_await api.query([now = api.clock.now()](pqxx::work& tx) { return repo::auto_posting_overview(tx, now); }));
               });

    router.add(verb::post, "/api/auto-posting/resume", {Access::user, "auto_posting.resume"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   co_return ok(co_await api.change(request, [](pqxx::work& tx, audit::Writer& audit) {
                       return repo::resume_all_auto_posting(tx, audit);
                   }));
               });

    // The request that lets replies go out without a person. Its own action
    // name, so a role can later allow everything else and not this.
    router.add(verb::patch, "/api/locations/{id}/auto-posting", {Access::user, "auto_posting.update"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("location");
                   const auto patch = body_as<repo::AutoPostingPatch>(request);
                   if (!patch) co_return patch.error();
                   co_return respond(co_await api.change(request, [id = *id, patch = *patch](pqxx::work& tx, audit::Writer& audit) {
                       return repo::update_auto_posting(tx, audit, id, patch);
                   }));
               });

    router.add(verb::post, "/api/locations/{id}/auto-posting/resume", {Access::user, "auto_posting.resume"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("location");
                   co_return respond(co_await api.change(request, [id = *id](pqxx::work& tx, audit::Writer& audit) {
                       return repo::resume_auto_posting(tx, audit, id);
                   }));
               });

    router.add(verb::get, "/api/auto-posts", {Access::user, "auto_posts.list"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const QueryParams query(request.query);
                   db::Page page;
                   bool bad = false;
                   if (!read_page(query, page, bad)) co_return bad_query();
                   co_return ok(co_await api.query([page](pqxx::work& tx) { return repo::list_auto_posts(tx, page); }));
               });

    // A person takes a reply out of auto-posting before it goes out.
    router.add(verb::post, "/api/drafts/{id}/unschedule", {Access::user, "drafts.unschedule"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("draft");
                   co_return respond(co_await api.change(request, [id = *id](pqxx::work& tx, audit::Writer& audit) {
                       return repo::unschedule_draft(tx, audit, id);
                   }));
               });
}

}  // namespace poni::http
