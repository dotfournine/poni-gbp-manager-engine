#include "jobs/notifications.hpp"

#include "core/log.hpp"
#include "db/repo_auth.hpp"
#include "db/repo_notifications.hpp"

#include <format>
#include <stdexcept>

namespace poni::jobs {

namespace asio = boost::asio;

namespace {

// A function, not a lambda inside deliver(): a coroutine lambda's captures must outlive it.
asio::awaitable<void> set_status(db::Database& db, const core::Clock& clock, std::int64_t delivery_id, std::string status,
                                 std::string reason) {
    co_await db.tx([&, now = clock.now()](pqxx::work& tx) {
        repo::mark_delivery(tx, delivery_id, status, reason, now);
    });
}

}  // namespace

asio::awaitable<void> Notifier::deliver(std::int64_t delivery_id, bool last_attempt) {
    const auto delivery = co_await db_.tx([delivery_id, now = clock_.now()](pqxx::work& tx) { return repo::begin_delivery(tx, delivery_id, now); });
    if (!delivery) co_return;  // sent, failed or not sent already

    const auto channel = channels_.find(delivery->channel);
    std::string not_sent;
    if (delivery->recipient.empty()) {
        not_sent = "there is no recipient";
    } else if (!delivery->sending_enabled) {
        not_sent = "email sending is switched off";
    } else if (channel == channels_.end()) {
        not_sent = std::format("the engine has no '{}' channel", delivery->channel);
    }
    if (!not_sent.empty()) {
        co_await set_status(db_, clock_, delivery_id, "not_sent", not_sent);
        log::info("notification not sent", {{"delivery_id", delivery_id}, {"reason", not_sent}});
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

    const auto& error = sent.error();
    if (error.kind == notify::SendError::Kind::not_configured) {
        std::string reason = "no sending account is signed in: " + error.message;
        co_await set_status(db_, clock_, delivery_id, "not_sent", std::move(reason));
        log::info("notification not sent", {{"delivery_id", delivery_id}, {"reason", error.message}});
        co_return;
    }
    const bool retry = error.kind == notify::SendError::Kind::transient && !last_attempt;
    co_await db_.tx([&, now = clock_.now()](pqxx::work& tx) {
        repo::mark_delivery(tx, delivery_id, retry ? "pending" : "failed", error.message, now);
        repo::mark_integration_error(tx, repo::kEmailStatus, error.message, now);
    });
    log::warn("notification failed", {{"delivery_id", delivery_id}, {"error", error.message}, {"will_retry", retry}});
    if (retry) throw std::runtime_error(std::format("delivery {}: {}", delivery_id, error.message));
}

void install(Registry& registry, Notifier& notifier) {
    registry.add(std::string(repo::kDeliverNotificationKind), [&notifier](const Job& job) -> asio::awaitable<void> {
        co_await notifier.deliver(job.payload_id("delivery_id"), job.last_attempt());
    });
}

}  // namespace poni::jobs
