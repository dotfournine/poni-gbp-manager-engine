#include "db/repo_notifications.hpp"

#include "core/text.hpp"
#include "db/database.hpp"
#include "db/repo_auth.hpp"
#include "jobs/queue.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value_to.hpp>

#include <algorithm>
#include <format>

namespace poni::repo {

using db::iso;
using db::to_timestamptz;

namespace {

using OptString = std::optional<std::string>;
using OptId = std::optional<std::int64_t>;

// A failing email service is retried for about an hour (30 s doubling).
constexpr int kDeliveryAttempts = 8;

std::optional<std::int64_t> insert_delivery(pqxx::transaction_base& tx, const NotificationEvent& event, OptId rule_id,
                                            std::string_view channel, std::string_view recipient, std::string_view status,
                                            const OptString& reason, const std::string& now) {
    const auto rows = tx.exec(
        R"sql(
        INSERT INTO notification_deliveries (event_type, event_key, client_id, location_id, review_id, rule_id, channel,
                                             recipient, subject, body, link_path, status, status_reason, created_at, updated_at)
        VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14::timestamptz, $14::timestamptz)
        ON CONFLICT (event_key, channel, recipient) DO NOTHING
        RETURNING id)sql",
        pqxx::params{event.type, event.key, event.client_id, event.location_id, event.review_id, rule_id, std::string(channel),
                     std::string(recipient), event.subject, event.body, event.link_path, std::string(status), reason, now});
    if (rows.empty()) return std::nullopt;
    return rows[0][0].as<std::int64_t>();
}

void enqueue_delivery(pqxx::transaction_base& tx, std::int64_t delivery_id, core::TimePoint now) {
    jobs::sql::enqueue(tx,
                       {.kind = std::string(kDeliverNotificationKind),
                        .payload = boost::json::object{{"delivery_id", delivery_id}},
                        .run_at = std::nullopt,
                        .idempotency_key = std::to_string(delivery_id),
                        .max_attempts = kDeliveryAttempts},
                       now);
}

std::string rule_select() {
    return std::format("SELECT n.id, n.event_type, n.client_id, c.name AS client_name, n.channel, n.recipients, n.enabled, "
                       "{} AS created_at, {} AS updated_at "
                       "FROM notification_rules n LEFT JOIN clients c ON c.id = n.client_id",
                       iso("n.created_at"), iso("n.updated_at"));
}

boost::json::object rule_json(const model::NotificationRule& rule) {
    return {{"event_type", rule.event_type},
            {"client_id", rule.client_id ? boost::json::value(*rule.client_id) : boost::json::value(nullptr)},
            {"recipients", boost::json::value_from(rule.recipients)},
            {"enabled", rule.enabled}};
}

// Trimmed, lower-cased, without repeats; nothing if any address is not valid.
std::optional<std::vector<std::string>> clean_recipients(const std::vector<std::string>& recipients) {
    std::vector<std::string> out;
    for (const auto& raw : recipients) {
        // Only the ends are trimmed: a space inside an address is a mistake to report, not to repair.
        std::string address(text::trim(raw));
        for (auto& c : address) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        if (!valid_email(address)) return std::nullopt;
        if (!std::ranges::contains(out, address)) out.push_back(std::move(address));
    }
    if (out.empty() || out.size() > 20) return std::nullopt;
    return out;
}

std::string delivery_select() {
    return std::format("SELECT d.id, d.event_type, d.client_id, c.name AS client_name, d.location_id, d.review_id, "
                       "d.channel, d.recipient, d.subject, d.body, d.status, d.status_reason, d.attempts, {} AS created_at, "
                       "{} AS sent_at "
                       "FROM notification_deliveries d LEFT JOIN clients c ON c.id = d.client_id",
                       iso("d.created_at"), iso("d.sent_at"));
}

}  // namespace

bool valid_email(std::string_view address) {
    if (address.size() < 3 || address.size() > 254) return false;
    const auto at = address.find('@');
    if (at == std::string_view::npos || at == 0 || address.find('@', at + 1) != std::string_view::npos) return false;
    const auto domain = address.substr(at + 1);
    const auto dot = domain.find('.');
    if (dot == std::string_view::npos || dot == 0 || domain.back() == '.' || domain.contains("..")) return false;
    for (const char c : address) {
        const auto u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u >= 0x7F) return false;  // no spaces, control characters or non-ASCII
        if (c == '<' || c == '>' || c == ',' || c == ';' || c == '"' || c == '(' || c == ')' || c == '\\') return false;
    }
    return true;
}

