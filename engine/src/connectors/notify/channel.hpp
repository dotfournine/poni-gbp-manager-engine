// A notification channel (FR-7, D-097): one way of getting a message to a
// person. Email is the one behind this interface; anything like it (a chat
// webhook, SMS) is another implementation and a row in the rules, with no
// change to the code that raises events or to the delivery log. Push to the
// phone app has an interface of its own (notify::PushSender, apns.hpp):
// one delivery reaches each of a person's devices, and each answers apart.
#pragma once

#include <boost/asio/awaitable.hpp>

#include <expected>
#include <string>
#include <string_view>

namespace poni::notify {

struct Message {
    std::string recipient;  // an address the channel understands
    std::string subject;
    // Plain text. It may quote a review, so a channel must never treat it as markup.
    std::string body;
};

struct SendError {
    enum class Kind {
        not_configured,  // the channel has no sender signed in: nothing was tried
        transient,       // worth another attempt (network, rate limit, 5xx)
        permanent,       // the service refused this message or this sender
    };
    Kind kind = Kind::permanent;
    std::string message;  // never contains a secret
};

class Channel {
public:
    virtual ~Channel() = default;
    // The name rules and deliveries carry ("email").
    virtual std::string_view name() const = 0;
    virtual boost::asio::awaitable<std::expected<void, SendError>> send(Message message) = 0;
};

}  // namespace poni::notify
