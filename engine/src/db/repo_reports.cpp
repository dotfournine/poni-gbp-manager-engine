#include "db/repo_reports.hpp"

#include "db/database.hpp"
#include "db/repo_knowledge.hpp"
#include "db/repo_notifications.hpp"
#include "domain/posting_schedule.hpp"
#include "domain/reply_settings.hpp"
#include "domain/report_html.hpp"
#include "jobs/queue.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value_from.hpp>
#include <boost/json/value_to.hpp>

#include <chrono>
#include <format>

namespace poni::repo {

using db::iso;
using db::to_timestamptz;

namespace {

using OptString = std::optional<std::string>;
using OptId = std::optional<std::int64_t>;

constexpr int kMaxPeriodDays = 400;

core::TimePoint from_micros(std::int64_t micros) { return core::TimePoint(std::chrono::microseconds(micros)); }

std::string micros(std::string_view column) { return std::format("(extract(epoch FROM {}) * 1000000)::bigint", column); }

std::string report_select() {
    return std::format(R"sql(
        SELECT p.id, p.client_id, c.name AS client_name, p.period_start::text AS period_start, p.period_end::text AS period_end,
               p.time_zone, p.schedule_id, (SELECT u.email FROM users u WHERE u.id = p.requested_by) AS requested_by, p.status, p.error,
               p.hold_reason, (p.data->'reviews'->>'new_reviews')::bigint AS new_reviews, p.data->>'summary' AS summary,
               nullif(p.data->>'summary_note', '') AS summary_note, {} AS created_at, {} AS sent_at,
               (SELECT u.email FROM users u WHERE u.id = p.sent_by) AS sent_by
          FROM reports p
          JOIN clients c ON c.id = p.client_id)sql",
                       iso("p.created_at"), iso("p.sent_at"));
}

// A share link's object, r/<token>.html, in two parts: a link's address is put together where its row is read.
// Constants written into the SQL: no quote in either.
constexpr std::string_view kShareObjectPrefix = "r/";
constexpr std::string_view kShareObjectSuffix = ".html";

// The share links whose `column` is `id` ($2), each with its address: the bucket's ($1), then share_object_name(token).
std::string link_select(std::string_view column) {
    return std::format("SELECT s.id, $1::text || '{}' || s.token || '{}' AS url, s.origin, "
                       "(SELECT u.email FROM users u WHERE u.id = s.created_by) AS created_by, {} AS created_at, {} AS revoked_at "
                       "FROM report_share_links s WHERE {} = $2 ORDER BY s.id",
                       kShareObjectPrefix, kShareObjectSuffix, iso("s.created_at"), iso("s.revoked_at"), column);
}

model::ReportShareLink link_by_id(pqxx::transaction_base& tx, std::int64_t id, std::string_view link_base) {
    return db::one_as<model::ReportShareLink>(tx, link_select("s.id"), pqxx::params{std::string(link_base), id});
}

boost::json::value report_json(const model::Report& report) {
    return {{"client_id", report.client_id},
            {"period_start", report.period_start},
            {"period_end", report.period_end},
            {"time_zone", report.time_zone},
            {"status", report.status}};
}

// What has to hold for a report to go out, read with the report's row locked.
Result<std::int64_t> sendable(pqxx::transaction_base& tx, std::int64_t report_id) {
    const auto rows = tx.exec("SELECT status, client_id FROM reports WHERE id = $1 FOR UPDATE", pqxx::params{report_id});
    if (rows.empty()) return not_found("report");
    if (rows[0][0].as<std::string>() != "draft") {
        return conflict("report_not_ready", "Only a report that is built and has not been sent or rejected can be sent.");
    }
    const auto client_id = rows[0][1].as<std::int64_t>();
    if (tx.query_value<bool>("SELECT archived FROM clients WHERE id = $1", pqxx::params{client_id})) {
        return conflict("client_archived", "The client is archived, so its reports are not sent.");
    }
    if (!tx.query_value<bool>("SELECT email_sending_enabled FROM system_settings")) {
        return conflict("sending_off", "Email sending is switched off, so the report would reach nobody. Switch it on first.");
    }
    const auto recipients = tx.query_value<std::int64_t>(
        "SELECT coalesce(sum(jsonb_array_length(recipients)), 0) FROM notification_rules "
        "WHERE event_type = $1 AND enabled AND client_id = $2",
        pqxx::params{kEventClientReport, client_id});
    if (recipients == 0) {
        return conflict("no_recipients", "This client has no recipients for its reports. Add them first.");
    }
    return client_id;
}

// ---- schedules ----

std::string schedule_select() {
    return std::format("SELECT s.id, s.client_id, c.name AS client_name, s.frequency, s.weekday, s.month_day, "
                       "lpad((s.minute_of_day / 60)::text, 2, '0') || ':' || lpad((s.minute_of_day % 60)::text, 2, '0') AS \"time\", "
                       "s.time_zone, s.auto_send, s.enabled, s.last_run_on::text AS last_run_on, {} AS created_at, {} AS updated_at "
                       "FROM report_schedules s JOIN clients c ON c.id = s.client_id",
                       iso("s.created_at"), iso("s.updated_at"));
}

// The schedule as the rules read it, or what is wrong with what was asked for.
Result<domain::ReportSchedule> checked(const model::ReportSchedule& schedule) {
    const auto minute = domain::parse_clock_time(schedule.time);
    if (!minute) return invalid("\"time\" must be a time of day, HH:MM.");
    domain::ReportSchedule out{.frequency = schedule.frequency,
                               .weekday = static_cast<int>(schedule.weekday.value_or(0)),
                               .month_day = static_cast<int>(schedule.month_day.value_or(0)),
                               .minute_of_day = *minute,
                               .time_zone = schedule.time_zone};
    if (auto problem = domain::schedule_problem(out)) return invalid(std::move(*problem) + ".");
    return out;
}

boost::json::value schedule_json(const model::ReportSchedule& schedule) {
    auto json = boost::json::value_from(schedule).as_object();
    for (const auto key : {"id", "client_name", "created_at", "updated_at", "last_run_on"}) json.erase(key);
    return json;
}

}  // namespace

