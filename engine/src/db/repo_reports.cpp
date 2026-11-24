#include "db/repo_reports.hpp"

#include "db/database.hpp"
#include "db/repo_knowledge.hpp"
#include "domain/dates.hpp"
#include "domain/posting_schedule.hpp"
#include "domain/reply_settings.hpp"
#include "domain/report_html.hpp"
#include "jobs/queue.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value_from.hpp>
#include <boost/json/value_to.hpp>

#include <algorithm>
#include <chrono>
#include <format>

namespace poni::repo {

using db::iso;
using db::to_timestamptz;

namespace {

using OptString = std::optional<std::string>;
using OptId = std::optional<std::int64_t>;

// The refusal of an engine that has no bucket, `so` saying what that stops.
std::unexpected<Failure> no_public_bucket(std::string_view so) {
    return conflict("no_public_bucket", std::format("No public bucket is configured (PONI_PUBLIC_BUCKET), so {}.", so));
}

constexpr int kMaxPeriodDays = 400;

core::TimePoint from_micros(std::int64_t micros) { return core::TimePoint(std::chrono::microseconds(micros)); }

std::string micros(std::string_view column) { return std::format("(extract(epoch FROM {}) * 1000000)::bigint", column); }

// `base` is the placeholder of the public bucket's address ("$2"): a bound parameter, never part of the SQL.
std::string report_select(std::string_view base) {
    return std::format(R"sql(
        SELECT p.id, p.location_id, l.title AS location_title, p.period_start::text AS period_start, p.period_end::text AS period_end,
               p.time_zone, p.schedule_id, (SELECT u.email FROM users u WHERE u.id = p.requested_by) AS requested_by, p.status, p.error,
               CASE WHEN p.status = 'ready' AND {0}::text <> '' THEN {0} || p.page_object END AS url,
               CASE WHEN p.status IN ('discarded', 'expired') AND {0} <> '' AND p.archive_object IS NOT NULL
                    THEN {0} || p.archive_object END AS archive_url,
               p.auto_send_status, (p.data->'reviews'->>'new_reviews')::bigint AS new_reviews,
               (p.data->'reviews'->>'average_rating')::float8 AS average_rating,
               (p.data->'reviews'->>'response_rate')::float8 AS response_rate, p.data->>'summary' AS summary,
               nullif(p.data->>'summary_note', '') AS summary_note, {1} AS created_at
          FROM reports p
          JOIN locations l ON l.id = p.location_id)sql",
                       base, iso("p.created_at"));
}

boost::json::value report_json(const model::Report& report) {
    return {{"location_id", report.location_id},
            {"period_start", report.period_start},
            {"period_end", report.period_end},
            {"time_zone", report.time_zone},
            {"status", report.status}};
}

// ---- schedules ----

std::string schedule_select() {
    return std::format("SELECT s.id, s.location_id, l.title AS location_title, s.frequency, s.weekday, s.month_day, "
                       "lpad((s.minute_of_day / 60)::text, 2, '0') || ':' || lpad((s.minute_of_day % 60)::text, 2, '0') AS \"time\", "
                       "s.time_zone, s.auto_send, s.enabled, s.last_run_on::text AS last_run_on, {} AS created_at, {} AS updated_at "
                       "FROM report_schedules s JOIN locations l ON l.id = s.location_id",
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
    for (const auto key : {"id", "location_title", "created_at", "updated_at", "last_run_on"}) json.erase(key);
    return json;
}

}  // namespace

// ---- creating and building ---------------------------------------------------------

Result<model::Report> create_report(pqxx::transaction_base& tx, audit::Writer& audit, const NewReport& report, std::string_view link_base) {
    const auto location = tx.exec("SELECT enabled, time_zone FROM locations WHERE id = $1", pqxx::params{report.location_id});
    if (location.empty()) return not_found("location");
    if (!location[0][0].as<bool>()) return invalid("The location is switched off, so it gets no report.");
    auto time_zone = report.time_zone;
    if (time_zone.empty()) time_zone = location[0][1].as<OptString>().value_or("UTC");
    if (!domain::known_time_zone(time_zone)) return invalid(std::format("Unknown time zone '{}'.", time_zone));
    if (report.period.start > report.period.end) return invalid("The period ends before it starts.");
    if ((report.period.end - report.period.start).count() >= kMaxPeriodDays) {
        return invalid(std::format("A report covers at most {} days.", kMaxPeriodDays));
    }
    const auto bounds = domain::period_bounds(report.period, time_zone);
    if (!bounds) return invalid(bounds.error());
    if (bounds->to > audit.now()) return invalid("The period is not over yet: its last day must be before today.");
    if (link_base.empty()) {
        return no_public_bucket("a report's page has nowhere to go");
    }

    const auto now = audit.now();
    const auto id = tx.query_value<std::int64_t>(
        "INSERT INTO reports (location_id, period_start, period_end, time_zone, schedule_id, requested_by, auto_send_status, status, "
        "created_at, updated_at) VALUES ($1, $2::date, $3::date, $4, $5, $6, $7, 'building', $8::timestamptz, $8::timestamptz) "
        "RETURNING id",
        pqxx::params{report.location_id, domain::format_day(report.period.start), domain::format_day(report.period.end), time_zone,
                     report.schedule_id, audit.person_id(), report.auto_send ? "pending" : "off", to_timestamptz(now)});
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
    const auto rows = tx.exec("SELECT p.location_id, l.title, l.industry_key, p.period_start::text, p.period_end::text, p.time_zone, "
                              "l.enabled, l.gbp_location_id, l.access_status = 'active' "
                              "FROM reports p JOIN locations l ON l.id = p.location_id WHERE p.id = $1 AND p.status = 'building'",
                              pqxx::params{report_id});
    if (rows.empty()) return std::nullopt;
    const auto row = rows[0];
    if (!row[6].as<bool>()) {
        fail_report(tx, report_id, "The location was switched off before its report was built.", now);
        return std::nullopt;
    }
    ReportWork work;
    work.report_id = report_id;
    work.location = {.id = row[0].as<std::int64_t>(), .gbp_location_id = row[7].as<std::string>(), .accessible = row[8].as<bool>()};
    auto& input = work.input;
    input.location_id = work.location.id;
    input.location_title = row[1].as<std::string>();
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
    const auto running = tx.exec("SELECT count(*), coalesce(sum(rating), 0) FROM reviews WHERE location_id = $1 AND removed_at IS NULL "
                                 "AND rating IS NOT NULL AND created_time < $2::timestamptz",
                                 pqxx::params{input.location_id, to})
                             .one_row();
    input.running_count = running[0].as<std::int64_t>();
    input.running_rating_sum = running[1].as<std::int64_t>();

    const auto reviews = tx.exec(std::format(R"sql(
        SELECT coalesce(r.rating, 0), ((r.created_time AT TIME ZONE $4)::date)::text, {},
               coalesce(r.original_text, r.comment, ''), coalesce(r.reviewer_name, ''), r.reviewer_photo_url,
               CASE WHEN r.reply_text IS NOT NULL THEN {} END, coalesce(a.sentiment, ''), coalesce(a.topics::text, '[]'),
               coalesce(a.items::text, '[]'), r.created_time >= $3::timestamptz
          FROM reviews r
          LEFT JOIN LATERAL (SELECT x.sentiment, x.topics, x.items FROM review_analyses x
                              WHERE x.review_id = r.id AND x.status = 'ok' ORDER BY x.id DESC LIMIT 1) a ON true
         WHERE r.location_id = $1 AND r.removed_at IS NULL
           AND r.created_time >= $2::timestamptz AND r.created_time < $5::timestamptz
         ORDER BY r.created_time, r.id)sql",
                                             micros("r.created_time"), micros("r.reply_updated_time")),
                                 pqxx::params{input.location_id, earlier, from, input.time_zone, to});
    for (const auto& r : reviews) {
        domain::ReportReview review;
        review.rating = r[0].as<int>();
        review.day = domain::parse_report_day(r[1].as<std::string>()).value();
        review.created = from_micros(r[2].as<std::int64_t>());
        review.text = r[3].as<std::string>();
        review.reviewer_name = r[4].as<std::string>();
        review.reviewer_photo_url = r[5].as<OptString>();
        if (const auto replied = r[6].as<OptId>()) review.replied_at = from_micros(*replied);
        review.sentiment = r[7].as<std::string>();
        review.topics = boost::json::value_to<std::vector<domain::TopicMention>>(boost::json::parse(r[8].as<std::string>()));
        review.items = boost::json::value_to<std::vector<domain::ItemMention>>(boost::json::parse(r[9].as<std::string>()));
        (r[10].as<bool>() ? input.reviews : input.previous_reviews).push_back(std::move(review));
    }

