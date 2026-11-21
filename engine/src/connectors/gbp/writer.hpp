// Google Business Profile, write side for reviews. It is one call wide on
// purpose: the engine can put a reply on a review (D-061). There is no
// delete and no update of anything else here, and none is to be added: the
// API reports canDelete even for a manager account (CLAUDE.md). The other
// three writes, creating, deleting and editing a local post, have an
// interface of their own (D-088, D-099).
//
// Google keeps one reply per review, so putting a reply on a review that
// has one replaces it. Callers must have checked what is there first
// (jobs/posting.hpp).
#pragma once

#include "connectors/gbp/reader.hpp"

#include <boost/asio/awaitable.hpp>

#include <string>

namespace poni::gbp {

class ReplyWriter {
public:
    virtual ~ReplyWriter() = default;

    // PUT .../reviews/{id}/reply. An ApiError means Google answered and
    // refused: nothing was posted. A transport failure throws
    // (boost::system::system_error) and says nothing about the outcome: the
    // request may have arrived, so the caller must not treat it as "not
    // posted".
    virtual boost::asio::awaitable<Result<ReviewReply>> put_reply(ReviewName name, std::string comment) = 0;
};

class HttpReplyWriter final : public ReplyWriter {
public:
    explicit HttpReplyWriter(google::Api api) : api_(api) {}

    boost::asio::awaitable<Result<ReviewReply>> put_reply(ReviewName name, std::string comment) override;

private:
    google::Api api_;
};

}  // namespace poni::gbp