int emit(pqxx::transaction_base& tx, const NotificationEvent& event, core::TimePoint now) {
    const auto now_text = to_timestamptz(now);
    // A client's own rule and the rule for every client both apply.
    const auto rules = tx.exec("SELECT id, channel, recipients::text FROM notification_rules "
                               "WHERE event_type = $1 AND enabled AND (client_id IS NULL OR client_id = $2) ORDER BY id",
                               pqxx::params{event.type, event.client_id});
    int created = 0;
    bool any_recipient = false;
    for (const auto& rule : rules) {
        const auto recipients = boost::json::value_to<std::vector<std::string>>(boost::json::parse(rule[2].as<std::string>()));
        for (const auto& recipient : recipients) {
            any_recipient = true;
            // The unique key drops an address that two rules both name.
            const auto id = insert_delivery(tx, event, rule[0].as<std::int64_t>(), rule[1].as<std::string>(), recipient, "pending",
                                            std::nullopt, now_text);
            if (!id) continue;
            enqueue_delivery(tx, *id, now);
            ++created;
        }
    }
    if (!any_recipient) {
        if (insert_delivery(tx, event, std::nullopt, kEmailChannel, "", "not_sent",
                            OptString("no notification rule names a recipient for this event"), now_text)) {
            ++created;
        }
    }
    return created;
}

std::optional<std::int64_t> emit_to(pqxx::transaction_base& tx, const NotificationEvent& event, std::string_view recipient,
                                    core::TimePoint now) {
    const auto id = insert_delivery(tx, event, std::nullopt, kEmailChannel, recipient, "pending", std::nullopt, to_timestamptz(now));
    if (id) enqueue_delivery(tx, *id, now);
    return id;
}

// ---- the delivery job ------------------------------------------------------------

std::optional<PendingDelivery> begin_delivery(pqxx::transaction_base& tx, std::int64_t id, core::TimePoint now) {
    const auto rows = tx.exec(
        "UPDATE notification_deliveries SET attempts = attempts + 1, updated_at = $2::timestamptz "
        "WHERE id = $1 AND status = 'pending' "
        "RETURNING channel, recipient, subject, body, link_path, (SELECT email_sending_enabled FROM system_settings)",
        pqxx::params{id, to_timestamptz(now)});
    if (rows.empty()) return std::nullopt;
    const auto row = rows[0];
    return PendingDelivery{.id = id,
                           .channel = row[0].as<std::string>(),
                           .recipient = row[1].as<std::string>(),
                           .subject = row[2].as<std::string>(),
                           .body = row[3].as<std::string>(),
                           .link_path = row[4].as<std::string>(),
                           .sending_enabled = row[5].as<bool>()};
}

void mark_delivery_sent(pqxx::transaction_base& tx, std::int64_t id, core::TimePoint now) {
    tx.exec("UPDATE notification_deliveries SET status = 'sent', status_reason = NULL, sent_at = $2::timestamptz, "
            "updated_at = $2::timestamptz WHERE id = $1 AND status = 'pending'",
            pqxx::params{id, to_timestamptz(now)})
        .no_rows();
}

void mark_delivery(pqxx::transaction_base& tx, std::int64_t id, std::string_view status, std::string_view reason,
                   core::TimePoint now) {
    tx.exec("UPDATE notification_deliveries SET status = $2, status_reason = $3, updated_at = $4::timestamptz "
            "WHERE id = $1 AND status = 'pending'",
            pqxx::params{id, std::string(status), std::string(reason), to_timestamptz(now)})
        .no_rows();
}

// ---- rules -------------------------------------------------------------------------

model::NotificationRuleList list_rules(pqxx::transaction_base& tx) {
    model::NotificationRuleList out;
    out.items = db::rows_as<model::NotificationRule>(tx, rule_select() + " ORDER BY n.event_type, c.name NULLS FIRST, n.id");
    out.total = static_cast<std::int64_t>(out.items.size());
    return out;
}

std::optional<model::NotificationRule> get_rule(pqxx::transaction_base& tx, std::int64_t id) {
    return db::row_as<model::NotificationRule>(tx, rule_select() + " WHERE n.id = $1", pqxx::params{id});
}

