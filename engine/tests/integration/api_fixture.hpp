// The engine's HTTP layer as main assembles it (router, sign-in, API
// routes), driven without sockets: requests go straight to Router::dispatch.
// Sign-in uses a fake identity provider.
#pragma once

#include "engine_fixture.hpp"
#include "fakes.hpp"
#include "http/api.hpp"
#include "http/auth.hpp"
#include "http/health.hpp"
#include "http/router.hpp"
#include "jobs/analysis.hpp"
#include "jobs/post_advisor.hpp"
#include "jobs/reports.hpp"

#include <boost/json/parse.hpp>

#include <algorithm>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace poni::test {

struct Browser {
    std::string cookie;  // the Cookie header this "browser" sends
    std::string csrf;
};

class ApiTest : public EngineTest {
protected:
    using verb = boost::beast::http::verb;

    void SetUp() override {
        EngineTest::SetUp();
        build({});
    }

    // (Re)builds the HTTP layer with this configuration (sign-in reads it).
    void build(core::Config config, bool with_provider = true) {
        router_ = std::make_unique<http::Router>();
        auth_.reset();
        config_ = std::move(config);
        auth_ = std::make_unique<http::AuthService>(db(), clock_, with_provider ? &provider_ : nullptr, config_);
        router_->set_auth_hook(auth_->hook());
        router_->add(verb::get, "/api/health", {http::Access::anonymous, "system.health"}, http::make_health_handler(db()));
        auth_->add_routes(*router_);
        // The engine always has the three services; a test that does not bring its own gets one made of fakes.
        if (!api_pipeline_ && !own_pipeline_) own_pipeline_.emplace(db(), clock_, own_llm_, queue(), jobs::AnalysisOptions{});
        if (!api_reporter_ && !own_reporter_) own_reporter_.emplace(db(), clock_, own_performance_, own_llm_, own_channel_, &own_store_);
        if (!api_advisor_ && !own_advisor_) own_advisor_.emplace(db(), clock_, own_llm_, own_performance_);
        http::add_api_routes(*router_, db(), clock_, api_options_, api_pipeline_ ? *api_pipeline_ : *own_pipeline_,
                             api_reporter_ ? *api_reporter_ : *own_reporter_, api_public_store_,
                             api_advisor_ ? *api_advisor_ : *own_advisor_, directory_, config_.signin_domains);
    }

    // What the engine was started with; set before build().
    http::ApiOptions api_options_;
    // The pipeline that writes a draft again when staff ask; set before build(), or one of fakes is used.
    jobs::ReviewPipeline* api_pipeline_ = nullptr;
    // The service that builds, sends and discards reports; set before build(), or one of fakes is used.
    jobs::Reporter* api_reporter_ = nullptr;
    // The public bucket that takes a post's photo; set before build().
    storage::ObjectStore* api_public_store_ = nullptr;
    // The model's help with local posts; set before build(), or one of fakes is used.
    jobs::PostAdvisor* api_advisor_ = nullptr;

    http::Response call(verb method, const std::string& target, const Browser& browser = {}, std::string body = {},
                        std::map<std::string, std::string> headers = {}) {
        http::Request request;
        request.method = method;
        const auto question = target.find('?');
        request.path = target.substr(0, question);
        if (question != std::string::npos) request.query = target.substr(question + 1);
        // A route that takes a file gets its body on disk, as from the server.
        if (router_->body_limit(method, request.path) > http::Router::kDefaultBodyLimit) {
            request.file = test::mapped(body);
        } else {
            request.body = std::move(body);
        }
        request.remote_ip = "203.0.113.10";
        for (auto& [name, value] : headers) request.headers[name] = std::move(value);
        if (!browser.cookie.empty()) request.headers["cookie"] = browser.cookie;
        if (!browser.csrf.empty()) request.headers["x-csrf-token"] = browser.csrf;
        return await(router_->dispatch(std::move(request)));
    }

    http::Response get(const std::string& target, const Browser& browser = {}) { return call(verb::get, target, browser); }

    // Calls as browser_, the signed-in browser of most tests (a fixture sets browser_ = sign_in()).
    http::Response post(const std::string& target, std::string body = "{}") { return call(verb::post, target, browser_, std::move(body)); }
    http::Response put(const std::string& target, std::string body) { return call(verb::put, target, browser_, std::move(body)); }
    http::Response patch(const std::string& target, std::string body) { return call(verb::patch, target, browser_, std::move(body)); }
    http::Response del(const std::string& target) { return call(verb::delete_, target, browser_); }
    // The same with headers of their own: if_match(tag) for a change guarded by If-Match (D-108).
    using Headers = std::map<std::string, std::string>;
    http::Response put(const std::string& target, std::string body, Headers headers) {
        return call(verb::put, target, browser_, std::move(body), std::move(headers));
    }
    http::Response patch(const std::string& target, std::string body, Headers headers) {
        return call(verb::patch, target, browser_, std::move(body), std::move(headers));
    }
    http::Response del(const std::string& target, Headers headers) { return call(verb::delete_, target, browser_, {}, std::move(headers)); }