std::string share_object_name(std::string_view token) { return std::format("{}{}{}", kShareObjectPrefix, token, kShareObjectSuffix); }

// ---- creating and building ---------------------------------------------------------

Result<model::Report> create_report(pqxx::transaction_base& tx, audit::Writer& audit, const NewReport& report, std::string_view link_base) {
    const auto client = tx.exec("SELECT archived FROM clients WHERE id = $1", pqxx::params{report.client_id});
    if (client.empty()) return not_found("client");
    if (client[0][0].as<bool>()) return invalid("The client is archived.");
    auto time_zone = report.time_zone;
    if (time_zone.empty()) {
        // That of the client's first location that has one.
        const auto zone = tx.exec("SELECT time_zone FROM locations WHERE client_id = $1 AND time_zone IS NOT NULL ORDER BY id LIMIT 1",
                                  pqxx::params{report.client_id});
        time_zone = zone.empty() ? std::string("UTC") : zone[0][0].as<std::string>();
    }
    if (!domain::known_time_zone(time_zone)) return invalid(std::format("Unknown time zone '{}'.", time_zone));
    if (report.period.start > report.period.end) return invalid("The period ends before it starts.");
    if ((report.period.end - report.period.start).count() >= kMaxPeriodDays) {
        return invalid(std::format("A report covers at most {} days.", kMaxPeriodDays));
    }
    const auto bounds = domain::period_bounds(report.period, time_zone);
    if (!bounds) return invalid(bounds.error());
    if (bounds->to > audit.now()) return invalid("The period is not over yet: its last day must be before today.");

    const auto now = audit.now();
    const auto id = tx.query_value<std::int64_t>(
        "INSERT INTO reports (client_id, period_start, period_end, time_zone, schedule_id, requested_by, auto_send, status, "
        "created_at, updated_at) VALUES ($1, $2::date, $3::date, $4, $5, $6, $7, 'building', $8::timestamptz, $8::timestamptz) "
        "RETURNING id",
        pqxx::params{report.client_id, domain::format_day(report.period.start), domain::format_day(report.period.end), time_zone,
                     report.schedule_id, audit.person_id(), report.auto_send, to_timestamptz(now)});
    jobs::sql::enqueue(tx,
                       {.kind = std::string(kBuildReportKind),
                        .payload = boost::json::object{{"report_id", id}},
                        .run_at = std::nullopt,
                        .idempotency_key = std::format("report:{}", id),
                        .max_attempts = kBuildReportAttempts},
                       now);
    auto created = *get_report(tx, id, link_base);
    auto after = report_json(created).as_object();
    if (report.schedule_id) after["schedule_id"] = *report.schedule_id;
    audit.record("report.create", "report", id, {.after = std::move(after)});
    return created;
}

