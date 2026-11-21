// HTTP/2 for the outbound client (D-097): one client connection's framing,
// with nghttp2 and without any I/O. The caller writes what take_output()
// gives to the connection and feeds what it reads back, until done(). The
// requests are POSTs over https and go one after another, each on its own
// stream, so a reply is matched to its request by order. Apple's push
// service is the one server the engine speaks HTTP/2 to;
// http::NetClient::send_h2 drives this over TLS.
//
// Internal to src/http and its tests.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct nghttp2_session;

namespace poni::http {

struct H2Request {
    std::string authority;  // the host
    std::string path;       // as sent, already encoded
    // Lower-case names: HTTP/2 refuses capitals.
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

struct H2Response {
    unsigned status = 0;
    std::string body;
};

class H2Exchange {
public:
    // Throws boost::system::system_error if nghttp2 can't start a session.
    H2Exchange(std::vector<H2Request> requests, std::size_t max_body_bytes);
    ~H2Exchange();
    H2Exchange(const H2Exchange&) = delete;
    H2Exchange& operator=(const H2Exchange&) = delete;

    // The bytes to send now; empty when there is nothing to send.
    std::string take_output();
    // Bytes read from the connection. Throws boost::system::system_error
    // (errc::protocol_error) when they break HTTP/2, a stream is reset, or
    // the server ends the connection before every request has its answer.
    void feed(std::string_view bytes);
    // Every request has its answer.
    bool done() const { return answered_ == requests_.size(); }
    // Says goodbye (GOAWAY); take_output() then gives the frame to send.
    void finish();
    // The answers, in the order of the requests. Valid once done().
    std::vector<H2Response> take_responses() { return std::move(responses_); }

    // nghttp2's callbacks, which hold no state of their own.
    struct Callbacks;

private:
    void submit_next();
    void check_failure();

    std::vector<H2Request> requests_;
    std::vector<H2Response> responses_;
    std::size_t max_body_bytes_;
    std::size_t submitted_ = 0;
    std::size_t answered_ = 0;
    std::int32_t stream_ = -1;      // the stream of the request in flight
    std::size_t body_sent_ = 0;     // of the request in flight
    std::string failure_;           // set by a callback; thrown after it returns
    nghttp2_session* session_ = nullptr;
};

}  // namespace poni::http
