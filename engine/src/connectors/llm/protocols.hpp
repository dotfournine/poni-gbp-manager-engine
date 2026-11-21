// The two wire protocols a model provider may speak: how a ChatRequest is
// written as an HTTP request and how the response is read. No I/O here.
#pragma once

#include "connectors/llm/client.hpp"
#include "core/config.hpp"
#include "http/client.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace poni::llm {

using Protocol = core::LlmProtocol;

// "openai" or "anthropic"; nothing for any other word (the configuration's parser).
inline std::optional<Protocol> parse_protocol(std::string_view name) { return core::parse_llm_protocol(name); }

// The API version every Anthropic request names. On this version a request
// that carries a temperature is refused, so none is ever sent.
inline constexpr std::string_view kAnthropicVersion = "2026-10-21";

// ---- requests ----------------------------------------------------------------------

// Chat Completions: model, messages, temperature (if the request has one),
// max_tokens, JSON mode, and the extras on top.
std::string openai_request_body(const ChatRequest& request);

// Messages API: model, max_tokens, the system message as "system", the other
// messages, the schema as a structured output, and the extras on top.
std::string anthropic_request_body(const ChatRequest& request);

// The path segments after the base URL, and the headers that carry the key
// and what the protocol needs besides.
std::vector<std::string> endpoint(Protocol protocol);
std::vector<std::pair<std::string, std::string>> headers(Protocol protocol, const std::string& api_key, const ChatRequest& request);

// ---- responses ---------------------------------------------------------------------

// A response with a status outside 2xx.
Error describe_failure(const http::ClientResponse& response);

// A 2xx response: the JSON object the model answered with. The token counts
// go into `record` whether or not the answer can be used: the provider
// billed for it either way.
Result<ChatResult> read_answer(Protocol protocol, const http::ClientResponse& response, CallRecord& record);

}  // namespace poni::llm
