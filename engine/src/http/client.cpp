#include "http/client.hpp"

#include "core/text.hpp"
#include "core/version.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <boost/url/encode.hpp>
#include <boost/url/rfc/unreserved_chars.hpp>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <stdexcept>

namespace poni::http {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = beast::http;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

namespace {

constexpr std::size_t kMaxResponseBytes = 16 * 1024 * 1024;

bhttp::request<bhttp::string_body> build(ClientRequest& request, const std::string& host) {
    const auto verb = request.method == ClientMethod::post  ? bhttp::verb::post
                      : request.method == ClientMethod::put ? bhttp::verb::put
                      : request.method == ClientMethod::del ? bhttp::verb::delete_
                                                            : bhttp::verb::get;
    std::string target(request.url.encoded_target());
    if (target.empty()) target = "/";
    bhttp::request<bhttp::string_body> out{verb, target, 11};
    // A non-default port belongs in Host (RFC 9110).
    out.set(bhttp::field::host, request.url.has_port() ? std::format("{}:{}", host, std::string_view(request.url.port())) : host);
    out.set(bhttp::field::user_agent, std::format("poni-gbp-engine/{}", core::kVersion));
    out.set(bhttp::field::accept, "application/json");
    for (const auto& [name, value] : request.headers) out.set(name, value);
    if (!request.body.empty() || verb != bhttp::verb::get) {
        if (!request.content_type.empty()) out.set(bhttp::field::content_type, request.content_type);
        out.body() = std::move(request.body);
        out.prepare_payload();
    }
    return out;
}

template <class Stream>
asio::awaitable<ClientResponse> exchange(Stream& stream, beast::tcp_stream& tcp_layer,
                                         bhttp::request<bhttp::string_body>& request, std::chrono::seconds timeout) {
    tcp_layer.expires_after(timeout);
    co_await bhttp::async_write(stream, request, asio::use_awaitable);

    beast::flat_buffer buffer;
    bhttp::response_parser<bhttp::string_body> parser;
    parser.body_limit(kMaxResponseBytes);
    tcp_layer.expires_after(timeout);
    co_await bhttp::async_read(stream, buffer, parser, asio::use_awaitable);
    auto response = parser.release();
    co_return ClientResponse{response.result_int(), std::move(response.body())};
}

// What the mock stands in for (D-087). Pub/Sub, Cloud Storage and the
// metadata server are not here: against the mock they are refused.
constexpr std::array<std::string_view, 7> kMockedHosts{
    "accounts.google.com",                          // the consent page of connecting an account (a browser address)
    "oauth2.googleapis.com",                        // tokens
    "mybusinessaccountmanagement.googleapis.com",   // accounts
    "mybusinessbusinessinformation.googleapis.com", // locations
    "mybusiness.googleapis.com",                    // reviews and the reply
    "businessprofileperformance.googleapis.com",    // the Performance API
    "gmail.googleapis.com",                         // sending email
};

}  // namespace

std::optional<boost::urls::url> google_mock_address(const boost::urls::url& mock, const boost::urls::url& google) {
    if (google.scheme_id() != boost::urls::scheme::https || google.has_port() || google.has_userinfo()) return std::nullopt;
    const std::string host = google.host();
    if (!std::ranges::contains(kMockedHosts, std::string_view(host))) return std::nullopt;
    boost::urls::url out = mock;
    out.set_encoded_path("");
    out.segments().push_back(host);
    // The path as it is: its segments are already percent-encoded, so an id still can't change it.
    for (const auto segment : google.encoded_segments()) out.encoded_segments().push_back(segment);
    if (google.has_query()) out.set_encoded_query(google.encoded_query());
    return out;
}

bool is_google_address(const boost::urls::url& url) {
    std::string host = text::ascii_lower(url.host());
    while (host.ends_with('.')) host.pop_back();
    for (const std::string_view domain : {"googleapis.com", "google.com", "google.internal"}) {
        if (host == domain || (host.ends_with(domain) && host[host.size() - domain.size() - 1] == '.')) return true;
    }
    return false;
}

std::string form_encode(std::string_view value) { return boost::urls::encode(value, boost::urls::unreserved_chars); }

namespace {

constexpr std::string_view kGbpHost = "mybusiness.googleapis.com";

// ASCII, whatever the locale.
bool is_letter_or_digit(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

}  // namespace

// So no id is empty, a dot segment (a server that resolves those would take
// ".../localPosts/.." for the location itself), or holds a separator,
// encoded or not.
bool is_plain_id(std::string_view id) {
    return !id.empty() && id.size() <= 128 &&
           std::ranges::all_of(id, [](unsigned char c) { return is_letter_or_digit(c) || c == '-' || c == '_'; });
}

namespace {

// Whether `url` is https://mybusiness.googleapis.com/<shape> and nothing
// else. An empty entry of `shape` stands for an id. The address must equal
// the one built here from the same parts, character for character: then
// there is no port, user, query or fragment, nothing is percent-encoded, and
// what was checked is what is sent.
template <std::size_t N>
bool has_gbp_shape(const boost::urls::url& url, const std::array<std::string_view, N>& shape) {
    if (url.scheme_id() != boost::urls::scheme::https || url.host() != kGbpHost) return false;
    const auto segments = url.segments();
    if (segments.size() != shape.size()) return false;
    boost::urls::url built("https://mybusiness.googleapis.com");
    std::size_t i = 0;
    for (const auto& segment : segments) {
        const auto expected = shape[i++];
        if (expected.empty() ? !is_plain_id(segment) : segment != expected) return false;
        built.segments().push_back(segment);
    }
    return built.buffer() == url.buffer();
}

// A host name of letters, digits, '.' and '-', or an IP address. Anything
// else (an encoded NUL, a space, another alphabet) can be one name to this
// client's rules and another to the resolver and the certificate check.
bool is_plain_host(const boost::urls::url& url) {
    if (url.host_type() == boost::urls::host_type::ipv4 || url.host_type() == boost::urls::host_type::ipv6) return true;
    if (url.host_type() != boost::urls::host_type::name) return false;
    const std::string_view written = url.encoded_host();
    return !written.empty() &&
           std::ranges::all_of(written, [](unsigned char c) { return is_letter_or_digit(c) || c == '.' || c == '-'; });
}

bool breaks_a_line(std::string_view text) { return text.find_first_of(std::string_view("\r\n\0", 3)) != std::string_view::npos; }

}  // namespace

bool is_reply_url(const boost::urls::url& url) {
    static constexpr std::array<std::string_view, 8> shape{"v4", "accounts", "", "locations", "", "reviews", "", "reply"};
    return has_gbp_shape(url, shape);
}

bool is_local_post_collection_url(const boost::urls::url& url) {
    static constexpr std::array<std::string_view, 6> shape{"v4", "accounts", "", "locations", "", "localPosts"};
    return has_gbp_shape(url, shape);
}

bool is_local_post_url(const boost::urls::url& url) {
    static constexpr std::array<std::string_view, 7> shape{"v4", "accounts", "", "locations", "", "localPosts", ""};
    return has_gbp_shape(url, shape);
}

bool is_batch_get_reviews_url(const boost::urls::url& url) {
    static constexpr std::array<std::string_view, 4> shape{"v4", "accounts", "", "locations:batchGetReviews"};
    return has_gbp_shape(url, shape);
}

bool is_gbp_host(const boost::urls::url& url) {
    std::string host = text::ascii_lower(url.host());
    while (host.ends_with('.')) host.pop_back();
    return host == kGbpHost;
}

std::optional<std::string_view> why_refused(ClientMethod method, const boost::urls::url& url) {
    if (!is_plain_host(url)) return "the outbound client only sends to a host named with letters, digits, '.' and '-'";
    switch (method) {
        case ClientMethod::get: return std::nullopt;
        case ClientMethod::put:
            if (!is_reply_url(url)) return "the outbound client only sends PUT for a reply to a review";
            return std::nullopt;
        case ClientMethod::del:
            if (!is_local_post_url(url)) return "the outbound client only sends DELETE for a local post";
            return std::nullopt;
        case ClientMethod::post:
            // The host however it is written; the two allowed addresses only as Google writes them.
            if (is_gbp_host(url) && !is_local_post_collection_url(url) && !is_batch_get_reviews_url(url)) {
                return "the outbound client only sends a POST to Google Business Profile to create a local post or to read reviews";
            }
            return std::nullopt;
    }
    return "the outbound client does not know this method";
}

std::optional<std::string_view> why_refused(const ClientRequest& request) {
    if (const auto refusal = why_refused(request.method, request.url)) return refusal;
    // Google's front ends take a method from these headers on a POST; the
    // others would change where the request goes or how its body is read.
    static constexpr std::array<std::string_view, 6> forbidden{"Host",           "X-HTTP-Method-Override", "X-HTTP-Method",
                                                               "X-Method-Override", "Content-Length",         "Transfer-Encoding"};
    for (const auto& [name, value] : request.headers) {
        if (std::ranges::any_of(forbidden, [&](std::string_view f) { return text::ascii_iequals(name, f); })) {
            return "the outbound client sets the method, the host and the length of a request itself";
        }
        if (breaks_a_line(name) || breaks_a_line(value)) return "the outbound client sends no header that holds a line break";
    }
    if (breaks_a_line(request.content_type)) return "the outbound client sends no header that holds a line break";
    if (request.method == ClientMethod::del && !request.body.empty()) return "the outbound client sends a DELETE without a body";
    return std::nullopt;
}

asio::awaitable<ClientResponse> NetClient::send(ClientRequest request) {
    const auto ex = co_await asio::this_coro::executor;
    if (request.url.scheme_id() != boost::urls::scheme::https && request.url.scheme_id() != boost::urls::scheme::http) {
        throw std::invalid_argument("outbound requests need an http or https URL");
    }
    // What may change something at Google is three address shapes wide (client.hpp).
    if (const auto refusal = why_refused(request)) throw std::invalid_argument(std::string(*refusal));
    if (google_mock_ && is_google_address(request.url)) {
        auto mocked = google_mock_address(*google_mock_, request.url);
        // A mock database must never reach a real Google service, nor a real one the mock (D-087).
        if (!mocked) throw std::invalid_argument("against the Google mock the engine calls no other Google service");
        request.url = std::move(*mocked);
    }
    const bool https = request.url.scheme_id() == boost::urls::scheme::https;
    const std::string host = request.url.host();
    const std::string port = request.url.has_port() ? std::string(request.url.port()) : (https ? "443" : "80");
    const auto timeout = request.timeout;
    auto message = build(request, host);

    tcp::resolver resolver(ex);
    const auto endpoints = co_await resolver.async_resolve(host, port, asio::use_awaitable);

    if (!https) {
        beast::tcp_stream stream(ex);
        stream.expires_after(timeout);
        co_await stream.async_connect(endpoints, asio::use_awaitable);
        auto response = co_await exchange(stream, stream, message, timeout);
        boost::system::error_code ignored;
        stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
        co_return response;
    }

    beast::ssl_stream<beast::tcp_stream> stream(ex, tls_);
    if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {  // SNI
        throw boost::system::system_error(
            boost::system::error_code(static_cast<int>(::ERR_get_error()), asio::error::get_ssl_category()), "SNI");
    }
    stream.set_verify_callback(ssl::host_name_verification(host));

    auto& tcp_layer = beast::get_lowest_layer(stream);
    tcp_layer.expires_after(timeout);
    co_await tcp_layer.async_connect(endpoints, asio::use_awaitable);
    tcp_layer.expires_after(timeout);
    co_await stream.async_handshake(ssl::stream_base::client, asio::use_awaitable);

    auto response = co_await exchange(stream, tcp_layer, message, timeout);

    // Best-effort close_notify; servers often just drop the connection.
    boost::system::error_code ignored;
    tcp_layer.expires_after(3s);
    co_await stream.async_shutdown(asio::redirect_error(asio::use_awaitable, ignored));
    co_return response;
}

}  // namespace poni::http