Result<model::NotificationRule> create_rule(pqxx::transaction_base& tx, audit::Writer& audit, const NewRule& rule) {
    if (!std::ranges::contains(kEventTypes, std::string_view(rule.event_type)) || rule.event_type == kEventTest) {
        return invalid("\"event_type\" must be escalation, approval_needed, system_problem or client_report.");
    }
    const auto& client_id = rule.client_id.value();
    if (rule.event_type == kEventClientReport && !client_id) {
        return invalid("A rule for client reports must name the client whose reports it sends.");
    }
    const auto recipients = clean_recipients(rule.recipients);
    if (!recipients) {
        return invalid("\"recipients\" must be 1 to 20 valid email addresses.");
    }
    if (client_id && tx.exec("SELECT 1 FROM clients WHERE id = $1", pqxx::params{*client_id}).empty()) {
        return conflict("client_not_found", "There is no such client.");
    }
    const auto now = to_timestamptz(audit.now());
    const auto rows = tx.exec(
        "INSERT INTO notification_rules (event_type, client_id, channel, recipients, enabled, created_at, updated_at) "
        "VALUES ($1, $2, 'email', $3::jsonb, $4, $5::timestamptz, $5::timestamptz) "
        "ON CONFLICT (event_type, channel, coalesce(client_id, 0)) DO NOTHING RETURNING id",
        pqxx::params{rule.event_type, client_id, db::json_list(*recipients), rule.enabled.value_or(true), now});
    if (rows.empty()) {
        return conflict("rule_exists", "A rule for this event type and client already exists; change that one.");
    }
    auto created = *get_rule(tx, rows[0][0].as<std::int64_t>());
    audit.record("notification_rule.create", "notification_rule", created.id, {.after = rule_json(created)});
    return created;
}

Result<model::NotificationRule> update_rule(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id, const RulePatch& patch) {
    if (tx.exec("SELECT 1 FROM notification_rules WHERE id = $1 FOR UPDATE", pqxx::params{id}).empty()) {
        return not_found("notification rule");
    }
    const auto before = *get_rule(tx, id);
    auto recipients = before.recipients;
    if (patch.recipients) {
        const auto cleaned = clean_recipients(*patch.recipients);
        if (!cleaned) return invalid("\"recipients\" must be 1 to 20 valid email addresses.");
        recipients = *cleaned;
    }
    const bool enabled = patch.enabled.value_or(before.enabled);
    if (recipients == before.recipients && enabled == before.enabled) {
        audit.nothing_changed();
        return before;
    }
    tx.exec("UPDATE notification_rules SET recipients = $2::jsonb, enabled = $3, updated_at = $4::timestamptz WHERE id = $1",
            pqxx::params{id, db::json_list(recipients), enabled, to_timestamptz(audit.now())})
        .no_rows();
    auto after = *get_rule(tx, id);
    audit.record("notification_rule.update", "notification_rule", id, {.before = rule_json(before), .after = rule_json(after)});
    return after;
}

Result<void> delete_rule(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id) {
    const auto before = get_rule(tx, id);
    if (!before) return not_found("notification rule");
    tx.exec("DELETE FROM notification_rules WHERE id = $1", pqxx::params{id}).no_rows();
    audit.record("notification_rule.delete", "notification_rule", id, {.before = rule_json(*before)});
    return {};
}

// ---- the delivery log ------------------------------------------------------------

model::NotificationDeliveryList list_deliveries(pqxx::transaction_base& tx, const DeliveryFilter& filter) {
    db::Conditions where;
    if (!filter.event_type.empty()) where.add(std::format("d.event_type = {}", where.bind(filter.event_type)));
    if (!filter.status.empty()) where.add(std::format("d.status = {}", where.bind(filter.status)));
    return db::page<model::NotificationDelivery>(tx, delivery_select(), where, "d.created_at DESC, d.id DESC", filter.page);
}

std::optional<model::NotificationDelivery> get_delivery(pqxx::transaction_base& tx, std::int64_t id) {
    return db::row_as<model::NotificationDelivery>(tx, delivery_select() + " WHERE d.id = $1", pqxx::params{id});
}

// ---- the switches ----------------------------------------------------------------

