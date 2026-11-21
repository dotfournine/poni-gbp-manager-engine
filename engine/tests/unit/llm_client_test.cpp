// The LLM connector against a scripted HTTP client: what goes to the
// provider, how answers and failures are read, when it retries, and that
// every attempt is reported for llm_calls.

#include "connectors/llm/net_client.hpp"
#include "unit/connector_fixture.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace poni;
using namespace std::chrono_literals;
using llm::Error;

namespace {

// A Chat Completions response whose message content is `content`.
std::string completion(std::string_view content, int prompt_tokens = 1400, int completion_tokens = 90, std::string_view finish = "stop") {
    return boost::json::serialize(boost::json::object{
        {"id", "chatcmpl-test"},
        {"choices", boost::json::array{boost::json::object{
                        {"index", 0},
                        {"finish_reason", finish},
                        {"message", boost::json::object{{"role", "assistant"}, {"content", content}}}}}},
        {"usage", boost::json::object{{"prompt_tokens", prompt_tokens}, {"completion_tokens", completion_tokens}}}});
}

// Advances the test clock while "the network" is busy, so latency is measurable.
class SlowHttp final : public http::Client {
public:
    SlowHttp(test::FakeHttp& inner, core::ManualClock& clock) : inner_(inner), clock_(clock) {}
    boost::asio::awaitable<http::ClientResponse> send(http::ClientRequest request) override {
        clock_.advance(1500ms);
        co_return co_await inner_.send(std::move(request));
    }
    boost::asio::awaitable<std::vector<http::ClientResponse>> send_h2(std::vector<http::ClientRequest> requests) override {
        co_return co_await inner_.send_h2(std::move(requests));
    }

private:
    test::FakeHttp& inner_;
    core::ManualClock& clock_;
};

class LlmClient : public test::ConnectorTest {
protected:
    LlmClient() : ConnectorTest({llm::kLlmApi}) {}

    llm::NetClient client(llm::NetOptions options = {}, http::Client* http = nullptr) {
        if (options.providers.empty()) options.providers = {{"dashscope", {"https://llm.example/compatible-mode/v1", "test-key"}}};
        options.retry_backoff = 0ms;  // no sleeping in tests
        return llm::NetClient(http ? *http : http_, clock_, limiter_, std::move(options), [this](llm::CallRecord record) -> boost::asio::awaitable<void> {
            records_.push_back(std::move(record));
            co_return;
        });
    }

    static llm::ChatRequest request() {
        return {.task = "analyzer",
                .provider = "dashscope",
                .model = "qwen3.7-plus",
                .messages = {{"system", "You analyze one review."}, {"user", "Analyze this review."}},
                .temperature = 0.25,
                .max_tokens = 900,
                .extras = boost::json::object{{"enable_thinking", false}, {"model", "sneaky-other-model"}},
                .client_id = 7,
                .location_id = 8,
                .review_id = 9};
    }

    std::vector<llm::CallRecord> records_;
};

}  // namespace

