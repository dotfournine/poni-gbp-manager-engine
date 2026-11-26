#include "jobs/reports.hpp"

#include "core/crypto.hpp"
#include "core/log.hpp"
#include "db/repo_settings.hpp"
#include "domain/prompts.hpp"
#include "domain/report.hpp"
#include "domain/report_html.hpp"
#include "jobs/search_keywords.hpp"

#include <chrono>
#include <exception>
#include <format>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace poni::jobs {

namespace asio = boost::asio;

namespace {

// The reason of an address the channel threw on. What it threw may say more than a person should read; the log has it.
constexpr std::string_view kNotHandedToGmail = "The engine could not hand this message to Gmail; its log says why.";

// Worth asking Google again in a later attempt of the job.
bool passing(const google::ApiError& error) {
    return error.kind == google::ApiError::Kind::unavailable || error.kind == google::ApiError::Kind::rate_limited;
}

// What a person reads of a call to the bucket that failed, after the sentence that says which: how it failed, never
// the store's words (they may quote an object's name, D-104).
std::string bucket_failure(const storage::ApiError& error) {
    using Kind = storage::ApiError::Kind;
    if (error.kind == Kind::not_configured) return " (the engine has no access token for Cloud Storage)";
    if (error.http_status >= 200 && error.http_status < 300) return " (Cloud Storage's answer could not be read)";
    if (error.http_status != 0) return std::format(" (HTTP {})", error.http_status);
    return {};
}

// One of a discard's two calls to the bucket, `what` saying which ("copied to the archive"). Nothing: it was done, or
// found no original to act on. Otherwise the 502's message, which names neither an object nor the store's words.
asio::awaitable<std::optional<std::string>> page_move_failure(asio::awaitable<storage::Result<void>> step, std::string_view what,
                                                              std::int64_t report_id) {
    std::optional<storage::Result<void>> answer;
    // co_await is not allowed in a catch block: note the error, act after it.
    std::optional<std::string> thrown;
    try {
        answer = co_await std::move(step);
    } catch (const std::exception& e) {
        thrown = e.what();
    }
    std::string how;
    if (thrown) {
        log::warn("report page not moved", {{"report_id", report_id}, {"step", what}, {"error", *thrown}});  // a transport error names no address
        how = " (the connection to Cloud Storage failed)";
    } else if (*answer || answer->error().kind == storage::ApiError::Kind::not_found) {
        co_return std::nullopt;
    } else {
        how = bucket_failure(answer->error());
        log::warn("report page not moved", {{"report_id", report_id}, {"step", what}, {"status", answer->error().http_status}});
    }
    co_return std::format("The report's page could not be {}{}. The report stays \"discarding\"; call again to finish.", what, how);
}

}  // namespace

std::string Reporter::link_base() const { return public_store_ ? public_store_->base_url() : std::string{}; }

asio::awaitable<domain::LocationPerformance> Reporter::read_performance(const repo::ReportWork& work, bool last_attempt) {
    using namespace std::chrono;
    const auto& period = work.input.period;
    const auto from = domain::previous_period(period).start;
    const year_month_day end{period.end};
    const year_month month = end.year() / end.month();
    const auto& location = work.location;

    domain::LocationPerformance out;
    if (!location.accessible) {
        out.error = "access to the location is lost";
        co_return out;
    }
    auto days = co_await performance_.daily_metrics(location.gbp_location_id, from, period.end);
    if (!days) {
        log::warn("report: daily metrics not read", {{"location_id", location.id}, {"error", days.error().message}});
        // An outage may pass: let the job try again, except on its last attempt.
        if (passing(days.error()) && !last_attempt) {
            throw std::runtime_error(std::format("the Performance API gave no answer: {}", days.error().message));
        }
        out.error = days.error().message;
        co_return out;
    }
    out.days = std::move(*days);

    // Keywords are monthly. The month the period ends in, or the one before while Google has nothing for it yet.
    auto keywords = co_await newest_search_keywords(performance_, location.gbp_location_id, month, 2);
    if (keywords) {
        out.keywords_month = std::move(keywords->month);
        out.keywords = std::move(keywords->keywords);
    } else {
        log::warn("report: search keywords not read", {{"location_id", location.id}, {"error", keywords.error().message}});
    }
    co_return out;
}