model::SystemSettings get_system_settings(pqxx::transaction_base& tx) {
    return db::one_as<model::SystemSettings>(
        tx, std::format("SELECT posting_enabled, email_sending_enabled, {} AS updated_at FROM system_settings", iso("updated_at")));
}

model::SystemSettings update_system_settings(pqxx::transaction_base& tx, audit::Writer& audit, const SystemSettingsPatch& patch) {
    tx.exec("SELECT 1 FROM system_settings FOR UPDATE").one_row();
    const auto before = get_system_settings(tx);
    const bool posting = patch.posting_enabled.value_or(before.posting_enabled);
    const bool sending = patch.email_sending_enabled.value_or(before.email_sending_enabled);
    if (posting == before.posting_enabled && sending == before.email_sending_enabled) {
        audit.nothing_changed();
        return before;
    }
    tx.exec("UPDATE system_settings SET posting_enabled = $1, email_sending_enabled = $2, updated_at = $3::timestamptz",
            pqxx::params{posting, sending, to_timestamptz(audit.now())})
        .no_rows();
    // One entry per switch, named for what happened: the kill switch must be easy to find in the log.
    if (posting != before.posting_enabled) {
        audit.record({.action = posting ? "posting.enabled" : "posting.disabled",
                      .object_type = "system_settings",
                      .object_id = "posting",
                      .before = {{"posting_enabled", before.posting_enabled}},
                      .after = {{"posting_enabled", posting}}});
    }
    if (sending != before.email_sending_enabled) {
        audit.record({.action = sending ? "email_sending.enabled" : "email_sending.disabled",
                      .object_type = "system_settings",
                      .object_id = "email_sending",
                      .before = {{"email_sending_enabled", before.email_sending_enabled}},
                      .after = {{"email_sending_enabled", sending}}});
    }
    return get_system_settings(tx);
}

model::EmailStatus email_status(pqxx::transaction_base& tx, core::TimePoint now) {
    model::EmailStatus status;
    status.sending_enabled = get_system_settings(tx).email_sending_enabled;
    status.connected = load_credential(tx, kGmailCredential).has_value();
    auto health = integration(tx, kEmailStatus);
    status.last_ok_at = std::move(health.last_ok_at);
    status.last_error = std::move(health.last_error);
    status.last_error_at = std::move(health.last_error_at);
    const auto counts = tx.exec("SELECT count(*) FILTER (WHERE status = 'pending'), "
                                "count(*) FILTER (WHERE status = 'failed' AND created_at > $1::timestamptz) FROM notification_deliveries",
                                pqxx::params{to_timestamptz(now - std::chrono::hours(24 * 7))})
                            .one_row();
    status.pending = counts[0].as<std::int64_t>();
    status.failed = counts[1].as<std::int64_t>();
    return status;
}

void record_gbp_token_health(pqxx::transaction_base& tx, const std::optional<std::string>& error, core::TimePoint now) {
    record_integration(tx, kGbpTokenStatus, error, now);
    if (!error) return;
    emit(tx,
         {.type = std::string(kEventSystemProblem),
          .key = std::format("gbp_token:{:%F}", now),
          .client_id = std::nullopt,
          .location_id = std::nullopt,
          .review_id = std::nullopt,
          .subject = "The Google Business Profile token stopped working",
          .body = std::format("Google did not accept the GBP managing account's token ({}). Reviews are not synced and replies can't be "
                              "posted until it works again. If it stays this way, connect the account again on the status page.",
                              *error),
          .link_path = "/#/status"},
         now);
}

void emit_job_failed(pqxx::transaction_base& tx, const jobs::Job& job, std::string_view error, core::TimePoint now) {
    if (job.kind == kDeliverNotificationKind) return;
    emit(tx,
         {.type = std::string(kEventSystemProblem),
          .key = std::format("job_failed:{}:{:%F}", job.kind, now),
          .client_id = std::nullopt,
          .location_id = std::nullopt,
          .review_id = std::nullopt,
          .subject = std::format("A background job failed: {}", job.kind),
          .body = std::format("The job \"{}\" (id {}) failed {} time(s) and was given up on. Last error: {}\n\n"
                              "Only the first failure of this kind is reported each day; the status page lists all.",
                              job.kind, job.id, job.attempts, error),
          .link_path = "/#/status"},
         now);
}

}  // namespace poni::repo