TEST_F(LlmClient, SendsAChatCompletionsRequestInJsonMode) {
    http_.reply(200, completion(R"({"sentiment": "positive"})"));
    auto llm = client();
    const auto result = await(llm.chat_json(request()));
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result->json.at("sentiment").as_string(), "positive");
    EXPECT_EQ(result->prompt_tokens, 1400);
    EXPECT_EQ(result->completion_tokens, 90);
    EXPECT_EQ(result->finish_reason, "stop");

    ASSERT_EQ(http_.requests.size(), 1u);
    const auto& sent = http_.requests[0];
    EXPECT_EQ(sent.method, http::ClientMethod::post);
    EXPECT_EQ(sent.url.buffer(), "https://llm.example/compatible-mode/v1/chat/completions");
    EXPECT_EQ(http_.header(0, "Authorization"), "Bearer test-key");
    EXPECT_EQ(sent.content_type, "application/json");
    EXPECT_EQ(sent.timeout, 60s);

    const auto body = boost::json::parse(sent.body).as_object();
    EXPECT_EQ(body.at("model").as_string(), "qwen3.7-plus") << "extras never replace what the engine sets";
    EXPECT_EQ(body.at("response_format").at("type").as_string(), "json_object");
    EXPECT_DOUBLE_EQ(body.at("temperature").as_double(), 0.25);
    EXPECT_EQ(body.at("max_tokens").as_int64(), 900);
    EXPECT_EQ(body.at("enable_thinking").as_bool(), false);
    ASSERT_EQ(body.at("messages").as_array().size(), 2u);
    EXPECT_EQ(body.at("messages").as_array()[0].at("role").as_string(), "system");
    EXPECT_EQ(body.at("messages").as_array()[1].at("content").as_string(), "Analyze this review.");
    // What the call is for is recorded, never sent.
    EXPECT_FALSE(body.contains("client_id"));
    EXPECT_FALSE(body.contains("review_id"));
    EXPECT_EQ(sent.body.find("test-key"), std::string::npos);
}

TEST_F(LlmClient, EveryAttemptIsReportedWithTokensAndLatency) {
    http_.reply(200, completion(R"({"ok": true})", 1000, 50));
    SlowHttp slow(http_, clock_);
    auto llm = client({}, &slow);
    ASSERT_TRUE(await(llm.chat_json(request())));
    ASSERT_EQ(records_.size(), 1u);
    const auto& record = records_[0];
    EXPECT_TRUE(record.ok);
    EXPECT_EQ(record.task, "analyzer");
    EXPECT_EQ(record.model, "qwen3.7-plus");
    EXPECT_EQ(record.client_id, 7);
    EXPECT_EQ(record.location_id, 8);
    EXPECT_EQ(record.review_id, 9);
    EXPECT_EQ(record.attempt, 1);
    EXPECT_EQ(record.http_status, 200u);
    EXPECT_EQ(record.prompt_tokens, 1000);
    EXPECT_EQ(record.completion_tokens, 50);
    EXPECT_EQ(record.latency_ms, 1500);
    EXPECT_EQ(record.at, test::t0());
    EXPECT_TRUE(record.error.empty());
}

TEST_F(LlmClient, RetriesWhenTheProviderIsBusyOrDown) {
    http_.reply(503, R"({"error": {"message": "overloaded"}})");
    http_.reply(429, R"({"error": {"code": "limit_requests", "message": "slow down"}})");
    http_.reply(200, completion(R"({"ok": true})"));
    auto llm = client({.max_attempts = 3});
    const auto result = await(llm.chat_json(request()));
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(http_.requests.size(), 3u);
    ASSERT_EQ(records_.size(), 3u);
    EXPECT_FALSE(records_[0].ok);
    EXPECT_EQ(records_[0].http_status, 503u);
    EXPECT_THAT(records_[0].error, testing::HasSubstr("unavailable"));
    EXPECT_THAT(records_[1].error, testing::HasSubstr("rate_limited"));
    EXPECT_THAT(records_[1].error, testing::HasSubstr("slow down"));
    EXPECT_EQ(records_[1].attempt, 2);
    EXPECT_TRUE(records_[2].ok);
}

TEST_F(LlmClient, GivesUpAfterTheLastAttempt) {
    for (int i = 0; i < 5; ++i) http_.reply(500, "upstream exploded");
    auto llm = client({.max_attempts = 2});
    const auto result = await(llm.chat_json(request()));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, Error::Kind::unavailable);
    EXPECT_EQ(result.error().http_status, 500u);
    EXPECT_TRUE(result.error().transient());
    EXPECT_EQ(http_.requests.size(), 2u);
    EXPECT_EQ(records_.size(), 2u);
}