    // The ETag a GET of `get_target` answers with; the test fails if the GET is not a 200 or has none (D-108).
    std::string etag(const std::string& get_target, const Browser& browser) {
        const auto response = get(get_target, browser);
        EXPECT_EQ(response.status, 200u) << get_target << ": " << response.body;
        auto tag = header(response, "ETag");
        EXPECT_FALSE(tag.empty()) << get_target << " answers without an ETag";
        return tag;
    }
    // The headers of a change made against `tag`.
    static Headers if_match(std::string tag) { return {{"if-match", std::move(tag)}}; }
    // A GET as browser_ that must answer 200, and its JSON.
    boost::json::value read(const std::string& target) {
        const auto response = get(target, browser_);
        EXPECT_EQ(response.status, 200u) << target << ": " << response.body;
        return json(response);
    }

    // ---- a location's reply settings (D-093) ----

    static std::string settings_path(std::int64_t location) { return std::format("/api/locations/{}/reply-settings", location); }
    // The body of a save that changes nothing: the three parts as the API gives them.
    boost::json::object settings_body(std::int64_t location) {
        const auto stored = read(settings_path(location));
        return {{"voice", stored.at("voice")}, {"localization", stored.at("localization")}, {"policy", stored.at("policy")}};
    }
    http::Response save_settings(std::int64_t location, const boost::json::object& body) {
        return put(settings_path(location), boost::json::serialize(body));
    }
    // A save of what the location has with `members` laid on one part ("voice", "localization" or "policy"),
    // as a person who changes a few fields of the form sends it.
    http::Response save_settings(std::int64_t location, std::string_view part, std::string_view members) {
        auto body = settings_body(location);
        body[part] = merge_layer(body.at(part).as_object(), boost::json::parse(members).as_object());
        return save_settings(location, body);
    }

    // A route of the router's own table that needs a session: its pattern ("/api/clients/{id}"), the path a test
    // calls it at, and its name ("PATCH /api/clients/{id}").
    struct UserRoute {
        verb method;
        std::string pattern;
        std::string path;
        std::string name;
    };
    // Every such route, in the table's order, with `id` in place of each path parameter.
    std::vector<UserRoute> user_routes(std::string_view id) {
        std::vector<UserRoute> out;
        for (const auto& route : router_->routes()) {
            if (route.info.access != http::Access::user) continue;
            UserRoute found{.method = route.method, .pattern = {}, .path = {}, .name = {}};
            for (const auto& segment : route.segments) {
                found.pattern += "/" + segment;
                found.path += "/" + (segment.starts_with('{') ? std::string(id) : segment);
            }
            found.name = std::format("{} {}", std::string_view(boost::beast::http::to_string(route.method)), found.pattern);
            out.push_back(std::move(found));
        }
        return out;
    }

    // Walks the router's own table. Every route that needs a session and whose pattern contains one of `areas`
    // ("" for every route) answers 401 unauthenticated without a session and with a made-up one; with `csrf`, every
    // changing one answers 403 to browser_'s session without the CSRF token. Each call carries `body`, so that a
    // refusal is seen to change nothing. Returns how many routes it checked.
    std::size_t expect_session_and_csrf(std::initializer_list<std::string_view> areas, const std::string& body = "{}", bool csrf = true) {
        std::size_t checked = 0;
        for (const auto& route : user_routes("1")) {
            if (std::ranges::none_of(areas, [&](std::string_view area) { return route.pattern.contains(area); })) continue;
            const auto what = std::format("{} {}", boost::beast::http::to_string(route.method), route.path);
            const auto anonymous = call(route.method, route.path, {}, body);
            EXPECT_EQ(anonymous.status, 401u) << what;
            EXPECT_EQ(error_code(anonymous), "unauthenticated") << what;
            EXPECT_EQ(call(route.method, route.path, {.cookie = "poni_session=made-up-token"}, body).status, 401u) << what;
            if (csrf && route.method != verb::get) EXPECT_EQ(call(route.method, route.path, {.cookie = browser_.cookie}, body).status, 403u) << what;
            ++checked;
        }
        return checked;
    }

    static boost::json::value json(const http::Response& response) { return boost::json::parse(response.body); }

    static std::string error_code(const http::Response& response) {
        return std::string(json(response).at("error").at("code").as_string());
    }
    static std::string error_message(const http::Response& response) {
        return std::string(json(response).at("error").at("message").as_string());
    }

    static std::string header(const http::Response& response, std::string_view name) {
        for (const auto& [key, value] : response.headers) {
            if (key == name) return value;
        }
        return {};
    }

