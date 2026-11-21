#include "domain/image_info.hpp"

#include <algorithm>
#include <cstdint>
#include <format>

namespace poni::domain {

namespace {

std::uint8_t byte(std::string_view bytes, std::size_t at) { return static_cast<std::uint8_t>(bytes[at]); }

// Big-endian, as both formats write their numbers. The caller has checked the range.
std::uint32_t be16(std::string_view bytes, std::size_t at) { return (std::uint32_t{byte(bytes, at)} << 8) | byte(bytes, at + 1); }
std::uint32_t be32(std::string_view bytes, std::size_t at) { return (be16(bytes, at) << 16) | be16(bytes, at + 2); }

constexpr std::string_view kPngSignature = "\x89PNG\r\n\x1a\n";

std::optional<ImageInfo> inspect_png(std::string_view bytes) {
    // The signature, then the IHDR chunk: length, "IHDR", width, height.
    if (bytes.size() < 24 || !bytes.starts_with(kPngSignature) || bytes.substr(12, 4) != "IHDR") return std::nullopt;
    const auto width = be32(bytes, 16);
    const auto height = be32(bytes, 20);
    if (width == 0 || height == 0 || width > 100'000 || height > 100'000) return std::nullopt;
    return ImageInfo{.content_type = "image/png", .extension = "png", .width = static_cast<int>(width), .height = static_cast<int>(height)};
}

std::optional<ImageInfo> inspect_jpeg(std::string_view bytes) {
    if (bytes.size() < 4 || byte(bytes, 0) != 0xFF || byte(bytes, 1) != 0xD8) return std::nullopt;
    std::size_t at = 2;
    // Segment after segment until the frame header, which carries the size.
    while (at + 4 <= bytes.size()) {
        if (byte(bytes, at) != 0xFF) return std::nullopt;
        const auto marker = byte(bytes, at + 1);
        if (marker == 0xFF) {  // a fill byte before a marker
            ++at;
            continue;
        }
        at += 2;
        // Markers that stand alone.
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) continue;
        // The image data or the end, without a frame header before it.
        if (marker == 0xD9 || marker == 0xDA) return std::nullopt;
        if (at + 2 > bytes.size()) return std::nullopt;
        const std::size_t length = be16(bytes, at);
        if (length < 2 || at + length > bytes.size()) return std::nullopt;
        const bool frame = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        if (frame) {
            // length (2), precision (1), height (2), width (2)
            if (length < 7) return std::nullopt;
            const auto height = be16(bytes, at + 3);
            const auto width = be16(bytes, at + 5);
            if (width == 0 || height == 0) return std::nullopt;
            return ImageInfo{.content_type = "image/jpeg", .extension = "jpg", .width = static_cast<int>(width), .height = static_cast<int>(height)};
        }
        at += length;
    }
    return std::nullopt;
}

}  // namespace

std::optional<ImageInfo> inspect_image(std::string_view bytes) {
    if (auto png = inspect_png(bytes)) return png;
    return inspect_jpeg(bytes);
}

std::optional<std::string> photo_problem(std::string_view bytes, const std::optional<ImageInfo>& info) {
    if (!info) return "The file is not a JPG or a PNG image.";
    if (bytes.size() < kPhotoMinBytes) return "The photo is smaller than 10 KB, which Google does not take.";
    if (bytes.size() > kPhotoMaxBytes) return "The photo is larger than 5 MB, which Google does not take.";
    if (std::min(info->width, info->height) < kPhotoMinSide) {
        return std::format("The photo is {} by {} pixels; Google wants at least 250 on the short side.", info->width, info->height);
    }
    return std::nullopt;
}

}  // namespace poni::domain
