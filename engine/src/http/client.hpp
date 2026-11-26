// Outbound HTTP(S) client for the connectors. The interface keeps Beast and
// OpenSSL inside client.cpp and lets tests replace the network with a fake.
//
// What the engine can do to the outside world is limited here, in code
// (D-042, D-061, D-088, D-099, D-104). The engine changes a Google Business Profile
// with four requests, each on the one address shape of the v4 API
// (mybusiness.googleapis.com) that Google has for it:
//
//   PUT     .../accounts/{a}/locations/{l}/reviews/{r}/reply   a reply to a review
//   POST    .../accounts/{a}/locations/{l}/localPosts          a new local post
//   DELETE  .../accounts/{a}/locations/{l}/localPosts/{p}      that local post, and nothing else
//   PATCH   .../accounts/{a}/locations/{l}/localPosts/{p}?updateMask=<fields>
//                                                              an edit of that local post, in the fields named
//
// In its own public bucket at Cloud Storage (storage.googleapis.com) the
// engine writes objects, and withdraws a report's page by copying it to its
// archive name and deleting the original (D-104):
//
//   POST    /upload/storage/v1/b/{bucket}/o?uploadType=...     an upload, to any bucket
//   POST    /storage/v1/b/{public}/o/r%2F{page}/rewriteTo/b/{public}/o/a%2F{archive}
//                                                              the copy of a report's page to its archive
//   DELETE  /storage/v1/b/{public}/o/r%2F{page}                that page, and nothing else
//
// where {public} is the configured public bucket, which the client is given
// at its start: without one, neither the copy nor the delete is sent.
//
// What the client enforces, before a name is resolved: a PUT goes nowhere
// but to a reply, a DELETE nowhere but to a local post or, at Cloud Storage,
// a report's page in the public bucket, and a PATCH nowhere but to a local
// post with its mask, on any host. A POST to the v4 host is refused unless it
// is the create above or the read-only batchGetReviews, and a POST to Cloud
// Storage unless it is an upload or the copy above. A POST to any other host
// is not limited here: those are the engine's ordinary calls (OAuth tokens,
// Pub/Sub, Gmail, the models), and that the engine sends no other POST to a
// Google service is a rule for the code that calls this client (CLAUDE.md).
//
// An address that changes something is taken only as the engine itself
// writes it: the ids are letters, digits, '-' and '_', and the address must
// equal the one built from its own parts, so what is checked is what is
// sent. Only the PATCH has a query, and it is one parameter: `updateMask=`
// and one or more of six field names (summary, callToAction, event, offer,
// media, languageCode), each once, joined by commas, with nothing encoded.
// At Cloud Storage the copy and the delete are taken the same way: a
// bucket's name as Cloud Storage has them, an object's name as a report's
// page or archive is, the '/' of that name as %2F and nothing else encoded,
// and no query.
// No request names its host with other characters than letters,
// digits, '.' and '-', carries a header that would change its method or its
// host, or holds a line break in a header; a DELETE carries no body.
//
// To Apple's push service (D-097) the client sends one request and nothing
// else, on either of its two hosts, over HTTP/2 (send_h2):
//
//   POST    https://api.push.apple.com/3/device/{token}     a notification to one device
//
// with the token in lower-case hex, and the address equal to the one built
// from its parts, as for Google's writes.
//
// Against the local mock of Google (PONI_GOOGLE_MOCK_URL, D-087) the callers
// still address Google; this client sends what the mock stands in for to the
// mock instead, and refuses every other Google address. The rules above are
// checked on the Google address, before it is changed, so a write is the
// same write in both modes.
#pragma once