TEST_F(LlmClient, AnAnswerThatIsNotJsonIsRetriedAndItsTokensCounted) {
    http_.reply(200, completion("Sure! Here is the reply you asked for.", 1300, 12));
    http_.reply(200, completion(R"(["an", "array"])"));
    http_.reply(200, completion(R"({"reply": "Thank you!"})"));
    auto llm = client();
    const auto result = await(llm.chat_json(request()));
    ASSERT_TRUE(result);
    ASSERT_EQ(records_.size(), 3u);
    EXPECT_FALSE(records_[0].ok);
    EXPECT_THAT(records_[0].error, testing::HasSubstr("invalid_response"));
    // The provider billed for the answer nobody could use.
    EXPECT_EQ(records_[0].prompt_tokens, 1300);
    EXPECT_EQ(records_[0].completion_tokens, 12);
    // The answer itself is never put in an error message.
    EXPECT_THAT(records_[0].error, testing::Not(testing::HasSubstr("Sure!")));
}

TEST_F(LlmClient, ACutOffAnswerSaysSo) {
    for (int i = 0; i < 3; ++i) http_.reply(200, completion(R"({"reply": "Thank you so mu)", 1300, 1200, "length"));
    auto llm = client();
    const auto result = await(llm.chat_json(request()));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, Error::Kind::invalid_response);
    EXPECT_THAT(result.error().message, testing::HasSubstr("cut off"));
}

TEST_F(LlmClient, ARefusedKeyOrRequestIsNotRetried) {
    http_.reply(401, R"({"error": {"code": "invalid_api_key", "message": "Incorrect API key provided."}})");
    auto llm = client();
    const auto refused = await(llm.chat_json(request()));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().kind, Error::Kind::unauthenticated);
    EXPECT_TRUE(refused.error().configuration());
    EXPECT_FALSE(refused.error().transient());
    EXPECT_EQ(http_.requests.size(), 1u);

    http_.reply(400, R"({"error": {"code": "data_inspection_failed", "message": "Input data may contain inappropriate content."}})");
    const auto rejected = await(llm.chat_json(request()));
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().kind, Error::Kind::rejected);
    EXPECT_FALSE(rejected.error().transient());
    EXPECT_FALSE(rejected.error().configuration());
    EXPECT_THAT(rejected.error().message, testing::HasSubstr("data_inspection_failed"));
    EXPECT_EQ(http_.requests.size(), 2u);
}

TEST_F(LlmClient, WithoutAddressOrKeyNothingIsSent) {
    llm::NetClient unconfigured(http_, clock_, limiter_, {});
    EXPECT_TRUE(unconfigured.providers().empty());
    const auto result = await(unconfigured.chat_json(request()));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, Error::Kind::not_configured);
    EXPECT_TRUE(result.error().configuration());
    EXPECT_THAT(result.error().message, testing::HasSubstr("PONI_LLM_DASHSCOPE_API_KEY"));
    EXPECT_TRUE(http_.requests.empty());
}

// D-059: several providers at once; each call names the one it goes to.
TEST_F(LlmClient, ACallGoesToTheProviderItNamesWithThatProvidersKey) {
    auto llm = client({.providers = {{"dashscope", {"https://dashscope.example/compatible-mode/v1", "key-d"}},
                                     {"openai", {"https://openai.example/v1", "key-o"}}}});
    EXPECT_THAT(llm.providers(), testing::ElementsAre("dashscope", "openai"));
    EXPECT_TRUE(llm.configured("openai"));
    EXPECT_FALSE(llm.configured("anthropic"));

    http_.reply(200, completion(R"({"ok": 1})"));
    http_.reply(200, completion(R"({"ok": 2})"));
    auto to_openai = request();
    to_openai.provider = "openai";
    ASSERT_TRUE(await(llm.chat_json(request())));
    ASSERT_TRUE(await(llm.chat_json(to_openai)));
    ASSERT_EQ(http_.requests.size(), 2u);
    EXPECT_EQ(http_.requests[0].url.buffer(), "https://dashscope.example/compatible-mode/v1/chat/completions");
    EXPECT_EQ(http_.header(0, "Authorization"), "Bearer key-d");
    EXPECT_EQ(http_.requests[1].url.buffer(), "https://openai.example/v1/chat/completions");
    EXPECT_EQ(http_.header(1, "Authorization"), "Bearer key-o");
    ASSERT_EQ(records_.size(), 2u);
    EXPECT_EQ(records_[0].provider, "dashscope");
    EXPECT_EQ(records_[1].provider, "openai");

    auto to_nobody = request();
    to_nobody.provider = "anthropic";
    const auto refused = await(llm.chat_json(to_nobody));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().kind, Error::Kind::not_configured);
    EXPECT_EQ(http_.requests.size(), 2u) << "nothing is sent for a provider that isn't configured";
}

