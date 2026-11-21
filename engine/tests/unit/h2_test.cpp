// The HTTP/2 framing of the outbound client (D-097), against an nghttp2
// server in memory: requests one after another with their own answers, and
// what makes the whole connection fail.

#include "http/h2.hpp"

#include <boost/system/system_error.hpp>
#include <gtest/gtest.h>
#include <nghttp2/nghttp2.h>

#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

using namespace poni;

namespace {

const std::string kToken(64, 'a');

// ---- an HTTP/2 server in memory ----------------------------------------------------

class H2Server {
public:
    enum class Then { answer, reset, goaway };
    struct Script {
        Then then = Then::answer;
        unsigned status = 200;
        std::string body;
    };
    struct Seen {
        std::map<std::string, std::string> headers;
        std::string body;
    };

    std::deque<Script> script;
    std::vector<Seen> seen;  // in the order the requests came

    H2Server() {
        nghttp2_session_callbacks* cb = nullptr;
        nghttp2_session_callbacks_new(&cb);
        nghttp2_session_callbacks_set_on_begin_headers_callback(cb, [](nghttp2_session*, const nghttp2_frame* frame, void* self) {
            auto& s = *static_cast<H2Server*>(self);
            s.stream_index_[frame->hd.stream_id] = s.seen.size();
            s.seen.emplace_back();
            return 0;
        });
        nghttp2_session_callbacks_set_on_header_callback(
            cb, [](nghttp2_session*, const nghttp2_frame* frame, const uint8_t* name, size_t nlen, const uint8_t* value, size_t vlen,
                   uint8_t, void* self) {
                auto& s = *static_cast<H2Server*>(self);
                s.seen[s.stream_index_.at(frame->hd.stream_id)].headers[std::string(reinterpret_cast<const char*>(name), nlen)] =
                    std::string(reinterpret_cast<const char*>(value), vlen);
                return 0;
            });
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
            cb, [](nghttp2_session*, uint8_t, int32_t stream, const uint8_t* data, size_t len, void* self) {
                auto& s = *static_cast<H2Server*>(self);
                s.seen[s.stream_index_.at(stream)].body.append(reinterpret_cast<const char*>(data), len);
                return 0;
            });
        nghttp2_session_callbacks_set_on_frame_recv_callback(cb, [](nghttp2_session*, const nghttp2_frame* frame, void* self) {
            const bool request_ends = (frame->hd.type == NGHTTP2_HEADERS || frame->hd.type == NGHTTP2_DATA) &&
                                      (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0;
            if (request_ends) static_cast<H2Server*>(self)->respond(frame->hd.stream_id);
            return 0;
        });
        nghttp2_session_server_new(&session_, cb, this);
        nghttp2_session_callbacks_del(cb);
        nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, nullptr, 0);
    }
    ~H2Server() { nghttp2_session_del(session_); }

    // Takes what the client sent and gives back what the server sends.
    std::string exchange(std::string_view in) {
        if (!in.empty()) nghttp2_session_mem_recv2(session_, reinterpret_cast<const uint8_t*>(in.data()), in.size());
        std::string out;
        for (;;) {
            const uint8_t* data = nullptr;
            const auto n = nghttp2_session_mem_send2(session_, &data);
            if (n <= 0) break;
            out.append(reinterpret_cast<const char*>(data), static_cast<std::size_t>(n));
        }
        return out;
    }

private:
    void respond(int32_t stream) {
        Script s = script.empty() ? Script{} : script.front();
        if (!script.empty()) script.pop_front();
        if (s.then == Then::reset) {
            nghttp2_submit_rst_stream(session_, NGHTTP2_FLAG_NONE, stream, NGHTTP2_INTERNAL_ERROR);
            return;
        }
        if (s.then == Then::goaway) {
            nghttp2_submit_goaway(session_, NGHTTP2_FLAG_NONE, 0, NGHTTP2_NO_ERROR, nullptr, 0);
            return;
        }
        bodies_[stream] = {std::move(s.body), 0};
        status_text_[stream] = std::to_string(s.status);
        nghttp2_nv nva[] = {{const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(":status")),
                             reinterpret_cast<uint8_t*>(status_text_[stream].data()), 7, status_text_[stream].size(), NGHTTP2_NV_FLAG_NONE}};
        nghttp2_data_provider2 provider{};
        provider.source.ptr = &bodies_[stream];
        provider.read_callback = [](nghttp2_session*, int32_t, uint8_t* buf, size_t length, uint32_t* flags, nghttp2_data_source* source,
                                    void*) -> nghttp2_ssize {
            auto& [body, sent] = *static_cast<std::pair<std::string, std::size_t>*>(source->ptr);
            const auto n = std::min(length, body.size() - sent);
            std::memcpy(buf, body.data() + sent, n);
            sent += n;
            if (sent == body.size()) *flags |= NGHTTP2_DATA_FLAG_EOF;
            return static_cast<nghttp2_ssize>(n);
        };
        nghttp2_submit_response2(session_, stream, nva, 1, bodies_[stream].first.empty() ? nullptr : &provider);
    }

    nghttp2_session* session_ = nullptr;
    std::map<int32_t, std::size_t> stream_index_;
    std::map<int32_t, std::pair<std::string, std::size_t>> bodies_;
    std::map<int32_t, std::string> status_text_;
};

