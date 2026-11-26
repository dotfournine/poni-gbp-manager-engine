#include "db/repo_notifications.hpp"

#include "core/etag.hpp"
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
        INSERT INTO notification_deliveries (event_type, event_key, location_id, review_id, rule_id, channel, recipient, subject,
                                             body, link_path, status, status_reason, created_at, updated_at, escalation_id,
                                             local_post_id)
        VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13::timestamptz, $13::timestamptz, $14, $15)
        ON CONFLICT (event_key, channel, recipient) DO NOTHING
        RETURNING id)sql",
        pqxx::params{event.type, event.key, event.location_id, event.review_id, rule_id, std::string(channel), std::string(recipient),
                     event.subject, event.body, event.link_path, std::string(status), reason, now, event.escalation_id,
                     event.local_post_id});
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
    return std::format("SELECT n.id, n.event_type, n.location_id, l.title AS location_title, n.channel, n.recipients, n.enabled, "
                       "{} AS created_at, {} AS updated_at "
                       "FROM notification_rules n LEFT JOIN locations l ON l.id = n.location_id",
                       iso("n.created_at"), iso("n.updated_at"));
}

boost::json::object rule_json(const model::NotificationRule& rule) {
    return {{"event_type", rule.event_type},
            {"channel", rule.channel},
            {"location_id", rule.location_id ? boost::json::value(*rule.location_id) : boost::json::value(nullptr)},
            {"recipients", boost::json::value_from(rule.recipients)},
            {"enabled", rule.enabled}};
}

std::string delivery_select() {
    return std::format("SELECT d.id, d.event_type, d.location_id, d.review_id, d.escalation_id, d.report_id, "
                       "(SELECT u.email FROM users u WHERE u.id = d.requested_by) AS requested_by, d.channel, d.recipient, d.subject, "
                       "d.body, d.status, d.status_reason, d.attempts, {} AS created_at, {} AS sent_at "
                       "FROM notification_deliveries d",
                       iso("d.created_at"), iso("d.sent_at"));
}

}  // namespace

std::string normal_address(std::string_view raw) {
    // Only the ends are trimmed: a space inside an address is a mistake to report, not to repair.
    std::string address(text::trim(raw));
    for (auto& c : address) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return address;
}

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

std::optional<std::vector<std::string>> clean_recipients(const std::vector<std::string>& recipients, std::size_t min_count) {
    std::vector<std::string> out;
    for (const auto& raw : recipients) {
        auto address = normal_address(raw);
        if (!valid_email(address)) return std::nullopt;
        if (!std::ranges::contains(out, address)) out.push_back(std::move(address));
    }
    if (out.size() < min_count || out.size() > 20) return std::nullopt;
    return out;
}

int emit(pqxx::transaction_base& tx, const NotificationEvent& event, core::TimePoint now) {
    const auto now_text = to_timestamptz(now);
    // A location's own rule and the rule for every location both apply.
    const auto rules = tx.exec("SELECT id, channel, array_to_json(recipients)::text FROM notification_rules "
                               "WHERE event_type = $1 AND enabled AND (location_id IS NULL OR location_id = $2) ORDER BY id",
                               pqxx::params{event.type, event.location_id});
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
        "RETURNING channel, recipient, subject, body, link_path, (SELECT email_sending_enabled FROM system_settings), "
        "event_type, event_key, location_id, review_id, escalation_id, local_post_id",
        pqxx::params{id, to_timestamptz(now)});
    if (rows.empty()) return std::nullopt;
    const auto row = rows[0];
    return PendingDelivery{.id = id,
                           .channel = row[0].as<std::string>(),
                           .recipient = row[1].as<std::string>(),
                           .subject = row[2].as<std::string>(),
                           .body = row[3].as<std::string>(),
                           .link_path = row[4].as<std::string>(),
                           .sending_enabled = row[5].as<bool>(),
                           .event_type = row[6].as<std::string>(),
                           .event_key = row[7].as<std::string>(),
                           .location_id = row[8].as<std::optional<std::int64_t>>(),
                           .review_id = row[9].as<std::optional<std::int64_t>>(),
                           .escalation_id = row[10].as<std::optional<std::int64_t>>(),
                           .local_post_id = row[11].as<std::optional<std::int64_t>>()};
}

