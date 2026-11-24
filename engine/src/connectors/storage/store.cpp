#include "connectors/storage/store.hpp"

#include "core/crypto.hpp"

#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <system_error>

namespace poni::storage {

namespace asio = boost::asio;
using namespace std::chrono_literals;

namespace {

ApiError refused(std::string message) { return {ApiError::Kind::other, 0, std::move(message)}; }

// The slowest connection to Cloud Storage an upload is still given time for.
constexpr std::size_t kSlowestUploadBytesPerSecond = 256 * 1024;

// `prefix`, a token of 43 characters (32 random bytes in base64url), '.', and
// an extension of 1 to 8 lower-case letters and digits.
bool is_report_object(std::string_view name, std::string_view prefix) {
    constexpr std::size_t kTokenLength = 43;
    if (!name.starts_with(prefix)) return false;
    name.remove_prefix(prefix.size());
    if (name.size() < kTokenLength + 2 || name.size() > kTokenLength + 9 || name[kTokenLength] != '.') return false;
    const auto token = name.substr(0, kTokenLength);
    const auto extension = name.substr(kTokenLength + 1);
    const auto lower_or_digit = [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    return std::ranges::all_of(token, [&](unsigned char c) { return lower_or_digit(c) || (c >= 'A' && c <= 'Z') || c == '-' || c == '_'; }) &&
           std::ranges::all_of(extension, lower_or_digit);
}

// A page and its archive name, in the same format: the archive keeps the page's extension.
bool is_archive_of(std::string_view page, std::string_view archive) {
    return is_report_page(page) && is_report_archive(archive) && page.substr(page.find('.')) == archive.substr(archive.find('.'));
}

// Google's words about an object may quote its name ("No such object: <bucket>/r/..."), and a report's
// object name is a secret (D-104): an answer of Google's keeps its kind and status and loses its text.
// An error without an answer and an answer that can't be read (a 2xx) name no object. A token that could not be
// had keeps its endpoint's status, if it had one (400, 404), and is cut to "HTTP n" too, which is harmless.
ApiError without_google_text(ApiError error) {
    if (error.http_status >= 300) error.message = std::format("HTTP {}", error.http_status);
    return error;
}

ApiError no_such_file() { return {ApiError::Kind::not_found, 404, "no such file"}; }

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

bool is_report_page(std::string_view name) { return is_report_object(name, "r/"); }

bool is_report_archive(std::string_view name) { return is_report_object(name, "a/"); }

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

asio::awaitable<Result<void>> GcsStore::copy(std::string from, std::string to) {
    if (!is_archive_of(from, to)) co_return std::unexpected(refused("the object's name is not allowed"));
    http::ClientRequest request;
    request.method = http::ClientMethod::post;
    request.url = google::url("https://storage.googleapis.com", {"storage", "v1", "b", bucket_, "o", from, "rewriteTo", "b", bucket_, "o", to});
    // As an upload's base; Google asks for at least 30 seconds.
    request.timeout = 60s;
    const auto answer = co_await google::fetch_json(api_, kStorageApi, std::move(request));
    if (!answer) co_return std::unexpected(without_google_text(answer.error()));
    // One call or none: a rewriteToken is not followed (store.hpp says why).
    const auto* done = answer->is_object() ? answer->get_object().if_contains("done") : nullptr;
    if (!done || !done->is_bool() || !done->get_bool()) {
        co_return std::unexpected(ApiError{ApiError::Kind::unavailable, 200, "Cloud Storage did not finish the copy in one call"});
    }
    co_return Result<void>{};
}

asio::awaitable<Result<void>> GcsStore::remove(std::string name) {
    if (!is_report_page(name)) co_return std::unexpected(refused("the object's name is not allowed"));
    http::ClientRequest request;
    request.method = http::ClientMethod::del;
    request.url = google::url("https://storage.googleapis.com", {"storage", "v1", "b", bucket_, "o", name});
    const auto response = co_await google::send(api_, kStorageApi, std::move(request));
    if (!response) co_return std::unexpected(without_google_text(response.error()));
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

asio::awaitable<Result<void>> FolderStore::copy(std::string from, std::string to) {
    if (!is_archive_of(from, to)) co_return std::unexpected(refused("the object's name is not allowed"));
    const auto target = folder_ / to;
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) co_return std::unexpected(refused(std::format("the folder can't be created: {}", ec.message())));
    std::filesystem::copy_file(folder_ / from, target, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec == std::errc::no_such_file_or_directory) co_return std::unexpected(no_such_file());
    if (ec) co_return std::unexpected(refused(std::format("the page can't be copied: {}", ec.message())));
    co_return Result<void>{};
}

asio::awaitable<Result<void>> FolderStore::remove(std::string name) {
    if (!is_report_page(name)) co_return std::unexpected(refused("the object's name is not allowed"));
    std::error_code ec;
    const bool removed = std::filesystem::remove(folder_ / name, ec);
    if (ec) co_return std::unexpected(refused(std::format("the page can't be deleted: {}", ec.message())));
    if (!removed) co_return std::unexpected(no_such_file());
    co_return Result<void>{};
}

std::string FolderStore::base_url() const {
    if (!served_at_.empty()) return served_at_ + "/";
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(folder_, ec);
    return "file://" + (ec ? folder_ : absolute).generic_string() + "/";
}

}  // namespace poni::storage
