// The Workspace directory, read-only: the list of the account's users, a GET
// and nothing else (D-106).
//
// The list holds suspended and archived users too, each with its flag; what
// each means is the caller's to decide. A list that could not be read whole
// is an error, never a shorter list.
#pragma once

#include "connectors/google/auth.hpp"

#include <boost/asio/awaitable.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace poni::workspace {

// Rate limiter key. Google's quota is 2,400 requests a minute; the engine reads one list every few minutes,
// and one more when an account is connected or a person searches or picks someone.
inline constexpr std::string_view kDirectoryApi = "workspace.directory";

// What the directory says about one person.
struct DirectoryUser {
    std::string id;  // the Directory id: the `sub` of that person's sign-in (D-106 point 2)
    std::string primary_email;
    std::string full_name;  // empty when Google names none
    bool suspended = false;
    bool archived = false;
    bool is_admin = false;            // Google's isAdmin: a super administrator
    bool is_delegated_admin = false;  // Google's isDelegatedAdmin: an administrator with a role
};

class Directory {
public:
    virtual ~Directory() = default;
    // Every user of the administrator's Workspace account, all pages, or an error: never a part of the list.
    virtual boost::asio::awaitable<google::Result<std::vector<DirectoryUser>>> list_users() = 0;
};

// The Admin SDK's Directory API. A transport failure throws: the caller
// wraps the read in google::guarded.
class HttpDirectory final : public Directory {
public:
    explicit HttpDirectory(google::Api api) : api_(api) {}

    boost::asio::awaitable<google::Result<std::vector<DirectoryUser>>> list_users() override;

private:
    google::Api api_;
};

}  // namespace poni::workspace