TEST_F(LlmClient, ANetworkFailureIsATransientErrorNotAnException) {
    http_.fail();
    http_.fail();
    auto llm = client({.max_attempts = 2});
    const auto result = await(llm.chat_json(request()));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, Error::Kind::unavailable);
    EXPECT_THAT(result.error().message, testing::HasSubstr("network error"));
    EXPECT_EQ(http_.requests.size(), 2u);
    ASSERT_EQ(records_.size(), 2u);
    EXPECT_EQ(records_[0].http_status, 0u);
}

TEST_F(LlmClient, StrangeResponsesAreInvalidNotCrashes) {
    for (const auto* body : {"not json at all", R"({"choices": []})", R"({"choices": [{"message": {"content": null}}]})", R"([1, 2])"}) {
        http_.reply(200, body);
        auto llm = client({.max_attempts = 1});
        const auto result = await(llm.chat_json(request()));
        ASSERT_FALSE(result) << body;
        EXPECT_EQ(result.error().kind, Error::Kind::invalid_response) << body;
    }
}

TEST_F(LlmClient, LongProviderMessagesAreCut) {
    http_.reply(400, boost::json::serialize(boost::json::object{{"error", boost::json::object{{"message", std::string(5000, 'x')}}}}));
    auto llm = client();
    const auto result = await(llm.chat_json(request()));
    ASSERT_FALSE(result);
    EXPECT_LT(result.error().message.size(), 400u);
}

// ---- Anthropic's Messages API (D-074) ----------------------------------------------

namespace {

std::string message(std::string_view text, std::string_view stop_reason = "end_turn") {
    return boost::json::serialize(boost::json::object{
        {"id", "msg_1"},
        {"type", "message"},
        {"role", "assistant"},
        {"model", "claude-opus-5-5"},
        // A thinking block comes before the answer.
        {"content", boost::json::array{boost::json::object{{"type", "thinking"}, {"thinking", ""}},
                                       boost::json::object{{"type", "text"}, {"text", text}}}},
        {"stop_reason", stop_reason},
        {"usage", boost::json::object{{"input_tokens", 2100}, {"output_tokens", 640}}}});
}

llm::NetOptions with_anthropic() {
    return {.providers = {{"anthropic", {"https://api.anthropic.example/v1", "key-a", llm::Protocol::anthropic}},
                          {"dashscope", {"https://llm.example/compatible-mode/v1", "key-d"}}}};
}

llm::ChatRequest judge_request() {
    return {.task = "judge",
            .provider = "anthropic",
            .model = "claude-opus-5-5",
            .messages = {{"system", "You rate replies."}, {"user", "Rate the reply below."}},
            // A caller that sets one anyway: this protocol never sends it.
            .temperature = 0.4,
            .max_tokens = 8000,
            .schema = boost::json::object{{"type", "object"}},
            .extras = boost::json::object{{"fallbacks", "default"}, {"output_config", boost::json::object{{"effort", "medium"}}}},
            .timeout = 240s,
            .go_live_run_id = 12};
}

}  // namespace

