#include "connectors/workspace/directory.hpp"

#include "core/json.hpp"
#include "http/client.hpp"

#include <boost/describe/class.hpp>

#include <optional>
#include <string>
#include <unordered_set>
#include <utility>

namespace poni::workspace {

namespace asio = boost::asio;
using google::invalid_response;

namespace {

// A page holds at most 500 users (the API's maximum); 20 pages are 10,000
// users, far beyond this account, and a server that always answers a token
// cannot hold the read forever.
constexpr int kMaxPages = 20;
constexpr std::string_view kFirstQuery = "customer=my_customer&maxResults=500";

// One entry of `users`, in Google's names. Google leaves out a flag that is false.
struct GoogleName {
    std::optional<std::string> fullName;
};
BOOST_DESCRIBE_STRUCT(GoogleName, (), (fullName))

struct GoogleUser {
    std::string id;
    std::string primaryEmail;
    std::optional<GoogleName> name;
    std::optional<bool> suspended;
    std::optional<bool> archived;
    std::optional<bool> isAdmin;
    std::optional<bool> isDelegatedAdmin;
};
BOOST_DESCRIBE_STRUCT(GoogleUser, (), (id, primaryEmail, name, suspended, archived, isAdmin, isDelegatedAdmin))

// Nothing if the entry is no user: no id or address, or an id that is not
// one of Google's (the id is what a sign-in's `sub` is compared with).
std::optional<DirectoryUser> parse_user(const boost::json::value& entry) {
    auto user = core::from_json<GoogleUser>(entry);
    if (!user || user->id.empty() || user->primaryEmail.empty() || !http::is_plain_id(user->id)) return std::nullopt;
    return DirectoryUser{
        .id = std::move(user->id),
        .primary_email = std::move(user->primaryEmail),
        .full_name = user->name ? user->name->fullName.value_or("") : "",
        .suspended = user->suspended.value_or(false),
        .archived = user->archived.value_or(false),
        .is_admin = user->isAdmin.value_or(false),
        .is_delegated_admin = user->isDelegatedAdmin.value_or(false),
    };
}

// The address of one page, written as the outbound rule takes it: the token
// encoded by hand, because params().append leaves characters unencoded.
boost::urls::url page_url(const std::optional<std::string>& page_token) {
    auto url = google::url("https://admin.googleapis.com", {"admin", "directory", "v1", "users"});
    std::string query(kFirstQuery);
    if (page_token) query += "&pageToken=" + http::form_encode(*page_token);
    url.set_encoded_query(query);
    return url;
}

}  // namespace

asio::awaitable<google::Result<std::vector<DirectoryUser>>> HttpDirectory::list_users() {
    std::vector<DirectoryUser> all;
    std::unordered_set<std::string> seen;
    std::optional<std::string> page_token;
    for (int page_number = 0; page_number < kMaxPages; ++page_number) {
        http::ClientRequest request;
        request.url = page_url(page_token);
        const auto page = co_await google::fetch_json(api_, kDirectoryApi, std::move(request));
        if (!page) co_return std::unexpected(page.error());
        if (const auto* entries = google::array_member(*page, "users")) {
            for (const auto& entry : *entries) {
                auto user = parse_user(entry);
                if (!user) co_return std::unexpected(invalid_response("the directory's list could not be read"));
                if (!seen.insert(user->id).second) co_return std::unexpected(invalid_response("the directory's list names a user twice"));
                all.push_back(std::move(*user));
            }
        }
        page_token = google::next_page_token(*page);
        if (!page_token) co_return all;
    }
    co_return std::unexpected(invalid_response("the directory's list does not end"));
}

}  // namespace poni::workspace
