#include "http/router.hpp"

#include <algorithm>

#include "core/log.hpp"

#include <boost/json/serialize.hpp>
#include <boost/url/encoding_opts.hpp>
#include <boost/url/params_encoded_view.hpp>
#include <boost/url/parse_query.hpp>

#include <charconv>
#include <exception>

namespace poni::http {

namespace asio = boost::asio;

namespace {

std::vector<std::string> split_path(std::string_view path) {
    std::vector<std::string> segments;
    if (path.starts_with('/')) path.remove_prefix(1);
    while (true) {
        const auto slash = path.find('/');
        segments.emplace_back(path.substr(0, slash));
        if (slash == std::string_view::npos) break;
        path.remove_prefix(slash + 1);
    }
    return segments;
}

bool is_parameter(std::string_view segment) {
    return segment.size() > 2 && segment.front() == '{' && segment.back() == '}';
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    return s;
}

// What a hook or a handler threw: logged with what was asked, and answered without the details.
Response internal_error(std::string_view what, const Request& request, const RouteInfo& route, const std::string& error) {
    log::error(what, {{"path", request.path}, {"action", route.action}, {"error", error}});
    return Response::error(500, "internal_error", "Something went wrong on the server.");
}

}  // namespace

std::string_view Request::header(std::string_view name) const {
    const auto it = headers.find(name);
    return it == headers.end() ? std::string_view{} : std::string_view(it->second);
}

Response Response::json(unsigned status, const boost::json::value& body) {
    return Response{.status = status, .content_type = "application/json", .body = boost::json::serialize(body), .headers = {}};
}

Response Response::error(unsigned status, std::string_view code, std::string_view message) {
    return json(status, boost::json::object{{"error", boost::json::object{{"code", code}, {"message", message}}}});
}

Response Response::redirect(std::string location) {
    return Response{.status = 302, .content_type = "text/plain", .body = {}, .headers = {{"Location", std::move(location)}}};
}

Response Response::no_content() { return Response{.status = 204, .content_type = "application/json", .body = {}, .headers = {}}; }

void Router::add(boost::beast::http::verb method, std::string pattern, RouteInfo info, Handler handler) {
    routes_.push_back({method, split_path(pattern), std::move(info), std::move(handler)});
}

namespace {

bool matches(const std::vector<std::string>& pattern, const std::vector<std::string>& segments) {
    if (pattern.size() != segments.size()) return false;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (is_parameter(pattern[i]) ? segments[i].empty() : pattern[i] != segments[i]) return false;
    }
    return true;
}

}  // namespace

const Router::Route* Router::route_for(boost::beast::http::verb method, std::string_view path) const {
    const auto segments = split_path(path);
    for (const auto& route : routes_) {
        if (route.method == method && matches(route.segments, segments)) return &route;
    }
    return nullptr;
}

std::size_t Router::body_limit(boost::beast::http::verb method, std::string_view path) const {
    const auto* route = route_for(method, path);
    return route && route->info.max_body_bytes > 0 ? route->info.max_body_bytes : kDefaultBodyLimit;
}

std::size_t Router::largest_body_limit() const {
    std::size_t largest = kDefaultBodyLimit;
    for (const auto& route : routes_) largest = std::max(largest, route.info.max_body_bytes);
    return largest;
}

std::string client_ip(const Request& request, bool trust_forwarded_for) {
    if (trust_forwarded_for) {
        auto forwarded = request.header("x-forwarded-for");
        if (const auto comma = forwarded.rfind(','); comma != std::string_view::npos) forwarded.remove_prefix(comma + 1);
        if (const auto last = trim(forwarded); !last.empty()) return std::string(last);
    }
    return request.remote_ip;
}

asio::awaitable<std::optional<Response>> Router::authorize(Request& request, const RouteInfo& route) const {
    if (hook_) co_return co_await hook_(request, route);
    if (route.access != Access::anonymous) co_return Response::error(401, "unauthenticated", "Sign in to continue.");
    co_return std::nullopt;
}