std::optional<ReportWork> load_report_work(pqxx::transaction_base& tx, std::int64_t report_id, core::TimePoint now) {
    const auto rows = tx.exec("SELECT p.client_id, c.name, c.industry_key, p.period_start::text, p.period_end::text, p.time_zone, "
                              "p.auto_send FROM reports p JOIN clients c ON c.id = p.client_id WHERE p.id = $1 AND p.status = 'building'",
                              pqxx::params{report_id});
    if (rows.empty()) return std::nullopt;
    const auto row = rows[0];
    ReportWork work;
    work.report_id = report_id;
    work.auto_send = row[6].as<bool>();
    auto& input = work.input;
    input.client_id = row[0].as<std::int64_t>();
    input.client_name = row[1].as<std::string>();
    input.period = {domain::parse_report_day(row[3].as<std::string>()).value(), domain::parse_report_day(row[4].as<std::string>()).value()};
    input.time_zone = row[5].as<std::string>();
    input.generated_at = std::format("{:%FT%T}Z", std::chrono::floor<std::chrono::seconds>(now));

    // create_report accepted the zone; if the system's database has lost it since, the job fails with that message.
    const auto bounds = domain::period_bounds(input.period, input.time_zone);
    const auto before = domain::period_bounds(domain::previous_period(input.period), input.time_zone);
    if (!bounds || !before) throw std::runtime_error(bounds ? before.error() : bounds.error());
    const auto from = to_timestamptz(bounds->from), to = to_timestamptz(bounds->to), earlier = to_timestamptz(before->from);

    if (const auto industry = row[2].as<OptString>()) {
        if (const auto rules = industry_rules(tx, *industry)) {
            for (const auto& topic : rules->topics) input.topic_labels[topic.id] = topic.label;
            for (const auto& type : rules->item_types) {
                if (type.is_person()) input.person_item_types.insert(type.id);
            }
        }
    }

    // Every review up to the end of the period counts for the overall rating; removed ones for nothing.
    for (const auto& location : tx.exec(
             "SELECT l.id, l.title, l.gbp_location_id, l.access_status = 'active', count(r.id), coalesce(sum(r.rating), 0) "
             "FROM locations l LEFT JOIN reviews r ON r.location_id = l.id AND r.removed_at IS NULL AND r.rating IS NOT NULL "
             "AND r.created_time < $2::timestamptz WHERE l.client_id = $1 GROUP BY l.id ORDER BY l.title, l.id",
             pqxx::params{input.client_id, to})) {
        input.locations.push_back({.id = location[0].as<std::int64_t>(),
                                   .title = location[1].as<std::string>(),
                                   .running_count = location[4].as<std::int64_t>(),
                                   .running_rating_sum = location[5].as<std::int64_t>()});
        work.locations.push_back({.id = location[0].as<std::int64_t>(),
                                  .gbp_location_id = location[2].as<std::string>(),
                                  .accessible = location[3].as<bool>()});
    }

    const auto reviews = tx.exec(std::format(R"sql(
        SELECT r.location_id, coalesce(r.rating, 0), ((r.created_time AT TIME ZONE $4)::date)::text, {},
               coalesce(r.original_text, r.comment, ''), coalesce(r.reviewer_name, ''), r.reviewer_photo_url,
               CASE WHEN r.reply_text IS NOT NULL THEN {} END, coalesce(a.sentiment, ''), coalesce(a.topics::text, '[]'),
               coalesce(a.items::text, '[]'), r.created_time >= $3::timestamptz
          FROM reviews r
          JOIN locations l ON l.id = r.location_id
          LEFT JOIN LATERAL (SELECT x.sentiment, x.topics, x.items FROM review_analyses x
                              WHERE x.review_id = r.id AND x.status = 'ok' ORDER BY x.id DESC LIMIT 1) a ON true
         WHERE l.client_id = $1 AND r.removed_at IS NULL
           AND r.created_time >= $2::timestamptz AND r.created_time < $5::timestamptz
         ORDER BY r.created_time, r.id)sql",
                                             micros("r.created_time"), micros("r.reply_updated_time")),
                                 pqxx::params{input.client_id, earlier, from, input.time_zone, to});
    for (const auto& r : reviews) {
        domain::ReportReview review;
        review.location_id = r[0].as<std::int64_t>();
        review.rating = r[1].as<int>();
        review.day = domain::parse_report_day(r[2].as<std::string>()).value();
        review.created = from_micros(r[3].as<std::int64_t>());
        review.text = r[4].as<std::string>();
        review.reviewer_name = r[5].as<std::string>();
        review.reviewer_photo_url = r[6].as<OptString>();
        if (const auto replied = r[7].as<OptId>()) review.replied_at = from_micros(*replied);
        review.sentiment = r[8].as<std::string>();
        review.topics = boost::json::value_to<std::vector<domain::TopicMention>>(boost::json::parse(r[9].as<std::string>()));
        review.items = boost::json::value_to<std::vector<domain::ItemMention>>(boost::json::parse(r[10].as<std::string>()));
        (r[11].as<bool>() ? input.reviews : input.previous_reviews).push_back(std::move(review));
    }

    const auto cases = tx.exec(
        "SELECT count(*) FILTER (WHERE k.opened_at >= $2::timestamptz AND k.opened_at < $3::timestamptz), "
        "count(*) FILTER (WHERE k.closed_at >= $2::timestamptz AND k.closed_at < $3::timestamptz) "
        "FROM cases k JOIN reviews r ON r.id = k.review_id JOIN locations l ON l.id = r.location_id WHERE l.client_id = $1",
        pqxx::params{input.client_id, from, to}).one_row();
    input.escalations_opened = cases[0].as<std::int64_t>();
    input.escalations_closed = cases[1].as<std::int64_t>();
    return work;
}