// Moves bytes between the two until the client has every answer.
void pump(http::H2Exchange& client, H2Server& server) {
    for (int i = 0; i < 100 && !client.done(); ++i) {
        const auto out = client.take_output();
        const auto back = server.exchange(out);
        if (!back.empty()) {
            client.feed(back);
        } else if (out.empty()) {
            break;
        }
    }
}

http::H2Request push_request(std::string body) {
    return {.authority = "api.sandbox.push.apple.com",
            .path = "/3/device/" + kToken,
            .headers = {{"apns-topic", "com.example.app"}, {"content-type", "application/json"}},
            .body = std::move(body)};
}

}  // namespace

// ---- HTTP/2 ---------------------------------------------------------------------------

TEST(H2Exchange, RequestsGoOneAfterAnotherAndEachGetsItsOwnAnswer) {
    H2Server server;
    server.script = {{.then = H2Server::Then::answer, .status = 200, .body = ""},
                     {.then = H2Server::Then::answer, .status = 410, .body = R"({"reason":"Unregistered"})"}};
    http::H2Exchange client({push_request(R"({"aps":{}})"), push_request(R"({"second":true})")}, 64 * 1024);
    pump(client, server);
    ASSERT_TRUE(client.done());
    const auto answers = client.take_responses();
    ASSERT_EQ(answers.size(), 2u);
    EXPECT_EQ(answers[0].status, 200u);
    EXPECT_EQ(answers[0].body, "");
    EXPECT_EQ(answers[1].status, 410u);
    EXPECT_EQ(answers[1].body, R"({"reason":"Unregistered"})");

    ASSERT_EQ(server.seen.size(), 2u);
    EXPECT_EQ(server.seen[0].headers.at(":method"), "POST");
    EXPECT_EQ(server.seen[0].headers.at(":scheme"), "https");
    EXPECT_EQ(server.seen[0].headers.at(":authority"), "api.sandbox.push.apple.com");
    EXPECT_EQ(server.seen[0].headers.at(":path"), "/3/device/" + kToken);
    EXPECT_EQ(server.seen[0].headers.at("apns-topic"), "com.example.app");
    EXPECT_EQ(server.seen[0].body, R"({"aps":{}})");
    EXPECT_EQ(server.seen[1].body, R"({"second":true})");

    // A goodbye goes out last, and the server takes it.
    client.finish();
    EXPECT_FALSE(client.take_output().empty());
}

TEST(H2Exchange, AStreamTheServerResetsIsAFailureOfTheWhole) {
    H2Server server;
    server.script = {{.then = H2Server::Then::answer, .status = 200, .body = ""}, {.then = H2Server::Then::reset}};
    http::H2Exchange client({push_request("{}"), push_request("{}")}, 64 * 1024);
    EXPECT_THROW(pump(client, server), boost::system::system_error);
}

TEST(H2Exchange, AServerThatSaysGoodbyeBeforeItAnswersIsAFailure) {
    H2Server server;
    server.script = {{.then = H2Server::Then::goaway}};
    http::H2Exchange client({push_request("{}")}, 64 * 1024);
    EXPECT_THROW(pump(client, server), boost::system::system_error);
    EXPECT_FALSE(client.done());
}

TEST(H2Exchange, AnAnswerLongerThanTheClientReadsIsAFailure) {
    H2Server server;
    server.script = {{.then = H2Server::Then::answer, .status = 200, .body = std::string(100, 'x')}};
    http::H2Exchange client({push_request("{}")}, 10);
    EXPECT_THROW(pump(client, server), boost::system::system_error);
}
