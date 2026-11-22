#include "jobs/reports.hpp"

#include "core/crypto.hpp"
#include "core/log.hpp"
#include "db/repo_settings.hpp"
#include "domain/prompts.hpp"
#include "domain/report_html.hpp"
#include "jobs/search_keywords.hpp"

#include <format>
#include <stdexcept>

namespace poni::jobs {

namespace asio = boost::asio;

namespace {

// Worth asking Google again in a later attempt of the job.
bool passing(const google::ApiError& error) {
    return error.kind == google::ApiError::Kind::unavailable || error.kind == google::ApiError::Kind::rate_limited;
}

// The bucket is not ours to fix from here: a gateway error, like a model that can't be reached.
std::unexpected<repo::Failure> storage_failure(const google::ApiError& error) {
    return repo::unavailable("storage_failed", std::format("The page could not be put into the public bucket ({}).", error.message));
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
    if (data.reviews.new_reviews == 0 && !data.visibility.available) {
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
    auto work = co_await db_.tx([report_id, now = clock_.now()](pqxx::work& tx) { return repo::load_report_work(tx, report_id, now); });
    if (!work) co_return;  // built, failed or rejected already

    work->input.performance = co_await read_performance(*work, last_attempt);
    auto data = domain::build_report(work->input);
    co_await summarize(data);
    const auto html = domain::render_report(data);

    const bool built = co_await db_.tx([&, now = clock_.now()](pqxx::work& tx) { return repo::store_built_report(tx, report_id, data, html, now); });
    log::info("report built", {{"report_id", report_id}, {"location_id", data.location_id}, {"summary", data.summary.has_value()}});
    if (!built || !work->auto_send) co_return;

    // Its schedule sends by itself. If that can't be done the report waits for a person, with the reason.
    // The reason is written first, so a report is never left as a plain draft when the engine stops
    // or the upload throws between here and the send; sending clears it.
    std::string held = "The engine was to send this report by itself and was interrupted before it did.";
    co_await hold(report_id, held);
    try {
        const auto sent = co_await send(report_id, audit::Actor::system("report-schedule"));
        if (sent) co_return;
        held = sent.error().message;
    } catch (const std::exception& e) {
        // A thrown error is a network or database failure. Not retried: the job has stored its report,
        // and a second upload and email after an unknown outcome is worse than a report that waits.
        held = std::format("Sending failed ({}).", e.what());
    }
    co_await hold(report_id, held);
}

asio::awaitable<void> Reporter::hold(std::int64_t report_id, std::string reason) {
    co_await db_.tx([&, now = clock_.now()](pqxx::work& tx) {
        repo::hold_report(tx, report_id, reason, now);
    });
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
    const int started = co_await db_.tx([now = clock_.now()](pqxx::work& tx) {
        repo::fail_stalled_reports(tx, now);
        return repo::start_due_reports(tx, now);
    });
    if (started > 0) log::info("scheduled reports started", {{"count", started}});
    co_return started;
}

asio::awaitable<repo::Result<std::string>> Reporter::publish(std::string html) {
    using Out = repo::Result<std::string>;
    if (!public_store_) {
        Out none = repo::conflict("no_public_bucket", "No public bucket is configured (PONI_PUBLIC_BUCKET), so a report cannot be shared.");
        co_return none;
    }
    // 32 random bytes: the whole secret of a share link.
    auto token = core::base64url_encode(core::random_bytes(32));
    const auto stored = co_await public_store_->put(
        {.name = repo::share_object_name(token), .content_type = "text/html; charset=utf-8", .body = std::move(html)});
    if (!stored) {
        Out failed = storage_failure(stored.error());
        co_return failed;
    }
    Out out = std::move(token);
    co_return out;
}

asio::awaitable<repo::Result<model::Report>> Reporter::send(std::int64_t report_id, audit::Actor actor) {
    using Out = repo::Result<model::Report>;
    auto html = co_await db_.tx([report_id](pqxx::work& tx) { return repo::check_send(tx, report_id); });
    if (!html) {
        Out refused = std::unexpected(html.error());
        co_return refused;
    }
    const auto token = co_await publish(std::move(*html));
    if (!token) {
        Out refused = std::unexpected(token.error());
        co_return refused;
    }
    Out result = co_await db_.audited(actor, clock_.now(), [&, base = link_base()](pqxx::work& tx, audit::Writer& audit) {
        return repo::mark_report_sent(tx, audit, report_id, *token, base);
    });
    if (!result) {
        // Something changed between the check and now. The page is under a name nobody was given; take it down anyway.
        (void)co_await public_store_->put({.name = repo::share_object_name(*token),
                                           .content_type = "text/html; charset=utf-8",
                                           .body = domain::render_revoked_page()});
    }
    co_return result;
}

asio::awaitable<repo::Result<model::ReportShareLink>> Reporter::create_link(std::int64_t report_id, audit::Actor actor) {
    using Out = repo::Result<model::ReportShareLink>;
    auto html = co_await db_.tx([report_id](pqxx::work& tx) { return repo::shareable_html(tx, report_id); });
    if (!html) {
        Out refused = std::unexpected(html.error());
        co_return refused;
    }
    const auto token = co_await publish(std::move(*html));
    if (!token) {
        Out refused = std::unexpected(token.error());
        co_return refused;
    }
    Out result = co_await db_.audited(actor, clock_.now(), [&, base = link_base()](pqxx::work& tx, audit::Writer& audit) {
        return repo::add_share_link(tx, audit, report_id, *token, base);
    });
    co_return result;
}

asio::awaitable<repo::Result<model::ReportShareLink>> Reporter::revoke_link(std::int64_t link_id, audit::Actor actor) {
    using Out = repo::Result<model::ReportShareLink>;
    const auto ref = co_await db_.tx([link_id](pqxx::work& tx) { return repo::share_link_ref(tx, link_id); });
    if (!ref) {
        Out missing = repo::not_found("share link");
        co_return missing;
    }
    if (!ref->revoked) {
        if (!public_store_) {
            Out none = repo::conflict("no_public_bucket",
                                      "No public bucket is configured (PONI_PUBLIC_BUCKET), so the link's page cannot be replaced.");
            co_return none;
        }
        // The page first: a link is marked revoked only once its address no longer shows the report.
        const auto replaced = co_await public_store_->put({.name = repo::share_object_name(ref->token),
                                                           .content_type = "text/html; charset=utf-8",
                                                           .body = domain::render_revoked_page()});
        if (!replaced) {
            Out failed = storage_failure(replaced.error());
            co_return failed;
        }
    }
    Out revoked = co_await db_.audited(actor, clock_.now(), [&, base = link_base()](pqxx::work& tx, audit::Writer& audit) {
        return repo::revoke_share_link(tx, audit, link_id, base);
    });
    co_return revoked;
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
