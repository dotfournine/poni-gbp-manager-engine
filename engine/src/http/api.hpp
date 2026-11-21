// The dashboard's API: clients, locations, the review inbox, the audit log
// and the system status. Paths, parameters and bodies are defined in
// api/openapi.yaml; change that file first.
#pragma once

#include "core/clock.hpp"
#include "db/database.hpp"
#include "http/router.hpp"

namespace poni::http {

struct ApiOptions {
    // Shown on the status page: whether Pub/Sub notifications are switched on.
    bool pubsub_enabled = false;
    // Shown on the status page: the engine has an LLM address and key.
    // The model providers that have an address and a key (D-059).
    std::vector<std::string> llm_providers;
    // How many job workers the engine runs: that many cases of a go-live test are in the queue at a time.
    int job_workers = 2;
    // Shown on the status page: the address notifications are sent from.
    std::string email_sender;
    // Shown on the status page: the engine has the OAuth client and the key
    // that the "Connect" buttons need (http/connect.hpp).
    bool connect_configured = false;
    // The engine works against the local mock of Google, which has no posts
    // (D-088 point 13): the routes that would call Google about posts refuse.
    bool google_mock = false;
};

} // namespace poni::http

namespace poni::jobs {
class ReviewPipeline;
class Reporter;
class PostAdvisor;
}

namespace poni::storage {
class ObjectStore;
}

namespace poni::http {

// Everything passed by reference must outlive the router.
// `pipeline` writes a draft again when staff ask; `reporter` sends and shares reports;
// `advisor` asks the model for ideas and texts for local posts.
// `public_store` takes the photo of a local post; without one (no PONI_PUBLIC_BUCKET) a post can have no media.
void add_api_routes(Router& router, db::Database& db, const core::Clock& clock, ApiOptions options, jobs::ReviewPipeline& pipeline,
                    jobs::Reporter& reporter, storage::ObjectStore* public_store, jobs::PostAdvisor& advisor);

// The context every handler file gets (http/api_support.hpp).
struct Api;

// Each called by add_api_routes:
// analysis, drafts, the settings they are made with, and LLM usage (http/api_analysis.cpp);
void add_analysis_routes(Router& router, const Api& api);
// the approval queue, escalations, notification rules and the delivery log,
// and the posting and sending switches (http/api_replies.cpp);
void add_reply_routes(Router& router, const Api& api, jobs::ReviewPipeline& pipeline);
// auto-posting per location, its pauses, the list of what was posted without
// a person, and taking a scheduled reply back (http/api_auto_posting.cpp);
void add_auto_posting_routes(Router& router, const Api& api);
// the go-live test (http/api_go_live.cpp);
void add_go_live_routes(Router& router, const Api& api);
// client reports, their share links and schedules (http/api_reports.cpp);
void add_report_routes(Router& router, const Api& api, jobs::Reporter& reporter);
// local posts (http/api_local_posts.cpp).
void add_local_post_routes(Router& router, const Api& api, storage::ObjectStore* public_store, jobs::PostAdvisor& advisor);

}  // namespace poni::http