void mark_delivery_sent(pqxx::transaction_base& tx, std::int64_t id, core::TimePoint now, std::optional<std::string> reason) {
    tx.exec("UPDATE notification_deliveries SET status = 'sent', status_reason = $3, sent_at = $2::timestamptz, "
            "updated_at = $2::timestamptz WHERE id = $1 AND status = 'pending'",
            pqxx::params{id, to_timestamptz(now), reason})
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
    out.items = db::rows_as<model::NotificationRule>(tx, rule_select() + " ORDER BY n.event_type, l.title NULLS FIRST, n.id");
    out.total = static_cast<std::int64_t>(out.items.size());
    return out;
}

std::optional<model::NotificationRule> get_rule(pqxx::transaction_base& tx, std::int64_t id) {
    return db::row_as<model::NotificationRule>(tx, rule_select() + " WHERE n.id = $1", pqxx::params{id});
}

Result<model::NotificationRule> create_rule(pqxx::transaction_base& tx, audit::Writer& audit, const NewRule& rule, bool push_configured) {
    if (!std::ranges::contains(kRuleEventTypes, std::string_view(rule.event_type))) {
        return invalid("\"event_type\" must be escalation, approval_needed or system_problem.");
    }
    const std::string channel = rule.channel.value_or(std::string(kEmailChannel));
    if (channel != kEmailChannel && channel != kPushChannel) return invalid("\"channel\" must be email or push.");
    if (channel == kPushChannel) {
        // Drafts waiting stay email (D-097).
        if (rule.event_type != kEventEscalation && rule.event_type != kEventSystemProblem) {
            return invalid("A push rule is for escalation or system_problem only.");
        }
        if (!push_configured) {
            return conflict("push_not_configured", "This engine has no APNs settings, so it can't push; ask whoever runs it.");
        }
    }
    const auto& location_id = rule.location_id.value();
    const auto recipients = clean_recipients(rule.recipients, 1);
    if (!recipients) {
        return invalid("\"recipients\" must be 1 to 20 valid email addresses.");
    }
    if (location_id && tx.exec("SELECT 1 FROM locations WHERE id = $1", pqxx::params{*location_id}).empty()) {
        return conflict("location_not_found", "There is no such location.");
    }
    const auto now = to_timestamptz(audit.now());
    const auto rows = tx.exec(
        std::format("INSERT INTO notification_rules (event_type, location_id, channel, recipients, enabled, created_at, updated_at) "
                    "VALUES ($1, $2, $6, {}, $4, $5::timestamptz, $5::timestamptz) "
                    "ON CONFLICT (event_type, channel, coalesce(location_id, 0)) DO NOTHING RETURNING id",
                    db::text_array("$3")),
        pqxx::params{rule.event_type, location_id, db::json_list(*recipients), rule.enabled.value_or(true), now, channel});
    if (rows.empty()) {
        return conflict("rule_exists", "A rule for this event type, channel and location already exists; change that one.");
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
        const auto cleaned = clean_recipients(*patch.recipients, 1);
        if (!cleaned) return invalid("\"recipients\" must be 1 to 20 valid email addresses.");
        recipients = *cleaned;
    }
    const bool enabled = patch.enabled.value_or(before.enabled);
    if (recipients == before.recipients && enabled == before.enabled) {
        audit.nothing_changed();
        return before;
    }
    tx.exec(std::format("UPDATE notification_rules SET recipients = {}, enabled = $3, updated_at = $4::timestamptz WHERE id = $1",
                        db::text_array("$2")),
            pqxx::params{id, db::json_list(recipients), enabled, to_timestamptz(audit.now())})
        .no_rows();
    auto after = *get_rule(tx, id);
    audit.record("notification_rule.update", "notification_rule", id, {.before = rule_json(before), .after = rule_json(after)});
    return after;
}

boost::json::object editable(const model::NotificationRule& rule) {
    return {{"recipients", boost::json::value_from(rule.recipients)}, {"enabled", rule.enabled}};
}

Result<std::string> current_rule_etag(pqxx::transaction_base& tx, std::int64_t id) {
    if (tx.exec("SELECT 1 FROM notification_rules WHERE id = $1 FOR UPDATE", pqxx::params{id}).empty()) {
        return not_found("notification rule");
    }
    return core::etag(editable(*get_rule(tx, id)));
}