    const auto cases = tx.exec(
        "SELECT count(*) FILTER (WHERE k.opened_at >= $2::timestamptz AND k.opened_at < $3::timestamptz), "
        "count(*) FILTER (WHERE k.closed_at >= $2::timestamptz AND k.closed_at < $3::timestamptz) "
        "FROM cases k JOIN reviews r ON r.id = k.review_id WHERE r.location_id = $1",
        pqxx::params{input.location_id, from, to}).one_row();
    input.escalations_opened = cases[0].as<std::int64_t>();
    input.escalations_closed = cases[1].as<std::int64_t>();
    return work;
}

std::optional<std::string> name_report_page(pqxx::transaction_base& tx, std::int64_t report_id, const std::string& candidate) {
    const auto rows = tx.exec("UPDATE reports SET page_object = coalesce(page_object, $2) WHERE id = $1 AND status = 'building' "
                              "RETURNING page_object",
                              pqxx::params{report_id, candidate});
    if (rows.empty()) return std::nullopt;
    return rows[0][0].as<std::string>();
}

std::optional<ReportMail> store_built_report(pqxx::transaction_base& tx, std::int64_t report_id, const domain::ReportData& data,
                                             std::string_view link_base, core::TimePoint now) {
    const auto rows = tx.exec("UPDATE reports SET status = 'ready', data = $2::jsonb, error = NULL, built_at = $3::timestamptz, "
                              "updated_at = $3::timestamptz WHERE id = $1 AND status = 'building' RETURNING auto_send_status",
                              pqxx::params{report_id, boost::json::serialize(boost::json::value_from(data)), to_timestamptz(now)});
    if (rows.empty() || rows[0][0].as<std::string>() != "pending") return std::nullopt;
    // Checked here, so that `done` is written before any email and in the one run that made the report ready:
    // a schedule's send happens at most once, and nothing is checked again between `done` and the send.
    auto mail = prepare_report_send(tx, report_id, std::nullopt, link_base);
    if (!mail) return std::nullopt;  // pending: a person can send it, and the 409 they would get says why
    tx.exec("UPDATE reports SET auto_send_status = 'done' WHERE id = $1", pqxx::params{report_id}).no_rows();
    return std::move(*mail);
}

