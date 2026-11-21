#include "jobs/notifications.hpp"

#include "core/log.hpp"
#include "db/repo_auth.hpp"
#include "db/repo_devices.hpp"
#include "db/repo_notifications.hpp"
#include "domain/push.hpp"

#include <format>
#include <stdexcept>

namespace poni::jobs {

namespace asio = boost::asio;

namespace {

// Functions, not lambdas inside deliver(): a coroutine lambda's captures must outlive it.

// The engine did not try, and won't later: an alert that arrives late, out of nowhere, is worse than one visibly missing.
asio::awaitable<void> not_sent(db::Database& db, const core::Clock& clock, std::int64_t delivery_id, std::string reason) {
    co_await db.tx([&, now = clock.now()](pqxx::work& tx) { repo::mark_delivery(tx, delivery_id, "not_sent", reason, now); });
    log::info("notification not sent", {{"delivery_id", delivery_id}, {"reason", reason}});
}

// The channel failed: pending again, and a throw for the job's backoff, while the failure may pass; failed otherwise.
asio::awaitable<void> channel_failed(db::Database& db, const core::Clock& clock, std::int64_t delivery_id, std::string_view integration,
                                     notify::SendError error, bool last_attempt) {
    const bool retry = error.kind == notify::SendError::Kind::transient && !last_attempt;
    co_await db.tx([&, now = clock.now()](pqxx::work& tx) {
        repo::mark_delivery(tx, delivery_id, retry ? "pending" : "failed", error.message, now);
        repo::mark_integration_error(tx, integration, error.message, now);
    });
    log::warn("notification failed", {{"delivery_id", delivery_id}, {"error", error.message}, {"will_retry", retry}});
    if (retry) throw std::runtime_error(std::format("delivery {}: {}", delivery_id, error.message));
}

}  // namespace

asio::awaitable<void> Notifier::deliver(std::int64_t delivery_id, bool last_attempt) {
    const auto delivery = co_await db_.tx([delivery_id, now = clock_.now()](pqxx::work& tx) { return repo::begin_delivery(tx, delivery_id, now); });
    if (!delivery) co_return;  // sent, failed or not sent already
    if (delivery->channel == repo::kPushChannel) {
        co_await deliver_push(*delivery, last_attempt);
        co_return;
    }

    const auto channel = channels_.find(delivery->channel);
    std::string reason;
    if (delivery->recipient.empty()) {
        reason = "there is no recipient";
    } else if (!delivery->sending_enabled) {
        reason = "email sending is switched off";
    } else if (channel == channels_.end()) {
        reason = std::format("the engine has no '{}' channel", delivery->channel);
    }
    if (!reason.empty()) {
        co_await not_sent(db_, clock_, delivery_id, std::move(reason));
        co_return;
    }

    std::string body = delivery->body;
    if (!delivery->link_path.empty()) body += std::format("\n\nOpen in the dashboard: {}{}\n", options_.public_url, delivery->link_path);
    notify::Message message{.recipient = delivery->recipient, .subject = delivery->subject, .body = std::move(body)};
    const auto sent = co_await channel->second->send(std::move(message));

    if (sent) {
        co_await db_.tx([delivery_id, now = clock_.now()](pqxx::work& tx) {
            repo::mark_delivery_sent(tx, delivery_id, now);
            repo::mark_integration_ok(tx, repo::kEmailStatus, now);
        });
        // The address is staff's own; the subject may quote nothing from a review, the body may, so neither is logged.
        log::info("notification sent", {{"delivery_id", delivery_id}, {"channel", delivery->channel}});
        co_return;
    }
    if (sent.error().kind == notify::SendError::Kind::not_configured) {
        co_await not_sent(db_, clock_, delivery_id, "no sending account is signed in: " + sent.error().message);
        co_return;
    }
    co_await channel_failed(db_, clock_, delivery_id, repo::kEmailStatus, sent.error(), last_attempt);
}

asio::awaitable<void> Notifier::deliver_push(repo::PendingDelivery delivery, bool last_attempt) {
    const auto id = delivery.id;
    std::vector<repo::PushDevice> devices;
    if (push_ && !delivery.recipient.empty()) {
        devices = co_await db_.tx([&, now = clock_.now()](pqxx::work& tx) {
            return repo::push_devices(tx, delivery.recipient, now, options_.lifetimes);
        });
    }
    if (delivery.recipient.empty() || !push_ || devices.empty()) {
        co_await not_sent(db_, clock_, id,
                          delivery.recipient.empty() ? "there is no recipient"
                          : !push_                   ? "this engine has no APNs settings"
                                                     : "the person has no device signed in to the phone app");
        co_return;
    }

    // Built from the subject and the ids only: the body may quote a review.
    const auto payload = domain::push_payload({.event_type = delivery.event_type,
                                               .subject = delivery.subject,
                                               .escalation_id = delivery.escalation_id,
                                               .review_id = delivery.review_id,
                                               .location_id = delivery.location_id,
                                               .local_post_id = delivery.local_post_id});
    const auto collapse_id = domain::push_collapse_id(delivery.event_key);
    std::vector<notify::Push> pushes;
    for (const auto& device : devices) pushes.push_back({.device_token = device.apns_token, .payload = payload, .collapse_id = collapse_id});
    const auto answers = co_await push_->send(std::move(pushes));
    if (!answers) {
        co_await channel_failed(db_, clock_, id, repo::kPushStatus, answers.error(), last_attempt);
        co_return;
    }

    const auto outcome = domain::push_outcome(*answers, last_attempt);
    co_await db_.tx([&, now = clock_.now()](pqxx::work& tx) {
        for (std::size_t i = 0; i < answers->size(); ++i) {
            if ((*answers)[i].kind == notify::PushAnswer::Kind::device_gone) repo::drop_device(tx, devices[i].id, devices[i].apns_token);
        }
        if (outcome.status == "sent") {
            repo::mark_delivery_sent(tx, id, now, outcome.summary);
            if (outcome.sender_problem.empty()) repo::mark_integration_ok(tx, repo::kPushStatus, now);
        } else {
            repo::mark_delivery(tx, id, outcome.status, outcome.summary, now);
        }
        if (outcome.sender_problem.empty()) return;
        repo::mark_integration_error(tx, repo::kPushStatus, "Apple refused the APNs settings: " + outcome.sender_problem, now);
        // Once a day: the same key adds no delivery that exists.
        repo::emit(tx,
                   {.type = std::string(repo::kEventSystemProblem),
                    .key = std::format("push_refused:{:%F}", now),
                    .client_id = std::nullopt,
                    .location_id = std::nullopt,
                    .review_id = std::nullopt,
                    .subject = "Apple refused the engine's push settings",
                    .body = std::format("Apple's push service refused a push with \"{}\". The APNs key, its id, the team id or the bundle "
                                        "id in the engine's settings is wrong, or the key was revoked. No push reaches the phone app "
                                        "until the settings are fixed.",
                                        outcome.sender_problem),
                    .link_path = "/#/notifications"},
                   now);
    });
    // Counts only: a device token is in no log.
    log::info("push delivered",
              {{"delivery_id", id}, {"devices", static_cast<std::int64_t>(devices.size())}, {"status", std::string(outcome.status)}});
    if (outcome.status == "pending") throw std::runtime_error(std::format("delivery {}: {}", id, outcome.summary));
}

void install(Registry& registry, Notifier& notifier) {
    registry.add(std::string(repo::kDeliverNotificationKind), [&notifier](const Job& job) -> asio::awaitable<void> {
        co_await notifier.deliver(job.payload_id("delivery_id"), job.last_attempt());
    });
}

}  // namespace poni::jobs