bool store_built_report(pqxx::transaction_base& tx, std::int64_t report_id, const domain::ReportData& data, const std::string& html,
                        core::TimePoint now) {
    return !tx.exec("UPDATE reports SET status = 'draft', data = $2::jsonb, html = $3, error = NULL, built_at = $4::timestamptz, "
                    "updated_at = $4::timestamptz WHERE id = $1 AND status = 'building' RETURNING id",
                    pqxx::params{report_id, boost::json::serialize(boost::json::value_from(data)), html, to_timestamptz(now)})
                .empty();
}

void fail_report(pqxx::transaction_base& tx, std::int64_t report_id, const std::string& error, core::TimePoint now) {
    tx.exec("UPDATE reports SET status = 'failed', error = $2, updated_at = $3::timestamptz WHERE id = $1 AND status = 'building'",
            pqxx::params{report_id, error, to_timestamptz(now)});
}

// ---- reading -------------------------------------------------------------------------

model::ReportList list_reports(pqxx::transaction_base& tx, const ReportFilter& filter) {
    db::Conditions where;
    if (filter.client_id) where.add("p.client_id = " + where.bind(*filter.client_id));
    if (!filter.status.empty()) where.add("p.status = " + where.bind(filter.status));
    // A list has no share links: they are read with one report.
    return db::page<model::Report>(tx, report_select(), std::move(where), "p.id DESC", filter.page, db::FilledLater{"share_links"});
}

std::optional<model::Report> get_report(pqxx::transaction_base& tx, std::int64_t id, std::string_view link_base) {
    auto report = db::row_as<model::Report>(tx, report_select() + " WHERE p.id = $1", pqxx::params{id}, db::FilledLater{"share_links"});
    if (report) {
        report->share_links = db::rows_as<model::ReportShareLink>(tx, link_select("s.report_id"), pqxx::params{std::string(link_base), id});
    }
    return report;
}

std::optional<std::string> report_html(pqxx::transaction_base& tx, std::int64_t id) {
    const auto rows = tx.exec("SELECT html FROM reports WHERE id = $1 AND html IS NOT NULL", pqxx::params{id});
    if (rows.empty()) return std::nullopt;
    return rows[0][0].as<std::string>();
}

std::optional<std::string> report_data(pqxx::transaction_base& tx, std::int64_t id) {
    const auto rows = tx.exec("SELECT data::text FROM reports WHERE id = $1 AND data IS NOT NULL", pqxx::params{id});
    if (rows.empty()) return std::nullopt;
    return rows[0][0].as<std::string>();
}