void fail_report(pqxx::transaction_base& tx, std::int64_t report_id, const std::string& error, core::TimePoint now) {
    tx.exec("UPDATE reports SET status = 'failed', error = $2, updated_at = $3::timestamptz WHERE id = $1 AND status = 'building'",
            pqxx::params{report_id, error, to_timestamptz(now)});
}

// ---- sending -------------------------------------------------------------------------

Result<ReportMail> prepare_report_send(pqxx::transaction_base& tx, std::int64_t report_id,
                                       const std::optional<std::vector<std::string>>& only, std::string_view link_base) {
    if (only && only->empty()) return invalid("\"recipients\" must name at least one address; leave it out to send to all.");
    const auto rows = tx.exec("SELECT p.status, p.location_id, l.enabled, p.page_object, p.data::text "
                              "FROM reports p JOIN locations l ON l.id = p.location_id WHERE p.id = $1 FOR UPDATE OF p",
                              pqxx::params{report_id});
    if (rows.empty()) return not_found("report");
    const auto row = rows[0];
    if (row[0].as<std::string>() != "ready") return conflict("report_not_ready", "Only a report that is ready can be sent.");
    const auto location_id = row[1].as<std::int64_t>();
    if (!row[2].as<bool>()) return conflict("location_not_enabled", "The location is switched off, so its reports are not sent.");
    if (!tx.query_value<bool>("SELECT email_sending_enabled FROM system_settings")) {
        return conflict("sending_off", "Email sending is switched off, so the report would reach nobody. Switch it on first.");
    }
    // What the Gmail channel needs before it sends (http::stored_mailbox_address). Whether its token still works shows
    // as each address's failure.
    if (!tx.query_value<bool>("SELECT EXISTS (SELECT 1 FROM credentials WHERE name = $1 AND account_email IS NOT NULL)",
                              pqxx::params{kGmailCredential})) {
        return conflict("sending_off", "No sending mailbox is connected, so the report would reach nobody. Connect it on the status page first.");
    }
    if (link_base.empty()) {
        return no_public_bucket("the report has no page to link to");
    }
    auto recipients = report_recipients(tx, location_id);
    if (recipients.empty()) return conflict("no_recipients", "This location has no recipients for its reports. Add them first.");
    if (only) {
        std::vector<std::string> wanted;
        for (const auto& raw : *only) {
            auto address = normal_address(raw);
            if (!std::ranges::binary_search(recipients, address)) {
                return invalid(std::format("\"{}\" is not a recipient of this location's reports.", address));
            }
            wanted.push_back(std::move(address));
        }
        // What is left stays sorted and holds each address once, however often it was asked for.
        std::erase_if(recipients, [&wanted](const std::string& address) { return !std::ranges::contains(wanted, address); });
    }
    const auto data = boost::json::value_to<domain::ReportData>(boost::json::parse(row[4].as<std::string>()));
    return ReportMail{.location_id = location_id,
                      .recipients = std::move(recipients),
                      .subject = std::format("Your Google reviews and visibility report, {}", domain::period_label(data.period_start, data.period_end)),
                      .body = domain::render_report_email(data, std::string(link_base) + row[3].as<std::string>())};
}

