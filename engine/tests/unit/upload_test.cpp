// What an upload needs before anything is stored: reading what a file is
// from its own bytes (a JPEG or PNG and its size; an MP4 or QuickTime video,
// its size and its length), Google's requirements for a post's photo and for
// its video, and a request body limit that belongs to the route, with the
// question of who is calling asked before a large body is read.

#include "core/tls.hpp"
#include "domain/image_info.hpp"
#include "domain/video_info.hpp"
#include "http/client.hpp"
#include "loopback.hpp"
#include "video_bytes.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/json/parse.hpp>
#include <gmock/gmock.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

using namespace poni;
namespace asio = boost::asio;
using boost::beast::http::verb;
using testing::HasSubstr;

namespace {

std::string be16(unsigned n) { return {static_cast<char>(n >> 8), static_cast<char>(n & 0xFF)}; }
std::string be32(unsigned n) { return be16(n >> 16) + be16(n & 0xFFFF); }

// The first bytes of a PNG of that size, padded to `bytes`.
std::string png(unsigned width, unsigned height, std::size_t bytes = 12 * 1024) {
    std::string out = "\x89PNG\r\n\x1a\n" + be32(13) + "IHDR" + be32(width) + be32(height) + std::string("\x08\x02\x00\x00\x00", 5);
    out.resize(std::max(bytes, out.size()), '\0');
    return out;
}

// A JPEG header as a camera writes it: a JFIF segment, an Exif segment (which a phone fills with where and when), then the frame.
std::string jpeg(unsigned width, unsigned height, std::size_t bytes = 12 * 1024, unsigned char frame_marker = 0xC0) {
    std::string out = "\xFF\xD8";
    out += "\xFF\xE0" + be16(16) + std::string("JFIF\0\x01\x01\0\0\x01\0\x01\0\0", 14);
    out += "\xFF\xE1" + be16(2 + 40) + std::string(40, 'E');
    out += std::string("\xFF", 1) + static_cast<char>(frame_marker) + be16(17) + std::string("\x08", 1) + be16(height) + be16(width) +
           std::string(10, '\x01');
    out += "\xFF\xDA" + be16(2);
    out.resize(std::max(bytes, out.size()), '\0');
    return out;
}

}  // namespace

// ---- what a file is ---------------------------------------------------------------------

TEST(ImageInfo, APngSaysItsSizeInItsHeader) {
    const auto info = domain::inspect_image(png(1200, 900));
    ASSERT_TRUE(info);
    EXPECT_EQ(info->content_type, "image/png");
    EXPECT_EQ(info->extension, "png");
    EXPECT_EQ(info->width, 1200);
    EXPECT_EQ(info->height, 900);
}

TEST(ImageInfo, AJpegSaysItsSizeInItsFrameHeaderAfterWhateverThePhoneWrote) {
    const auto info = domain::inspect_image(jpeg(4032, 3024));
    ASSERT_TRUE(info);
    EXPECT_EQ(info->content_type, "image/jpeg");
    EXPECT_EQ(info->extension, "jpg");
    EXPECT_EQ(info->width, 4032);
    EXPECT_EQ(info->height, 3024);
    // A progressive JPEG has another frame marker.
    EXPECT_EQ(domain::inspect_image(jpeg(800, 600, 12 * 1024, 0xC2))->width, 800);
}

TEST(ImageInfo, WhatIsNotAJpegOrAPngIsNothingWhateverItClaims) {
    for (const std::string& bytes : {std::string{}, std::string("GIF89a") + std::string(2000, '\0'), std::string("<svg xmlns='x'/>"),
                                     std::string("%PDF-1.7"), std::string("\xFF\xD8"), std::string("\x89PNG\r\n\x1a\n"),
                                     std::string(5000, '\xFF')}) {
        EXPECT_FALSE(domain::inspect_image(bytes)) << bytes.substr(0, 8);
    }
    // A PNG signature that is not followed by its header, and one that claims no size.
    auto no_header = png(800, 600);
    no_header.replace(12, 4, "IDAT");
    EXPECT_FALSE(domain::inspect_image(no_header));
    EXPECT_FALSE(domain::inspect_image(png(0, 600)));

    // A JPEG cut off before its frame, one whose segment says it is longer than the file, and one that has no frame at all.
    EXPECT_FALSE(domain::inspect_image(jpeg(800, 600).substr(0, 30)));
    auto overrun = jpeg(800, 600, 64);
    overrun.replace(4, 2, be16(60000));
    EXPECT_FALSE(domain::inspect_image(overrun));
    EXPECT_FALSE(domain::inspect_image(std::string("\xFF\xD8\xFF\xDA", 4) + be16(2) + std::string(100, '\0')));
    // A table segment whose marker sits among the frame markers is not a frame.
    EXPECT_FALSE(domain::inspect_image(std::string("\xFF\xD8\xFF\xC4", 4) + be16(8) + be16(800) + be16(600) + be16(0) + "\xFF\xD9"));
}