// ---- sending, rejecting and share links ---------------------------------------------

Result<std::string> check_send(pqxx::transaction_base& tx, std::int64_t report_id) {
    const auto client = sendable(tx, report_id);
    if (!client) return std::unexpected(client.error());
    return report_html(tx, report_id).value();
}

Result<model::Report> mark_report_sent(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t report_id, const std::string& token,
                                       std::string_view link_base) {
    const auto client = sendable(tx, report_id);
    if (!client) return std::unexpected(client.error());
    const auto user_id = audit.person_id();  // nothing: its schedule sends it
    const auto now = audit.now();
    const auto now_text = to_timestamptz(now);
    tx.exec("INSERT INTO report_share_links (report_id, token, origin, created_by, created_at) VALUES ($1, $2, 'email', $3, $4::timestamptz)",
            pqxx::params{report_id, token, user_id, now_text});
    tx.exec("UPDATE reports SET status = 'sent', sent_at = $2::timestamptz, sent_by = $3, hold_reason = NULL, updated_at = $2::timestamptz "
            "WHERE id = $1",
            pqxx::params{report_id, now_text, user_id});

    const auto data = boost::json::value_to<domain::ReportData>(boost::json::parse(report_data(tx, report_id).value()));
    const auto link = std::string(link_base) + share_object_name(token);
    emit(tx,
         {.type = std::string(kEventClientReport),
          .key = std::format("report:{}:sent", report_id),
          .client_id = *client,
          .location_id = std::nullopt,
          .review_id = std::nullopt,
          .subject = std::format("Your Google reviews and visibility report, {}", domain::period_label(data.period_start, data.period_end)),
          .body = domain::render_report_email(data, link),
          // The link is to the shared page and is in the body: the notifier's "open in the dashboard" line is for staff.
          .link_path = ""},
         now);
    auto sent = *get_report(tx, report_id, link_base);
    audit.record("report.send", "report", report_id, {.after = report_json(sent)});
    return sent;
}

void hold_report(pqxx::transaction_base& tx, std::int64_t report_id, const std::string& reason, core::TimePoint now) {
    tx.exec("UPDATE reports SET hold_reason = $2, updated_at = $3::timestamptz WHERE id = $1 AND status = 'draft'",
            pqxx::params{report_id, reason, to_timestamptz(now)});
}

Result<model::Report> reject_report(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t report_id, std::string_view link_base) {
    const auto rows = tx.exec("SELECT status FROM reports WHERE id = $1 FOR UPDATE", pqxx::params{report_id});
    if (rows.empty()) return not_found("report");
    const auto status = rows[0][0].as<std::string>();
    if (status != "draft" && status != "failed") {
        return conflict("report_not_ready", "Only a report that is waiting or failed can be rejected.");
    }
    // A link that works would go on showing a report that was set aside.
    if (!tx.exec("SELECT 1 FROM report_share_links WHERE report_id = $1 AND revoked_at IS NULL LIMIT 1", pqxx::params{report_id}).empty()) {
        return conflict("report_shared", "This report has share links that work. Revoke them first.");
    }
    const auto before = *get_report(tx, report_id, link_base);
    tx.exec("UPDATE reports SET status = 'rejected', updated_at = $2::timestamptz WHERE id = $1",
            pqxx::params{report_id, to_timestamptz(audit.now())});
    auto after = *get_report(tx, report_id, link_base);
    audit.record("report.reject", "report", report_id, {.before = report_json(before), .after = report_json(after)});
    return after;
}

Result<std::string> shareable_html(pqxx::transaction_base& tx, std::int64_t report_id) {
    const auto rows = tx.exec("SELECT status, html FROM reports WHERE id = $1", pqxx::params{report_id});
    if (rows.empty()) return not_found("report");
    const auto status = rows[0][0].as<std::string>();
    if (status != "draft" && status != "sent") {
        return conflict("report_not_ready", "A link can be made for a report that is built and not rejected.");
    }
    return rows[0][1].as<std::string>();
}

