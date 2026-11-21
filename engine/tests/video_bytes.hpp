// The bytes of a video file as the engine reads one: the boxes of an MP4 or
// QuickTime file that say what it is, how long it runs and how large its
// picture is, around a block of filler where the pictures would be. Enough
// for the engine, which decodes nothing and stores the rest untouched.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace poni::test {

struct Clip {
    unsigned width = 1280;
    unsigned height = 720;
    // The length is `duration` ticks of `timescale` per second: 10 seconds here.
    std::uint32_t timescale = 600;
    std::uint64_t duration = 6000;
    std::string brand = "isom";  // "qt  " is a QuickTime file
    bool movie_box_last = false;  // as a camera writes it: the pictures first
    bool wide_headers = false;    // version 1: 64-bit times
    bool sound_track_first = false;
    std::string handler = "vide";  // "soun": a file with sound only
    bool fragmented = false;       // cut into fragments: the movie box only describes the first part
    std::size_t bytes = 64 * 1024;
};

namespace video_bytes {

inline std::string be32(std::uint32_t n) {
    return {static_cast<char>(n >> 24), static_cast<char>((n >> 16) & 0xFF), static_cast<char>((n >> 8) & 0xFF), static_cast<char>(n & 0xFF)};
}
inline std::string be64(std::uint64_t n) { return be32(static_cast<std::uint32_t>(n >> 32)) + be32(static_cast<std::uint32_t>(n & 0xFFFFFFFFu)); }
inline std::string zeros(std::size_t n) { return std::string(n, '\0'); }
inline std::string box(std::string_view type, const std::string& content) {
    return be32(static_cast<std::uint32_t>(8 + content.size())) + std::string(type) + content;
}

inline std::string movie_header(const Clip& clip) {
    if (clip.wide_headers) return box("mvhd", std::string("\x01\0\0\0", 4) + zeros(16) + be32(clip.timescale) + be64(clip.duration) + zeros(80));
    return box("mvhd", zeros(4) + zeros(8) + be32(clip.timescale) + be32(static_cast<std::uint32_t>(clip.duration)) + zeros(80));
}

inline std::string track(const Clip& clip, std::string_view handler, unsigned width, unsigned height) {
    const auto times = clip.wide_headers ? std::string("\x01\0\0\0", 4) + zeros(16) + be32(1) + zeros(4) + be64(clip.duration)
                                         : zeros(4) + zeros(8) + be32(1) + zeros(4) + be32(static_cast<std::uint32_t>(clip.duration));
    const auto header = box("tkhd", times + zeros(8) + zeros(8) + zeros(36) + be32(width << 16) + be32(height << 16));
    const auto media = box("mdia", box("mdhd", zeros(24)) + box("hdlr", zeros(4) + zeros(4) + std::string(handler) + zeros(12) + std::string(1, '\0')));
    return box("trak", header + media);
}

}  // namespace video_bytes

inline std::string video_file(const Clip& clip = {}) {
    using namespace video_bytes;
    const auto type = box("ftyp", clip.brand + be32(0) + clip.brand + "avc1");
    auto movie = movie_header(clip);
    if (clip.sound_track_first) movie += track(clip, "soun", 0, 0);
    movie += track(clip, clip.handler, clip.width, clip.height);
    if (clip.fragmented) movie += box("mvex", box("mehd", zeros(8)));
    movie = box("moov", movie);
    // Where the pictures and the sound would be. A phone writes where and when into the file too; it all stays.
    std::string filler = "GPS 40.7421,-73.9911";
    const std::size_t around = type.size() + movie.size() + 8;
    filler.resize(clip.bytes > around + filler.size() ? clip.bytes - around : filler.size(), 'v');
    const auto pictures = box("mdat", filler);
    return clip.movie_box_last ? type + pictures + movie : type + movie + pictures;
}

}  // namespace poni::test
