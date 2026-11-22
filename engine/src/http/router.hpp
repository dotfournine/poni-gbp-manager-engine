// Request routing, independent of the socket code in server.cpp so handlers
// and routes can be tested without a network.
//
// Every route states who may call it (Access) and names the action it
// performs. Before a handler runs, the router asks the authorization hook
// (architecture 4.5): the hook finds the signed-in user, refuses requests
// that aren't allowed, and is the one place where roles will be added later.
#pragma once

#include "core/mapped_file.hpp"
#include "domain/session.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/json/value.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace poni::http {

// The signed-in user and their session, set by the authorization hook.
struct Principal {
    std::int64_t user_id = 0;
    std::string email;
    std::string name;
    std::optional<std::string> picture_url;
    std::string session_token_hash;
    // How the session came: the dashboard's cookie, or the phone app's bearer token (D-095).
    domain::SessionKind kind = domain::SessionKind::browser;
    // A browser session's CSRF token. An app session has none.
    std::optional<std::string> csrf_token;
};

struct Request {
    boost::beast::http::verb method = boost::beast::http::verb::get;
    std::string path;   // decoded, without the query string
    std::string query;  // still percent-encoded; read it with QueryParams
    std::string body;
    // The body of a route that takes a file (RouteInfo::max_body_bytes), as
    // the server got it: on disk and mapped, with `body` empty (D-101).
    // Nothing for every other route.
    std::shared_ptr<const core::MappedFile> file;
    // Header names in lower case.
    std::map<std::string, std::string, std::less<>> headers;
    // The visitor's address. The server fills in the peer's address as the
    // socket saw it; behind a trusted reverse proxy the router replaces it
    // with the address the proxy reports (Router::trust_forwarded_for).
    std::string remote_ip;

    // Filled in by the router.
    std::map<std::string, std::string, std::less<>> params;  // "{id}" path parameters
    std::optional<Principal> principal;

    // The header's value, or "" if it is absent. `name` in lower case.
    std::string_view header(std::string_view name) const;
};

struct Response {
    unsigned status = 200;
    std::string content_type = "application/json";
    std::string body;
    // May hold the same name more than once (Set-Cookie).
    std::vector<std::pair<std::string, std::string>> headers;

    static Response json(unsigned status, const boost::json::value& body);
    // The API's error shape: {"error": {"code": "...", "message": "..."}}.
    static Response error(unsigned status, std::string_view code, std::string_view message);
    static Response redirect(std::string location);
    static Response no_content();
};

// A handler reads the request. One that takes it as `Request&` may also take its body away
// (the upload of a post's file gives it to the store), so that a file is never copied.
using Handler = std::function<boost::asio::awaitable<Response>(Request&)>;

enum class Access {
    anonymous,  // no sign-in needed: health, the sign-in flow itself
    user,       // a signed-in user
};

struct RouteInfo {
    Access access = Access::user;
    // "<area>.<verb>", e.g. "reviews.list". What a role will later allow or deny.
    std::string action;
    // The largest request body this route takes, in bytes; 0 is the server's
    // own limit (Router::kDefaultBodyLimit). Only a route that takes a file
    // sets it.
    std::size_t max_body_bytes = 0;
};

// Decides whether the request may proceed. Returns a response to refuse it
// (401, 403), or nothing to let it through. Sets request.principal when the
// request carries a valid session, for anonymous routes too.
using AuthHook = std::function<boost::asio::awaitable<std::optional<Response>>(Request&, const RouteInfo&)>;

class Router {
public:
    // `pattern` is a path whose segments may be parameters: "/api/clients/{id}".
    void add(boost::beast::http::verb method, std::string pattern, RouteInfo info, Handler handler);

    // Without a hook, routes with Access::user answer 401: closed by default.
    void set_auth_hook(AuthHook hook) { hook_ = std::move(hook); }

    // Behind a reverse proxy the peer is the proxy: with this on, a request's
    // remote_ip becomes the last entry of X-Forwarded-For (client_ip) before
    // the hook or a handler sees it.
    void trust_forwarded_for(bool trust) { trust_forwarded_for_ = trust; }

    // What a request body may weigh by default: the API's bodies are small JSON documents.
    static constexpr std::size_t kDefaultBodyLimit = 1024 * 1024;
    // The largest body the route for this method and path takes; the default
    // if no route matches. The server asks before it reads a body.
    std::size_t body_limit(boost::beast::http::verb method, std::string_view path) const;
    // The largest limit of any route: no request may be larger than this.
    std::size_t largest_body_limit() const;

    // For a route that takes a file (a body limit of its own): whether the
    // caller may use it, asked with the request's headers before its body is
    // read, so that nobody who is not signed in can make the engine hold a
    // large body in memory. A response refuses the request; nothing lets
    // the body be read, and dispatch() asks again. Never throws.
    boost::asio::awaitable<std::optional<Response>> refuse_before_body(Request request) const;

    // Never throws: unknown path -> 404, known path with another method -> 405
    // with an Allow header, handler or hook exception -> 500 (logged, details
    // not sent).
    boost::asio::awaitable<Response> dispatch(Request request) const;

    // A route as add() took it: its method, the segments of its pattern ("{id}" for a parameter) and its info.
    struct RouteView {
        boost::beast::http::verb method;
        const std::vector<std::string>& segments;
        const RouteInfo& info;
    };
    // Every route, in the order add() took them. Read-only: for the tests that walk the whole table.
    std::vector<RouteView> routes() const {
        std::vector<RouteView> out;
        out.reserve(routes_.size());
        for (const auto& route : routes_) out.push_back({route.method, route.segments, route.info});
        return out;
    }

private:
    struct Route {
        boost::beast::http::verb method;
        std::vector<std::string> segments;
        RouteInfo info;
        Handler handler;
    };
    // The route dispatch() picks for this method and path: the first that matches. Nothing if none does.
    const Route* route_for(boost::beast::http::verb method, std::string_view path) const;
    // The hook's answer for `route`; without a hook only an anonymous route is open. Throws what the hook throws.
    boost::asio::awaitable<std::optional<Response>> authorize(Request& request, const RouteInfo& route) const;
    std::vector<Route> routes_;
    AuthHook hook_;
    bool trust_forwarded_for_ = false;
};

// The query string's parameters, decoded ('+' is a space).
class QueryParams {
public:
    explicit QueryParams(std::string_view encoded_query);

    // The first value of `name`, if present and not empty.
    std::optional<std::string> get(std::string_view name) const;
    // Every value of `name`; a value "a,b" counts as two.
    std::vector<std::string> all(std::string_view name) const;
    // Whole number, if present and valid; `bad` is set when present but not a number.
    std::optional<std::int64_t> integer(std::string_view name, bool& bad) const;
    // "true"/"false", if present and valid.
    std::optional<bool> boolean(std::string_view name, bool& bad) const;

private:
    std::vector<std::pair<std::string, std::string>> values_;
};

// The client's address: the socket peer, or behind a trusted reverse proxy
// the last entry of X-Forwarded-For (the one the proxy added; earlier ones
// are the client's own claim).
std::string client_ip(const Request& request, bool trust_forwarded_for);

// The value of cookie `name` in a Cookie header, if present.
std::optional<std::string> cookie_value(std::string_view cookie_header, std::string_view name);

}  // namespace poni::http