#include "core/mapped_file.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/url/url.hpp>

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace poni::http {

enum class ClientMethod { get, post, put, del, patch };

// True for the only address a PUT may go to:
// https://mybusiness.googleapis.com/v4/accounts/{a}/locations/{l}/reviews/{r}/reply
bool is_reply_url(const boost::urls::url& url);

// True for the address a local post is created at (POST):
// https://mybusiness.googleapis.com/v4/accounts/{a}/locations/{l}/localPosts
bool is_local_post_collection_url(const boost::urls::url& url);

// True for the address a DELETE may go to at Google Business Profile, one
// local post (the other is a report's page, is_report_page_delete_url):
// https://mybusiness.googleapis.com/v4/accounts/{a}/locations/{l}/localPosts/{p}
bool is_local_post_url(const boost::urls::url& url);

// The fields of a local post that a PATCH's mask may name (D-099), by
// Google's names. They are domain::kLocalPostEditFields, held here a second
// time on purpose: the rule for what may go out does not follow a list that
// other code widens. A static_assert holds the two equal
// (connectors/gbp/local_posts.hpp).
inline constexpr std::array<std::string_view, 6> kLocalPostUpdateFields{"summary", "callToAction", "event",
                                                                        "offer",   "media",        "languageCode"};

// True for the only address a PATCH may go to, one local post with the
// fields an edit changes (D-099):
// https://mybusiness.googleapis.com/v4/accounts/{a}/locations/{l}/localPosts/{p}?updateMask=<names>
// where <names> is one or more of kLocalPostUpdateFields, each at most once,
// joined by commas. It is the one
// address with a query that changes something, and the query is that one
// parameter, written exactly so.
bool is_local_post_update_url(const boost::urls::url& url);

// True for GBP's read that travels as a POST:
// https://mybusiness.googleapis.com/v4/accounts/{a}/locations:batchGetReviews
bool is_batch_get_reviews_url(const boost::urls::url& url);

// True when the address names the GBP v4 host in any case, with a trailing
// dot, a port or another scheme: the host whose POSTs are limited to the two
// above. (A host written with other characters is refused for every method.)
bool is_gbp_host(const boost::urls::url& url);

// True when the address names the host of Google's Admin SDK,
// admin.googleapis.com, in any case, with a trailing dot, a port or another
// scheme: the host to which the client sends one GET, the list below, and nothing else.
bool is_directory_host(const boost::urls::url& url);

// True for the only address the client sends the Admin SDK, the list of the
// account's users (D-106 point 3):
// https://admin.googleapis.com/admin/directory/v1/users?customer=my_customer&maxResults=500
// alone or followed by &pageToken=<token>, the token 1 to 2,048 unreserved
// characters and %XX escapes, as form_encode writes it.
bool is_directory_user_list_url(const boost::urls::url& url);

// True when the address names Google Places' host, places.googleapis.com,
// in any case, with a trailing dot, a port or another scheme: the host to
// which the client sends one kind of request, the one below, and nothing else.
bool is_places_host(const boost::urls::url& url);

// An id as Google's are, of accounts, locations, reviews and local posts:
// letters, digits, '-' and '_' (ASCII, whatever the locale), at most 128 of
// them. The ids the four write addresses take.
bool is_plain_id(std::string_view id);

// A place id of Google Places: 1 to 1,024 letters, digits, '-' and '_'.
// The characters of is_plain_id, without its cap of 128: Google documents
// no maximum length for a place id.
bool is_place_id(std::string_view id);

// True when the address names one of Apple's two push hosts in any case,
// with a trailing dot, a port or another scheme.
bool is_apns_host(const boost::urls::url& url);

// True for the only address the client sends to Apple:
// https://api.push.apple.com/3/device/{token}, or the same on
// api.sandbox.push.apple.com, the token as domain::is_device_token says.
bool is_apns_device_url(const boost::urls::url& url);

// True when the address names Cloud Storage's host in any case, with a
// trailing dot, a port or another scheme: the host whose POSTs and DELETEs
// are limited to the three addresses below.
bool is_storage_host(const boost::urls::url& url);

// True for an upload to a bucket, whatever its query says:
// https://storage.googleapis.com/upload/storage/v1/b/{bucket}/o?...
// where {bucket} is a bucket's name as Cloud Storage has them.
bool is_storage_upload_url(const boost::urls::url& url);

// True for the only copy the client sends Cloud Storage, a report's page to
// its archive name with the same extension, both in `public_bucket` (D-104):
// https://storage.googleapis.com/storage/v1/b/{public_bucket}/o/r%2F{token}.{ext}/rewriteTo/b/{public_bucket}/o/a%2F{token}.{ext}
// The names' shape is storage::is_report_page and storage::is_report_archive,
// held here a second time on purpose: what a store accepts may be widened,
// what goes out may not follow it. Nothing with an empty `public_bucket`.
bool is_report_page_copy_url(const boost::urls::url& url, std::string_view public_bucket);

// True for the only DELETE the client sends Cloud Storage, a report's page
// in `public_bucket` (D-104):
// https://storage.googleapis.com/storage/v1/b/{public_bucket}/o/r%2F{token}.{ext}
bool is_report_page_delete_url(const boost::urls::url& url, std::string_view public_bucket);

// The rule for a method and an address alone: why the client will not send
// to it, or nothing. `public_bucket`: the configured public bucket, the one
// a report's page is copied and deleted in; none allows neither.
std::optional<std::string_view> why_refused(ClientMethod method, const boost::urls::url& url, std::string_view public_bucket = {});

// Where the mock serves what `google` names: http://127.0.0.1:8090/mybusiness.googleapis.com/v4/...,
// the Google host as the first segment, then the same path and query. Nothing
// if `google` is an address the mock does not stand in for.
std::optional<boost::urls::url> google_mock_address(const boost::urls::url& mock, const boost::urls::url& google);

// True for an address of Google's (googleapis.com, google.com, the metadata
// server), which against the mock must either be the mock's or not be called.
bool is_google_address(const boost::urls::url& url);

struct ClientRequest {
    ClientMethod method = ClientMethod::get;
    boost::urls::url url;  // https://, or http:// for loopback and the metadata server
    std::vector<std::pair<std::string, std::string>> headers;
    std::string content_type;  // sent when the body is not empty or the method is not GET
    std::string body;
    // The body as a file on disk instead of `body`, sent from where it lies:
    // the upload of a post's photo or video to the bucket, which is never
    // read into memory (D-101). Only a POST carries one, only over HTTP/1.1.
    std::shared_ptr<const core::MappedFile> file;
    // Applied to each stage (connect, TLS handshake, write, read) separately.
    std::chrono::seconds timeout{30};
};

struct ClientResponse {
    unsigned status = 0;
    std::string body;
    bool ok() const { return status >= 200 && status < 300; }
};

// True for the only request the client sends Google Places, a place's time
// zone (D-109): a GET of https://places.googleapis.com/v1/places/{id}, the id
// as is_place_id says, no query, and the header X-Goog-FieldMask exactly once
// and exactly "timeZone". The key travels in X-Goog-Api-Key, never in the address.
bool is_place_time_zone_request(const ClientRequest& request);

// Why the client will not send this request, or nothing if it may be sent:
// the rule above for its method and address, and what its headers and body
// may not be. NetClient asks before it resolves a name; a test's stand-in
// for the network asks too, so a connector that builds a forbidden request
// fails its test the way it would fail in the engine.
std::optional<std::string_view> why_refused(const ClientRequest& request, std::string_view public_bucket = {});

class Client {
public:
    virtual ~Client() = default;
    // Throws boost::system::system_error on transport or TLS failure, and
    // std::invalid_argument for a request the client refuses to send. HTTP
    // error statuses are returned, not thrown.
    virtual boost::asio::awaitable<ClientResponse> send(ClientRequest request) = 0;

    // HTTP/2, for Apple's push service only (D-097): the requests one after
    // another on one connection to their one host, and their answers in the
    // same order. Every request is checked as send() checks it and must be
    // the POST to one device, and all are refused if one is not. Throws as
    // send() does, also when the connection fails after some answers: then
    // none is returned.
    virtual boost::asio::awaitable<std::vector<ClientResponse>> send_h2(std::vector<ClientRequest> requests) = 0;
};

// One connection per request, or per send_h2. Polling a handful of Google
// endpoints every few minutes doesn't justify a connection pool yet, nor do
// a few pushes an hour.
class NetClient final : public Client {
public:
    // `tls` must outlive the client (core::make_tls_client_context()).
    // `google_mock`: PONI_GOOGLE_MOCK_URL, already checked to be loopback.
    // `public_bucket`: PONI_PUBLIC_BUCKET when it names a bucket, empty for a
    // folder or none; the one bucket a report's page is copied and deleted in.
    explicit NetClient(boost::asio::ssl::context& tls, std::optional<boost::urls::url> google_mock = std::nullopt,
                       std::string public_bucket = {})
        : tls_(tls), google_mock_(std::move(google_mock)), public_bucket_(std::move(public_bucket)) {}
    boost::asio::awaitable<ClientResponse> send(ClientRequest request) override;
    boost::asio::awaitable<std::vector<ClientResponse>> send_h2(std::vector<ClientRequest> requests) override;

private:
    boost::asio::ssl::context& tls_;
    std::optional<boost::urls::url> google_mock_;
    std::string public_bucket_;
};

// application/x-www-form-urlencoded encoding of one value.
std::string form_encode(std::string_view value);

}  // namespace poni::http