TEST(ImageInfo, GooglesRequirementsForAPostsPhoto) {
    const auto problem = [](const std::string& bytes) { return domain::photo_problem(bytes, domain::inspect_image(bytes)); };
    EXPECT_FALSE(problem(png(1200, 900)));
    EXPECT_FALSE(problem(jpeg(250, 4000)));
    EXPECT_FALSE(problem(png(720, 720, domain::kPhotoMinBytes)));
    EXPECT_FALSE(problem(png(720, 720, domain::kPhotoMaxBytes)));

    EXPECT_THAT(problem(png(720, 720, domain::kPhotoMinBytes - 1)).value(), HasSubstr("10 KB"));
    EXPECT_THAT(problem(png(720, 720, domain::kPhotoMaxBytes + 1)).value(), HasSubstr("5 MB"));
    EXPECT_THAT(problem(jpeg(249, 4000)).value(), HasSubstr("249 by 4000")) << "the short side counts, whichever it is";
    EXPECT_THAT(problem(jpeg(4000, 249)).value(), HasSubstr("at least 250"));
    EXPECT_THAT(problem(std::string(20000, 'x')).value(), HasSubstr("not a JPG or a PNG"));
}

// ---- what a video is (D-090) -----------------------------------------------------------------

TEST(VideoInfo, AnMp4SaysItsLengthAndItsSizeInItsHeaders) {
    const auto info = domain::inspect_video(test::video_file());
    ASSERT_TRUE(info);
    EXPECT_EQ(info->content_type, "video/mp4");
    EXPECT_EQ(info->extension, "mp4");
    EXPECT_EQ(info->width, 1280);
    EXPECT_EQ(info->height, 720);
    EXPECT_EQ(info->duration_ms, 10'000);

    // A length that is no whole number of seconds: 30000 / 1001 pictures a second, 299 of them.
    const auto odd = domain::inspect_video(test::video_file({.timescale = 30000, .duration = 299 * 1001}));
    ASSERT_TRUE(odd);
    EXPECT_EQ(odd->duration_ms, 9976);
}

TEST(VideoInfo, AFileARealEncoderWroteIsRead) {
    // Two seconds of grey at 1280 by 720, written by ffmpeg with x264: the header bytes are an encoder's, not
    // this test's own idea of them (the other tests build their files with tests/video_bytes.hpp).
    std::ifstream in(std::filesystem::path(PONI_TEST_FIXTURES_DIR) / "video-720p-2s.mp4", std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ASSERT_EQ(bytes.size(), 1802u);
    const auto info = domain::inspect_video(bytes);
    ASSERT_TRUE(info);
    EXPECT_EQ(info->content_type, "video/mp4");
    EXPECT_EQ(info->width, 1280);
    EXPECT_EQ(info->height, 720);
    EXPECT_EQ(info->duration_ms, 2000);
    EXPECT_FALSE(domain::video_problem(bytes, info));
}

TEST(VideoInfo, AFileAsAPhoneWritesItIsReadToo) {
    // The pictures first and the movie box at the end, a sound track before the picture track, upright
    // (the file stores 1080 by 1920 or the other way round; the short side is what counts), 64-bit times.
    const auto info = domain::inspect_video(test::video_file(
        {.width = 1080, .height = 1920, .brand = "qt  ", .movie_box_last = true, .wide_headers = true, .sound_track_first = true}));
    ASSERT_TRUE(info);
    EXPECT_EQ(info->content_type, "video/quicktime");
    EXPECT_EQ(info->extension, "mov");
    EXPECT_EQ(info->width, 1080);
    EXPECT_EQ(info->height, 1920);
    EXPECT_EQ(info->duration_ms, 10'000);

    // A box may say its length in 64 bits, and the last one may run "to the end".
    using test::video_bytes::be64, test::video_bytes::box, test::video_bytes::zeros;
    const auto file = test::video_file();  // the type, the movie box, the pictures
    const auto type = file.substr(0, file.find("moov") - 4);
    const auto movie = file.substr(type.size(), file.find("mdat") - 4 - type.size());
    const std::string pictures(4000, 'v');
    EXPECT_TRUE(domain::inspect_video(type + be32(1) + "mdat" + be64(16 + pictures.size()) + pictures + movie));
    EXPECT_TRUE(domain::inspect_video(type + movie + be32(0) + "mdat" + pictures));
}

TEST(VideoInfo, WhatIsNotAVideoWhoseLengthAndSizeCanBeReadIsNothing) {
    using test::video_bytes::be64, test::video_bytes::box, test::video_bytes::zeros;
    EXPECT_FALSE(domain::inspect_video(""));
    EXPECT_FALSE(domain::inspect_video(jpeg(1280, 720)));
    EXPECT_FALSE(domain::looks_like_video(jpeg(1280, 720)));
    EXPECT_FALSE(domain::looks_like_video("ftyp"));
    EXPECT_TRUE(domain::looks_like_video(test::video_file()));

    // A still image in the same format (HEIC) has the first box and no movie.
    EXPECT_FALSE(domain::inspect_video(box("ftyp", "heic" + be32(0) + "mif1heic") + box("meta", zeros(200)) + box("mdat", zeros(4000))));
    EXPECT_FALSE(domain::inspect_video(test::video_file({.handler = "soun"}))) << "sound only";
    EXPECT_FALSE(domain::inspect_video(test::video_file({.timescale = 0})));
    EXPECT_FALSE(domain::inspect_video(test::video_file({.duration = 0}))) << "a file cut into fragments states no length";
    EXPECT_FALSE(domain::inspect_video(test::video_file({.duration = 0xFFFFFFFFu}))) << "nor does one still being recorded";
    EXPECT_FALSE(domain::inspect_video(test::video_file({.width = 0})));
    EXPECT_FALSE(domain::inspect_video(test::video_file({.fragmented = true})))
        << "a file in fragments states the length of its first part, which may be a second of many minutes";
    EXPECT_FALSE(domain::inspect_video(test::video_file({.duration = ~std::uint64_t{0}, .wide_headers = true}))) << "unknown, in 64 bits";
    EXPECT_FALSE(domain::inspect_video(test::video_file({.duration = 600ull * 90'000}))) << "longer than a day is not believed";

    // No box is believed to be longer than the bytes that are there.
    const auto file = test::video_file({.movie_box_last = true});
    for (const std::size_t cut : {std::size_t{4}, std::size_t{20}, file.size() - 200, file.size() - 1}) {
        EXPECT_FALSE(domain::inspect_video(file.substr(0, cut))) << cut;
    }
    const auto type = box("ftyp", std::string("isom") + be32(0) + "isomavc1");
    EXPECT_FALSE(domain::inspect_video(type + be32(0x7FFFFFFF) + "moov" + zeros(400))) << "a movie box that says it is 2 GB long";
    EXPECT_FALSE(domain::inspect_video(type + be32(4) + "moov" + zeros(400))) << "a box shorter than its own header";
    EXPECT_FALSE(domain::inspect_video(type + be32(1) + "moov" + be64(~std::uint64_t{0}) + zeros(400)));
    EXPECT_FALSE(domain::inspect_video(type + box("moov", box("mvhd", zeros(6)))));
    EXPECT_FALSE(domain::inspect_video(type + box("moov", box("mvhd", zeros(12) + be32(600) + be32(6000) + zeros(80)) +
                                                           box("trak", box("tkhd", zeros(40)) +
                                                                           box("mdia", box("hdlr", zeros(8) + "vide" + zeros(13)))))))
        << "a track header too short to hold a size";
    EXPECT_FALSE(domain::inspect_video(type + box("moov", box("mvhd", zeros(12) + be32(600) + be32(6000) + zeros(80)) +
                                                           box("trak", box("tkhd", std::string("\x01\0\0\0", 4) + zeros(80)) +
                                                                           box("mdia", box("hdlr", zeros(8) + "vide" + zeros(13)))))))
        << "nor one with 64-bit times: its size sits twelve bytes further on";
    // A box inside the movie box that says it reaches beyond it, into the pictures, is not read.
    const auto header = zeros(12) + be32(600) + be32(6000) + zeros(80);
    EXPECT_FALSE(domain::inspect_video(type + be32(8 + 8 + 20) + "moov" + be32(8 + 100) + "mvhd" + header + box("mdat", zeros(4000))));
}

TEST(VideoInfo, GooglesRequirementsForAPostsVideo) {
    const auto problem = [](const std::string& bytes) { return domain::video_problem(bytes, domain::inspect_video(bytes)); };
    EXPECT_FALSE(problem(test::video_file()));
    EXPECT_FALSE(problem(test::video_file({.width = 720, .height = 1280}))) << "upright";
    EXPECT_FALSE(problem(test::video_file({.duration = 30 * 600}))) << "exactly 30 seconds";
    EXPECT_FALSE(problem(test::video_file({.bytes = domain::kVideoMaxBytes})));

    EXPECT_THAT(problem(test::video_file({.duration = 30 * 600 + 60})).value(), HasSubstr("runs 30.1 seconds"));
    EXPECT_THAT(problem(test::video_file({.width = 1280, .height = 719})).value(), HasSubstr("1280 by 719"));
    EXPECT_THAT(problem(test::video_file({.width = 640, .height = 1280})).value(), HasSubstr("at least 720")) << "the short side counts, whichever it is";
    EXPECT_THAT(problem(test::video_file({.bytes = domain::kVideoMaxBytes + 1})).value(), HasSubstr("75 MB"));
    EXPECT_THAT(problem(test::video_file({.handler = "soun"})).value(), HasSubstr("not an MP4 or QuickTime video"));
    // A phone's own picture format starts as a video file does; the person is told what it is.
    using test::video_bytes::box, test::video_bytes::zeros;
    EXPECT_THAT(problem(box("ftyp", "heic" + zeros(4) + "mif1heic") + box("meta", zeros(200)) + box("mdat", zeros(4000))).value(),
                HasSubstr("HEIC or AVIF picture"));
}

// ---- a body limit that belongs to the route --------------------------------------------------

namespace {

class BodyLimit : public ::testing::Test {
protected:
    BodyLimit() : tls_(core::make_tls_client_context()), client_(tls_.ctx) {
        const auto size = [](const http::Request& request) -> asio::awaitable<http::Response> {
            co_return http::Response::json(200, boost::json::object{{"bytes", request.body.size()}});
        };
        router_.add(verb::post, "/small", {http::Access::anonymous, "test.small"}, size);
        router_.add(verb::post, "/files", {http::Access::anonymous, "test.files", 3 * 1024 * 1024}, size);
        router_.add(verb::post, "/files/{id}/note", {http::Access::anonymous, "test.note"}, size);
        router_.add(verb::post, "/staff-files", {http::Access::user, "test.staff_files", 3 * 1024 * 1024}, size);
    }

    http::ClientResponse post(const std::string& path, std::size_t bytes) {
        return test::with_server(io_, router_, [&](std::uint16_t port) {
            http::ClientRequest request;
            request.method = http::ClientMethod::post;
            request.url = boost::urls::url(std::format("http://127.0.0.1:{}{}", port, path));
            request.content_type = "application/octet-stream";
            request.body = std::string(bytes, 'x');
            return client_.send(std::move(request));
        });
    }

    // Only the head of a request that announces `bytes` of body, and what the server says to that: a body that is
    // too large is refused on its announced length, before any of it is read.
    std::string announce(const std::string& path, std::size_t bytes) {
        return test::with_server(io_, router_, [&](std::uint16_t port) -> asio::awaitable<std::string> {
            asio::ip::tcp::socket socket(io_);
            co_await socket.async_connect({asio::ip::make_address("127.0.0.1"), port}, asio::use_awaitable);
            const auto head = std::format("POST {} HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/octet-stream\r\n"
                                          "Content-Length: {}\r\n\r\n",
                                          path, bytes);
            co_await asio::async_write(socket, asio::buffer(head), asio::use_awaitable);
            std::string answer;
            boost::system::error_code ec;
            co_await asio::async_read(socket, asio::dynamic_buffer(answer), asio::redirect_error(asio::use_awaitable, ec));
            co_return answer;
        });
    }

    asio::io_context io_;
    core::TlsSetup tls_;
    http::NetClient client_;
    http::Router router_;
};

}  // namespace

TEST_F(BodyLimit, ARouteSaysHowLargeItsBodyMayBeAndEveryOtherRouteKeepsTheSmallLimit) {
    EXPECT_EQ(router_.body_limit(verb::post, "/small"), http::Router::kDefaultBodyLimit);
    EXPECT_EQ(router_.body_limit(verb::post, "/files"), 3u * 1024 * 1024);
    EXPECT_EQ(router_.body_limit(verb::post, "/files/7/note"), http::Router::kDefaultBodyLimit) << "a route under it does not inherit it";
    EXPECT_EQ(router_.body_limit(verb::put, "/files"), http::Router::kDefaultBodyLimit) << "nor does another method";
    EXPECT_EQ(router_.body_limit(verb::post, "/nowhere"), http::Router::kDefaultBodyLimit);
    EXPECT_EQ(router_.largest_body_limit(), 3u * 1024 * 1024);
}

TEST_F(BodyLimit, TheServerTakesALargeBodyOnlyWhereTheRouteAllowsIt) {
    const auto accepted = post("/files", 2 * 1024 * 1024);
    ASSERT_EQ(accepted.status, 200u) << accepted.body;
    EXPECT_EQ(boost::json::parse(accepted.body).at("bytes").as_int64(), 2 * 1024 * 1024);
    EXPECT_EQ(post("/files", 3 * 1024 * 1024).status, 200u) << "exactly the limit";

    EXPECT_EQ(post("/small", 512 * 1024).status, 200u);

    const auto too_large = announce("/files", 3 * 1024 * 1024 + 1);
    EXPECT_THAT(too_large, testing::StartsWith("HTTP/1.1 413")) << "refused on the announced length, before the body is read";
    EXPECT_THAT(too_large, HasSubstr("payload_too_large"));
    EXPECT_THAT(announce("/small", 2 * 1024 * 1024), testing::StartsWith("HTTP/1.1 413")) << "what the upload route takes, no other route does";
    EXPECT_THAT(announce("/files/7/note", 2 * 1024 * 1024), testing::StartsWith("HTTP/1.1 413"));
    EXPECT_THAT(announce("/nowhere", 2 * 1024 * 1024), testing::StartsWith("HTTP/1.1 413")) << "an unknown path gets no more than the default";
    EXPECT_THAT(announce("/files", 200ull * 1024 * 1024 * 1024), testing::StartsWith("HTTP/1.1 413"));
}

TEST_F(BodyLimit, WhoIsCallingIsAskedBeforeALargeBodyIsRead) {
    // Without a hook a route for signed-in people is closed. The answer comes on the head alone:
    // the announced body is never sent, and the server does not wait for it.
    const auto closed = announce("/staff-files", 2 * 1024 * 1024);
    EXPECT_THAT(closed, testing::StartsWith("HTTP/1.1 401"));
    EXPECT_THAT(closed, HasSubstr("unauthenticated"));

    // With a hook, it decides, from the headers, and is asked again when the request is carried out.
    int asked = 0;
    bool let_in = false;
    std::vector<std::size_t> bodies;  // what the hook was shown of the body, each time it was asked
    router_.set_auth_hook([&](http::Request& request, const http::RouteInfo& info) -> asio::awaitable<std::optional<http::Response>> {
        if (info.action != "test.staff_files") co_return std::nullopt;
        ++asked;
        bodies.push_back(request.body.size());
        EXPECT_EQ(request.header("content-type"), "application/octet-stream");
        if (!let_in) co_return http::Response::error(403, "forbidden", "Not for you.");
        co_return std::nullopt;
    });
    EXPECT_THAT(announce("/staff-files", 2 * 1024 * 1024), testing::StartsWith("HTTP/1.1 403"));
    EXPECT_EQ(asked, 1);

    let_in = true;
    const auto accepted = post("/staff-files", 2 * 1024 * 1024);
    ASSERT_EQ(accepted.status, 200u) << accepted.body;
    EXPECT_EQ(boost::json::parse(accepted.body).at("bytes").as_int64(), 2 * 1024 * 1024);
    EXPECT_EQ(asked, 3) << "once before the body, once with it";
    EXPECT_EQ(bodies, (std::vector<std::size_t>{0, 0, 2 * 1024 * 1024})) << "the first question of a request is asked with no body read";

    // A small route is not asked early: its body is small, and the request is answered as before.
    asked = 0;
    EXPECT_EQ(post("/small", 1024).status, 200u);
    EXPECT_EQ(asked, 0);
    EXPECT_THAT(announce("/staff-files", 3 * 1024 * 1024 + 1), testing::StartsWith("HTTP/1.1 413")) << "too large is refused before anyone is asked";
    EXPECT_EQ(asked, 0);
}

TEST_F(BodyLimit, ARefusalReachesACallerWhoIsAlreadySendingTheFile) {
    // A browser does not wait for an answer before it sends: the whole file is on its way when the
    // refusal is written. The server reads the rest and throws it away, so that the answer arrives
    // and not a reset connection.
    std::size_t seen = 0;
    router_.set_auth_hook([&](http::Request& request, const http::RouteInfo&) -> asio::awaitable<std::optional<http::Response>> {
        seen += request.body.size();
        co_return http::Response::error(403, "forbidden", "Not for you.");
    });
    const auto refused = post("/staff-files", 3 * 1024 * 1024);
    EXPECT_EQ(refused.status, 403u);
    EXPECT_THAT(refused.body, HasSubstr("Not for you."));
    EXPECT_EQ(seen, 0u) << "none of the file was kept";
}

TEST_F(BodyLimit, ABodySentInChunksIsNotReadBeforeTheCallerIsKnownEither) {
    // No length is announced: the question is asked because of the route, not because of a length.
    http::Server server(io_.get_executor(), router_, "127.0.0.1", 0);
    server.start();
    const auto answer = test::run(io_, [&]() -> asio::awaitable<std::string> {
        asio::ip::tcp::socket socket(io_);
        co_await socket.async_connect({asio::ip::make_address("127.0.0.1"), server.port()}, asio::use_awaitable);
        const std::string head = "POST /staff-files HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/octet-stream\r\n"
                                 "Transfer-Encoding: chunked\r\n\r\n";
        co_await asio::async_write(socket, asio::buffer(head), asio::use_awaitable);
        // The answer comes on the head alone. The caller then closes its side, as one does that reads a refusal.
        std::string text(12, ' ');
        co_await asio::async_read(socket, asio::buffer(text), asio::use_awaitable);
        boost::system::error_code ec;
        socket.shutdown(asio::ip::tcp::socket::shutdown_send, ec);
        server.stop();
        co_return text;
    }());
    EXPECT_EQ(answer, "HTTP/1.1 401");
}

TEST_F(BodyLimit, AQuestionThatFailsIsAnErrorAndNoBodyIsRead) {
    router_.set_auth_hook([](http::Request&, const http::RouteInfo&) -> asio::awaitable<std::optional<http::Response>> {
        throw std::runtime_error("the session store is down");
        co_return std::nullopt;
    });
    const auto answer = announce("/staff-files", 2 * 1024 * 1024);
    EXPECT_THAT(answer, testing::StartsWith("HTTP/1.1 500"));
    EXPECT_THAT(answer, testing::Not(HasSubstr("session store"))) << "what went wrong is logged, not sent";
}

TEST_F(BodyLimit, AFileOnItsWayInIsGivenUpWhenTheServerStops) {
    http::Server server(io_.get_executor(), router_, "127.0.0.1", 0);
    server.start();
    const auto closed = test::run(io_, [&]() -> asio::awaitable<bool> {
        asio::ip::tcp::socket socket(io_);
        co_await socket.async_connect({asio::ip::make_address("127.0.0.1"), server.port()}, asio::use_awaitable);
        // The head of a file of 2 MB and its first thousand bytes; the rest never comes.
        const auto start = std::format("POST /files HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/octet-stream\r\n"
                                       "Content-Length: {}\r\n\r\n{}",
                                       2 * 1024 * 1024, std::string(1000, 'x'));
        co_await asio::async_write(socket, asio::buffer(start), asio::use_awaitable);
        // Let the server take the head and begin to wait for the rest, then stop it.
        asio::steady_timer pause(io_, std::chrono::milliseconds(200));
        co_await pause.async_wait(asio::use_awaitable);
        server.stop();
        std::string rest;
        boost::system::error_code ec;
        co_await asio::async_read(socket, asio::dynamic_buffer(rest), asio::redirect_error(asio::use_awaitable, ec));
        co_return rest.empty();
    }());
    EXPECT_TRUE(closed) << "the connection is closed at once, without an answer: the engine does not wait minutes for a file while it stops";
}
