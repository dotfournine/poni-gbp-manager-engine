// Notifications (FR-7): the rules that say who is told about what, and the
// delivery log. An event is raised with emit() inside the transaction that
// caused it, so a message exists exactly when its cause was committed; a
// job (jobs/notifications.hpp) then sends each delivery and records the
// outcome. A report's send is the exception (D-104): it sends first and then
// writes each address's row as it ended, sent or failed, with nothing queued.
// Blocking calls on a transaction the caller owns.
#pragma once

#include "core/body.hpp"
#include "core/clock.hpp"
#include "db/audit.hpp"
#include "db/failure.hpp"
#include "db/sql.hpp"
#include "domain/models.hpp"

#include <boost/describe/class.hpp>
#include <boost/json/object.hpp>
#include <pqxx/pqxx>

#include <array>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace poni::jobs {
struct Job;
}

namespace poni::repo {

inline constexpr std::string_view kDeliverNotificationKind = "notifications.deliver";
inline constexpr std::string_view kEmailChannel = "email";
// Push to the phone app (D-097): a rule's recipients are staff, each told on their devices.
inline constexpr std::string_view kPushChannel = "push";
// integration_status row of the last push to Apple.
inline constexpr std::string_view kPushStatus = "push";
// integration_status row of the sending account's token.
inline constexpr std::string_view kEmailStatus = "email";
// credentials row of the sending account (connectors/notify/gmail.hpp).
inline constexpr std::string_view kGmailCredential = "gmail";

inline constexpr std::string_view kEventEscalation = "escalation";
inline constexpr std::string_view kEventApprovalNeeded = "approval_needed";
inline constexpr std::string_view kEventSystemProblem = "system_problem";
inline constexpr std::string_view kEventTest = "test";
// The type of a report send's delivery rows (D-104). No rule has it: a report goes to its
// location's schedules' recipients (D-111).
inline constexpr std::string_view kEventLocationReport = "location_report";
// Every event type, as notification_deliveries.event_type holds them: the delivery log's filter.
inline constexpr std::array<std::string_view, 5> kEventTypes{kEventEscalation, kEventApprovalNeeded, kEventSystemProblem, kEventTest,
                                                             kEventLocationReport};
// The event types a rule can be for, as notification_rules.event_type holds them: neither the test
// message nor a report's send (D-111).
inline constexpr std::array<std::string_view, 3> kRuleEventTypes{kEventEscalation, kEventApprovalNeeded, kEventSystemProblem};
// The values of notification_deliveries.status.
inline constexpr std::array<std::string_view, 4> kDeliveryStatuses{"pending", "sent", "failed", "not_sent"};

struct NotificationEvent {
    std::string type;  // one of the kEvent* names
    // What it is about. One key reaches one recipient once, so code that
    // raises the same event again (a job that runs twice) sends nothing new.
    std::string key;
    // The rules that name this location apply, and the rules for every location.
    std::optional<std::int64_t> location_id;
    std::optional<std::int64_t> review_id;
    std::string subject;
    // Plain text. May quote a review, which is untrusted: it is never
    // rendered as HTML and never put in a header.
    std::string body;
    // Where to look in the dashboard, e.g. "/#/escalations/12/show".
    std::string link_path;
    // What a push opens in the app, beside the location and the review (D-097).
    std::optional<std::int64_t> escalation_id = std::nullopt;
    std::optional<std::int64_t> local_post_id = std::nullopt;
};

// Writes one delivery for every recipient of every enabled rule that matches
// the event's type and location, and enqueues the job that sends it. With no
// matching recipient one row with an empty recipient is written as not sent,
// so the event still shows in the log. Returns how many deliveries are new.
int emit(pqxx::transaction_base& tx, const NotificationEvent& event, core::TimePoint now);

// One delivery to an address no rule names (the test message). Returns its
// id, or nothing if this key already went to this recipient.
std::optional<std::int64_t> emit_to(pqxx::transaction_base& tx, const NotificationEvent& event, std::string_view recipient,
                                    core::TimePoint now);

// ---- system problems ---------------------------------------------------------------

// What a refresh of the GBP managing account's token came to, for the status
// page: `error`, or nothing for a success. Without the token nothing syncs
// and nothing posts, so a failure also tells people, once a day.
void record_gbp_token_health(pqxx::transaction_base& tx, const std::optional<std::string>& error, core::TimePoint now);

// A job that failed for good is a system problem (FR-7): one message per
// kind and day. Failed deliveries are left out: they are in the delivery
// log, and a mail about mail that can't be sent helps nobody.
void emit_job_failed(pqxx::transaction_base& tx, const jobs::Job& job, std::string_view error, core::TimePoint now);

// ---- the delivery job ------------------------------------------------------------

struct PendingDelivery {
    std::int64_t id = 0;
    std::string channel;
    std::string recipient;
    std::string subject;
    std::string body;
    std::string link_path;
    // The switch in system_settings, for email.
    bool sending_enabled = false;
    // What a push is made of.
    std::string event_type;
    std::string event_key;
    std::optional<std::int64_t> location_id;
    std::optional<std::int64_t> review_id;
    std::optional<std::int64_t> escalation_id;
    std::optional<std::int64_t> local_post_id;
};

// The delivery if it is still pending; counts the attempt.
std::optional<PendingDelivery> begin_delivery(pqxx::transaction_base& tx, std::int64_t id, core::TimePoint now);
// `reason`: what to note beside a send, such as how many devices a push reached.
void mark_delivery_sent(pqxx::transaction_base& tx, std::int64_t id, core::TimePoint now, std::optional<std::string> reason = std::nullopt);
// `status` is "failed" or "not_sent"; "pending" keeps it for the next attempt and only notes the reason.
void mark_delivery(pqxx::transaction_base& tx, std::int64_t id, std::string_view status, std::string_view reason,
                   core::TimePoint now);

// ---- what the dashboard reads and changes --------------------------------------

// The bodies of the POST and the PATCH of a rule, member for member.
struct NewRule {
    std::string event_type;
    std::optional<std::string> channel;      // nothing: email
    core::Nullable<std::int64_t> location_id;  // null or left out: every location
    std::vector<std::string> recipients;
    std::optional<bool> enabled;  // nothing: enabled
};
BOOST_DESCRIBE_STRUCT(NewRule, (), (event_type, channel, location_id, recipients, enabled))
struct RulePatch {
    std::optional<std::vector<std::string>> recipients;
    std::optional<bool> enabled;
};
BOOST_DESCRIBE_STRUCT(RulePatch, (), (recipients, enabled))

// Good enough to keep a typo and a header injection out: one '@', a dot in
// the domain, no spaces, control characters or angle brackets.
bool valid_email(std::string_view address);
// An address as a rule stores it: the ends trimmed and ASCII lower-cased. An address
// someone asks for is compared with stored ones in this form.
std::string normal_address(std::string_view raw);
// A list of addresses as a rule or a report schedule stores it: each one normal_address'd, a repeat dropped, the order
// kept. Nothing if an address is not valid, or there are fewer than `min_count` or more than 20.
std::optional<std::vector<std::string>> clean_recipients(const std::vector<std::string>& recipients, std::size_t min_count);

model::NotificationRuleList list_rules(pqxx::transaction_base& tx);
std::optional<model::NotificationRule> get_rule(pqxx::transaction_base& tx, std::int64_t id);
// `push_configured`: the engine has the APNs settings; without them a push rule is refused.
Result<model::NotificationRule> create_rule(pqxx::transaction_base& tx, audit::Writer& audit, const NewRule& rule, bool push_configured);
Result<model::NotificationRule> update_rule(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id, const RulePatch& patch);
Result<void> delete_rule(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id);
// What PATCH /api/notification-rules/{id} can set, in RulePatch's order, with the rule's values: what its ETag covers (D-108).
boost::json::object editable(const model::NotificationRule& rule);
// Locks the rule's row (FOR UPDATE, the lock update_rule and delete_rule take) and gives the ETag of what it holds.
Result<std::string> current_rule_etag(pqxx::transaction_base& tx, std::int64_t id);

struct DeliveryFilter {
    std::string event_type;  // one of kEventTypes; empty: any
    std::string status;      // one of kDeliveryStatuses; empty: any
    std::optional<std::int64_t> escalation_id;  // the deliveries of one escalation's events
    std::optional<std::int64_t> report_id;      // the deliveries of one report's sends
    db::Page page;
};
model::NotificationDeliveryList list_deliveries(pqxx::transaction_base& tx, const DeliveryFilter& filter);
std::optional<model::NotificationDelivery> get_delivery(pqxx::transaction_base& tx, std::int64_t id);

// ---- a report's sends (D-104) --------------------------------------------------

// What came of one address of a send.
struct ReportDelivery {
    std::string recipient;
    bool sent = false;
    std::optional<std::string> reason;  // why it failed
};
// One row per address, as it ended: sent or failed, one attempt, nothing queued. `event_key` is
// new for every send, so a row that is there already is a bug, and throws. `requested_by` is the
// person who asked for the send; nothing for its schedule.
void record_report_deliveries(pqxx::transaction_base& tx, std::int64_t report_id, std::int64_t location_id,
                              std::optional<std::int64_t> requested_by, std::string_view event_key, std::string_view subject,
                              std::string_view body, const std::vector<ReportDelivery>& deliveries, core::TimePoint now);
// Every row of the report's sends, newest first.
std::vector<model::NotificationDelivery> report_deliveries(pqxx::transaction_base& tx, std::int64_t report_id);

// ---- the switches ----------------------------------------------------------------

model::SystemSettings get_system_settings(pqxx::transaction_base& tx);
// The body of the PATCH, member for member.
struct SystemSettingsPatch {
    std::optional<bool> posting_enabled;
    std::optional<bool> email_sending_enabled;
};
BOOST_DESCRIBE_STRUCT(SystemSettingsPatch, (), (posting_enabled, email_sending_enabled))
model::SystemSettings update_system_settings(pqxx::transaction_base& tx, audit::Writer& audit, const SystemSettingsPatch& patch);
// The two switches, in SystemSettingsPatch's order: what the ETag covers (D-108). The row's pause columns and
// google_target are the engine's, and not in it.
boost::json::object editable(const model::SystemSettings& settings);
// Locks the row (FOR UPDATE, the lock update_system_settings takes) and gives the ETag of what it holds.
Result<std::string> current_system_settings_etag(pqxx::transaction_base& tx);

// Everything on the status page's email block except `sender`.
model::EmailStatus email_status(pqxx::transaction_base& tx, core::TimePoint now);

}  // namespace poni::repo