    // The whole Set-Cookie header for `name`, or "".
    static std::string set_cookie(const http::Response& response, std::string_view name) {
        for (const auto& [key, value] : response.headers) {
            if (key == "Set-Cookie" && value.starts_with(std::string(name) + "=")) return value;
        }
        return {};
    }

    // "name=value" from a Set-Cookie header.
    static std::string pair_of(const std::string& set_cookie_header) {
        return set_cookie_header.substr(0, set_cookie_header.find(';'));
    }

    static identity::Identity staff(std::string email = "staff@xvisionmediaus.com") {
        return {.subject = "sub-" + email,
                .email = std::move(email),
                .email_verified = true,
                .hosted_domain = "xvisionmediaus.com",
                .name = "Staff Member",
                .picture_url = std::nullopt};
    }

    // Runs the Google sign-in flow for `identity` and returns the callback's response.
    http::Response sign_in_response(const identity::Identity& identity) {
        const std::string code = "code-" + identity.subject;
        provider_.identities[code] = identity;
        const auto start = get("/api/auth/google/start");
        const auto flow = pair_of(set_cookie(start, "poni_signin"));
        return get("/api/auth/google/callback?code=" + code + "&state=" + provider_.last_request->state, {.cookie = flow});
    }

    // Signs in and returns a browser carrying the session.
    Browser sign_in(const identity::Identity& identity = staff()) {
        const auto response = sign_in_response(identity);
        Browser browser{.cookie = pair_of(set_cookie(response, auth_->session_cookie_name()))};
        browser.csrf = std::string(json(get("/api/session", browser)).at("csrf_token").as_string());
        return browser;
    }

    // ---- the phone app (D-095) ----

    // A PKCE verifier as the app makes one.
    static inline const std::string kAppVerifier = "the-apps-verifier-0123456789-abcdefghijklmnop";

    // The header a request of the app carries its token in.
    static std::map<std::string, std::string> bearer(const std::string& token) { return {{"authorization", "Bearer " + token}}; }
    // A request as the app sends it: its token, no cookie and no CSRF token.
    http::Response as_app(verb method, const std::string& target, const std::string& token, std::string body = {},
                          std::map<std::string, std::string> headers = {}) {
        headers.merge(bearer(token));
        return call(method, target, {}, std::move(body), std::move(headers));
    }

    // The address the app opens in its browser sheet.
    static std::string app_start(const std::string& challenge = http::s256_challenge(kAppVerifier)) {
        return "/api/auth/google/start?client=ios&code_challenge=" + challenge;
    }

    // Runs the Google sign-in flow for `identity` as the app's browser sheet does, and returns the callback's response.
    http::Response app_sign_in_response(const identity::Identity& identity, const std::string& challenge = http::s256_challenge(kAppVerifier)) {
        const std::string code = "code-" + identity.subject;
        provider_.identities[code] = identity;
        const auto start = get(app_start(challenge));
        const auto flow = pair_of(set_cookie(start, "poni_signin"));
        return get("/api/auth/google/callback?code=" + code + "&state=" + provider_.last_request->state, {.cookie = flow});
    }

    // The one-time code in the callback's redirect to the app; "" if it went anywhere else.
    static std::string app_code(const http::Response& callback) {
        constexpr std::string_view to_app = "gbpmanager://auth?code=";
        const auto location = header(callback, "Location");
        return location.starts_with(to_app) ? location.substr(to_app.size()) : std::string{};
    }

    http::Response exchange(const std::string& code, const std::string& verifier = kAppVerifier) {
        return call(verb::post, "/api/auth/app-token", {}, boost::json::serialize(boost::json::object{{"code", code}, {"code_verifier", verifier}}));
    }

    // Signs in as the app and returns its token.
    std::string app_sign_in(const identity::Identity& identity = staff()) {
        const auto answer = exchange(app_code(app_sign_in_response(identity)));
        EXPECT_EQ(answer.status, 200u) << answer.body;
        return answer.status == 200 ? std::string(json(answer).at("token").as_string()) : std::string{};
    }

    FakeIdentityProvider provider_;
    // The Workspace directory the users' routes read (D-106 point 8); empty unless a test lists people in it.
    FakeDirectory directory_;
    // What the services of fakes stand on, for a test that brings none of its own.
    FakeLlm own_llm_;
    FakePerformance own_performance_;
    // Without a bucket a report is not even started (D-104).
    FakeStore own_store_;
    // Where its reports' emails go.
    FakeChannel own_channel_;
    std::optional<jobs::ReviewPipeline> own_pipeline_;
    std::optional<jobs::Reporter> own_reporter_;
    std::optional<jobs::PostAdvisor> own_advisor_;
    Browser browser_;
    // What the engine was started with, as far as sign-in goes; build() sets it.
    core::Config config_;
    std::unique_ptr<http::Router> router_;
    std::unique_ptr<http::AuthService> auth_;
};

}  // namespace poni::test
