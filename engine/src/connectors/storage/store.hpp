// Where the engine puts files for others to read: a shared report's page
// (D-085) and the photo or video of a local post (D-088), in the public
// bucket, under names nobody can guess. Writing an object is the only operation: there is no
// read, no list and no delete. A share link is revoked by writing another
// page over it, and old objects are removed by the bucket's lifecycle rule,
// not by the engine.
//
// GcsStore talks to Cloud Storage; FolderStore writes into a local folder,
// for development and tests, so that nothing leaves the machine.
#pragma once

#include "connectors/google/auth.hpp"

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
    // The Cache-Control the object is served with. Cloud Storage caches a
    // public object for an hour by default, which would keep a revoked page readable.
    std::string cache_control = "no-store";
};

class ObjectStore {
public:
    virtual ~ObjectStore() = default;
    // Creates the object, or replaces the one with that name.
    virtual boost::asio::awaitable<Result<void>> put(Object object) = 0;
    // Where its objects are read: an object's address is this and its name.
    virtual std::string base_url() const = 0;
};

// True for a name put() accepts.
bool valid_object_name(std::string_view name);

// Rate limiter key.
inline constexpr std::string_view kStorageApi = "google.storage";

class GcsStore final : public ObjectStore {
public:
    GcsStore(google::Api api, std::string bucket) : api_(api), bucket_(std::move(bucket)) {}

    boost::asio::awaitable<Result<void>> put(Object object) override;
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