Result<model::Report> record_report_send(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t report_id, const ReportMail& mail,
                                         const std::vector<ReportDelivery>& outcomes, std::string_view event_key,
                                         std::string_view link_base) {
    record_report_deliveries(tx, report_id, mail.location_id, audit.person_id(), event_key, mail.subject, mail.body, outcomes, audit.now());
    const auto tried = static_cast<std::int64_t>(outcomes.size());
    const auto sent = static_cast<std::int64_t>(std::ranges::count_if(outcomes, &ReportDelivery::sent));
    // Counts only: the addresses are in the delivery rows.
    audit.record("report.send", "report", report_id,
                 {.after = boost::json::object{{"recipients", tried}, {"sent", sent}, {"failed", tried - sent}}});
    return *get_report(tx, report_id, link_base);
}

// ---- discarding ----------------------------------------------------------------------

Result<std::optional<DiscardWork>> begin_discard(pqxx::transaction_base& tx, audit::Writer& audit, std::int64_t report_id,
                                                 std::string_view archive_token, std::string_view link_base) {
    const auto rows = tx.exec("SELECT status, page_object, archive_object FROM reports WHERE id = $1 FOR UPDATE", pqxx::params{report_id});
    if (rows.empty()) return not_found("report");
    const auto status = rows[0][0].as<std::string>();
    const auto page = rows[0][1].as<OptString>();

    if (status == "ready" || status == "failed") {
        const auto before = *get_report(tx, report_id, link_base);
        std::optional<DiscardWork> work;
        if (!page) {
            // Nothing was ever put into the bucket for it (D-104 point 11): there is nothing to move.
            tx.exec("UPDATE reports SET status = 'discarded', updated_at = $2::timestamptz WHERE id = $1",
                    pqxx::params{report_id, to_timestamptz(audit.now())});
        } else {
            if (link_base.empty()) return no_public_bucket("the report's page cannot be moved");
            // Named before anything is copied, and never cleared or replaced afterwards: every object a discard writes is
            // named by its row for good. The extension is the page's.
            const auto dot = page->rfind('.');
            const auto archive = std::format("a/{}{}", archive_token, dot == std::string::npos ? std::string{} : page->substr(dot));
            const auto named = tx.exec("UPDATE reports SET status = 'discarding', archive_object = coalesce(archive_object, $2), "
                                       "updated_at = $3::timestamptz WHERE id = $1 RETURNING archive_object",
                                       pqxx::params{report_id, archive, to_timestamptz(audit.now())})
                                   .one_row();
            work = DiscardWork{.page_object = *page, .archive_object = named[0].as<std::string>()};
        }
        // Written when the discard begins: one that breaks off after this step still has its entry, in the name of the
        // person who began it. No object's name or address is in it.
        audit.record("report.discard", "report", report_id,
                     {.before = report_json(before), .after = report_json(*get_report(tx, report_id, link_base))});
        return work;
    }
    if (status == "discarding") {
        if (link_base.empty()) return no_public_bucket("the report's page cannot be moved");
        audit.nothing_changed();  // the entry was written when it began
        return DiscardWork{.page_object = page.value(), .archive_object = rows[0][2].as<std::string>()};
    }
    if (status == "building") return conflict("report_not_ready", "A report that is still being built cannot be discarded.");
    // "discarded", "expired", and any status the engine does not write: never taken for one that may be finished.
    return conflict("report_discarded", "The report is discarded already.");
}

void finish_discard(pqxx::transaction_base& tx, std::int64_t report_id, core::TimePoint now) {
    tx.exec("UPDATE reports SET status = 'discarded', updated_at = $2::timestamptz WHERE id = $1 AND status = 'discarding'",
            pqxx::params{report_id, to_timestamptz(now)});
}

// ---- reading -------------------------------------------------------------------------

