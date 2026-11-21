#include "connectors/notify/apns.hpp"

#include "core/crypto.hpp"

#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/system/errc.hpp>
#include <boost/system/system_error.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <stdexcept>

namespace poni::notify {

namespace asio = boost::asio;
using Kind = PushAnswer::Kind;

namespace {

std::int64_t unix_seconds(core::TimePoint t) { return std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count(); }

// Apple's reason, from a body like {"reason":"BadDeviceToken"}; empty if there is none.
std::string reason_of(std::string_view body) {
    boost::system::error_code ec;
    const auto value = boost::json::parse(body, ec);
    if (ec || !value.is_object()) return {};
    const auto* reason = value.as_object().if_contains("reason");
    if (!reason || !reason->is_string()) return {};
    std::string out(reason->as_string());
    // A word of Apple's: kept short and to letters, since it goes into the delivery log.
    std::erase_if(out, [](char c) { return !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')); });
    if (out.size() > 64) out.resize(64);
    return out;
}

}  // namespace

std::string provider_token(const core::Es256Key& key, std::string_view key_id, std::string_view team_id, core::TimePoint issued_at) {
    const auto header = core::base64url_encode(boost::json::serialize(boost::json::object{{"alg", "ES256"}, {"kid", key_id}}));
    const auto claims = core::base64url_encode(boost::json::serialize(boost::json::object{{"iss", team_id}, {"iat", unix_seconds(issued_at)}}));
    const auto signing_input = header + "." + claims;
    return signing_input + "." + core::base64url_encode(key.sign(signing_input));
}

PushAnswer read_answer(const http::ClientResponse& response) {
    const auto reason = reason_of(response.body);
    const auto said = std::format("{} {}", response.status, reason.empty() ? std::string("(no reason)") : reason);
    if (response.ok()) return {Kind::accepted, {}};
    // The token will never work again: the app was removed, or the token is of another environment or app.
    if (response.status == 410 || reason == "BadDeviceToken" || reason == "DeviceTokenNotForTopic" || reason == "Unregistered") {
        return {Kind::device_gone, said};
    }
    if (reason == "ExpiredProviderToken") return {Kind::retry, said, true};
    if (response.status == 429 || response.status >= 500) return {Kind::retry, said};
    // A setting of the engine's, the same for every push.
    static constexpr std::array<std::string_view, 7> sender{"InvalidProviderToken", "MissingProviderToken", "BadEnvironmentKeyInToken",
                                                            "BadTopic",             "TopicDisallowed",      "MissingTopic",
                                                            "Forbidden"};
    if (std::ranges::contains(sender, std::string_view(reason)) || response.status == 403) return {Kind::sender_refused, said};
    return {Kind::refused, said};
}

std::string ApnsSender::token() {
    const auto now = clock_.now();
    if (!token_ || now - token_made_ >= kTokenLifetime || now < token_made_) {
        token_ = provider_token(settings_.key, settings_.key_id, settings_.team_id, now);
        token_made_ = now;
    }
    return *token_;
}

asio::awaitable<std::expected<std::vector<PushAnswer>, SendError>> ApnsSender::send(std::vector<Push> pushes) {
    using ErrorKind = SendError::Kind;
    if (pushes.empty()) co_return std::vector<PushAnswer>{};
    const auto host = settings_.environment == core::ApnsEnvironment::production ? "api.push.apple.com" : "api.sandbox.push.apple.com";
    const auto bearer = "bearer " + token();
    const auto expires = std::to_string(unix_seconds(clock_.now() + kExpiresAfter));

    std::vector<http::ClientRequest> requests;
    for (auto& push : pushes) {
        http::ClientRequest request;
        request.method = http::ClientMethod::post;
        request.url.set_scheme_id(boost::urls::scheme::https);
        request.url.set_host_name(host);
        request.url.segments() = {"3", "device", push.device_token};
        request.headers = {{"authorization", bearer},
                           {"apns-topic", settings_.bundle_id},
                           {"apns-push-type", "alert"},
                           {"apns-priority", "10"},
                           {"apns-expiration", expires},
                           {"apns-collapse-id", push.collapse_id}};
        request.content_type = "application/json";
        request.body = std::move(push.payload);
        request.timeout = std::chrono::seconds(15);
        requests.push_back(std::move(request));
    }

    co_await limiter_.acquire(kApnsApi);
    std::string network_error;
    std::string refusal;
    std::vector<http::ClientResponse> responses;
    try {
        responses = co_await http_.send_h2(std::move(requests));
    } catch (const boost::system::system_error& e) {
        // The HTTP/2 framing says what broke in its message; the network's code says enough.
        network_error = e.code() == boost::system::errc::protocol_error ? std::string(e.what()) : e.code().message();
    } catch (const std::invalid_argument& e) {
        refusal = e.what();
    }
    if (!refusal.empty()) co_return std::unexpected(SendError{ErrorKind::permanent, "the outbound client refused the push: " + refusal});
    if (!network_error.empty()) co_return std::unexpected(SendError{ErrorKind::transient, "network error: " + network_error});

    std::vector<PushAnswer> answers;
    for (const auto& response : responses) {
        auto answer = read_answer(response);
        if (answer.token_expired || answer.kind == Kind::sender_refused) token_.reset();
        answers.push_back(std::move(answer));
    }
    co_return answers;
}

}  // namespace poni::notify
