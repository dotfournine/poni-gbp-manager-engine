#include "http/h2.hpp"

#include <boost/system/errc.hpp>
#include <boost/system/system_error.hpp>

#include <nghttp2/nghttp2.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <format>

namespace poni::http {

namespace {

[[noreturn]] void protocol_error(const std::string& what) {
    throw boost::system::system_error(boost::system::errc::make_error_code(boost::system::errc::protocol_error), "HTTP/2: " + what);
}

nghttp2_nv header(std::string_view name, std::string_view value) {
    // nghttp2 copies both (no NO_COPY flag), so they need only live until the submit.
    return nghttp2_nv{const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(name.data())),
                      const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(value.data())), name.size(), value.size(),
                      NGHTTP2_NV_FLAG_NONE};
}

}  // namespace

struct H2Exchange::Callbacks {
    static H2Exchange& self(void* user_data) { return *static_cast<H2Exchange*>(user_data); }

    static int on_header(nghttp2_session*, const nghttp2_frame* frame, const std::uint8_t* name, std::size_t name_length,
                         const std::uint8_t* value, std::size_t value_length, std::uint8_t, void* user_data) {
        auto& x = self(user_data);
        if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_RESPONSE || frame->hd.stream_id != x.stream_) return 0;
        const std::string_view n(reinterpret_cast<const char*>(name), name_length);
        if (n != ":status") return 0;
        unsigned status = 0;
        const auto* begin = reinterpret_cast<const char*>(value);
        const auto [end, ec] = std::from_chars(begin, begin + value_length, status);
        if (ec != std::errc{} || end != begin + value_length || status < 100 || status > 999) {
            x.failure_ = "the server sent a status that is not a number";
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        x.responses_.back().status = status;
        return 0;
    }

    static int on_data(nghttp2_session*, std::uint8_t, std::int32_t stream_id, const std::uint8_t* data, std::size_t length,
                       void* user_data) {
        auto& x = self(user_data);
        if (stream_id != x.stream_) return 0;
        auto& body = x.responses_.back().body;
        if (body.size() + length > x.max_body_bytes_) {
            x.failure_ = "the answer is longer than the client reads";
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        body.append(reinterpret_cast<const char*>(data), length);
        return 0;
    }

    static int on_stream_close(nghttp2_session*, std::int32_t stream_id, std::uint32_t error_code, void* user_data) {
        auto& x = self(user_data);
        if (stream_id != x.stream_) return 0;
        x.stream_ = -1;
        // A status below 200 is an interim one: without a final answer the request has none.
        if (error_code != NGHTTP2_NO_ERROR) {
            x.failure_ = std::format("the server reset the stream ({})", nghttp2_http2_strerror(error_code));
        } else if (x.responses_.back().status < 200) {
            x.failure_ = "the stream ended without an answer";
        } else {
            ++x.answered_;
        }
        return 0;
    }

    static nghttp2_ssize read_body(nghttp2_session*, std::int32_t, std::uint8_t* buffer, std::size_t length, std::uint32_t* flags,
                                   nghttp2_data_source* source, void*) {
        auto& x = *static_cast<H2Exchange*>(source->ptr);
        const auto& body = x.requests_[x.submitted_ - 1].body;
        const auto n = std::min(length, body.size() - x.body_sent_);
        std::memcpy(buffer, body.data() + x.body_sent_, n);
        x.body_sent_ += n;
        if (x.body_sent_ == body.size()) *flags |= NGHTTP2_DATA_FLAG_EOF;
        return static_cast<nghttp2_ssize>(n);
    }
};

H2Exchange::H2Exchange(std::vector<H2Request> requests, std::size_t max_body_bytes)
    : requests_(std::move(requests)), max_body_bytes_(max_body_bytes) {
    nghttp2_session_callbacks* callbacks = nullptr;
    if (nghttp2_session_callbacks_new(&callbacks) != 0) protocol_error("cannot start a session");
    nghttp2_session_callbacks_set_on_header_callback(callbacks, &Callbacks::on_header);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, &Callbacks::on_data);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, &Callbacks::on_stream_close);
    const int created = nghttp2_session_client_new(&session_, callbacks, this);
    nghttp2_session_callbacks_del(callbacks);
    if (created != 0) protocol_error("cannot start a session");
    // The server may not push to the client: nothing here would read it.
    const nghttp2_settings_entry settings[] = {{NGHTTP2_SETTINGS_ENABLE_PUSH, 0}};
    if (nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, settings, 1) != 0) {
        nghttp2_session_del(session_);
        protocol_error("cannot start a session");
    }
    responses_.reserve(requests_.size());
    if (!requests_.empty()) submit_next();
}

H2Exchange::~H2Exchange() { nghttp2_session_del(session_); }

void H2Exchange::submit_next() {
    const auto& request = requests_[submitted_];
    std::vector<nghttp2_nv> nva{header(":method", "POST"), header(":scheme", "https"), header(":authority", request.authority),
                                header(":path", request.path)};
    for (const auto& [name, value] : request.headers) nva.push_back(header(name, value));
    nghttp2_data_provider2 provider{};
    provider.source.ptr = this;
    provider.read_callback = &Callbacks::read_body;
    ++submitted_;
    body_sent_ = 0;
    responses_.emplace_back();
    const auto id = nghttp2_submit_request2(session_, nullptr, nva.data(), nva.size(), request.body.empty() ? nullptr : &provider, nullptr);
    if (id < 0) protocol_error(std::format("cannot send the request ({})", nghttp2_strerror(id)));
    stream_ = id;
}

void H2Exchange::check_failure() {
    if (!failure_.empty()) protocol_error(failure_);
}

std::string H2Exchange::take_output() {
    std::string out;
    for (;;) {
        const std::uint8_t* data = nullptr;
        const auto n = nghttp2_session_mem_send2(session_, &data);
        if (n < 0) protocol_error(nghttp2_strerror(static_cast<int>(n)));
        if (n == 0) break;
        out.append(reinterpret_cast<const char*>(data), static_cast<std::size_t>(n));
    }
    check_failure();
    return out;
}

void H2Exchange::feed(std::string_view bytes) {
    const auto n = nghttp2_session_mem_recv2(session_, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    check_failure();
    if (n < 0) protocol_error(nghttp2_strerror(static_cast<int>(n)));
    if (stream_ < 0 && submitted_ < requests_.size()) submit_next();
    if (!done() && nghttp2_session_want_read(session_) == 0 && nghttp2_session_want_write(session_) == 0) {
        protocol_error("the server ended the connection before every request had its answer");
    }
}

void H2Exchange::finish() { nghttp2_session_terminate_session(session_, NGHTTP2_NO_ERROR); }

}  // namespace poni::http