model::ReportList list_reports(pqxx::transaction_base& tx, const ReportFilter& filter, std::string_view link_base) {
    db::Conditions where;
    const auto base = where.bind(std::string(link_base));
    if (filter.location_id) where.add("p.location_id = " + where.bind(*filter.location_id));
    if (!filter.status.empty()) where.add("p.status = " + where.bind(filter.status));
    return db::page<model::Report>(tx, report_select(base), std::move(where), "p.id DESC", filter.page,
                                   db::FilledLater{"recipients", "deliveries"});
}

std::optional<model::Report> get_report(pqxx::transaction_base& tx, std::int64_t id, std::string_view link_base) {
    auto report = db::row_as<model::Report>(tx, report_select("$2") + " WHERE p.id = $1", pqxx::params{id, std::string(link_base)},
                                            db::FilledLater{"recipients", "deliveries"});
    if (!report) return report;
    report->recipients = report_recipients(tx, report->location_id);
    report->deliveries = report_deliveries(tx, id);
    return report;
}

// ---- schedules ------------------------------------------------------------------------

model::ReportScheduleList list_schedules(pqxx::transaction_base& tx) {
    model::ReportScheduleList out;
    out.items = db::rows_as<model::ReportSchedule>(tx, schedule_select() + " ORDER BY l.title, s.id");
    out.total = std::ssize(out.items);
    return out;
}

namespace {

std::optional<model::ReportSchedule> get_schedule(pqxx::transaction_base& tx, std::int64_t id) {
    return db::row_as<model::ReportSchedule>(tx, schedule_select() + " WHERE s.id = $1", pqxx::params{id});
}

}  // namespace

Result<model::ReportSchedule> create_schedule(pqxx::transaction_base& tx, audit::Writer& audit, const NewSchedule& schedule) {
    const auto location = tx.exec("SELECT enabled FROM locations WHERE id = $1", pqxx::params{schedule.location_id});
    if (location.empty()) return not_found("location");
    if (!location[0][0].as<bool>()) return invalid("The location is switched off, so it gets no report.");
    model::ReportSchedule wanted{.frequency = schedule.frequency,
                                 .weekday = schedule.frequency == domain::kWeekly ? schedule.weekday.value() : std::nullopt,
                                 .month_day = schedule.frequency == domain::kMonthly ? schedule.month_day.value() : std::nullopt,
                                 .time = schedule.time,
                                 .time_zone = schedule.time_zone};
    const auto rules = checked(wanted);
    if (!rules) return std::unexpected(rules.error());
    const auto id = tx.query_value<std::int64_t>(
        "INSERT INTO report_schedules (location_id, frequency, weekday, month_day, minute_of_day, time_zone, auto_send, enabled, "
        "created_at, updated_at) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9::timestamptz, $9::timestamptz) RETURNING id",
        pqxx::params{schedule.location_id, wanted.frequency, wanted.weekday, wanted.month_day, rules->minute_of_day, wanted.time_zone,
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
    audit.record("report_schedule.delete", "report_schedule", id, {.before = schedule_json(*before), .location_id = before->location_id});  // the row is gone
    return {};
}

int fail_stalled_reports(pqxx::transaction_base& tx, core::TimePoint now) {
    return static_cast<int>(
        tx.exec("UPDATE reports SET status = 'failed', error = 'The job that builds this report did not finish.', "
                "updated_at = $1::timestamptz WHERE status = 'building' AND created_at < $2::timestamptz RETURNING id",
                pqxx::params{to_timestamptz(now), to_timestamptz(now - kReportStalledAfter)})
            .size());
}

int start_due_reports(pqxx::transaction_base& tx, std::string_view link_base, core::TimePoint now) {
    int started = 0;
    const auto rows = tx.exec(std::format(
        "SELECT s.id, s.location_id, s.frequency, coalesce(s.weekday, 0), coalesce(s.month_day, 0), s.minute_of_day, s.time_zone, "
        "s.auto_send, s.last_run_on::text, {} FROM report_schedules s JOIN locations l ON l.id = s.location_id "
        "WHERE s.enabled AND l.enabled ORDER BY s.id FOR UPDATE OF s",
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
                                           {.location_id = row[1].as<std::int64_t>(),
                                            .period = domain::period_for(schedule.frequency, run->local_day),
                                            .time_zone = schedule.time_zone,
                                            .schedule_id = id,
                                            .auto_send = row[7].as<bool>()},
                                           link_base);
        if (created) ++started;
    }
    return started;
}

}  // namespace poni::repo
