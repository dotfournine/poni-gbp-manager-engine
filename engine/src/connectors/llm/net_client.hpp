// llm::Client over HTTPS. Each provider speaks one of two protocols:
//
//   openai     POST {base_url}/chat/completions, the OpenAI-compatible
//              protocol most providers offer (DashScope, OpenAI, ...)
//   anthropic  POST {base_url}/messages, Anthropic's Messages API (D-074)
//
// The retries, the rate limiter and the call records are the same for both;
// what differs is how a request is written and an answer is read
// (protocols.hpp).
#pragma once

#include "connectors/llm/client.hpp"
#include "connectors/llm/protocols.hpp"
#include "core/config.hpp"
#include "core/rate_limiter.hpp"
#include "http/client.hpp"

#include <map>

namespace poni::llm {

struct NetOptions {
    // As the configuration has it: "https://.../compatible-mode/v1" (DashScope), "https://api.openai.com/v1"
    // or "https://api.anthropic.com/v1", its key, and its protocol.
    using Provider = core::LlmProvider;
    // By name. The configuration lists a provider only with its address and its key.
    std::map<std::string, Provider> providers;
    // Applied to each stage of a request (connect, TLS, write, read).
    std::chrono::seconds timeout{60};
    // Requests per call: the first and up to max_attempts - 1 retries.
    int max_attempts = 3;
    // Wait before the first retry; doubles for each further one.
    std::chrono::milliseconds retry_backoff{2000};
};

class NetClient final : public Client {
public:
    // `observer` may be empty. Everything passed by reference must outlive the client.
    NetClient(http::Client& http, const core::Clock& clock, core::RateLimiter& limiter, NetOptions options,
              CallObserver observer = {});

    std::vector<std::string> providers() const override;
    boost::asio::awaitable<Result<ChatResult>> chat_json(ChatRequest request) override;

private:
    struct Attempt {
        Result<ChatResult> result;
        CallRecord record;
    };
    boost::asio::awaitable<Attempt> attempt(const ChatRequest& request, const std::string& body, int number);

    http::Client& http_;
    const core::Clock& clock_;
    core::RateLimiter& limiter_;
    NetOptions options_;
    CallObserver observer_;
};

}  // namespace poni::llm