asio::awaitable<std::optional<Response>> Router::refuse_before_body(Request request) const {
    request.remote_ip = client_ip(request, trust_forwarded_for_);
    const Route* match = route_for(request.method, request.path);
    // Only a route with a limit of its own: every other body is small, and dispatch() answers.
    if (!match || match->info.max_body_bytes == 0) co_return std::nullopt;
    std::optional<std::string> error;
    try {
        co_return co_await authorize(request, match->info);
    } catch (const std::exception& e) {
        error = e.what();
    } catch (...) {
        error = "unknown error";
    }
    co_return internal_error("the check before a request body failed", request, match->info, *error);
}

asio::awaitable<Response> Router::dispatch(Request request) const {
    request.remote_ip = client_ip(request, trust_forwarded_for_);
    const auto segments = split_path(request.path);
    const Route* match = nullptr;
    std::string allowed;
    for (const auto& route : routes_) {
        if (!matches(route.segments, segments)) continue;
        if (route.method == request.method) {
            match = &route;
            break;
        }
        if (!allowed.empty()) allowed += ", ";
        allowed += boost::beast::http::to_string(route.method);
    }

    if (!match) {
        if (allowed.empty()) co_return Response::error(404, "not_found", "No such endpoint.");
        auto response = Response::error(405, "method_not_allowed", "This endpoint doesn't support that method.");
        response.headers.emplace_back("Allow", std::move(allowed));
        co_return response;
    }
    for (std::size_t i = 0; i < segments.size(); ++i) {
        const std::string_view pattern = match->segments[i];
        if (is_parameter(pattern)) request.params.emplace(pattern.substr(1, pattern.size() - 2), segments[i]);
    }

    std::optional<std::string> error;
    try {
        if (auto refused = co_await authorize(request, match->info)) co_return std::move(*refused);
        co_return co_await match->handler(request);
    } catch (const std::exception& e) {
        error = e.what();
    } catch (...) {
        error = "unknown error";
    }
    co_return internal_error("request handler failed", request, match->info, *error);
}

QueryParams::QueryParams(std::string_view encoded_query) {
    const auto parsed = boost::urls::parse_query(encoded_query);
    if (!parsed) return;
    boost::urls::encoding_opts opts;
    opts.space_as_plus = true;
    for (const auto& param : *parsed) {
        if (!param.has_value) continue;
        values_.emplace_back(param.key.decode(opts), param.value.decode(opts));
    }
}

std::optional<std::string> QueryParams::get(std::string_view name) const {
    for (const auto& [key, value] : values_) {
        if (key == name && !value.empty()) return value;
    }
    return std::nullopt;
}

std::vector<std::string> QueryParams::all(std::string_view name) const {
    std::vector<std::string> out;
    for (const auto& [key, value] : values_) {
        if (key != name) continue;
        std::string_view rest = value;
        while (!rest.empty()) {
            const auto comma = rest.find(',');
            if (const auto part = rest.substr(0, comma); !part.empty()) out.emplace_back(part);
            if (comma == std::string_view::npos) break;
            rest.remove_prefix(comma + 1);
        }
    }
    return out;
}

std::optional<std::int64_t> QueryParams::integer(std::string_view name, bool& bad) const {
    const auto text = get(name);
    if (!text) return std::nullopt;
    std::int64_t n = 0;
    const auto [end, ec] = std::from_chars(text->data(), text->data() + text->size(), n);
    if (ec != std::errc{} || end != text->data() + text->size()) {
        bad = true;
        return std::nullopt;
    }
    return n;
}

std::optional<bool> QueryParams::boolean(std::string_view name, bool& bad) const {
    const auto text = get(name);
    if (!text) return std::nullopt;
    if (*text == "true") return true;
    if (*text == "false") return false;
    bad = true;
    return std::nullopt;
}

std::optional<std::string> cookie_value(std::string_view cookie_header, std::string_view name) {
    while (!cookie_header.empty()) {
        const auto semicolon = cookie_header.find(';');
        const auto pair = trim(cookie_header.substr(0, semicolon));
        if (const auto eq = pair.find('='); eq != std::string_view::npos && pair.substr(0, eq) == name) {
            return std::string(pair.substr(eq + 1));
        }
        if (semicolon == std::string_view::npos) break;
        cookie_header.remove_prefix(semicolon + 1);
    }
    return std::nullopt;
}

}  // namespace poni::http
