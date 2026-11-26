// The dashboard's API for auto reply's own state (D-110): the pauses the
// engine put on itself and how a person lifts them, what each location's
// auto reply looks like to the engine, the list of what was posted without a
// person, and taking a scheduled reply back. Paths, parameters and bodies are
// defined in api/openapi.yaml; change that file first.
//
// Nothing here posts, and nothing here switches auto reply on: that is a
// person's save of the location's reply settings (repo::replace_reply_settings).

#include "db/audit.hpp"
#include "db/database.hpp"
#include "db/repo_auto_reply.hpp"
#include "db/repo_posting.hpp"
#include "http/api.hpp"
#include "http/api_support.hpp"

namespace poni::http {

namespace asio = boost::asio;
using boost::beast::http::verb;
using namespace api_support;

void add_auto_reply_routes(Router& router, const Api& api) {
    router.add(verb::get, "/api/system/auto-reply", {Access::user, "auto_reply.read"},
               [api](const Request&) -> asio::awaitable<Response> {
                   co_return ok(co_await api.query([now = api.clock.now()](pqxx::work& tx) { return repo::auto_reply_overview(tx, now); }));
               });

    // Lifts the pause on every location at once, and nothing else.
    router.add(verb::post, "/api/system/auto-reply/resume", {Access::user, "auto_reply.resume"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   co_return ok(co_await api.change(request, [](pqxx::work& tx, audit::Writer& audit) {
                       return repo::resume_all_auto_reply(tx, audit);
                   }));
               });

    router.add(verb::post, "/api/locations/{id}/auto-reply/resume", {Access::user, "auto_reply.resume"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("location");
                   co_return respond(co_await api.change(request, [id = *id](pqxx::work& tx, audit::Writer& audit) {
                       return repo::resume_auto_reply(tx, audit, id);
                   }));
               });

    router.add(verb::get, "/api/auto-replies", {Access::user, "auto_replies.list"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const QueryParams query(request.query);
                   db::Page page;
                   bool bad = false;
                   if (!read_page(query, page, bad)) co_return bad_query();
                   co_return ok(co_await api.query([page](pqxx::work& tx) { return repo::list_auto_replies(tx, page); }));
               });

    // A person takes a reply out of auto reply before it goes out.
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
