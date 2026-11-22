#include "http/server.hpp"

#include "core/log.hpp"
#include "core/mapped_file.hpp"
#include "core/text.hpp"
#include "core/version.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/file_body.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/url/parse.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <optional>
#include <unordered_set>
#include <vector>

namespace poni::http {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = beast::http;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

namespace {

constexpr auto kIdleTimeout = 60s;
constexpr auto kWriteTimeout = 30s;
// A file is given time by its size: a video of 75 MB from a phone's
// connection does not arrive in a minute. This is the slowest sender that
// is still waited for.
constexpr std::size_t kSlowestUploadBytesPerSecond = 128 * 1024;

// The router's Request for what the header says: the method, the target and
// the header names in lower case. Nothing for a target that is no path.
template <class Header>
std::optional<Request> request_of(const Header& header, const std::string& remote_ip) {
    const auto target = boost::urls::parse_origin_form(header.target());
    if (!target) return std::nullopt;
    Request request;
    request.method = header.method();
    request.path = target->path();
    request.query = target->encoded_query();
    request.remote_ip = remote_ip;
    for (const auto& field : header) request.headers.insert_or_assign(text::ascii_lower(field.name_string()), std::string(field.value()));
    return request;
}

}  // namespace

// All members are used on the io_context thread only.
struct Server::Impl : std::enable_shared_from_this<Server::Impl> {
    Impl(asio::any_io_executor ex, const Router& r) : acceptor(ex), router(r) {}

    tcp::acceptor acceptor;
    const Router& router;
    bool stopping = false;
    // Connections waiting for a request, or in the middle of sending a file; closed on stop().
    std::unordered_set<beast::tcp_stream*> waiting;

    asio::awaitable<void> accept_loop() {
        const auto self = shared_from_this();
        while (!stopping) {
            boost::system::error_code ec;
            auto socket = co_await acceptor.async_accept(asio::redirect_error(asio::use_awaitable, ec));
            if (ec) {
                if (stopping || ec == asio::error::operation_aborted) break;
                log::warn("http: accept failed", {{"error", ec.message()}});
                continue;
            }
            asio::co_spawn(acceptor.get_executor(), session(beast::tcp_stream(std::move(socket))), asio::detached);
        }
    }

