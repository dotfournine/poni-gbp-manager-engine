// The LLM connector (D-010): one client behind an interface so tests use a
// fake. Several providers can be configured at once (D-059); the provider and
// the model are parameters of every call, because they are settings per task
// (D-030). A provider speaks one of two protocols (D-074): the
// OpenAI-compatible Chat Completions protocol, or Anthropic's Messages API.
//
// Every request asks for a JSON object and the answer is parsed before it is
// returned: callers never see free text. Each HTTP attempt, failed ones
// included, is reported to an observer, which is how llm_calls gets its rows.
#pragma once

#include "core/clock.hpp"
#include "domain/prompts.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/json/object.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace poni::llm {

// Rate limiter key.
inline constexpr std::string_view kLlmApi = "llm";

struct Error {
    enum class Kind {
        not_configured,    // the provider the call names has no base URL and API key
        unauthenticated,   // the key was refused (401, 403)
        rate_limited,      // 429
        unavailable,       // 5xx, or the request timed out or never connected
        invalid_response,  // a 2xx whose body isn't an answer holding a JSON object
        rejected,          // any other 4xx, or the model declined: a bad request, or the provider's content filter said no
    };
    Kind kind = Kind::unavailable;
    unsigned http_status = 0;
    std::string message;  // never contains the key, the prompt or the answer

    // Worth trying again later: the provider or the network, not the request.
    bool transient() const { return kind == Kind::rate_limited || kind == Kind::unavailable || kind == Kind::invalid_response; }
    // The engine's own setup is wrong; no review can be processed until it is fixed.
    bool configuration() const { return kind == Kind::not_configured || kind == Kind::unauthenticated; }
};

template <class T>
using Result = std::expected<T, Error>;

std::string_view to_string(Error::Kind kind);

struct ChatRequest {
    // "analyzer", "generator" or "judge": recorded with the call.
    std::string task;
    // The configured provider to ask, by name ("dashscope").
    std::string provider;
    std::string model;
    // A leading "system" message, then "user" messages.
    std::vector<domain::ChatMessage> messages;
    // Nothing: the request carries no temperature (some models refuse one).
    // Anthropic's API takes none at all, so that protocol never sends it.
    std::optional<double> temperature;
    std::int64_t max_tokens = 1200;
    // A JSON Schema the answer must match. Sent where the protocol can
    // enforce one (Anthropic); elsewhere the prompt alone asks for the shape.
    std::optional<boost::json::object> schema;
    // Extra members for the request body (the provider's entry in ai_settings.request_extras).
    boost::json::object extras;
    // For a call that takes longer than the usual limit.
    std::optional<std::chrono::seconds> timeout;
    // What the call is for; recorded, never sent.
    std::optional<std::int64_t> location_id;
    std::optional<std::int64_t> review_id;
    std::optional<std::int64_t> go_live_run_id;
};

struct ChatResult {
    // The JSON object the model returned.
    boost::json::object json;
    std::int64_t prompt_tokens = 0;      // of the attempt that succeeded
    std::int64_t completion_tokens = 0;
    std::string finish_reason;
    // The model that answered instead of the one asked, when the provider
    // switched to its fallback (Anthropic, after a safety classifier declined).
    std::optional<std::string> fallback_model;
};

// One HTTP attempt.
struct CallRecord {
    core::TimePoint at;
    std::string task;
    std::string provider;
    std::string model;
    std::optional<std::int64_t> location_id;
    std::optional<std::int64_t> review_id;
    std::optional<std::int64_t> go_live_run_id;
    int attempt = 1;
    bool ok = false;
    std::string error;
    unsigned http_status = 0;
    std::int64_t prompt_tokens = 0;
    std::int64_t completion_tokens = 0;
    std::int64_t latency_ms = 0;
    std::string finish_reason;
};

using CallObserver = std::function<boost::asio::awaitable<void>(CallRecord)>;

class Client {
public:
    virtual ~Client() = default;
    // The providers that have an address and a key, in name order. A call
    // that names any other fails with not_configured.
    virtual std::vector<std::string> providers() const = 0;
    bool configured(std::string_view provider) const {
        const auto names = providers();
        return std::ranges::find(names, provider) != names.end();
    }
    // Transport failures are returned as Error::unavailable, not thrown.
    virtual boost::asio::awaitable<Result<ChatResult>> chat_json(ChatRequest request) = 0;
};

}  // namespace poni::llm
