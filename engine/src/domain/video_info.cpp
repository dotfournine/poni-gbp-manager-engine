#include "domain/video_info.hpp"

#include <algorithm>
#include <format>

namespace poni::domain {

namespace {

std::uint64_t be(std::string_view bytes, std::size_t at, std::size_t count) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < count; ++i) value = (value << 8) | static_cast<unsigned char>(bytes[at + i]);
    return value;
}

struct Box {
    std::string_view type;
    std::string_view content;  // what follows the header, inside the box
};

// The box that starts at `at` in `bytes`, and where the next one starts.
// Nothing if no whole box is there: a box that says it is longer than the
// bytes that follow is not one.
std::optional<Box> box_at(std::string_view bytes, std::size_t at, std::size_t& next) {
    if (at > bytes.size() || bytes.size() - at < 8) return std::nullopt;
    std::uint64_t size = be(bytes, at, 4);
    std::size_t header = 8;
    if (size == 1) {  // the length is in the eight bytes after the type
        if (bytes.size() - at < 16) return std::nullopt;
        size = be(bytes, at + 8, 8);
        header = 16;
    } else if (size == 0) {  // to the end
        size = bytes.size() - at;
    }
    if (size < header || size > bytes.size() - at) return std::nullopt;
    next = at + static_cast<std::size_t>(size);
    return Box{.type = bytes.substr(at + 4, 4), .content = bytes.substr(at + header, static_cast<std::size_t>(size) - header)};
}

// The first box of `type` directly inside `bytes`.
std::optional<std::string_view> child(std::string_view bytes, std::string_view type) {
    std::size_t at = 0;
    while (true) {
        std::size_t next = 0;
        const auto box = box_at(bytes, at, next);
        if (!box) return std::nullopt;
        if (box->type == type) return box->content;
        at = next;
    }
}

// The movie header: how long the whole file runs. A version 1 header has
// 64-bit times.
std::optional<std::int64_t> duration_ms(std::string_view mvhd) {
    if (mvhd.size() < 4) return std::nullopt;
    const bool wide = static_cast<unsigned char>(mvhd[0]) == 1;
    const std::size_t timescale_at = wide ? 4 + 16 : 4 + 8;
    const std::size_t length = wide ? 8 : 4;
    if (mvhd.size() < timescale_at + 4 + length) return std::nullopt;
    const std::uint64_t timescale = be(mvhd, timescale_at, 4);
    const std::uint64_t duration = be(mvhd, timescale_at + 4, length);
    const std::uint64_t unknown = wide ? ~std::uint64_t{0} : 0xFFFFFFFFu;
    if (timescale == 0 || duration == 0 || duration == unknown) return std::nullopt;
    // A day is far beyond anything that is taken; it keeps the arithmetic small.
    const std::uint64_t seconds = duration / timescale;
    if (seconds > 86'400) return std::nullopt;
    const std::uint64_t ms = seconds * 1000 + (duration % timescale) * 1000 / timescale;
    return static_cast<std::int64_t>(std::max<std::uint64_t>(ms, 1));
}

// A track header's size: two 16.16 fixed-point numbers at its end.
std::optional<std::pair<int, int>> track_size(std::string_view tkhd) {
    if (tkhd.size() < 4) return std::nullopt;
    const bool wide = static_cast<unsigned char>(tkhd[0]) == 1;
    // version and flags, the times, the track's id, a reserved word, the duration,
    // two reserved words, layer, group, volume, a reserved half, the matrix.
    const std::size_t size_at = 4 + (wide ? 32 : 20) + 8 + 8 + 36;
    if (tkhd.size() < size_at + 8) return std::nullopt;
    const auto width = static_cast<int>(be(tkhd, size_at, 4) >> 16);
    const auto height = static_cast<int>(be(tkhd, size_at + 4, 4) >> 16);
    if (width <= 0 || height <= 0) return std::nullopt;
    return std::pair{width, height};
}

// True if the track is a picture track: its media handler says 'vide'.
bool is_video_track(std::string_view trak) {
    const auto mdia = child(trak, "mdia");
    if (!mdia) return false;
    const auto hdlr = child(*mdia, "hdlr");
    // version and flags, a predefined word, then the handler's type.
    return hdlr && hdlr->size() >= 12 && hdlr->substr(8, 4) == "vide";
}

}  // namespace

bool looks_like_video(std::string_view bytes) { return bytes.size() >= 12 && bytes.substr(4, 4) == "ftyp"; }

std::optional<VideoInfo> inspect_video(std::string_view bytes) {
    // The file's first box names what it is; a still image in this format (HEIC, AVIF) has one too,
    // but no movie box.
    std::size_t next = 0;
    const auto first = box_at(bytes, 0, next);
    if (!first || first->type != "ftyp" || first->content.size() < 4) return std::nullopt;
    const bool quicktime = first->content.substr(0, 4) == "qt  ";

    // The movie box is at the start of a file made for the web and at the end of one a camera wrote.
    const auto moov = child(bytes, "moov");
    if (!moov) return std::nullopt;
    // A file cut into fragments states at most the length of its first part: its length can't be read here.
    if (child(*moov, "mvex")) return std::nullopt;
    const auto mvhd = child(*moov, "mvhd");
    if (!mvhd) return std::nullopt;
    const auto length = duration_ms(*mvhd);
    if (!length) return std::nullopt;

    std::size_t at = 0;
    while (true) {
        const auto box = box_at(*moov, at, next);
        if (!box) return std::nullopt;  // no picture track
        at = next;
        if (box->type != "trak" || !is_video_track(box->content)) continue;
        const auto tkhd = child(box->content, "tkhd");
        if (!tkhd) return std::nullopt;
        const auto size = track_size(*tkhd);
        if (!size) return std::nullopt;
        return VideoInfo{.content_type = quicktime ? "video/quicktime" : "video/mp4",
                         .extension = quicktime ? "mov" : "mp4",
                         .width = size->first,
                         .height = size->second,
                         .duration_ms = *length};
    }
}

std::optional<std::string> video_problem(std::string_view bytes, const std::optional<VideoInfo>& info) {
    if (bytes.size() > kVideoMaxBytes) return "The video is larger than 75 MB, which Google does not take.";
    if (!info) {
        // A phone's own picture format starts as a video file does.
        const auto brand = bytes.size() >= 12 ? bytes.substr(8, 4) : std::string_view{};
        for (const std::string_view still : {"heic", "heix", "heim", "heis", "mif1", "msf1", "avif", "avis"}) {
            if (brand == still) return "The file is a HEIC or AVIF picture. Google takes a photo as JPG or PNG only.";
        }
        return "The file is not an MP4 or QuickTime video whose length and size can be read.";
    }
    if (info->duration_ms > kVideoMaxMs) {
        return std::format("The video runs {:.1f} seconds; Google takes at most 30.", static_cast<double>(info->duration_ms) / 1000.0);
    }
    if (std::min(info->width, info->height) < kVideoMinSide) {
        return std::format("The video is {} by {} pixels; Google wants at least 720 on the short side.", info->width, info->height);
    }
    return std::nullopt;
}

}  // namespace poni::domain
