// The local posts part of the dashboard's API (FR-14, D-088): the posts a
// business publishes on its Google Business Profile. Paths, parameters and
// bodies are defined in api/openapi.yaml; change that file first.
//
// Every route here needs a signed-in person. A post reaches Google in one
// way: the approval of a signed-in person, which carries the content that
// person read (D-088 point 3).
//
// The last three routes are the help with writing one (D-088 point 11): what
// customers praised lately, from the stored analysis, and the model's ideas
// and texts, which are asked for by a person, stored nowhere and published
// by nothing.

#include "connectors/storage/store.hpp"
#include "core/crypto.hpp"
#include "core/log.hpp"
#include "core/text.hpp"
#include "db/audit.hpp"
#include "db/database.hpp"
#include "db/repo_local_posts.hpp"
#include "db/repo_post_help.hpp"
#include "db/repo_catalog.hpp"
#include "domain/image_info.hpp"
#include "domain/video_info.hpp"
#include "http/api.hpp"
#include "http/api_support.hpp"
#include "jobs/post_advisor.hpp"

#include <boost/describe/class.hpp>
#include <boost/json/object.hpp>

#include <optional>
#include <string>

namespace poni::http {

namespace asio = boost::asio;
using boost::beast::http::verb;
using namespace api_support;

namespace {

struct RejectionBody {
    std::optional<std::string> reason;
};
BOOST_DESCRIBE_STRUCT(RejectionBody, (), (reason))

// Every member is sent, the title as null where there is none.
struct DraftRequestBody {
    std::string topic_type;
    std::string language_code;
    std::string brief;
    std::string summary;
    std::optional<std::string> title;
    static constexpr bool sent_whole = true;
};
BOOST_DESCRIBE_STRUCT(DraftRequestBody, (), (topic_type, language_code, brief, summary, title))

// The limits of LocalPostDraftRequest, in code points.
constexpr std::size_t kBriefMax = 2000;
constexpr std::size_t kCurrentTextMax = 6000;
constexpr std::size_t kCurrentTitleMax = 200;


// The Google mock has no posts: nothing that would call Google about them is started.
Response unavailable() {
    return Response::error(409, "local_posts_unavailable",
                           "The engine works against the Google mock, which has no local posts. They need a real Google account.");
}

}  // namespace

void add_local_post_routes(Router& router, const Api& api, storage::ObjectStore* public_store, jobs::PostAdvisor& advisor) {
    // Where an uploaded photo is read from: the public bucket's address.
    // Where an uploaded photo of a local post is read from; empty without the bucket.
    const std::string media_base = public_store ? public_store->base_url() : std::string{};

    router.add(verb::get, "/api/local-posts", {Access::user, "local_posts.list"},
               [api, media_base](const Request& request) -> asio::awaitable<Response> {
                   const QueryParams query(request.query);
                   repo::LocalPostFilter filter;
                   bool bad = false;
                   filter.client_id = query.integer("client_id", bad);
                   filter.location_id = query.integer("location_id", bad);
                   read_choices(query, "status", repo::kLocalPostStatuses, filter.statuses, bad);
                   read_choices(query, "google_state", repo::kLocalPostGoogleStates, filter.google_states, bad);
                   if (!read_page(query, filter.page, bad)) co_return bad_query();
                   co_return ok(co_await api.query([filter, media_base](pqxx::work& tx) { return repo::list_local_posts(tx, filter, media_base); }));
               });

    add_get_by_id(router, api, "/api/local-posts/{id}", "local_posts.read", "local post",
                  [media_base](pqxx::work& tx, std::int64_t id) { return repo::get_local_post(tx, id, media_base); });

    // A new draft. Nothing goes to Google until a person approves it.
    router.add(verb::post, "/api/locations/{id}/local-posts", {Access::user, "local_posts.create"},
               [api, media_base](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("location");
                   if (api.options.google_mock) co_return unavailable();
                   const auto body = body_as<domain::LocalPostContent>(request);
                   if (!body) co_return body.error();
                   co_return respond(co_await api.change(request, [id = *id, content = *body, media_base](pqxx::work& tx, audit::Writer& audit) {
                       return repo::create_local_post(tx, audit, id, content, media_base);
                   }), 201);
               });

    router.add(verb::put, "/api/local-posts/{id}", {Access::user, "local_posts.update"},
               [api, media_base](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("local post");
                   const auto body = body_as<domain::LocalPostContent>(request);
                   if (!body) co_return body.error();
                   co_return respond(co_await api.change(request, [id = *id, content = *body, media_base](pqxx::work& tx, audit::Writer& audit) {
                       return repo::edit_local_post(tx, audit, id, content, media_base);
                   }));
               });

    // Only what Google never had: a draft or a rejected one (D-094). Nothing at Google is touched.
    router.add(verb::delete_, "/api/local-posts/{id}", {Access::user, "local_posts.delete"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("local post");
                   co_return respond(co_await api.change(request, [id = *id](pqxx::work& tx, audit::Writer& audit) {
                       return repo::delete_local_post(tx, audit, id);
                   }), Response::no_content());
               });

    // A new draft from any post, also one made at Google (D-094). Like writing one, it is not offered
    // against the mock; nothing goes to Google and nothing is queued.
    router.add(verb::post, "/api/local-posts/{id}/copy", {Access::user, "local_posts.copy"},
               [api, media_base](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("local post");
                   if (api.options.google_mock) co_return unavailable();
                   co_return respond(co_await api.change(request, [id = *id, media_base](pqxx::work& tx, audit::Writer& audit) {
                       return repo::copy_local_post(tx, audit, id, media_base);
                   }), 201);
               });

    // The only way a local post reaches Google: the body is the content the person read.
    router.add(verb::post, "/api/local-posts/{id}/approve", {Access::user, "local_posts.approve"},
               [api, media_base](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("local post");
                   if (api.options.google_mock) co_return unavailable();
                   const auto body = body_as<domain::LocalPostContent>(request);
                   if (!body) co_return body.error();
                   co_return respond(co_await api.change(request, [id = *id, content = *body, media_base](pqxx::work& tx, audit::Writer& audit) {
                       return repo::approve_local_post(tx, audit, id, content, media_base);
                   }), 202);
               });

    router.add(verb::post, "/api/local-posts/{id}/reject", {Access::user, "local_posts.reject"},
               [api, media_base](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("local post");
                   // The body, and the reason in it, are optional.
                   const auto body = optional_body_as<RejectionBody>(request);
                   if (!body) co_return body.error();
                   const std::string reason = body->reason.value_or("");
                   co_return respond(co_await api.change(request, [id = *id, reason, media_base](pqxx::work& tx, audit::Writer& audit) {
                       return repo::reject_local_post(tx, audit, id, reason, media_base);
                   }));
               });

    // Deleting at Google: any post Google has, also one made in its own interface. It works while posting is off.
    router.add(verb::post, "/api/local-posts/{id}/remove", {Access::user, "local_posts.remove"},
               [api, media_base](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("local post");
                   if (api.options.google_mock) co_return unavailable();
                   co_return respond(co_await api.change(request, [id = *id, media_base](pqxx::work& tx, audit::Writer& audit) {
                       return repo::request_local_post_removal(tx, audit, id, media_base);
                   }), 202);
               });

    // A photo or a video for posts: the body is the file. It goes into the public bucket exactly as
    // it was uploaded, under a name nobody can guess, because Google takes a post's media only from
    // a public address (D-088 point 9, D-090). The route's own body limit is the largest video
    // Google takes; a photo's own limit is checked below.
    router.add(verb::post, "/api/local-post-media", {Access::user, "local_posts.upload_media", domain::kVideoMaxBytes},
               [api, public_store](Request& request) -> asio::awaitable<Response> {
                   if (api.options.google_mock) co_return unavailable();
                   if (!public_store) {
                       co_return Response::error(409, "no_public_bucket",
                                                 "The engine has no public bucket (PONI_PUBLIC_BUCKET), so a post can have no photo or video.");
                   }
                   // What the file says it is, from its own bytes: the Content-Type header is the browser's guess.
                   repo::NewLocalPostMedia media;
                   std::string extension;
                   if (domain::looks_like_video(request.body)) {
                       const auto info = domain::inspect_video(request.body);
                       if (const auto problem = domain::video_problem(request.body, info)) co_return invalid(*problem);
                       media = {.object_name = {},
                                .format = "video",
                                .content_type = info->content_type,
                                .bytes = static_cast<std::int64_t>(request.body.size()),
                                .width = info->width,
                                .height = info->height,
                                .duration_ms = info->duration_ms};
                       extension = info->extension;
                   } else {
                       const auto info = domain::inspect_image(request.body);
                       if (!info) co_return invalid("The file is neither a JPG or PNG photo nor an MP4 or QuickTime video.");
                       if (const auto problem = domain::photo_problem(request.body, info)) co_return invalid(*problem);
                       media = {.object_name = {},
                                .format = "photo",
                                .content_type = info->content_type,
                                .bytes = static_cast<std::int64_t>(request.body.size()),
                                .width = info->width,
                                .height = info->height,
                                .duration_ms = std::nullopt};
                       extension = info->extension;
                   }
                   media.object_name = std::format("p/{}.{}", core::base64url_encode(core::random_bytes(32)), extension);
                   // The bytes as they came: nothing is resized, converted or stripped.
                   std::string failure;
                   try {
                       // The file is given to the store, not copied: nothing below reads the request's body.
                       const auto stored = co_await public_store->put({.name = media.object_name,
                                                                       .content_type = media.content_type,
                                                                       .body = std::move(request.body),
                                                                       .cache_control = "public, max-age=3600"});
                       if (!stored) failure = stored.error().message;
                   } catch (const std::exception& e) {
                       failure = e.what();
                   }
                   if (!failure.empty()) {
                       log::warn("a post's photo or video could not be stored", {{"error", failure}});
                       co_return Response::error(502, "storage_failed", "The file could not be put into the public bucket. Nothing was recorded.");
                   }
                   const auto base = public_store->base_url();
                   co_return ok(co_await api.change(request, [media, base](pqxx::work& tx, audit::Writer& audit) {
                       return repo::create_local_post_media(tx, audit, media, base);
                   }), 201);
               });

    // "Refresh": the read that otherwise runs once an hour, for one location, now.
    router.add(verb::post, "/api/locations/{id}/local-posts/refresh", {Access::user, "local_posts.refresh"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("location");
                   if (api.options.google_mock) {
                       // Nothing is queued against the mock. A location that does not exist is a 404 all the same.
                       const bool exists = co_await api.query([id = *id](pqxx::work& tx) { return repo::get_location(tx, id).has_value(); });
                       if (!exists) co_return not_found("location");
                       co_return unavailable();
                   }
                   co_return respond(co_await api.change(request, [id = *id](pqxx::work& tx, audit::Writer& audit) {
                       return repo::request_local_posts_sync(tx, audit, id);
                   }), enqueued());
               });

    // What customers praised lately: counted from the stored analysis, without a model and without Google.
    router.add(verb::get, "/api/locations/{id}/review-highlights", {Access::user, "local_posts.highlights"},
               [api](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("location");
                   co_return respond(co_await api.query([id = *id, now = api.clock.now()](pqxx::work& tx) -> std::optional<model::ReviewHighlights> {
                       const auto help = repo::load_post_help(tx, id, now);
                       if (!help) return std::nullopt;
                       return repo::review_highlights(*help);
                   }), "location");
               });

    // The model, asked by a person. Nothing is stored and nothing changes, so there is no audit entry:
    // the call itself is in llm_calls, with the location.
    router.add(verb::post, "/api/locations/{id}/local-post-ideas", {Access::user, "local_posts.suggest_ideas"},
               [api, &advisor](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("location");
                   if (api.options.google_mock) co_return unavailable();
                   co_return respond(co_await advisor.ideas(*id));
               });

    router.add(verb::post, "/api/locations/{id}/local-post-drafts", {Access::user, "local_posts.suggest_draft"},
               [api, &advisor](const Request& request) -> asio::awaitable<Response> {
                   const auto id = path_id(request);
                   if (!id) co_return not_found("location");
                   const auto body = body_as<DraftRequestBody>(request);
                   if (!body) co_return body.error();
                   domain::PostDraftRequest asked{.topic_type = body->topic_type,
                                                  .language_code = std::string(text::trim(body->language_code)),
                                                  .brief = std::string(text::trim(body->brief)),
                                                  .summary = std::string(text::trim(body->summary)),
                                                  .title = std::nullopt};
                   if (body->title && !text::trim(*body->title).empty()) asked.title = std::string(text::trim(*body->title));
                   if (!std::ranges::contains(domain::kPostTopicTypes, std::string_view(asked.topic_type))) {
                       co_return invalid("The kind of post must be standard, event or offer.");
                   }
                   // The tag goes into the model's instructions: only what a language tag is made of.
                   if (!domain::is_post_language_tag(asked.language_code)) {
                       co_return invalid("The language must be a tag such as en or zh-Hant.");
                   }
                   if (asked.brief.empty() && asked.summary.empty()) co_return invalid("Say what the post is about, or give a text to improve.");
                   if (text::length(asked.brief) > kBriefMax || text::length(asked.summary) > kCurrentTextMax ||
                       text::length(asked.title.value_or("")) > kCurrentTitleMax) {
                       co_return invalid("The brief or the text is too long.");
                   }
                   if (api.options.google_mock) co_return unavailable();
                   co_return respond(co_await advisor.draft(*id, std::move(asked)));
               });
}

}  // namespace poni::http
