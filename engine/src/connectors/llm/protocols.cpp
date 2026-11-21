#include "connectors/llm/protocols.hpp"

#include "core/text.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>

#include <format>

namespace poni::llm {

namespace {

constexpr std::size_t kMaxErrorLength = 300;
// Needed for "fallbacks" in a Messages request (the provider's fallback model answers when a safety classifier declines).
constexpr std::string_view kAnthropicFallbackBeta = "server-side-fallback-2026-07-01";

Error error(Error::Kind kind, unsigned status, std::string message) {
    return {kind, status, text::truncate(message, kMaxErrorLength)};
}

std::unexpected<Error> invalid(const http::ClientResponse& response, std::string why) {
    return std::unexpected(error(Error::Kind::invalid_response, response.status, std::move(why)));
}

// The provider's own words for a failure. OpenAI-compatible:
// {"error": {"message", "code"}}; Anthropic: {"error": {"type", "message"}}.
std::string provider_message(const http::ClientResponse& response) {
    boost::system::error_code ec;
    const auto body = boost::json::parse(response.body, ec);
    if (!ec && body.is_object()) {
        if (const auto* err = body.get_object().if_contains("error"); err && err->is_object()) {
            const auto& object = err->get_object();
            std::string out;
            for (const std::string_view key : {"code", "type"}) {
                if (const auto* code = object.if_contains(key); out.empty() && code && code->is_string()) {
                    out = std::string(code->get_string());
                }
            }
            if (const auto* message = object.if_contains("message"); message && message->is_string()) {
                out += (out.empty() ? "" : ": ") + std::string(message->get_string());
            }
            if (!out.empty()) return std::format("HTTP {}: {}", response.status, out);
        }
    }
    return std::format("HTTP {}", response.status);
}

std::int64_t number_of(const boost::json::object& object, std::string_view key) {
    const auto* value = object.if_contains(key);
    if (!value || !value->is_number()) return 0;
    return value->to_number<std::int64_t>();
}

std::string_view string_of(const boost::json::object& object, std::string_view key) {
    const auto* value = object.if_contains(key);
    return value && value->is_string() ? std::string_view(value->get_string()) : std::string_view{};
}

// The names differ: prompt_tokens and completion_tokens, or input_tokens and output_tokens.
std::pair<std::int64_t, std::int64_t> usage_of(Protocol protocol, const boost::json::object& body) {
    const auto* usage = body.if_contains("usage");
    if (!usage || !usage->is_object()) return {0, 0};
    const auto& object = usage->get_object();
    if (protocol == Protocol::openai) return {number_of(object, "prompt_tokens"), number_of(object, "completion_tokens")};
    // What was read from or written to the prompt cache is input too.
    return {number_of(object, "input_tokens") + number_of(object, "cache_read_input_tokens") +
                number_of(object, "cache_creation_input_tokens"),
            number_of(object, "output_tokens")};
}

// A model without an enforced schema sometimes wraps its JSON in a code fence.
std::string_view without_fence(std::string_view answer) {
    answer = text::trim(answer);
    if (!answer.starts_with("```")) return answer;
    const auto first_line = answer.find('\n');
    if (first_line == std::string_view::npos) return answer;
    answer.remove_prefix(first_line + 1);
    if (const auto end = answer.rfind("```"); end != std::string_view::npos) answer = answer.substr(0, end);
    return text::trim(answer);
}

// The model's text as the JSON object it should be.
Result<ChatResult> finish(const http::ClientResponse& response, ChatResult result, std::string_view answer, bool cut_off) {
    boost::system::error_code ec;
    auto parsed = boost::json::parse(without_fence(answer), ec);
    if (ec || !parsed.is_object()) {
        return invalid(response, cut_off ? "the answer was cut off before the JSON ended" : "the answer is not a JSON object");
    }
    result.json = std::move(parsed.get_object());
    return result;
}

Result<ChatResult> read_openai(const http::ClientResponse& response, const boost::json::object& body) {
    ChatResult result;
    const auto* choices = body.if_contains("choices");
    if (!choices || !choices->is_array() || choices->get_array().empty() || !choices->get_array().front().is_object()) {
        return invalid(response, "the response has no choices");
    }
    const auto& choice = choices->get_array().front().get_object();
    result.finish_reason = std::string(string_of(choice, "finish_reason"));
    const auto* message = choice.if_contains("message");
    const auto* content = message && message->is_object() ? message->get_object().if_contains("content") : nullptr;
    if (!content || !content->is_string()) return invalid(response, "the response has no message content");
    const bool cut_off = result.finish_reason == "length";
    return finish(response, std::move(result), content->get_string(), cut_off);
}

Result<ChatResult> read_anthropic(const http::ClientResponse& response, const boost::json::object& body) {
    ChatResult result;
    result.finish_reason = std::string(string_of(body, "stop_reason"));
    if (result.finish_reason == "refusal") {
        // A safety classifier declined (HTTP 200): the request's content, not an outage.
        std::string category;
        if (const auto* details = body.if_contains("stop_details"); details && details->is_object()) {
            category = std::string(string_of(details->get_object(), "category"));
        }
        return std::unexpected(error(Error::Kind::rejected, response.status,
                                     category.empty() ? std::string("the model declined the request")
                                                      : std::format("the model declined the request ({})", category)));
    }
    const auto* content = body.if_contains("content");
    if (!content || !content->is_array()) return invalid(response, "the response has no content");
    // Did the provider's fallback model answer? A "fallback" block marks the
    // switch in this turn; a "fallback_message" entry in the usage marks a
    // turn the fallback served. Either way "model" names who answered.
    bool fell_back = false;
    for (const auto& block : content->get_array()) {
        if (block.is_object() && string_of(block.get_object(), "type") == "fallback") fell_back = true;
    }
    if (const auto* usage = body.if_contains("usage"); usage && usage->is_object()) {
        if (const auto* iterations = usage->get_object().if_contains("iterations"); iterations && iterations->is_array()) {
            for (const auto& step : iterations->get_array()) {
                if (step.is_object() && string_of(step.get_object(), "type") == "fallback_message") fell_back = true;
            }
        }
    }
    if (fell_back) {
        const auto answered_by = string_of(body, "model");
        result.fallback_model = answered_by.empty() ? std::string("a fallback model") : std::string(answered_by);
    }
    // Thinking blocks may come first; the answer is the first text block.
    for (const auto& block : content->get_array()) {
        if (!block.is_object() || string_of(block.get_object(), "type") != "text") continue;
        const auto* answer = block.get_object().if_contains("text");
        if (!answer || !answer->is_string()) break;
        const bool cut_off = result.finish_reason == "max_tokens";
        return finish(response, std::move(result), answer->get_string(), cut_off);
    }
    return invalid(response, result.finish_reason == "max_tokens" ? "the answer was cut off before the JSON began"
                                                                  : "the response has no text");
}

// Extras never replace what the engine sets itself; an object the engine
// also sets ("output_config") gets the extra's other members.
void add_extras(boost::json::object& body, const boost::json::object& extras) {
    for (const auto& member : extras) {
        auto* own = body.if_contains(member.key());
        if (!own) {
            body[member.key()] = member.value();
        } else if (own->is_object() && member.value().is_object()) {
            for (const auto& inner : member.value().get_object()) {
                if (!own->get_object().contains(inner.key())) own->get_object()[inner.key()] = inner.value();
            }
        }
    }
}

}  // namespace

std::string_view to_string(Error::Kind kind) {
    switch (kind) {
        case Error::Kind::not_configured: return "not_configured";
        case Error::Kind::unauthenticated: return "unauthenticated";
        case Error::Kind::rate_limited: return "rate_limited";
        case Error::Kind::unavailable: return "unavailable";
        case Error::Kind::invalid_response: return "invalid_response";
        case Error::Kind::rejected: return "rejected";
    }
    return "unknown";
}

std::string openai_request_body(const ChatRequest& request) {
    boost::json::array messages;
    for (const auto& message : request.messages) {
        messages.push_back(boost::json::object{{"role", message.role}, {"content", message.content}});
    }
    boost::json::object body{{"model", request.model},
                             {"messages", std::move(messages)},
                             {"max_tokens", request.max_tokens},
                             {"response_format", boost::json::object{{"type", "json_object"}}}};
    if (request.temperature) body["temperature"] = *request.temperature;
    add_extras(body, request.extras);
    return boost::json::serialize(body);
}

std::string anthropic_request_body(const ChatRequest& request) {
    // The system message is a field of its own; "messages" holds the turns.
    std::string system;
    boost::json::array messages;
    for (const auto& message : request.messages) {
        if (message.role == "system") {
            system += (system.empty() ? "" : "\n\n") + message.content;
        } else {
            messages.push_back(boost::json::object{{"role", message.role}, {"content", message.content}});
        }
    }
    boost::json::object body{{"model", request.model}, {"max_tokens", request.max_tokens}, {"messages", std::move(messages)}};
    if (!system.empty()) body["system"] = system;
    if (request.schema) {
        body["output_config"] =
            boost::json::object{{"format", boost::json::object{{"type", "json_schema"}, {"schema", *request.schema}}}};
    }
    // No temperature: this API version refuses one on every model.
    add_extras(body, request.extras);
    return boost::json::serialize(body);
}

std::vector<std::string> endpoint(Protocol protocol) {
    if (protocol == Protocol::anthropic) return {"messages"};
    return {"chat", "completions"};
}

std::vector<std::pair<std::string, std::string>> headers(Protocol protocol, const std::string& api_key, const ChatRequest& request) {
    if (protocol == Protocol::openai) return {{"Authorization", "Bearer " + api_key}};
    std::vector<std::pair<std::string, std::string>> out{{"x-api-key", api_key},
                                                         {"anthropic-version", std::string(kAnthropicVersion)}};
    if (request.extras.contains("fallbacks")) out.emplace_back("anthropic-beta", std::string(kAnthropicFallbackBeta));
    return out;
}

Error describe_failure(const http::ClientResponse& response) {
    const auto status = response.status;
    const auto kind = status == 401 || status == 403   ? Error::Kind::unauthenticated
                      : status == 429                  ? Error::Kind::rate_limited
                      : status >= 500 || status == 408 ? Error::Kind::unavailable
                                                       : Error::Kind::rejected;
    return error(kind, status, provider_message(response));
}

Result<ChatResult> read_answer(Protocol protocol, const http::ClientResponse& response, CallRecord& record) {
    boost::system::error_code ec;
    const auto body = boost::json::parse(response.body, ec);
    if (ec || !body.is_object()) return invalid(response, "the response is not a JSON object");
    std::tie(record.prompt_tokens, record.completion_tokens) = usage_of(protocol, body.get_object());
    auto result = protocol == Protocol::anthropic ? read_anthropic(response, body.get_object()) : read_openai(response, body.get_object());
    if (result) {
        result->prompt_tokens = record.prompt_tokens;
        result->completion_tokens = record.completion_tokens;
    }
    return result;
}

}  // namespace poni::llm
