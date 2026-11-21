// The email channel, through the Gmail API (D-026): users.messages.send as
// the sending mailbox, with an OAuth token that has the gmail.send scope and
// nothing else. The token is stored encrypted like the GBP one
// (connected on the dashboard's status page, D-079).
//
// Messages are plain text, UTF-8. The subject and the body may carry text
// from a review, which is untrusted: both are base64-encoded into the
// message, so nothing in them can add a header or a recipient.
#pragma once

#include "connectors/google/auth.hpp"
#include "connectors/notify/channel.hpp"

#include <string>
#include <string_view>

namespace poni::notify {

// Rate limiter key.
inline constexpr std::string_view kGmailApi = "gmail";
inline constexpr std::string_view kGmailSendScope = "https://www.googleapis.com/auth/gmail.send";

struct Sender {
    std::string address;  // "host@xvisionmediaus.com"
    std::string name;     // shown next to the address
};

// The RFC 5322 message Gmail is given. Line breaks are removed from the
// subject and the names; the caller has checked the addresses.
std::string build_mime(const Sender& from, std::string_view to, std::string_view subject, std::string_view body);

class GmailChannel final : public Channel {
public:
    GmailChannel(google::Api api, Sender sender) : api_(api), sender_(std::move(sender)) {}

    std::string_view name() const override { return "email"; }
    boost::asio::awaitable<std::expected<void, SendError>> send(Message message) override;

private:
    google::Api api_;
    Sender sender_;
};

}  // namespace poni::notify