model::ReportShareLink add_share_link(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t report_id, const std::string& token,
                                      std::string_view link_base) {
    const auto id = tx.query_value<std::int64_t>(
        "INSERT INTO report_share_links (report_id, token, origin, created_by, created_at) "
        "VALUES ($1, $2, 'manual', $3, $4::timestamptz) RETURNING id",
        pqxx::params{report_id, token, audit.user_id(), to_timestamptz(audit.now())});
    // The token opens the report: it is not written to the audit log.
    audit.record("report_share_link.create", "report_share_link", id, {.after = {{"report_id", report_id}}});
    return link_by_id(tx, id, link_base);
}

std::optional<ShareLinkRef> share_link_ref(pqxx::transaction_base& tx, std::int64_t link_id) {
    const auto rows = tx.exec("SELECT report_id, token, revoked_at IS NOT NULL FROM report_share_links WHERE id = $1", pqxx::params{link_id});
    if (rows.empty()) return std::nullopt;
    return ShareLinkRef{.report_id = rows[0][0].as<std::int64_t>(), .token = rows[0][1].as<std::string>(), .revoked = rows[0][2].as<bool>()};
}

Result<model::ReportShareLink> revoke_share_link(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t link_id,
                                                 std::string_view link_base) {
    const auto rows = tx.exec("SELECT report_id, revoked_at IS NOT NULL FROM report_share_links WHERE id = $1 FOR UPDATE",
                              pqxx::params{link_id});
    if (rows.empty()) return not_found("share link");
    if (rows[0][1].as<bool>()) {
        audit.nothing_changed();
    } else {
        tx.exec("UPDATE report_share_links SET revoked_at = $2::timestamptz, revoked_by = $3 WHERE id = $1",
                pqxx::params{link_id, to_timestamptz(audit.now()), audit.user_id()});
        audit.record("report_share_link.revoke", "report_share_link", link_id, {.after = {{"report_id", rows[0][0].as<std::int64_t>()}}});
    }
    return link_by_id(tx, link_id, link_base);
}

// ---- schedules ------------------------------------------------------------------------

model::ReportScheduleList list_schedules(pqxx::transaction_base& tx) {
    model::ReportScheduleList out;
    out.items = db::rows_as<model::ReportSchedule>(tx, schedule_select() + " ORDER BY c.name, s.id");
    out.total = std::ssize(out.items);
    return out;
}

namespace {

std::optional<model::ReportSchedule> get_schedule(pqxx::transaction_base& tx, std::int64_t id) {
    return db::row_as<model::ReportSchedule>(tx, schedule_select() + " WHERE s.id = $1", pqxx::params{id});
}

}  // namespace

Result<model::ReportSchedule> create_schedule(pqxx::transaction_base& tx, audit::Writer& audit, const NewSchedule& schedule) {
    const auto client = tx.exec("SELECT archived FROM clients WHERE id = $1", pqxx::params{schedule.client_id});
    if (client.empty()) return not_found("client");
    if (client[0][0].as<bool>()) return invalid("The client is archived.");
    model::ReportSchedule wanted{.frequency = schedule.frequency,
                                 .weekday = schedule.frequency == domain::kWeekly ? schedule.weekday.value() : std::nullopt,
                                 .month_day = schedule.frequency == domain::kMonthly ? schedule.month_day.value() : std::nullopt,
                                 .time = schedule.time,
                                 .time_zone = schedule.time_zone};
    const auto rules = checked(wanted);
    if (!rules) return std::unexpected(rules.error());
    const auto id = tx.query_value<std::int64_t>(
        "INSERT INTO report_schedules (client_id, frequency, weekday, month_day, minute_of_day, time_zone, auto_send, enabled, "
        "created_at, updated_at) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9::timestamptz, $9::timestamptz) RETURNING id",
        pqxx::params{schedule.client_id, wanted.frequency, wanted.weekday, wanted.month_day, rules->minute_of_day, wanted.time_zone,
                     schedule.auto_send.value_or(false), schedule.enabled.value_or(true), to_timestamptz(audit.now())});
    auto created = *get_schedule(tx, id);
    audit.record("report_schedule.create", "report_schedule", id, {.after = schedule_json(created)});
    return created;
}