TEST_F(LlmClient, AnAnthropicProviderIsAskedThroughTheMessagesApi) {
    http_.reply(200, message(R"({"ratings": []})"));
    auto llm = client(with_anthropic());
    const auto result = await(llm.chat_json(judge_request()));
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_TRUE(result->json.contains("ratings"));
    EXPECT_EQ(result->prompt_tokens, 2100);
    EXPECT_EQ(result->completion_tokens, 640);
    EXPECT_EQ(result->finish_reason, "end_turn");

    ASSERT_EQ(http_.requests.size(), 1u);
    const auto& sent = http_.requests[0];
    EXPECT_EQ(sent.url.buffer(), "https://api.anthropic.example/v1/messages");
    EXPECT_EQ(http_.header(0, "x-api-key"), "key-a");
    EXPECT_EQ(http_.header(0, "anthropic-version"), "2026-10-21");
    EXPECT_EQ(http_.header(0, "anthropic-beta"), "server-side-fallback-2026-07-01") << "needed for the fallbacks the extras ask for";
    EXPECT_EQ(http_.header(0, "Authorization"), "") << "the key goes in x-api-key, not as a bearer token";
    EXPECT_EQ(sent.timeout, 240s) << "the judge's own limit";

    const auto body = boost::json::parse(sent.body).as_object();
    EXPECT_EQ(body.at("model").as_string(), "claude-opus-5-5");
    EXPECT_EQ(body.at("max_tokens").as_int64(), 8000);
    EXPECT_EQ(body.at("system").as_string(), "You rate replies.") << "the system message is a field of its own";
    ASSERT_EQ(body.at("messages").as_array().size(), 1u);
    EXPECT_EQ(body.at("messages").as_array()[0].at("role").as_string(), "user");
    EXPECT_FALSE(body.contains("temperature")) << "this API refuses a temperature";
    EXPECT_FALSE(body.contains("response_format"));
    EXPECT_EQ(body.at("output_config").at("format").at("type").as_string(), "json_schema");
    EXPECT_EQ(body.at("output_config").at("format").at("schema").at("type").as_string(), "object");
    EXPECT_EQ(body.at("output_config").at("effort").as_string(), "medium") << "an extra joins what the engine set";
    EXPECT_EQ(body.at("fallbacks").as_string(), "default");

    ASSERT_EQ(records_.size(), 1u);
    EXPECT_EQ(records_[0].task, "judge");
    EXPECT_EQ(records_[0].provider, "anthropic");
    EXPECT_EQ(records_[0].go_live_run_id, 12);
    EXPECT_EQ(records_[0].prompt_tokens, 2100);
    EXPECT_TRUE(records_[0].ok);
}

TEST_F(LlmClient, WithoutFallbacksNoBetaHeaderIsSentAndAFencedAnswerIsStillRead) {
    // A writer on an Anthropic model has no schema; the prompt asks for JSON and the model may fence it.
    http_.reply(200, message("```json\n{\"reply\": \"Thank you!\"}\n```"));
    auto llm = client(with_anthropic());
    auto request = judge_request();
    request.task = "generator";
    request.schema.reset();
    request.extras = {};
    const auto result = await(llm.chat_json(request));
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result->json.at("reply").as_string(), "Thank you!");
    EXPECT_EQ(http_.header(0, "anthropic-beta"), "");
    EXPECT_FALSE(boost::json::parse(http_.requests[0].body).as_object().contains("output_config"));
}

