#include "connectors/storage/store.hpp"

#include "core/crypto.hpp"

#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>

#include <chrono>
#include <format>
#include <fstream>

namespace poni::storage {

namespace asio = boost::asio;
using namespace std::chrono_literals;

namespace {

ApiError refused(std::string message) { return {ApiError::Kind::other, 0, std::move(message)}; }

// The slowest connection to Cloud Storage an upload is still given time for.
constexpr std::size_t kSlowestUploadBytesPerSecond = 256 * 1024;

}  // namespace

bool valid_object_name(std::string_view name) {
    if (name.empty() || name.size() > 200 || name.front() == '/' || name.back() == '/') return false;
    std::size_t segment = 0;
    for (std::size_t i = 0; i <= name.size(); ++i) {
        if (i == name.size() || name[i] == '/') {
            const auto part = name.substr(segment, i - segment);
            if (part.empty() || part == "." || part == "..") return false;
            segment = i + 1;
            continue;
        }
        const char c = name[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!ok) return false;
    }
    return true;
}

bool is_folder_setting(std::string_view setting) { return setting.contains('/'); }

std::string multipart_upload_body(const Object& object, std::string_view boundary) {
    const boost::json::object metadata{{"name", object.name}, {"contentType", object.content_type}, {"cacheControl", object.cache_control}};
    std::string out;
    out.reserve(object.body.size() + 512);
    out += std::format("--{}\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n{}\r\n", boundary, boost::json::serialize(metadata));
    out += std::format("--{}\r\nContent-Type: {}\r\n\r\n", boundary, object.content_type);
    out += object.body;
    out += std::format("\r\n--{}--\r\n", boundary);
    return out;
}

asio::awaitable<Result<void>> GcsStore::put(Object object) {
    if (!valid_object_name(object.name)) co_return std::unexpected(refused("the object's name is not allowed"));
    http::ClientRequest request;
    request.method = http::ClientMethod::post;
    request.url = google::url("https://storage.googleapis.com", {"upload", "storage", "v1", "b", bucket_, "o"});
    // A page is sent in a moment; a post's video of 75 MB is given time by its size.
    request.timeout = 60s + std::chrono::seconds(object.bytes().size() / kSlowestUploadBytesPerSecond);
    if (object.file) {
        // The file is the request's body, sent from where it lies: nothing of it is copied into memory.
        request.url.params().append({"uploadType", "media"});
        request.url.params().append({"name", object.name});
        request.content_type = object.content_type;
        request.file = std::move(object.file);
    } else {
        // 16 random bytes: a body that happens to contain the boundary is not a thing to plan for.
        std::string boundary = "poni-";
        for (const unsigned char byte : core::random_bytes(16)) boundary += std::format("{:02x}", byte);
        request.url.params().append({"uploadType", "multipart"});
        request.content_type = "multipart/related; boundary=" + boundary;
        request.body = multipart_upload_body(object, boundary);
        // A page is small, but the request holds it now, not the object as well.
        std::string().swap(object.body);
    }
    const auto response = co_await google::send(api_, kStorageApi, std::move(request));
    if (!response) co_return std::unexpected(response.error());
    co_return Result<void>{};
}

std::string GcsStore::base_url() const {
    // The bucket's objects are under it: the address ends in a slash.
    return std::string(google::url("https://storage.googleapis.com", {bucket_, ""}).buffer());
}

asio::awaitable<Result<void>> FolderStore::put(Object object) {
    if (!valid_object_name(object.name)) co_return std::unexpected(refused("the object's name is not allowed"));
    const auto path = folder_ / object.name;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) co_return std::unexpected(refused(std::format("the folder can't be created: {}", ec.message())));
    const auto content = object.bytes();
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    file.close();
    if (!file) co_return std::unexpected(refused("the file can't be written"));
    co_return Result<void>{};
}

std::string FolderStore::base_url() const {
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(folder_, ec);
    return "file://" + (ec ? folder_ : absolute).generic_string() + "/";
}

}  // namespace poni::storage