    asio::awaitable<void> session(beast::tcp_stream stream) {
        const auto self = shared_from_this();
        beast::flat_buffer buffer;
        boost::system::error_code ec;
        std::string remote_ip;
        if (const auto peer = stream.socket().remote_endpoint(ec); !ec) remote_ip = peer.address().to_string();

        while (!stopping) {
            // The header first, into a parser without a body: how large the body may be, and whether
            // it goes to memory or to disk, depend on the route (Router::body_limit).
            bhttp::request_parser<bhttp::empty_body> head_parser;
            head_parser.body_limit(router.largest_body_limit());

            stream.expires_after(kIdleTimeout);
            waiting.insert(&stream);
            co_await bhttp::async_read_header(stream, buffer, head_parser, asio::redirect_error(asio::use_awaitable, ec));
            waiting.erase(&stream);
            if (ec) {
                co_await answer_failure(stream, ec);
                break;
            }
            const auto& header = head_parser.get();
            const unsigned version = header.version();
            const bool head = header.method() == bhttp::verb::head;
            const bool keep_alive = header.keep_alive() && !stopping;
            const std::string method(header.method_string());
            // Built once from the header; the body is put in when it has arrived.
            std::optional<Request> request = request_of(header, remote_ip);
            const std::size_t limit = request ? router.body_limit(request->method, request->path) : Router::kDefaultBodyLimit;
            // A declared length is refused before a byte of the body is read;
            // a body sent in chunks is cut off by the parser at the limit.
            if (const auto length = head_parser.content_length(); length && *length > limit) {
                co_await answer_failure(stream, bhttp::error::body_limit);
                break;
            }
            if (request && limit > Router::kDefaultBodyLimit) {
                // A route that takes a file: who is calling is asked first, from the header alone, so
                // that only a signed-in person's file is taken in.
                auto wait = std::chrono::seconds(kIdleTimeout) + std::chrono::seconds(limit / kSlowestUploadBytesPerSecond);
                auto refused = co_await router.refuse_before_body(*request);
                if (refused) {
                    log::debug("http request", {{"method", method}, {"path", request->path}, {"status", refused->status}});
                    co_await answer_before_body(stream, std::move(*refused), version, head_parser.content_length().value_or(limit), wait);
                    break;
                }
                // The file goes to disk, not to memory (D-101): into a file that has no name, so that
                // it is gone when the request is, whatever happens. Written on this thread, as the
                // system's page cache takes it; mapped once it is complete.
                bhttp::request_parser<bhttp::file_body> parser{std::move(head_parser)};
                parser.body_limit(limit);
                std::string failure;
                try {
                    parser.get().body().file().native_handle(core::nameless_file());
                } catch (const std::exception& e) {
                    failure = e.what();
                }
                if (!failure.empty()) {
                    log::error("http: no file for a request body", {{"path", request->path}, {"error", failure}});
                    co_await answer_before_body(stream, no_room(), version, parser.content_length().value_or(limit), wait);
                    break;
                }
                // A file on its way in is given up when the engine stops: nothing of it is stored yet.
                waiting.insert(&stream);
                stream.expires_after(wait);
                co_await bhttp::async_read(stream, buffer, parser, asio::redirect_error(asio::use_awaitable, ec));
                waiting.erase(&stream);
                if (!ec) {
                    try {
                        request->file = core::MappedFile::map(parser.get().body().file().native_handle());
                    } catch (const std::exception& e) {
                        failure = e.what();
                    }
                }
                if (!failure.empty()) {
                    log::error("http: a request body could not be mapped", {{"path", request->path}, {"error", failure}});
                    co_await write(stream, no_room(), version, false, false);
                    break;
                }
                // The parser closes its descriptor when it goes; the mapping keeps the file.
            } else {
                bhttp::request_parser<bhttp::string_body> parser{std::move(head_parser)};
                parser.body_limit(limit);
                stream.expires_after(kIdleTimeout);
                co_await bhttp::async_read(stream, buffer, parser, asio::redirect_error(asio::use_awaitable, ec));
                if (!ec && request) request->body = std::move(parser.get().body());
            }
            if (ec) {
                co_await answer_failure(stream, ec);
                break;
            }

            std::optional<Response> response;
            std::string path;
            if (request) {
                // HEAD is answered as GET without the body.
                if (head) request->method = bhttp::verb::get;
                path = request->path;
                response = co_await router.dispatch(std::move(*request));
            } else {
                response = Response::error(400, "bad_request", "The request target is not valid.");
            }

            log::debug("http request", {{"method", method}, {"path", path}, {"status", response->status}});
            if (!co_await write(stream, std::move(*response), version, keep_alive, head) || !keep_alive) break;
        }

        stream.socket().shutdown(tcp::socket::shutdown_both, ec);
    }

    // An answer given before the body was read, then what the caller is still sending, read and thrown
    // away: closing with unread bytes makes the system reset the connection, and a browser in the
    // middle of sending a file would never see the answer.
    asio::awaitable<void> answer_before_body(beast::tcp_stream& stream, Response answer, unsigned version, std::uint64_t bytes,
                                             std::chrono::seconds wait) {
        if (!co_await write(stream, std::move(answer), version, false, false)) co_return;
        waiting.insert(&stream);
        co_await discard(stream, bytes, wait);
        waiting.erase(&stream);
    }

    // The engine could not take a file to disk (D-101).
    static Response no_room() { return Response::error(503, "no_room", "The engine cannot take a file now."); }