TEST_F(LlmClient, AnAnswerFromTheFallbackModelSaysWhichModelAnswered) {
    // The model asked declined; the provider's fallback model answered in the same call.
    http_.reply(200, boost::json::serialize(boost::json::object{
                         {"type", "message"},
                         {"model", "claude-opus-4-8"},
                         {"content",
                          boost::json::array{boost::json::object{{"type", "fallback"},
                                                                 {"from", boost::json::object{{"model", "claude-opus-5-5"}}},
                                                                 {"to", boost::json::object{{"model", "claude-opus-4-8"}}}},
                                             boost::json::object{{"type", "text"}, {"text", R"({"ratings": []})"}}}},
                         {"stop_reason", "end_turn"},
                         {"usage", boost::json::object{{"input_tokens", 2100}, {"output_tokens", 640}}}}));
    auto llm = client(with_anthropic());
    const auto rescued = await(llm.chat_json(judge_request()));
    ASSERT_TRUE(rescued) << rescued.error().message;
    EXPECT_EQ(rescued->fallback_model, "claude-opus-4-8");

    // The usual case: the model asked answered.
    http_.reply(200, message(R"({"ratings": []})"));
    const auto direct = await(llm.chat_json(judge_request()));
    ASSERT_TRUE(direct);
    EXPECT_FALSE(direct->fallback_model);
}

TEST_F(LlmClient, ARefusalIsTheModelsAnswerNotAnOutage) {
    http_.reply(200, boost::json::serialize(boost::json::object{{"type", "message"},
                                                                {"content", boost::json::array{}},
                                                                {"stop_reason", "refusal"},
                                                                {"stop_details", boost::json::object{{"type", "refusal"}, {"category", "bio"}}},
                                                                {"usage", boost::json::object{{"input_tokens", 900}, {"output_tokens", 0}}}}));
    auto llm = client(with_anthropic());
    const auto refused = await(llm.chat_json(judge_request()));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().kind, Error::Kind::rejected);
    EXPECT_FALSE(refused.error().transient());
    EXPECT_THAT(refused.error().message, testing::HasSubstr("declined"));
    EXPECT_THAT(refused.error().message, testing::HasSubstr("bio"));
    EXPECT_EQ(http_.requests.size(), 1u) << "asking again would get the same answer";
    EXPECT_EQ(records_[0].prompt_tokens, 900) << "the tokens were spent all the same";
}

TEST_F(LlmClient, AnthropicErrorsAreSortedLikeAnyProviders) {
    auto llm = client(with_anthropic());
    http_.reply(401, R"({"type": "error", "error": {"type": "authentication_error", "message": "invalid x-api-key"}})");
    const auto unauthenticated = await(llm.chat_json(judge_request()));
    ASSERT_FALSE(unauthenticated);
    EXPECT_TRUE(unauthenticated.error().configuration());
    EXPECT_THAT(unauthenticated.error().message, testing::HasSubstr("authentication_error"));
    EXPECT_THAT(unauthenticated.error().message, testing::Not(testing::HasSubstr("key-a")));

    // Overloaded, three times: retried, then given up as an outage.
    for (int i = 0; i < 3; ++i) http_.reply(529, R"({"type": "error", "error": {"type": "overloaded_error", "message": "Overloaded"}})");
    const auto overloaded = await(llm.chat_json(judge_request()));
    ASSERT_FALSE(overloaded);
    EXPECT_EQ(overloaded.error().kind, Error::Kind::unavailable);
    EXPECT_EQ(http_.requests.size(), 4u);

    // An answer that ran out of tokens before the JSON ended.
    for (int i = 0; i < 3; ++i) http_.reply(200, message(R"({"ratings": [{"reply": 1, "rat)", "max_tokens"));
    const auto cut = await(llm.chat_json(judge_request()));
    ASSERT_FALSE(cut);
    EXPECT_EQ(cut.error().kind, Error::Kind::invalid_response);
    EXPECT_THAT(cut.error().message, testing::HasSubstr("cut off"));
}

TEST_F(LlmClient, ARequestWithoutATemperatureCarriesNone) {
    http_.reply(200, completion(R"({"sentiment": "positive"})"));
    auto llm = client();
    auto bare = request();
    bare.temperature.reset();
    ASSERT_TRUE(await(llm.chat_json(bare)));
    EXPECT_FALSE(boost::json::parse(http_.requests[0].body).as_object().contains("temperature")) << "for a model that refuses one";
}
