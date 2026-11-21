// What an uploaded video is, read from its own bytes: an MP4 or QuickTime
// file (the ISO base media format: boxes of a length and a four-letter
// type), how long it runs and its size in pixels. No I/O, no video library:
// the engine stores a post's video exactly as it was uploaded (D-088 point
// 9, D-090) and only has to know whether Google will take it. Nothing here
// decodes a frame or looks at a codec.
//
// The bytes are untrusted. No box is believed to be longer than the bytes
// that are there, and nothing is read outside the box it belongs to.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace poni::domain {

struct VideoInfo {
    std::string content_type;  // "video/mp4" or "video/quicktime"
    std::string extension;     // "mp4" or "mov"
    int width = 0;             // pixels of the video track, as stored (a phone's rotation is not applied)
    int height = 0;
    std::int64_t duration_ms = 0;
};

// Nothing if the bytes are not such a file, have no video track, or its
// length or size can't be read (a file that is still being recorded into,
// or one cut into fragments, states no length).
std::optional<VideoInfo> inspect_video(std::string_view bytes);

// What Google requires of a profile's video: at most 30 seconds, at most
// 75 MB, 720p or higher.
inline constexpr std::size_t kVideoMaxBytes = 75 * 1024 * 1024;
inline constexpr std::int64_t kVideoMaxMs = 30'000;
inline constexpr int kVideoMinSide = 720;

// Why the file can't be a post's video, as a sentence for the person who
// uploaded it; nothing if it can.
std::optional<std::string> video_problem(std::string_view bytes, const std::optional<VideoInfo>& info);

// True if the bytes start as an MP4 or QuickTime file does: enough to say
// "this was meant as a video" before anything else is known about it.
bool looks_like_video(std::string_view bytes);

}  // namespace poni::domain