asio::awaitable<void> Reporter::summarize(domain::ReportData& data) {
    const auto has = [&](std::string_view key) { return domain::has_section(data.sections, key); };
    // A report without the section asks no model and says nothing about it.
    if (!has("summary")) co_return;
    // The summary starts from the review figures or from visibility: topics and the rest come from the same reviews.
    const bool has_figures = (has("at_a_glance") || has("reviews")) && data.reviews.new_reviews > 0;
    const bool has_visibility = has("visibility") && data.visibility.available;
    if (!has_figures && !has_visibility) {
        data.summary_note = "There was nothing to summarize.";
        co_return;
    }
    const auto settings = co_await db_.tx([](pqxx::work& tx) { return repo::get_ai_settings(tx); });
    if (!settings.enabled) {
        data.summary_note = "No summary: the models are switched off.";
        co_return;
    }
    if (!llm_.configured(settings.summary_provider)) {
        data.summary_note = std::format("No summary: the model provider '{}' is not configured.", settings.summary_provider);
        co_return;
    }
    llm::ChatRequest request;
    request.task = "summary";
    request.location_id = data.location_id;
    request.provider = settings.summary_provider;
    request.model = settings.summary_model;
    request.messages = domain::summary_messages(data, domain::make_nonce());
    request.max_tokens = settings.max_output_tokens;
    request.extras = repo::extras_for(settings, settings.summary_provider);
    const auto answer = co_await llm_.chat_json(std::move(request));
    if (!answer) {
        // llm::Client retried what is worth retrying. A report without a summary is still a report.
        data.summary_note = std::format("No summary: the model gave no answer ({}).", llm::to_string(answer.error().kind));
        co_return;
    }
    auto summary = domain::parse_summary_answer(answer->json);
    if (!summary) {
        data.summary_note = std::format("No summary: {}.", summary.error());
        co_return;
    }
    data.summary = std::move(*summary);
}

asio::awaitable<void> Reporter::build_once(std::int64_t report_id, bool last_attempt) {
    if (!public_store_) {
        // No report is started without a bucket; this is for a row that exists anyway. Not retried.
        co_await db_.tx([report_id, now = clock_.now()](pqxx::work& tx) {
            repo::fail_report(tx, report_id, "No public bucket is configured (PONI_PUBLIC_BUCKET), so the report's page has nowhere to go.", now);
        });
        co_return;
    }
    auto work = co_await db_.tx([report_id, now = clock_.now()](pqxx::work& tx) { return repo::load_report_work(tx, report_id, now); });
    if (!work) co_return;  // built, failed or discarded already

    // Without the visibility section Google's Performance API is not asked at all.
    if (domain::has_section(work->input.sections, "visibility")) work->input.performance = co_await read_performance(*work, last_attempt);
    auto data = domain::build_report(work->input);
    co_await summarize(data);
    auto html = domain::render_report(data);

    // The page is named on the row right before its upload, and only while the report is still building: a run
    // that was overtaken (its job started again after the queue's lock ran out, or the stalled-report sweep failed
    // the report meanwhile) puts nothing over the page of a ready report and none back for a discarded one. That
    // leaves only the instant of the upload itself.
    const auto name = co_await db_.tx([report_id](pqxx::work& tx) {
        return repo::name_report_page(tx, report_id, std::format("r/{}.{}", core::random_token(), domain::kReportPageExtension));
    });
    if (!name) co_return;
    const auto stored = co_await public_store_->put(
        {.name = *name, .content_type = std::string(domain::kReportPageContentType), .body = std::move(html)});
    if (!stored) {
        // What a build throws reaches the log, the job, the report and an email: neither Google's message nor the
        // object's name goes into it.
        throw std::runtime_error(std::format("The report's page could not be put into the public bucket.{}", bucket_failure(stored.error())));
    }

    auto mail = co_await db_.tx(
        [&, base = link_base(), now = clock_.now()](pqxx::work& tx) { return repo::store_built_report(tx, report_id, data, base, now); });
    log::info("report built", {{"report_id", report_id}, {"location_id", data.location_id}, {"summary", data.summary.has_value()}});
    if (!mail) co_return;

    // Its schedule sends it, once: `done` is stored already, so a failure here is logged and never tried again,
    // and the report the job stored stands. Only the last transaction of a send can throw.
    std::optional<std::string> failure;
    try {
        // What came of each address is in the rows, and the counts in the audit entry `report.send`.
        static_cast<void>(co_await deliver(report_id, std::move(*mail), audit::Actor::system("report-schedule")));
    } catch (const std::exception& e) {
        failure = e.what();
    }
    if (failure) log::warn("report not recorded after its schedule's send", {{"report_id", report_id}, {"error", *failure}});
}

asio::awaitable<repo::Result<model::Report>> Reporter::send(std::int64_t report_id, std::optional<std::vector<std::string>> only,
                                                          audit::Actor actor) {
    // The row's lock ends with this transaction: none is open while the emails go out.
    auto mail = co_await db_.tx(
        [report_id, &only, base = link_base()](pqxx::work& tx) {
            return repo::prepare_report_send(tx, report_id, only, base, repo::SendFor::person);
        });
    if (!mail) co_return std::unexpected(mail.error());
    auto sent = co_await deliver(report_id, std::move(*mail), std::move(actor));
    co_return sent;
}