    // The answer to a request that could not be read: none for a connection that closed or timed
    // out, which is normal, 413 for a body too large, and 400 for anything else.
    static asio::awaitable<void> answer_failure(beast::tcp_stream& stream, boost::system::error_code ec) {
        if (ec == bhttp::error::end_of_stream || ec == beast::error::timeout || ec == asio::error::operation_aborted ||
            ec == asio::error::connection_reset) {
            co_return;
        }
        // A named value, so that no temporary is built inside the call.
        auto answer = Response::error(400, "bad_request", "The request could not be read.");
        if (ec == bhttp::error::body_limit) answer = Response::error(413, "payload_too_large", "The request body is too large.");
        co_await write(stream, std::move(answer), 11, false, false);
    }

    // Reads and throws away up to `bytes` of what the caller is still sending, for at most `wait`,
    // after an answer that was given before the body was read. Nothing of it is kept.
    static asio::awaitable<void> discard(beast::tcp_stream& stream, std::uint64_t bytes, std::chrono::seconds wait) {
        std::vector<char> scratch(64 * 1024);
        boost::system::error_code ec;
        // Nothing more will be said; the caller sees the end of the answer while it is still sending.
        stream.socket().shutdown(tcp::socket::shutdown_send, ec);
        ec = {};
        stream.expires_after(wait);
        while (bytes > 0 && !ec) {
            const auto wanted = static_cast<std::size_t>(std::min<std::uint64_t>(bytes, scratch.size()));
            const auto got = co_await stream.async_read_some(asio::buffer(scratch.data(), wanted), asio::redirect_error(asio::use_awaitable, ec));
            bytes -= std::min<std::uint64_t>(got, bytes);
        }
    }

    // Returns false if the connection can't be used any more.
    static asio::awaitable<bool> write(beast::tcp_stream& stream, Response response, unsigned version, bool keep_alive,
                                       bool head) {
        bhttp::response<bhttp::string_body> out{static_cast<bhttp::status>(response.status), version};
        out.set(bhttp::field::server, std::format("poni-gbp-engine/{}", core::kVersion));
        out.set(bhttp::field::content_type, response.content_type);
        out.set(bhttp::field::cache_control, "no-store");
        out.set("X-Content-Type-Options", "nosniff");
        // insert, not set: a response may carry several Set-Cookie headers.
        for (const auto& [name, value] : response.headers) out.insert(name, value);
        out.keep_alive(keep_alive);
        out.body() = std::move(response.body);
        out.prepare_payload();
        if (head) out.body().clear();  // Content-Length stays, as HEAD requires

        boost::system::error_code ec;
        stream.expires_after(kWriteTimeout);
        bhttp::response_serializer<bhttp::string_body> serializer{out};
        co_await bhttp::async_write(stream, serializer, asio::redirect_error(asio::use_awaitable, ec));
        co_return !ec;
    }
};

Server::Server(asio::any_io_executor ex, const Router& router, const std::string& host, std::uint16_t port)
    : impl_(std::make_shared<Impl>(std::move(ex), router)) {
    const tcp::endpoint endpoint(asio::ip::make_address(host), port);
    impl_->acceptor.open(endpoint.protocol());
    impl_->acceptor.set_option(asio::socket_base::reuse_address(true));
    impl_->acceptor.bind(endpoint);
    impl_->acceptor.listen(asio::socket_base::max_listen_connections);
}

Server::~Server() { stop(); }

void Server::start() { asio::co_spawn(impl_->acceptor.get_executor(), impl_->accept_loop(), asio::detached); }

void Server::stop() {
    if (impl_->stopping) return;
    impl_->stopping = true;
    boost::system::error_code ec;
    impl_->acceptor.close(ec);
    for (auto* stream : impl_->waiting) stream->cancel();
}

std::uint16_t Server::port() const { return impl_->acceptor.local_endpoint().port(); }

}  // namespace poni::http