Result<void> delete_rule(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id) {
    // Locked before it is read, so that a change made meanwhile is either in the entry or waits for the delete.
    if (tx.exec("SELECT 1 FROM notification_rules WHERE id = $1 FOR UPDATE", pqxx::params{id}).empty()) {
        return not_found("notification rule");
    }
    const auto before = *get_rule(tx, id);
    tx.exec("DELETE FROM notification_rules WHERE id = $1", pqxx::params{id}).no_rows();
    audit.record("notification_rule.delete", "notification_rule", id, {.before = rule_json(before), .location_id = before.location_id});  // the row is gone
    return {};
}

// ---- the delivery log ------------------------------------------------------------

model::NotificationDeliveryList list_deliveries(pqxx::transaction_base& tx, const DeliveryFilter& filter) {
    db::Conditions where;
    if (!filter.event_type.empty()) where.add(std::format("d.event_type = {}", where.bind(filter.event_type)));
    if (!filter.status.empty()) where.add(std::format("d.status = {}", where.bind(filter.status)));
    if (filter.escalation_id) where.add(std::format("d.escalation_id = {}", where.bind(*filter.escalation_id)));
    if (filter.report_id) where.add(std::format("d.report_id = {}", where.bind(*filter.report_id)));
    return db::page<model::NotificationDelivery>(tx, delivery_select(), where, "d.created_at DESC, d.id DESC", filter.page);
}

std::optional<model::NotificationDelivery> get_delivery(pqxx::transaction_base& tx, std::int64_t id) {
    return db::row_as<model::NotificationDelivery>(tx, delivery_select() + " WHERE d.id = $1", pqxx::params{id});
}

// ---- a report's sends ------------------------------------------------------------

void record_report_deliveries(pqxx::transaction_base& tx, std::int64_t report_id, std::int64_t location_id,
                              std::optional<std::int64_t> requested_by, std::string_view event_key, std::string_view subject,
                              std::string_view body, const std::vector<ReportDelivery>& deliveries, core::TimePoint now) {
    const auto now_text = to_timestamptz(now);
    for (const auto& delivery : deliveries) {
        tx.exec(R"sql(
            INSERT INTO notification_deliveries (event_type, event_key, location_id, report_id, requested_by, channel, recipient, subject,
                                                 body, link_path, status, status_reason, attempts, created_at, updated_at, sent_at)
            VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, '', $10, $11, 1, $12::timestamptz, $12::timestamptz,
                    CASE WHEN $13::boolean THEN $12::timestamptz END))sql",
                pqxx::params{kEventLocationReport, event_key, location_id, report_id, requested_by, kEmailChannel, delivery.recipient,
                             subject, body, delivery.sent ? "sent" : "failed", delivery.sent ? OptString{} : delivery.reason, now_text,
                             delivery.sent})
            .no_rows();
    }
}

std::vector<model::NotificationDelivery> report_deliveries(pqxx::transaction_base& tx, std::int64_t report_id) {
    return db::rows_as<model::NotificationDelivery>(tx, delivery_select() + " WHERE d.report_id = $1 ORDER BY d.created_at DESC, d.id DESC",
                                                    pqxx::params{report_id});
}

// ---- the switches ----------------------------------------------------------------

model::SystemSettings get_system_settings(pqxx::transaction_base& tx) {
    return db::one_as<model::SystemSettings>(
        tx, std::format("SELECT posting_enabled, email_sending_enabled, {} AS updated_at FROM system_settings", iso("updated_at")));
}

boost::json::object editable(const model::SystemSettings& settings) {
    return {{"posting_enabled", settings.posting_enabled}, {"email_sending_enabled", settings.email_sending_enabled}};
}

Result<std::string> current_system_settings_etag(pqxx::transaction_base& tx) {
    tx.exec("SELECT 1 FROM system_settings FOR UPDATE").one_row();
    return core::etag(editable(get_system_settings(tx)));
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
    const auto mailbox = load_credential(tx, kGmailCredential);
    status.connected = mailbox.has_value();
    if (mailbox) status.account_email = mailbox->account_email;
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