asio::awaitable<repo::Result<model::Report>> Reporter::deliver(std::int64_t report_id, repo::ReportMail mail, audit::Actor actor) {
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(clock_.now().time_since_epoch()).count();
    // New for every send: a second send of the same report to the same address is a second email and a second row.
    const auto key = std::format("report:{}:send:{}-{}", report_id, micros, core::random_token(6));
    std::vector<repo::ReportDelivery> outcomes;
    outcomes.reserve(mail.recipients.size());
    for (const auto& address : mail.recipients) {
        repo::ReportDelivery outcome{.recipient = address};
        // co_await is not allowed in a catch block: note the error, act after it.
        std::optional<std::string> thrown;
        try {
            const auto sent = co_await email_.send({.recipient = address, .subject = mail.subject, .body = mail.body});
            outcome.sent = sent.has_value();
            if (!sent) outcome.reason = sent.error().message;  // Gmail's own message: the channel puts no secret in it
        } catch (const std::exception& e) {
            thrown = e.what();
        }
        if (thrown) {
            outcome.reason = std::string(kNotHandedToGmail);
            log::warn("report email not sent", {{"report_id", report_id}, {"error", *thrown}});  // no address
        }
        outcomes.push_back(std::move(outcome));
    }
    auto recorded = co_await db_.audited(std::move(actor), clock_.now(), [&, base = link_base()](pqxx::work& tx, audit::Writer& audit) {
        return repo::record_report_send(tx, audit, report_id, mail, outcomes, key, base);
    });
    co_return recorded;
}

asio::awaitable<repo::Result<model::Report>> Reporter::discard(std::int64_t report_id, audit::Actor actor) {
    auto begun = co_await db_.audited(std::move(actor), clock_.now(),
                                      [report_id, token = core::random_token(), base = link_base()](pqxx::work& tx, audit::Writer& audit) {
                                          return repo::begin_discard(tx, audit, report_id, token, base);
                                      });
    if (!begun) co_return std::unexpected(begun.error());
    // Nothing to move: the report had no page. Otherwise there is a bucket, or begin_discard refused.
    if (const auto& work = *begun) {
        // Copy first. The original is deleted only after this call's copy succeeded or found no original, so a step
        // that fails loses no page; a later call starts again at the copy, which replaces the archive with the same bytes.
        auto copy = public_store_->copy(work->page_object, work->archive_object);
        auto failure = co_await page_move_failure(std::move(copy), "copied to the archive", report_id);
        if (!failure) {
            auto remove = public_store_->remove(work->page_object);
            failure = co_await page_move_failure(std::move(remove), "deleted after its copy to the archive", report_id);
        }
        if (failure) co_return repo::unavailable("storage_failed", std::move(*failure));
    }
    // Two calls at once both get here; the second's update changes nothing, and both answer the discarded report.
    auto discarded = co_await db_.tx([report_id, base = link_base(), now = clock_.now()](pqxx::work& tx) {
        repo::finish_discard(tx, report_id, now);
        return repo::get_report(tx, report_id, base);
    });
    co_return std::move(discarded.value());
}

asio::awaitable<void> Reporter::build(std::int64_t report_id, bool last_attempt) {
    // co_await is not allowed in a catch block: note the error, act after it.
    std::optional<std::string> failure;
    try {
        co_await build_once(report_id, last_attempt);
    } catch (const std::exception& e) {
        failure = e.what();
    }
    if (!failure) co_return;
    if (last_attempt) {
        co_await db_.tx([&, now = clock_.now()](pqxx::work& tx) {
            repo::fail_report(tx, report_id, *failure, now);
        });
    }
    throw std::runtime_error(*failure);
}

asio::awaitable<int> Reporter::start_due() {
    const int started = co_await db_.tx([base = link_base(), now = clock_.now()](pqxx::work& tx) {
        repo::fail_stalled_reports(tx, now);
        // Without a bucket a report's page has nowhere to go: no schedule starts one.
        return base.empty() ? 0 : repo::start_due_reports(tx, base, now);
    });
    if (started > 0) log::info("scheduled reports started", {{"count", started}});
    co_return started;
}

void install(Registry& registry, Reporter& reporter) {
    registry.add(std::string(repo::kBuildReportKind), [&reporter](const Job& job) -> asio::awaitable<void> {
        co_await reporter.build(job.payload_id("report_id"), job.last_attempt());
    });
    // One attempt: the next slot looks at the same schedules again.
    registry.every(std::string(kReportScheduleKind), kReportScheduleTick, 1,
                   [&reporter](const Job&) -> asio::awaitable<void> { co_await reporter.start_due(); });
}

}  // namespace poni::jobs
