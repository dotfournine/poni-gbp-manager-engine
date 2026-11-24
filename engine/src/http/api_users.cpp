// The users part of the dashboard's API: the list of users, and a person of
// the Workspace directory found and made a user before their first sign-in,
// so that they can be assigned an escalation (D-106 point 8). Paths,
// parameters and bodies are defined in api/openapi.yaml; change that file
// first.
//
// Both directory routes read the connector's one GET, the list of users, and
// write nothing at Google. A user is made from the list's own entry for the
// id the request names: nothing about the person is taken from the request.
// Nothing here writes a user's status (the staff check alone does).

#include "connectors/workspace/directory.hpp"
#include "core/log.hpp"
#include "core/text.hpp"
#include "db/audit.hpp"
#include "db/database.hpp"
#include "db/repo_auth.hpp"
#include "db/repo_users.hpp"
#include "http/api.hpp"
#include "http/api_support.hpp"
#include "http/auth.hpp"
#include "http/client.hpp"

#include <boost/describe/class.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace poni::http {

namespace asio = boost::asio;
using boost::beast::http::verb;
using namespace api_support;

namespace {

struct UserCreateBody {
    std::string directory_id;
};
BOOST_DESCRIBE_STRUCT(UserCreateBody, (), (directory_id))

// How many people a search answers.
constexpr std::size_t kDirectoryMatchesMax = 50;

std::string_view kind_name(google::ApiError::Kind kind) {
    using Kind = google::ApiError::Kind;
    switch (kind) {
        case Kind::not_configured: return "not_configured";
        case Kind::unauthenticated: return "unauthenticated";
        case Kind::permission_denied: return "permission_denied";
        case Kind::not_found: return "not_found";
        case Kind::rate_limited: return "rate_limited";
        case Kind::unavailable: return "unavailable";
        case Kind::invalid_response: return "invalid_response";
        case Kind::other: break;
    }
    return "other";
}

// The directory's whole list, read through the connected administrator; a refusal while none is connected, and a 502
// for a list that could not be read whole, never a part of it.
asio::awaitable<repo::Result<std::vector<workspace::DirectoryUser>>> read_directory(const Api& api, workspace::Directory& directory) {
    const bool connected =
        co_await api.query([](pqxx::work& tx) { return repo::load_credential(tx, repo::kWorkspaceCredential).has_value(); });
    if (!connected) {
        co_return repo::conflict("workspace_not_connected", "No Workspace administrator is connected. Connect one on the status page.");
    }
    auto list = co_await google::guarded(directory.list_users());
    if (!list) {
        log::warn("the Workspace directory could not be read", {{"kind", kind_name(list.error().kind)}, {"error", list.error().message}});
        co_return repo::unavailable("directory_unavailable", "The Workspace directory could not be read. Try again.");
    }
    co_return std::move(*list);
}

// A person who may be made a user: an account in use, with an address of the sign-in domains.
bool can_be_user(const workspace::DirectoryUser& person, std::span<const std::string> signin_domains) {
    return !person.suspended && !person.archived && address_in_domains(person.primary_email, signin_domains);
}

}  // namespace

void add_user_routes(Router& router, const Api& api, workspace::Directory& directory, std::span<const std::string> signin_domains) {
    router.add(verb::get, "/api/users", {Access::user, "users.list"}, [api](const Request& request) -> asio::awaitable<Response> {
        const QueryParams query(request.query);
        std::string status = "active";
        bool bad = false;
        read_choice(query, "status", repo::kUserStatusFilters, status, bad);
        if (bad) co_return bad_query();
        co_return ok(co_await api.query([status](pqxx::work& tx) { return repo::list_users(tx, status); }));
    });

    // The whole list, filtered here: Google's own search matches whole words and prefixes only, and may not list a new
    // account for a day or more, and the new colleague is the very person this is for. Nothing of it is kept.
    router.add(verb::get, "/api/users/directory", {Access::user, "users.directory"},
               [api, &directory, signin_domains](const Request& request) -> asio::awaitable<Response> {
                   const QueryParams query(request.query);
                   const std::string q(text::trim(query.get("q").value_or("")));
                   if (const auto length = text::length(q); length < 2 || length > 100) co_return invalid("\"q\" must be 2 to 100 characters.");
                   auto people = co_await read_directory(api, directory);
                   if (!people) co_return respond(people.error());

                   const auto needle = text::ascii_lower(q);
                   std::vector<workspace::DirectoryUser> found;
                   for (auto& person : *people) {
                       if (!can_be_user(person, signin_domains)) continue;
                       if (!text::ascii_lower(person.full_name).contains(needle) && !text::ascii_lower(person.primary_email).contains(needle)) {
                           continue;
                       }
                       found.push_back(std::move(person));
                   }
                   std::ranges::sort(found, [](const workspace::DirectoryUser& a, const workspace::DirectoryUser& b) {
                       return std::tuple(text::ascii_lower(a.full_name), std::string_view(a.primary_email)) <
                              std::tuple(text::ascii_lower(b.full_name), std::string_view(b.primary_email));
                   });
                   if (found.size() > kDirectoryMatchesMax) found.resize(kDirectoryMatchesMax);

                   const auto ids = found | std::views::transform(&workspace::DirectoryUser::id) | std::ranges::to<std::vector>();
                   const auto users = co_await api.query([ids](pqxx::work& tx) { return repo::user_ids_by_google_sub(tx, ids); });
                   model::DirectoryPersonList answer;
                   for (auto& person : found) {
                       const auto user = users.find(person.id);
                       answer.items.push_back({.directory_id = std::move(person.id),
                                               .email = std::move(person.primary_email),
                                               .name = std::move(person.full_name),
                                               .user_id = user == users.end() ? std::nullopt : std::optional(user->second)});
                   }
                   co_return ok(answer);
               });

    // The picker makes the user first and then assigns the escalation by the user's id: the assignment never makes users.
    router.add(verb::post, "/api/users", {Access::user, "users.create"},
               [api, &directory, signin_domains](const Request& request) -> asio::awaitable<Response> {
                   const auto body = body_as<UserCreateBody>(request);
                   if (!body) co_return body.error();
                   if (!is_plain_id(body->directory_id)) co_return invalid("\"directory_id\" must be an id from GET /api/users/directory.");
                   auto people = co_await read_directory(api, directory);
                   if (!people) co_return respond(people.error());

                   const auto person = std::ranges::find(*people, body->directory_id, &workspace::DirectoryUser::id);
                   if (person == people->end()) co_return not_found("directory user");
                   if (person->suspended || person->archived) {
                       co_return respond(repo::conflict("directory_user_inactive", "This account is suspended or archived at Google.").error());
                   }
                   if (!address_in_domains(person->primary_email, signin_domains)) {
                       co_return respond(repo::conflict("domain_not_allowed", "This address is not of the domains that may sign in.").error());
                   }
                   const auto made = co_await api.change(request, [person = *person](pqxx::work& tx, audit::Writer& audit) {
                       return repo::create_directory_user(tx, audit, person);
                   });
                   co_return ok(made.user, made.created ? 201 : 200);
               });
}

}  // namespace poni::http
