// Why a request was not carried out, in the API's own terms: the one failure
// type of the repositories and of the services a handler calls. A function
// that may refuse returns repo::Result<T>, and states the limit and its
// message where it checks it:
//
//     if (rows.empty()) return not_found("client");
//     if (name.size() > 200) return invalid("A client needs a name of 1 to 200 characters.");
//     if (taken) return conflict("name_taken", "Another client already has this name.");
//
// http::respond (http/api_support.hpp) turns a Failure into the answer; the
// code and the message are what the dashboard gets, byte for byte.
#pragma once

#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace poni::repo {

struct Failure {
    enum class Kind {
        not_found,    // 404
        invalid,      // 400: the request itself is wrong
        conflict,     // 409: the request is fine, what it meets is not
        unavailable,  // 502: something outside the engine (a model, a bucket) gave no usable answer
        precondition_required,  // 428: a change of an existing thing came without If-Match (D-108)
        precondition_failed,    // 412: the If-Match is not the ETag of what is stored now (D-108)
    };
    Kind kind;
    std::string code;     // the API's error code
    std::string message;  // for the person who asked
};

template <class T>
using Result = std::expected<T, Failure>;

// "No such client.", with the code not_found.
inline std::unexpected<Failure> not_found(std::string_view what) {
    return std::unexpected(Failure{Failure::Kind::not_found, "not_found", std::format("No such {}.", what)});
}
// The code is invalid_request; the message says what is wrong.
inline std::unexpected<Failure> invalid(std::string message) {
    return std::unexpected(Failure{Failure::Kind::invalid, "invalid_request", std::move(message)});
}
inline std::unexpected<Failure> conflict(std::string code, std::string message) {
    return std::unexpected(Failure{Failure::Kind::conflict, std::move(code), std::move(message)});
}
inline std::unexpected<Failure> unavailable(std::string code, std::string message) {
    return std::unexpected(Failure{Failure::Kind::unavailable, std::move(code), std::move(message)});
}
// The two refusals of a change guarded by If-Match (D-108, http::Api::change_if_match).
inline std::unexpected<Failure> precondition_required() {
    return std::unexpected(Failure{Failure::Kind::precondition_required, "precondition_required",
                                   "This change needs the If-Match header with the ETag of what you read."});
}
inline std::unexpected<Failure> precondition_failed() {
    return std::unexpected(Failure{Failure::Kind::precondition_failed, "precondition_failed",
                                   "What you changed was changed by someone else since you read it. Read it again and redo your change."});
}

}  // namespace poni::repo
