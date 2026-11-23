// Where the engine puts files for others to read: a report's page (D-085,
// D-104) and the photo or video of a local post (D-088), in the public
// bucket, under names nobody can guess. A store writes an object, copies a
// report's page to its archive name and deletes the original (D-104 point
// 6), and does nothing else: there is no read and no list. `copy` and
// `remove` take a report's names and no other, in every store, so that
// nothing else in a bucket can be copied or deleted through them; the
// outbound client holds the same two requests to the configured bucket
// (http::is_report_page_copy_url, http::is_report_page_delete_url).
//
// A copy at Cloud Storage is one call: inside one bucket, without a change
// of storage class, Google finishes a rewrite in the one request. An answer
// that says it did not (`done: false`, with a `rewriteToken`) is a failure,
// and the token is not followed: that would put an opaque query on an address
// the outbound client takes with no query at all, and a later call starts the
// copy again, which replaces its destination.
//
// GcsStore talks to Cloud Storage; FolderStore writes into a local folder,
// for development and tests, so that nothing leaves the machine.
#pragma once

#include "connectors/google/auth.hpp"
#include "core/mapped_file.hpp"

#include <boost/asio/awaitable.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace poni::storage {

using google::ApiError;
using google::Result;

struct Object {
    // The object's name: segments of letters, digits, '-', '_' and '.',
    // separated by '/'. Anything else is refused.
    std::string name;
    std::string content_type;
    std::string body;
    // A file on disk instead of `body`: a post's photo or video, sent as it
    // lies and never read into memory (D-101). It goes up as itself, without
    // metadata of its own, so Cloud Storage serves it with its default for a
    // public object, an hour in caches: what an upload is meant to have.
    std::shared_ptr<const core::MappedFile> file;
    // The Cache-Control a `body` is served with. Cloud Storage caches a
    // public object for an hour by default, which would keep a revoked page readable.
    std::string cache_control = "no-store";

    std::string_view bytes() const { return file ? file->bytes() : std::string_view(body); }
};

class ObjectStore {
public:
    virtual ~ObjectStore() = default;
    // Creates the object, or replaces the one with that name.
    virtual boost::asio::awaitable<Result<void>> put(Object object) = 0;
    // Copies a report's page to its archive name in the same store, replacing what is there.
    // ApiError::Kind::not_found: there is no such page.
    virtual boost::asio::awaitable<Result<void>> copy(std::string from, std::string to) = 0;
    // Deletes a report's page. ApiError::Kind::not_found: there is none.
    virtual boost::asio::awaitable<Result<void>> remove(std::string name) = 0;
    // Where its objects are read: an object's address is this and its name.
    virtual std::string base_url() const = 0;
};

// True for a name put() accepts.
bool valid_object_name(std::string_view name);

// A report's page, the only name remove() takes and the only one copy()
// copies from: "r/", the 43 characters of a token (A-Z a-z 0-9 - _), ".",
// and an extension of 1 to 8 characters of a-z 0-9.
bool is_report_page(std::string_view name);
// A report's archive, the only name copy() copies to: the same with "a/".
bool is_report_archive(std::string_view name);

// Rate limiter key.
inline constexpr std::string_view kStorageApi = "google.storage";

class GcsStore final : public ObjectStore {
public:
    GcsStore(google::Api api, std::string bucket) : api_(api), bucket_(std::move(bucket)) {}

    boost::asio::awaitable<Result<void>> put(Object object) override;
    boost::asio::awaitable<Result<void>> copy(std::string from, std::string to) override;
    boost::asio::awaitable<Result<void>> remove(std::string name) override;
    std::string base_url() const override;

private:
    google::Api api_;
    std::string bucket_;
};

// The files are small and this is for one developer's machine: it writes on
// the calling thread.
class FolderStore final : public ObjectStore {
public:
    explicit FolderStore(std::filesystem::path folder) : folder_(std::move(folder)) {}

    boost::asio::awaitable<Result<void>> put(Object object) override;
    boost::asio::awaitable<Result<void>> copy(std::string from, std::string to) override;
    boost::asio::awaitable<Result<void>> remove(std::string name) override;
    std::string base_url() const override;

private:
    std::filesystem::path folder_;
};

// A setting names a bucket ("xvisionmedia-gbp-public") or, with a slash in
// it, a folder ("/tmp/poni-public", "./public").
bool is_folder_setting(std::string_view setting);

// The multipart/related body of a Cloud Storage upload: the object's
// metadata as JSON, then its content. `boundary` must not occur in the body.
std::string multipart_upload_body(const Object& object, std::string_view boundary);

}  // namespace poni::storage
