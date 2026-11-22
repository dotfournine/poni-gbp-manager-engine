#include "connectors/llm/net_client.hpp"

#include "core/text.hpp"

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>
#include <boost/url/parse.hpp>
#include <boost/url/url.hpp>

#include <algorithm>
#include <cctype>
#include <format>

namespace poni::llm {

namespace asio = boost::asio;

NetClient::NetClient(http::Client& http, const core::Clock& clock, core::RateLimiter& limiter, NetOptions options,
                     CallObserver observer)
    : http_(http), clock_(clock), limiter_(limiter), options_(std::move(options)), observer_(std::move(observer)) {}

std::vector<std::string> NetClient::providers() const {
    std::vector<std::string> names;
    for (const auto& [name, provider] : options_.providers) names.push_back(name);
    return names;
}

asio::awaitable<NetClient::Attempt> NetClient::attempt(const ChatRequest& request, const std::string& body, int number) {
    Attempt out{.result = std::unexpected(Error{}),
                .record = {.at = clock_.now(),
                           .task = request.task,
                           .provider = request.provider,
                           .model = request.model,
                           .location_id = request.location_id,
                           .review_id = request.review_id,
                           .go_live_run_id = request.go_live_run_id,
                           .attempt = number}};

    const auto& provider = options_.providers.at(request.provider);  // chat_json checked that it exists
    const auto base = boost::urls::parse_uri(provider.base_url);
    if (!base) {
        out.result = std::unexpected(Error{Error::Kind::not_configured, 0, "the LLM base URL is not a valid address"});
        out.record.error = out.result.error().message;
        co_return out;
    }
    http::ClientRequest http_request;
    http_request.method = http::ClientMethod::post;
    http_request.url = *base;
    for (const auto& segment : endpoint(provider.protocol)) http_request.url.segments().push_back(segment);
    for (auto& header : headers(provider.protocol, provider.api_key, request)) http_request.headers.push_back(std::move(header));
    http_request.content_type = "application/json";
    http_request.body = body;
    http_request.timeout = request.timeout.value_or(options_.timeout);

    co_await limiter_.acquire(kLlmApi);
    const auto started = clock_.now();
    out.record.at = started;
    std::optional<http::ClientResponse> response;
    std::string transport_failure;
    try {
        response = co_await http_.send(std::move(http_request));
    } catch (const boost::system::system_error& e) {
        transport_failure = e.code().message();
    }
    out.record.latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock_.now() - started).count();

    if (!response) {
        out.result = std::unexpected(Error{Error::Kind::unavailable, 0, text::truncate("network error: " + transport_failure, 300)});
    } else if (!response->ok()) {
        out.record.http_status = response->status;
        out.result = std::unexpected(describe_failure(*response));
    } else {
        out.record.http_status = response->status;
        out.result = read_answer(provider.protocol, *response, out.record);
    }
    if (out.result) {
        out.record.ok = true;
        out.record.finish_reason = out.result->finish_reason;
    } else {
        out.record.error = std::format("{}: {}", to_string(out.result.error().kind), out.result.error().message);
    }
    co_return out;
}

asio::awaitable<Result<ChatResult>> NetClient::chat_json(ChatRequest request) {
    const auto found = options_.providers.find(request.provider);
    if (found == options_.providers.end()) {
        std::string upper = request.provider;
        std::ranges::transform(upper, upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        Error missing{Error::Kind::not_configured, 0,
                      std::format("no model provider named '{}' is configured (PONI_LLM_{}_BASE_URL and PONI_LLM_{}_API_KEY)",
                                  request.provider, upper, upper)};
        co_return std::unexpected(std::move(missing));
    }
    const auto body = found->second.protocol == Protocol::anthropic ? anthropic_request_body(request) : openai_request_body(request);
    auto wait = options_.retry_backoff;
    for (int number = 1;; ++number) {
        auto attempted = co_await attempt(request, body, number);
        if (observer_) co_await observer_(std::move(attempted.record));
        if (attempted.result || !attempted.result.error().transient() || number >= options_.max_attempts) co_return std::move(attempted.result);
        if (wait.count() > 0) {
            asio::steady_timer timer(co_await asio::this_coro::executor, wait);
            co_await timer.async_wait(asio::use_awaitable);
            wait *= 2;
        }
    }
}

}  // namespace poni::llm
