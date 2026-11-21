// Sends what the delivery log holds (FR-7). One job per delivery,
// `notifications.deliver`: it hands the message to the delivery's channel
// and records the outcome. A failure that may pass is retried by the job
// queue's backoff; on the last attempt the delivery is marked failed.
//
// Nothing leaves while email sending is switched off (system_settings) or no
// sending account is signed in: the delivery is then recorded as not sent,
// with the reason, and is not sent later either. An alert that arrives hours
// late, out of nowhere, is worse than one that is visibly missing.
#pragma once

#include "connectors/notify/channel.hpp"
#include "core/clock.hpp"
#include "db/database.hpp"
#include "jobs/registry.hpp"

#include <boost/asio/awaitable.hpp>

#include <cstdint>
#include <map>
#include <string>

namespace poni::jobs {

struct NotifierOptions {
    // The dashboard's address, put in front of a delivery's link.
    std::string public_url;
};

class Notifier {
public:
    Notifier(db::Database& db, const core::Clock& clock, NotifierOptions options)
        : db_(db), clock_(clock), options_(std::move(options)) {}

    // `channel` must outlive the notifier.
    void add_channel(notify::Channel& channel) { channels_[std::string(channel.name())] = &channel; }

    // Throws to be retried when the channel failed in a way that may pass.
    boost::asio::awaitable<void> deliver(std::int64_t delivery_id, bool last_attempt);

private:
    db::Database& db_;
    const core::Clock& clock_;
    NotifierOptions options_;
    std::map<std::string, notify::Channel*> channels_;
};

void install(Registry& registry, Notifier& notifier);

}  // namespace poni::jobs
