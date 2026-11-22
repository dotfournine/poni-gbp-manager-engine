// The email channel, through the Gmail API (D-026): users.messages.send as
// the sending mailbox, with an OAuth token that has the gmail.send scope and
// nothing else. The token is stored encrypted like the GBP one
// (connected on the dashboard's status page, D-079), and the mailbox is the
// account that signed in there: mail is sent from the address its
// connection named (D-100), read on every send.
//
// Messages are plain text, UTF-8. The subject and the body may carry text
// from a review, which is untrusted: both are base64-encoded into the
// message, so nothing in them can add a header or a recipient.
#pragma once

#include "connectors/google/auth.hpp"
#include "connectors/notify/channel.hpp"

#include <expected>
#include <functional>
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

// The connected mailbox's address, or why there is none (a message for the delivery log).
using MailboxAddress = std::function<boost::asio::awaitable<std::expected<std::string, std::string>>()>;

// The RFC 5322 message Gmail is given. Line breaks are removed from the
// subject, the names and the addresses. The recipient was checked when its
// rule was saved; the sender is the address Google named for the connected
// mailbox.
std::string build_mime(const Sender& from, std::string_view to, std::string_view subject, std::string_view body);

class GmailChannel final : public Channel {
public:
    // `name` is shown next to the address; `address` is asked before every message.
    GmailChannel(google::Api api, std::string name, MailboxAddress address)
        : api_(api), name_(std::move(name)), address_(std::move(address)) {}

    std::string_view name() const override { return "email"; }
    boost::asio::awaitable<std::expected<void, SendError>> send(Message message) override;

private:
    google::Api api_;
    std::string name_;
    MailboxAddress address_;
};

}  // namespace poni::notify
