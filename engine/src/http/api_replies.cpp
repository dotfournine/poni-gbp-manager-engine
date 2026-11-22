// The dashboard's API for the approval queue (edit,
// regenerate, approve, reject a draft), escalations, notification rules and
// the delivery log, and the posting and sending switches. Paths, parameters
// and bodies are defined in api/openapi.yaml; change that file first.
//
// Approving a draft is the only way a reply reaches Google, and all it does
// here is mark the draft and enqueue the posting job (jobs/posting.hpp).

#include "core/text.hpp"
#include "db/audit.hpp"
#include "db/database.hpp"
#include "db/repo_analysis.hpp"
#include "db/repo_cases.hpp"
#include "db/repo_devices.hpp"
#include "db/repo_notifications.hpp"
#include "db/repo_posting.hpp"
#include "db/repo_reviews.hpp"
#include "http/api.hpp"
#include "http/api_support.hpp"
#include "jobs/analysis.hpp"

#include <boost/describe/class.hpp>

#include <algorithm>
#include <chrono>
#include <format>

namespace poni::http {

namespace asio = boost::asio;
using boost::beast::http::verb;
using namespace api_support;

namespace {

struct TextBody {
    std::string text;
};
BOOST_DESCRIBE_STRUCT(TextBody, (), (text))

struct ReasonBody {
    std::string reason;
};
BOOST_DESCRIBE_STRUCT(ReasonBody, (), (reason))

struct ClosureBody {
    std::string reason;
    std::optional<std::string> note;
};
BOOST_DESCRIBE_STRUCT(ClosureBody, (), (reason, note))

// `assignee_id` must be there and may be null: nobody.
struct AssignmentBody {
    std::optional<std::int64_t> assignee_id;
    static constexpr bool sent_whole = true;
};
BOOST_DESCRIBE_STRUCT(AssignmentBody, (), (assignee_id))

struct InstructionsBody {
    std::string instructions;
};
BOOST_DESCRIBE_STRUCT(InstructionsBody, (), (instructions))

struct IgnoreBody {
    std::optional<std::string> reason;
};
BOOST_DESCRIBE_STRUCT(IgnoreBody, (), (reason))

struct RecipientBody {
    std::string recipient;
};
BOOST_DESCRIBE_STRUCT(RecipientBody, (), (recipient))

}  // namespace

void add_reply_routes(Router& router, const Api& api, jobs::ReviewPipeline& pipeline) {
    // ---- the approval queue ------------------------------------------------------

    add_get_by_id(router, api, "/api/drafts/{id}", "drafts.read", "draft", repo::get_draft);

    router.add(verb::patch, "/api/drafts/{id}", {Access::user, "drafts.edit"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("draft");
                   const auto body = body_as<TextBody>(request);
                   if (!body) co_return body.error();
                   co_return respond(co_await api.change(request, [id = *id, text = body->text](pqxx::work& tx, audit::Writer& audit) {
                       return repo::edit_draft(tx, audit, id, text);
                   }));
               });

    // The one request that leads to a write on Google. It names its own
    // action, so a role can later allow reading drafts without approving them.
    router.add(verb::post, "/api/drafts/{id}/approve", {Access::user, "drafts.approve"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("draft");
                   const auto body = body_as<TextBody>(request);
                   if (!body) co_return body.error();
                   co_return respond(co_await api.change(request, [id = *id, text = body->text](pqxx::work& tx, audit::Writer& audit) {
                       return repo::approve_draft(tx, audit, id, text);
                   }), 202);
               });

    router.add(verb::post, "/api/drafts/{id}/reject", {Access::user, "drafts.reject"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("draft");
                   const auto body = body_as<ReasonBody>(request);
                   if (!body) co_return body.error();
                   co_return respond(co_await api.change(request, [id = *id, reason = body->reason](pqxx::work& tx, audit::Writer& audit) {
                       return repo::reject_draft(tx, audit, id, reason);
                   }));
               });

    router.add(verb::post, "/api/drafts/{id}/regenerate", {Access::user, "drafts.regenerate"},
               [api, &pipeline](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("draft");
                   const auto body = body_as<InstructionsBody>(request);
                   if (!body) co_return body.error();
                   const std::string instructions(text::trim(body->instructions));
                   if (instructions.empty() || text::length(instructions) > 1000) {
                       co_return invalid("\"instructions\" must be 1 to 1000 characters.");
                   }
                   const auto who = api.actor(request);
                   const auto written = co_await pipeline.regenerate(*id, instructions, who);
                   if (!written) co_return respond(written.error());
                   co_return respond(co_await api.query([id = *written](pqxx::work& tx) { return repo::get_draft(tx, id); }), "draft", 201);
               });

    // ---- "no reply needed" --------------------------------------------------------

    router.add(verb::post, "/api/reviews/{id}/ignore", {Access::user, "reviews.ignore"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("review");
                   // The body is optional: ignoring a review is one click.
                   const auto body = optional_body_as<IgnoreBody>(request);
                   if (!body) co_return body.error();
                   const std::string reason = body->reason.value_or("");
                   co_return respond(co_await api.change(request, [id = *id, reason](pqxx::work& tx, audit::Writer& audit) {
                       return repo::ignore_review(tx, audit, id, reason);
                   }));
               });

    router.add(verb::post, "/api/reviews/{id}/unignore", {Access::user, "reviews.ignore"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("review");
                   co_return respond(co_await api.change(request, [id = *id](pqxx::work& tx, audit::Writer& audit) {
                       return repo::unignore_review(tx, audit, id);
                   }));
               });

    // ---- escalations -------------------------------------------------------------

    router.add(verb::get, "/api/escalations", {Access::user, "escalations.list"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const QueryParams query(request.query);
                   repo::EscalationFilter filter;
                   bool bad = false;
                   read_choice(query, "status", repo::kEscalationStatuses, filter.status, bad);
                   filter.location_id = query.integer("location_id", bad);
                   filter.assignee_id = query.integer("assignee_id", bad);
                   if (!read_page(query, filter.page, bad)) co_return bad_query();
                   co_return ok(co_await api.query([filter](pqxx::work& tx) { return repo::list_escalations(tx, filter); }));
               });

    add_get_by_id(router, api, "/api/escalations/{id}", "escalations.read", "escalation", repo::get_escalation);

    router.add(verb::post, "/api/escalations/{id}/assign", {Access::user, "escalations.assign"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("escalation");
                   const auto body = body_as<AssignmentBody>(request);
                   if (!body) co_return body.error();
                   co_return respond(co_await api.change(request, [id = *id, assignee_id = body->assignee_id](pqxx::work& tx, audit::Writer& audit) {
                       return repo::assign_escalation(tx, audit, id, assignee_id);
                   }));
               });

    router.add(verb::post, "/api/escalations/{id}/close", {Access::user, "escalations.close"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("escalation");
                   const auto body = body_as<ClosureBody>(request);
                   if (!body) co_return body.error();
                   co_return respond(co_await api.change(request, [id = *id, body = *body](pqxx::work& tx, audit::Writer& audit) {
                       return repo::close_escalation(tx, audit, id, body.reason, body.note.value_or(""));
                   }));
               });

    router.add(verb::get, "/api/users", {Access::user, "users.list"}, [api](const Request&) -> asio::awaitable<Response> {
        co_return ok(co_await api.query([](pqxx::work& tx) { return repo::list_users(tx); }));
    });

    // ---- notification rules and the delivery log ---------------------------------

    router.add(verb::get, "/api/notification-rules", {Access::user, "notification_rules.list"},
               [api](const Request&) -> asio::awaitable<Response> {
                   co_return ok(co_await api.query([](pqxx::work& tx) { return repo::list_rules(tx); }));
               });

    router.add(verb::post, "/api/notification-rules", {Access::user, "notification_rules.create"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const auto rule = body_as<repo::NewRule>(request);
                   if (!rule) co_return rule.error();
                   const bool push = api.options.push_environment.has_value();
                   co_return respond(co_await api.change(request, [rule = *rule, push](pqxx::work& tx, audit::Writer& audit) {
                       return repo::create_rule(tx, audit, rule, push);
                   }), 201);
               });

    add_get_by_id(router, api, "/api/notification-rules/{id}", "notification_rules.read", "notification rule", repo::get_rule);

    router.add(verb::patch, "/api/notification-rules/{id}", {Access::user, "notification_rules.update"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("notification rule");
                   const auto patch = body_as<repo::RulePatch>(request);
                   if (!patch) co_return patch.error();
                   co_return respond(co_await api.change(request, [id = *id, patch = *patch](pqxx::work& tx, audit::Writer& audit) {
                       return repo::update_rule(tx, audit, id, patch);
                   }));
               });

    // Our own row, not anything on Google: the engine never deletes there.
    router.add(verb::delete_, "/api/notification-rules/{id}", {Access::user, "notification_rules.delete"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("notification rule");
                   co_return respond(co_await api.change(request, [id = *id](pqxx::work& tx, audit::Writer& audit) {
                       return repo::delete_rule(tx, audit, id);
                   }), Response::no_content());
               });

    // ---- the phone app's devices, for push (D-097) ---------------------------------

    router.add(verb::post, "/api/devices", {Access::user, "devices.register"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const auto device = body_as<repo::NewDevice>(request);
                   if (!device) co_return device.error();
                   // A device is its app session: a browser's session has no phone to tell.
                   if (request.principal->kind != domain::SessionKind::app) {
                       co_return invalid("Only the phone app registers a device, with its bearer token.");
                   }
                   co_return respond(co_await api.change(request, [device = *device, session = request.principal->session_token_hash,
                                                                   environment = api.options.push_environment](pqxx::work& tx,
                                                                                                               audit::Writer& audit) {
                       return repo::register_device(tx, audit, session, device, environment);
                   }), 201);
               });

    router.add(verb::delete_, "/api/devices/{id}", {Access::user, "devices.unregister"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("device");
                   co_return respond(co_await api.change(request, [id = *id](pqxx::work& tx, audit::Writer& audit) {
                       return repo::unregister_device(tx, audit, audit.user_id(), id);
                   }), Response::no_content());
               });

    router.add(verb::get, "/api/notification-deliveries", {Access::user, "notification_deliveries.list"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const QueryParams query(request.query);
                   repo::DeliveryFilter filter;
                   bool bad = false;
                   read_choice(query, "event_type", repo::kEventTypes, filter.event_type, bad);
                   read_choice(query, "status", repo::kDeliveryStatuses, filter.status, bad);
                   filter.escalation_id = query.integer("escalation_id", bad);
                   if (!read_page(query, filter.page, bad)) co_return bad_query();
                   co_return ok(co_await api.query([filter](pqxx::work& tx) { return repo::list_deliveries(tx, filter); }));
               });

    router.add(verb::post, "/api/notifications/test", {Access::user, "notifications.test"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const auto body = body_as<RecipientBody>(request);
                   if (!body) co_return body.error();
                   if (!repo::valid_email(body->recipient)) co_return invalid("\"recipient\" must be an email address.");
                   const auto delivery = co_await api.change(
                       request, [recipient = body->recipient, asked_by = request.principal->email](pqxx::work& tx, audit::Writer& audit) {
                           const auto id = repo::emit_to(
                               tx,
                               {.type = std::string(repo::kEventTest),
                                .key = std::format("test:{}", db::to_timestamptz(audit.now())),
                                .client_id = std::nullopt,
                                .location_id = std::nullopt,
                                .review_id = std::nullopt,
                                .subject = "Test message from PONI GBP Manager",
                                .body = std::format("This is a test message, asked for by {}. If you can read it, notifications by email work.",
                                                    asked_by),
                                .link_path = "/#/notification-deliveries"},
                               recipient, audit.now());
                           audit.record({.action = "notification.test", .object_type = "notification_delivery",
                                         .object_id = id ? std::to_string(*id) : std::string{},
                                         .after = {{"recipient", recipient}}});
                           return id ? repo::get_delivery(tx, *id) : std::nullopt;
                       });
                   if (!delivery) co_return Response::error(409, "try_again", "A test message was queued this very moment. Try again.");
                   co_return ok(*delivery, 202);
               });

    // ---- the switches ------------------------------------------------------------

    router.add(verb::get, "/api/system/settings", {Access::user, "system_settings.read"},
               [api](const Request&) -> asio::awaitable<Response> {
                   co_return ok(co_await api.query([](pqxx::work& tx) { return repo::get_system_settings(tx); }));
               });

    router.add(verb::patch, "/api/system/settings", {Access::user, "system_settings.update"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const auto patch = body_as<repo::SystemSettingsPatch>(request);
                   if (!patch) co_return patch.error();
                   co_return ok(co_await api.change(request, [patch = *patch](pqxx::work& tx, audit::Writer& audit) {
                       return repo::update_system_settings(tx, audit, patch);
                   }));
               });
}

}  // namespace poni::http
