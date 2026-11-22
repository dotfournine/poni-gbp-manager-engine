// The reports part of the dashboard's API (FR-8, D-085, D-102): the list
// of reports, generating a location's report by hand, reading its page, sending or rejecting
// it, share links, and the schedules. Paths, parameters and bodies are
// defined in api/openapi.yaml; change that file first.
//
// Every route here needs a signed-in person. The engine serves no report to
// anyone else: the business reads a copy in the public bucket. Sending and
// sharing go through jobs::Reporter, which puts that copy there.

#include "db/audit.hpp"
#include "db/database.hpp"
#include "db/repo_reports.hpp"
#include "http/api.hpp"
#include "http/api_support.hpp"
#include "jobs/reports.hpp"

#include <boost/describe/class.hpp>

namespace poni::http {

namespace asio = boost::asio;
using boost::beast::http::verb;
using namespace api_support;

namespace {

struct ReportCreateBody {
    std::string period_start;
    std::string period_end;
    std::optional<std::string> time_zone;
};
BOOST_DESCRIBE_STRUCT(ReportCreateBody, (), (period_start, period_end, time_zone))

}  // namespace

void add_report_routes(Router& router, const Api& api, jobs::Reporter& reporter) {
    const std::string link_base = reporter.link_base();

    router.add(verb::get, "/api/reports", {Access::user, "reports.list"}, [api](const Request& request) -> asio::awaitable<Response> {
        const QueryParams query(request.query);
        repo::ReportFilter filter;
        bool bad = false;
        filter.location_id = query.integer("location_id", bad);
        filter.status = query.get("status").value_or("");
        if (!read_page(query, filter.page, bad)) co_return bad_query();
        co_return ok(co_await api.query([filter](pqxx::work& tx) { return repo::list_reports(tx, filter); }));
    });

    // "Generate now": a report for any period that is over, built by a job.
    router.add(verb::post, "/api/locations/{id}/reports", {Access::user, "reports.create"},
               [api, link_base](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("location");
                   const auto body = body_as<ReportCreateBody>(request);
                   if (!body) co_return body.error();
                   const auto start = domain::parse_report_day(body->period_start);
                   const auto end = domain::parse_report_day(body->period_end);
                   if (!start || !end) co_return invalid("\"period_start\" and \"period_end\" must be days, YYYY-MM-DD.");
                   repo::NewReport report{.location_id = *id,
                                          .period = {*start, *end},
                                          .time_zone = body->time_zone.value_or("")};
                   co_return respond(co_await api.change(request, [report, link_base](pqxx::work& tx, audit::Writer& audit) {
                       return repo::create_report(tx, audit, report, link_base);
                   }), 201);
               });

    add_get_by_id(router, api, "/api/reports/{id}", "reports.read", "report",
                  [link_base](pqxx::work& tx, std::int64_t id) { return repo::get_report(tx, id, link_base); });

    // The stored page, for the dashboard's preview and for printing. It holds
    // review text, which is untrusted: the page carries no script of its own,
    // and the sandbox makes sure nothing in it could run or use the session.
    router.add(verb::get, "/api/reports/{id}/html", {Access::user, "reports.read"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("report");
                   auto html = co_await api.query([id = *id](pqxx::work& tx) { return repo::report_html(tx, id); });
                   if (!html) co_return not_found("report page");
                   Response response;
                   response.content_type = "text/html; charset=utf-8";
                   response.body = std::move(*html);
                   response.headers = {{"Content-Security-Policy",
                                        "sandbox; default-src 'none'; style-src 'unsafe-inline'; img-src https://*.googleusercontent.com data:"},
                                       {"X-Content-Type-Options", "nosniff"},
                                       {"X-Robots-Tag", "noindex, nofollow"},
                                       {"Referrer-Policy", "no-referrer"},
                                       {"Cache-Control", "private, no-store"}};
                   co_return response;
               });

    // The request that sends email to a location's recipients. Its own action name.
    router.add(verb::post, "/api/reports/{id}/send", {Access::user, "reports.send"},
               [api, &reporter](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("report");
                   co_return respond(co_await reporter.send(*id, api.actor(request)));
               });

    router.add(verb::post, "/api/reports/{id}/reject", {Access::user, "reports.reject"},
               [api, link_base](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("report");
                   co_return respond(co_await api.change(request, [id = *id, link_base](pqxx::work& tx, audit::Writer& audit) {
                       return repo::reject_report(tx, audit, id, link_base);
                   }));
               });

    router.add(verb::post, "/api/reports/{id}/share-links", {Access::user, "reports.share"},
               [api, &reporter](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("report");
                   co_return respond(co_await reporter.create_link(*id, api.actor(request)), 201);
               });

    router.add(verb::post, "/api/report-share-links/{id}/revoke", {Access::user, "reports.share"},
               [api, &reporter](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("share link");
                   co_return respond(co_await reporter.revoke_link(*id, api.actor(request)));
               });

    // ---- schedules ---------------------------------------------------------------------

    router.add(verb::get, "/api/report-schedules", {Access::user, "report_schedules.list"},
               [api](const Request&) -> asio::awaitable<Response> {
                   co_return ok(co_await api.query([](pqxx::work& tx) { return repo::list_schedules(tx); }));
               });

    router.add(verb::post, "/api/report-schedules", {Access::user, "report_schedules.create"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   auto schedule = body_as<repo::NewSchedule>(request);
                   if (!schedule) co_return schedule.error();
                   co_return respond(co_await api.change(request, [schedule = std::move(*schedule)](pqxx::work& tx, audit::Writer& audit) {
                       return repo::create_schedule(tx, audit, schedule);
                   }), 201);
               });

    router.add(verb::patch, "/api/report-schedules/{id}", {Access::user, "report_schedules.update"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("report schedule");
                   auto patch = body_as<repo::SchedulePatch>(request);
                   if (!patch) co_return patch.error();
                   co_return respond(co_await api.change(request, [id = *id, patch = std::move(*patch)](pqxx::work& tx, audit::Writer& audit) {
                       return repo::update_schedule(tx, audit, id, patch);
                   }));
               });

    // Our own row: the reports it built stay.
    router.add(verb::delete_, "/api/report-schedules/{id}", {Access::user, "report_schedules.delete"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("report schedule");
                   co_return respond(co_await api.change(request, [id = *id](pqxx::work& tx, audit::Writer& audit) {
                       return repo::delete_schedule(tx, audit, id);
                   }), Response::no_content());
               });
}

}  // namespace poni::http