Result<model::ReportSchedule> update_schedule(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id, const SchedulePatch& patch) {
    if (tx.exec("SELECT 1 FROM report_schedules WHERE id = $1 FOR UPDATE", pqxx::params{id}).empty()) return not_found("report schedule");
    const auto before = *get_schedule(tx, id);
    auto after = before;
    if (patch.frequency) after.frequency = *patch.frequency;
    if (patch.weekday) after.weekday = patch.weekday;
    if (patch.month_day) after.month_day = patch.month_day;
    if (patch.time) after.time = *patch.time;
    if (patch.time_zone) after.time_zone = *patch.time_zone;
    if (patch.auto_send) after.auto_send = *patch.auto_send;
    if (patch.enabled) after.enabled = *patch.enabled;
    if (after.frequency != domain::kWeekly) after.weekday.reset();
    if (after.frequency != domain::kMonthly) after.month_day.reset();
    const auto rules = checked(after);
    if (!rules) return std::unexpected(rules.error());
    if (schedule_json(after) == schedule_json(before)) {
        audit.nothing_changed();
        return before;
    }
    // A new updated_at also means: no run for a moment that passed before this change.
    tx.exec("UPDATE report_schedules SET frequency = $2, weekday = $3, month_day = $4, minute_of_day = $5, time_zone = $6, "
            "auto_send = $7, enabled = $8, updated_at = $9::timestamptz WHERE id = $1",
            pqxx::params{id, after.frequency, after.weekday, after.month_day, rules->minute_of_day, after.time_zone, after.auto_send,
                         after.enabled, to_timestamptz(audit.now())});
    auto stored = *get_schedule(tx, id);
    audit.record("report_schedule.update", "report_schedule", id, {.before = schedule_json(before), .after = schedule_json(stored)});
    return stored;
}

Result<void> delete_schedule(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t id) {
    const auto before = get_schedule(tx, id);
    if (!before) return not_found("report schedule");
    // Its reports stay; they only forget which schedule asked for them.
    tx.exec("DELETE FROM report_schedules WHERE id = $1", pqxx::params{id});
    audit.record("report_schedule.delete", "report_schedule", id, {.before = schedule_json(*before)});
    return {};
}

int fail_stalled_reports(pqxx::transaction_base& tx, core::TimePoint now) {
    return static_cast<int>(
        tx.exec("UPDATE reports SET status = 'failed', error = 'The job that builds this report did not finish.', "
                "updated_at = $1::timestamptz WHERE status = 'building' AND created_at < $2::timestamptz RETURNING id",
                pqxx::params{to_timestamptz(now), to_timestamptz(now - kReportStalledAfter)})
            .size());
}

int start_due_reports(pqxx::transaction_base& tx, core::TimePoint now) {
    int started = 0;
    const auto rows = tx.exec(std::format(
        "SELECT s.id, s.client_id, s.frequency, coalesce(s.weekday, 0), coalesce(s.month_day, 0), s.minute_of_day, s.time_zone, "
        "s.auto_send, s.last_run_on::text, {} FROM report_schedules s JOIN clients c ON c.id = s.client_id "
        "WHERE s.enabled AND NOT c.archived ORDER BY s.id FOR UPDATE OF s",
        micros("s.updated_at")));
    for (const auto& row : rows) {
        const domain::ReportSchedule schedule{.frequency = row[2].as<std::string>(),
                                              .weekday = row[3].as<int>(),
                                              .month_day = row[4].as<int>(),
                                              .minute_of_day = row[5].as<int>(),
                                              .time_zone = row[6].as<std::string>()};
        const auto run = domain::last_occurrence(schedule, now);
        if (!run) continue;  // a zone the system no longer knows: nothing runs, and the schedule's page shows the zone
        if (run->at <= from_micros(row[9].as<std::int64_t>())) continue;
        const auto last = row[8].as<OptString>();
        if (last && domain::parse_report_day(*last).value() >= run->local_day) continue;

        const auto id = row[0].as<std::int64_t>();
        tx.exec("UPDATE report_schedules SET last_run_on = $2::date WHERE id = $1", pqxx::params{id, domain::format_day(run->local_day)});
        audit::Writer audit{tx, audit::Actor::system("report-schedule"), now};
        const auto created = create_report(tx, audit,
                                           {.client_id = row[1].as<std::int64_t>(),
                                            .period = domain::period_for(schedule.frequency, run->local_day),
                                            .time_zone = schedule.time_zone,
                                            .schedule_id = id,
                                            .auto_send = row[7].as<bool>()},
                                           "");
        if (created) ++started;
    }
    return started;
}

}  // namespace poni::repo
