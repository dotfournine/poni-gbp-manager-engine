// What an uploaded file is, read from its own first bytes: JPEG or PNG, and
// its size in pixels. No I/O, no image library: the engine stores a post's
// photo exactly as it was uploaded (D-088 point 9) and only has to know
// whether Google will take it.
//
// The bytes are untrusted. Nothing here trusts a length the file states
// beyond the bytes that are there.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace poni::domain {

struct ImageInfo {
    std::string content_type;  // "image/jpeg" or "image/png"
    std::string extension;     // "jpg" or "png"
    int width = 0;             // pixels, as stored (a phone's rotation flag is not applied)
    int height = 0;
};

// Nothing if the bytes are neither a JPEG nor a PNG, or their size can't be read.
std::optional<ImageInfo> inspect_image(std::string_view bytes);

// What Google requires of a post's photo: JPG or PNG, 10 KB to 5 MB, at
// least 250 pixels on the short side.
inline constexpr std::size_t kPhotoMinBytes = 10 * 1024;
inline constexpr std::size_t kPhotoMaxBytes = 5 * 1024 * 1024;
inline constexpr int kPhotoMinSide = 250;

// Why the file can't be a post's photo, as a sentence for the person who
// uploaded it; nothing if it can.
std::optional<std::string> photo_problem(std::string_view bytes, const std::optional<ImageInfo>& info);

}  // namespace poni::domain
