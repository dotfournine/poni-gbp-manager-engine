#include "connectors/notify/gmail.hpp"

#include "core/crypto.hpp"

#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>
#include <boost/system/system_error.hpp>

#include <format>

namespace poni::notify {

namespace asio = boost::asio;
using google::ApiError;

namespace {

// A header value can't hold a line break: one would start a new header.
std::string one_line(std::string_view text) {
    std::string out;
    for (const char c : text) out.push_back(c == '\r' || c == '\n' ? ' ' : c);
    return out;
}

// RFC 2047 encoded words, each at most 75 characters, split between UTF-8
// characters so every word decodes on its own.
std::string encoded_words(std::string_view text) {
    constexpr std::size_t kMaxRaw = 42;  // 56 base64 characters + "=?UTF-8?B?" + "?=" = 68
    std::string out;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = std::min(text.size(), start + kMaxRaw);
        // Don't cut inside a character: back up over continuation bytes.
        while (end < text.size() && end > start && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
        if (end == start) end = std::min(text.size(), start + kMaxRaw);  // invalid UTF-8: cut anyway
        if (!out.empty()) out += "\r\n ";
        out += "=?UTF-8?B?" + core::base64_encode(text.substr(start, end - start)) + "?=";
        start = end;
    }
    return out;  // empty text: an empty header value
}

// The sending mailbox's tokens, remembering whether the last one could be
// had: google::send returns the token source's error as it is, and a message
// to a person should say that it was the token.
struct WatchedTokens final : google::TokenSource {
    explicit WatchedTokens(google::TokenSource& inner) : inner(inner) {}
    asio::awaitable<google::Result<std::string>> access_token() override {
        auto token = co_await inner.access_token();
        failed = !token;
        co_return token;
    }
    void invalidate() override { inner.invalidate(); }

    google::TokenSource& inner;
    bool failed = false;
};

}  // namespace

std::string build_mime(const Sender& from, std::string_view to, std::string_view subject, std::string_view body) {
    std::string out;
    out += std::format("From: {} <{}>\r\n", encoded_words(one_line(from.name)), one_line(from.address));
    out += std::format("To: <{}>\r\n", one_line(to));
    out += std::format("Subject: {}\r\n", encoded_words(one_line(subject)));
    out += "MIME-Version: 1.0\r\n";
    out += "Content-Type: text/plain; charset=UTF-8\r\n";
    out += "Content-Transfer-Encoding: base64\r\n";
    // Tells mail servers this is automatic, so they don't answer it with out-of-office replies.
    out += "Auto-Submitted: auto-generated\r\n";
    out += "\r\n";
    const auto encoded = core::base64_encode(body);
    for (std::size_t i = 0; i < encoded.size(); i += 76) {
        out += encoded.substr(i, 76);
        out += "\r\n";
    }
    return out;
}

asio::awaitable<std::expected<void, SendError>> GmailChannel::send(Message message) {
    using Kind = SendError::Kind;
    http::ClientRequest request;
    request.method = http::ClientMethod::post;
    request.url = boost::urls::url("https://gmail.googleapis.com/gmail/v1/users/me/messages/send");
    request.content_type = "application/json";
    request.body = boost::json::serialize(boost::json::object{
        {"raw", core::base64url_encode(build_mime(sender_, message.recipient, message.subject, message.body))}});

    WatchedTokens tokens(api_.tokens);
    std::string network_error;
    try {
        const auto answer = co_await google::send({api_.http, tokens, api_.limiter}, kGmailApi, std::move(request));
        if (answer) co_return std::expected<void, SendError>{};
        const auto& error = answer.error();
        SendError failure{Kind::permanent, tokens.failed ? "the sending account's token: " + error.message : error.message};
        if (error.kind == ApiError::Kind::rate_limited || error.kind == ApiError::Kind::unavailable) failure.kind = Kind::transient;
        if (tokens.failed && error.kind == ApiError::Kind::not_configured) failure = SendError{Kind::not_configured, error.message};
        co_return std::unexpected(std::move(failure));
    } catch (const boost::system::system_error& e) {
        network_error = e.code().message();
    }
    SendError failure{Kind::transient, "network error: " + network_error};
    co_return std::unexpected(std::move(failure));
}

}  // namespace poni::notify
