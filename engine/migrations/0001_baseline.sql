-- The schema as migrations 0001 to 0026 left it, in one file (D-091 point 10),
-- with the two settings rows they left. A database that had all 26 is marked
-- as having this one by `poni-tools migrate`. From here on the rule holds
-- again: an applied migration is never edited; the next one is 0002.
--
-- Columns are in the order the old migrations added them, and a constraint
-- keeps the name they gave it, so that this file and the old chain make the
-- same schema. Times come from the engine's clock: no column defaults to
-- now(). The knowledge base (kb_*) is written by `poni-tools seed` only
-- (D-078); nothing here puts a row in it.

-- ---- people, sessions and the audit log --------------------------------------

CREATE TABLE users (
    id            bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    google_sub    text        NOT NULL UNIQUE,
    email         text        NOT NULL,
    name          text        NOT NULL DEFAULT '',
    picture_url   text,
    status        text        NOT NULL DEFAULT 'active' CHECK (status IN ('active', 'disabled')),
    created_at    timestamptz NOT NULL,
    last_login_at timestamptz NOT NULL
);

-- A session is known by the hash of its token only.
CREATE TABLE sessions (
    token_hash   text        PRIMARY KEY,
    user_id      bigint      NOT NULL REFERENCES users (id),
    csrf_token   text        NOT NULL,
    created_at   timestamptz NOT NULL,
    last_seen_at timestamptz NOT NULL,
    expires_at   timestamptz NOT NULL,
    user_agent   text        NOT NULL DEFAULT '',
    ip           text        NOT NULL DEFAULT ''
);
CREATE INDEX sessions_user_idx ON sessions (user_id);
CREATE INDEX sessions_expires_idx ON sessions (expires_at);

CREATE TABLE audit_log (
    id            bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    occurred_at   timestamptz NOT NULL,
    actor_type    text        NOT NULL CHECK (actor_type IN ('user', 'system', 'anonymous')),
    actor_user_id bigint      REFERENCES users (id),
    actor         text        NOT NULL,
    action        text        NOT NULL,
    object_type   text        NOT NULL DEFAULT '',
    object_id     text        NOT NULL DEFAULT '',
    before        jsonb,
    after         jsonb,
    ip            text
);
CREATE INDEX audit_log_time_idx ON audit_log (occurred_at DESC, id DESC);
CREATE INDEX audit_log_object_idx ON audit_log (object_type, object_id);
CREATE INDEX audit_log_action_idx ON audit_log (action);

-- Nobody changes or removes an audit entry, not even the engine.
CREATE FUNCTION audit_log_append_only() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    RAISE EXCEPTION 'audit_log is append-only';
END;
$$;
CREATE TRIGGER audit_log_no_update_or_delete BEFORE UPDATE OR DELETE ON audit_log
    FOR EACH ROW EXECUTE FUNCTION audit_log_append_only();
CREATE TRIGGER audit_log_no_truncate BEFORE TRUNCATE ON audit_log
    FOR EACH STATEMENT EXECUTE FUNCTION audit_log_append_only();

-- ---- the engine's own state ---------------------------------------------------

-- Encrypted secrets (D-043): the refresh tokens of the connected accounts.
CREATE TABLE credentials (
    name       text        PRIMARY KEY,
    ciphertext text        NOT NULL,
    key_id     text        NOT NULL,
    created_at timestamptz NOT NULL,
    updated_at timestamptz NOT NULL
);

-- What the status page shows for each outside service.
CREATE TABLE integration_status (
    name          text        PRIMARY KEY,
    last_ok_at    timestamptz,
    last_error    text,
    last_error_at timestamptz,
    detail        jsonb       NOT NULL DEFAULT '{}',
    updated_at    timestamptz NOT NULL
);

-- One row. Posting and email sending are off until a person switches them on
-- (D-066); the engine only ever pauses auto-posting (D-072). google_target is
-- written at the engine's first start (D-087).
CREATE TABLE system_settings (
    id                     boolean     PRIMARY KEY DEFAULT true CHECK (id),
    posting_enabled        boolean     NOT NULL DEFAULT false,
    email_sending_enabled  boolean     NOT NULL DEFAULT false,
    updated_at             timestamptz NOT NULL,
    auto_post_paused_at    timestamptz,
    auto_post_pause_reason text        CHECK (auto_post_pause_reason IN ('checks_failing', 'post_unconfirmed', 'post_refused')),
    auto_post_pause_detail text,
    auto_post_resumed_at   timestamptz,
    google_target          text        CHECK (google_target IN ('google', 'mock')),
    CONSTRAINT system_settings_auto_post_pause_check CHECK ((auto_post_paused_at IS NULL) = (auto_post_pause_reason IS NULL))
);

-- One row: each model task's provider and model (D-059), and the analyzer's settings.
CREATE TABLE ai_settings (
    id                         boolean     PRIMARY KEY DEFAULT true CHECK (id),
    enabled                    boolean     NOT NULL DEFAULT true,
    analyzer_model             text        NOT NULL CHECK (btrim(analyzer_model) <> ''),
    generator_model            text        NOT NULL CHECK (btrim(generator_model) <> ''),
    analyzer_temperature       real        NOT NULL DEFAULT 0 CHECK (analyzer_temperature BETWEEN 0 AND 2),
    max_output_tokens          integer     NOT NULL DEFAULT 1200 CHECK (max_output_tokens BETWEEN 100 AND 8000),
    request_extras             jsonb       NOT NULL DEFAULT '{}',
    batch_size                 integer     NOT NULL DEFAULT 10 CHECK (batch_size BETWEEN 0 AND 100),
    updated_at                 timestamptz NOT NULL,
    analyzer_provider          text        NOT NULL DEFAULT 'dashscope',
    generator_provider         text        NOT NULL DEFAULT 'dashscope',
    analyzer_send_temperature  boolean     NOT NULL DEFAULT true,
    generator_send_temperature boolean     NOT NULL DEFAULT true,
    judge_provider             text        NOT NULL DEFAULT 'anthropic',
    judge_model                text        NOT NULL DEFAULT 'claude-opus-5-5' CHECK (btrim(judge_model) <> ''),
    summary_provider           text        NOT NULL,
    summary_model              text        NOT NULL CHECK (btrim(summary_model) <> ''),
    posts_provider             text        NOT NULL,
    posts_model                text        NOT NULL CHECK (btrim(posts_model) <> '')
);

-- The job queue: at-least-once delivery, an idempotency key per kind.
CREATE TABLE jobs (
    id              bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    kind            text        NOT NULL,
    payload         jsonb       NOT NULL DEFAULT '{}',
    status          text        NOT NULL DEFAULT 'queued' CHECK (status IN ('queued', 'running', 'succeeded', 'failed')),
    run_at          timestamptz NOT NULL,
    attempts        integer     NOT NULL DEFAULT 0,
    max_attempts    integer     NOT NULL DEFAULT 5 CHECK (max_attempts >= 1),
    idempotency_key text,
    locked_at       timestamptz,
    locked_by       text,
    last_error      text,
    created_at      timestamptz NOT NULL,
    updated_at      timestamptz NOT NULL,
    finished_at     timestamptz
);
CREATE INDEX jobs_ready_idx ON jobs (run_at, id) WHERE status = 'queued';
CREATE INDEX jobs_running_idx ON jobs (locked_at) WHERE status = 'running';
CREATE INDEX jobs_finished_idx ON jobs (finished_at) WHERE status = 'succeeded';
CREATE UNIQUE INDEX jobs_idempotency_key_idx ON jobs (kind, idempotency_key) WHERE idempotency_key IS NOT NULL;

-- ---- clients and their locations -------------------------------------------

-- industry_key names an industry of the knowledge base by key, without a
-- foreign key (D-084).
CREATE TABLE clients (
    id           bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    name         text        NOT NULL CHECK (btrim(name) <> ''),
    notes        text        NOT NULL DEFAULT '',
    archived     boolean     NOT NULL DEFAULT false,
    created_at   timestamptz NOT NULL,
    updated_at   timestamptz NOT NULL,
    industry_key text,
    analyze_from date,
    audience     text        NOT NULL DEFAULT '' CHECK (char_length(audience) <= 500)
);
CREATE UNIQUE INDEX clients_name_idx ON clients (lower(name));

-- A location as Google lists it, linked to a client or not. Auto-posting is
-- off until a person switches it on, per location and per star rating (D-070).
CREATE TABLE locations (
    id                             bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    gbp_location_id                text        NOT NULL UNIQUE,
    gbp_account_id                 text        NOT NULL,
    client_id                      bigint      REFERENCES clients (id),
    title                          text        NOT NULL,
    store_code                     text,
    primary_category               text,
    address                        text,
    locality                       text,
    administrative_area            text,
    postal_code                    text,
    region_code                    text,
    phone                          text,
    website_uri                    text,
    place_id                       text,
    maps_uri                       text,
    access_status                  text        NOT NULL DEFAULT 'active' CHECK (access_status IN ('active', 'lost')),
    access_lost_at                 timestamptz,
    missed_discoveries             integer     NOT NULL DEFAULT 0,
    raw                            jsonb       NOT NULL,
    first_seen_at                  timestamptz NOT NULL,
    last_seen_at                   timestamptz NOT NULL,
    updated_at                     timestamptz NOT NULL,
    review_poll_interval_s         integer     NOT NULL DEFAULT 120 CHECK (review_poll_interval_s >= 60),
    next_review_poll_at            timestamptz,
    next_full_sync_at              timestamptz,
    reviews_polled_at              timestamptz,
    reviews_full_synced_at         timestamptz,
    last_full_sync                 jsonb,
    last_sync_error                text,
    last_sync_error_at             timestamptz,
    posting_enabled                boolean     NOT NULL DEFAULT true,
    auto_post_enabled              boolean     NOT NULL DEFAULT false,
    auto_post_five_star            boolean     NOT NULL DEFAULT false,
    auto_post_four_star            boolean     NOT NULL DEFAULT false,
    auto_post_daily_cap            integer     NOT NULL DEFAULT 100 CHECK (auto_post_daily_cap BETWEEN 1 AND 1000),
    time_zone                      text,
    auto_post_paused_at            timestamptz,
    auto_post_pause_reason         text        CHECK (auto_post_pause_reason IN ('checks_failing', 'post_unconfirmed', 'post_refused')),
    auto_post_pause_detail         text,
    auto_post_resumed_at           timestamptz,
    auto_post_pause_after_failures integer     NOT NULL DEFAULT 3 CHECK (auto_post_pause_after_failures BETWEEN 1 AND 20),
    local_posts_synced_at          timestamptz,
    local_posts_sync_error         text,
    local_posts_sync_error_at      timestamptz,
    CONSTRAINT locations_auto_post_pause_check CHECK ((auto_post_paused_at IS NULL) = (auto_post_pause_reason IS NULL))
);
CREATE INDEX locations_client_idx ON locations (client_id);
CREATE INDEX locations_poll_idx ON locations (next_review_poll_at) WHERE client_id IS NOT NULL AND access_status = 'active';

-- A linked location's voice, localization and policy, each complete (D-093):
-- what its replies are written with is exactly this row.
CREATE TABLE reply_settings (
    location_id  bigint      PRIMARY KEY REFERENCES locations (id),
    voice        jsonb       NOT NULL CHECK (jsonb_typeof(voice) = 'object'),
    localization jsonb       NOT NULL CHECK (jsonb_typeof(localization) = 'object'),
    policy       jsonb       NOT NULL CHECK (jsonb_typeof(policy) = 'object'),
    created_at   timestamptz NOT NULL,
    updated_at   timestamptz NOT NULL
);

-- ---- reviews, analysis, drafts and posting ------------------------------------

-- A review as Google returned it last. removed_at is set only after Google
-- answered NOT_FOUND for it (D-037).
CREATE TABLE reviews (
    id                 bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    location_id        bigint      NOT NULL REFERENCES locations (id),
    gbp_review_id      text        NOT NULL,
    gbp_name           text        NOT NULL,
    rating             smallint    CHECK (rating BETWEEN 1 AND 5),
    comment            text,
    original_text      text,
    translated_text    text,
    language           text,
    reviewer_name      text,
    reviewer_photo_url text,
    reviewer_anonymous boolean     NOT NULL DEFAULT false,
    created_time       timestamptz NOT NULL,
    updated_time       timestamptz NOT NULL,
    reply_text         text,
    reply_updated_time timestamptz,
    reply_state        text,
    reply_origin       text        CHECK (reply_origin IN ('external', 'system')),
    removed_at         timestamptz,
    raw                jsonb       NOT NULL,
    first_seen_at      timestamptz NOT NULL,
    last_seen_at       timestamptz NOT NULL,
    last_changed_at    timestamptz NOT NULL,
    ignored_at         timestamptz,
    ignored_by         bigint      REFERENCES users (id),
    ignore_reason      text        NOT NULL DEFAULT '',
    UNIQUE (location_id, gbp_review_id),
    CONSTRAINT reviews_ignored_check CHECK ((ignored_at IS NULL) = (ignored_by IS NULL))
);
CREATE INDEX reviews_location_created_idx ON reviews (location_id, created_time DESC);
CREATE INDEX reviews_created_idx ON reviews (created_time DESC, id DESC);
CREATE INDEX reviews_unreplied_idx ON reviews (location_id) WHERE reply_text IS NULL AND removed_at IS NULL;
CREATE INDEX reviews_ignored_idx ON reviews (location_id) WHERE ignored_at IS NOT NULL;

-- What the sync saw change on a review (D-039); not the audit log.
CREATE TABLE review_events (
    id          bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    review_id   bigint      NOT NULL REFERENCES reviews (id),
    kind        text        NOT NULL CHECK (kind IN ('discovered', 'review_edited', 'reply_added', 'reply_changed',
                                                     'reply_removed', 'removed', 'restored')),
    source      text        NOT NULL CHECK (source IN ('poll', 'full_sync', 'notification', 'posting')),
    occurred_at timestamptz NOT NULL,
    detail      jsonb       NOT NULL DEFAULT '{}'
);
CREATE INDEX review_events_review_idx ON review_events (review_id, id);
CREATE INDEX review_events_time_idx ON review_events (occurred_at DESC);

CREATE TABLE review_analyses (
    id                     bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    review_id              bigint      NOT NULL REFERENCES reviews (id),
    review_updated_time    timestamptz NOT NULL,
    status                 text        NOT NULL CHECK (status IN ('ok', 'failed')),
    error                  text,
    language               text        NOT NULL,
    script                 text        NOT NULL,
    sentiment              text        NOT NULL CHECK (sentiment IN ('positive', 'mixed', 'neutral', 'negative', 'none')),
    contains_complaint     boolean     NOT NULL,
    topics                 jsonb       NOT NULL DEFAULT '[]',
    items                  jsonb       NOT NULL DEFAULT '[]',
    severity_flags         jsonb       NOT NULL DEFAULT '[]',
    manipulation_suspected boolean     NOT NULL,
    manipulation_sources   jsonb       NOT NULL DEFAULT '[]',
    manipulation_evidence  text        NOT NULL DEFAULT '',
    summary_en             text        NOT NULL DEFAULT '',
    industry_key           text        NOT NULL,
    model                  text,
    prompt_version         text        NOT NULL,
    raw                    jsonb,
    created_at             timestamptz NOT NULL
);
CREATE INDEX review_analyses_review_idx ON review_analyses (review_id, id DESC);

-- An escalation: one per review, opened by a severity flag.
CREATE TABLE cases (
    id             bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    review_id      bigint      NOT NULL UNIQUE REFERENCES reviews (id),
    kind           text        NOT NULL DEFAULT 'escalation' CHECK (kind = 'escalation'),
    status         text        NOT NULL DEFAULT 'open' CHECK (status IN ('open', 'closed')),
    opened_by      text        NOT NULL CHECK (opened_by = 'severity_flag'),
    analysis_id    bigint      REFERENCES review_analyses (id),
    severity_flags jsonb       NOT NULL DEFAULT '[]',
    assignee_id    bigint      REFERENCES users (id),
    assigned_at    timestamptz,
    close_reason   text        CHECK (close_reason IN ('resolved', 'review_removed', 'review_updated', 'no_response', 'dropped')),
    close_note     text        NOT NULL DEFAULT '',
    closed_by      bigint      REFERENCES users (id),
    closed_at      timestamptz,
    opened_at      timestamptz NOT NULL,
    updated_at     timestamptz NOT NULL,
    CONSTRAINT cases_check CHECK ((status = 'closed') = (close_reason IS NOT NULL))
);
CREATE INDEX cases_status_idx ON cases (status, opened_at DESC);
CREATE INDEX cases_assignee_idx ON cases (assignee_id) WHERE status = 'open';

-- A reply the writer drafted, and what became of it. One live draft per review.
CREATE TABLE reply_drafts (
    id                               bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    review_id                        bigint      NOT NULL REFERENCES reviews (id),
    analysis_id                      bigint      NOT NULL REFERENCES review_analyses (id),
    review_updated_time              timestamptz NOT NULL,
    status                           text        NOT NULL DEFAULT 'draft'
                                                 CHECK (status IN ('draft', 'approved', 'scheduled', 'posting', 'posted', 'published',
                                                                   'rejected', 'cancelled', 'superseded')),
    status_reason                    text,
    route                            text        NOT NULL CHECK (route IN ('auto_post', 'approval_queue', 'escalate')),
    route_reasons                    jsonb       NOT NULL DEFAULT '[]',
    mode                             text        NOT NULL CHECK (mode IN ('positive', 'complaint', 'severity', 'rating_only', 'rating_only_low')),
    text                             text        NOT NULL,
    language                         text        NOT NULL,
    script                           text,
    generation_ok                    boolean     NOT NULL,
    generator_manipulation_suspected boolean     NOT NULL DEFAULT false,
    notes_for_staff                  text        NOT NULL DEFAULT '',
    checks                           jsonb       NOT NULL DEFAULT '[]',
    model                            text        NOT NULL,
    prompt_version                   text        NOT NULL,
    inputs                           jsonb       NOT NULL,
    created_at                       timestamptz NOT NULL,
    updated_at                       timestamptz NOT NULL,
    generated_text                   text        NOT NULL,
    instructions                     text,
    created_by                       bigint      REFERENCES users (id),
    edited_by                        bigint      REFERENCES users (id),
    edited_at                        timestamptz,
    decided_by                       bigint      REFERENCES users (id),
    decided_at                       timestamptz,
    post_at                          timestamptz,
    scheduled_at                     timestamptz,
    policy_version                   text,
    policy_snapshot                  jsonb,
    CONSTRAINT reply_drafts_scheduled_check CHECK (status <> 'scheduled' OR (post_at IS NOT NULL AND policy_version IS NOT NULL))
);
CREATE INDEX reply_drafts_review_idx ON reply_drafts (review_id, id DESC);
CREATE INDEX reply_drafts_route_idx ON reply_drafts (route, created_at DESC) WHERE status = 'draft';
CREATE UNIQUE INDEX reply_drafts_live_idx ON reply_drafts (review_id)
    WHERE status IN ('draft', 'scheduled', 'approved', 'posting', 'posted');

-- Every PUT of a reply, approved by a person or by the auto-post policy,
-- never both (D-062, D-069). A post whose outcome is open blocks the review.
CREATE TABLE reply_posts (
    id              bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    review_id       bigint      NOT NULL REFERENCES reviews (id),
    draft_id        bigint      NOT NULL REFERENCES reply_drafts (id),
    text            text        NOT NULL,
    approved_by     bigint      REFERENCES users (id),
    status          text        NOT NULL CHECK (status IN ('posting', 'posted', 'published', 'failed', 'unconfirmed', 'superseded')),
    error           text,
    started_at      timestamptz NOT NULL,
    posted_at       timestamptz,
    published_at    timestamptz,
    updated_at      timestamptz NOT NULL,
    policy_version  text,
    policy_snapshot jsonb,
    CONSTRAINT reply_posts_authority_check CHECK ((approved_by IS NULL) <> (policy_version IS NULL))
);
CREATE INDEX reply_posts_review_idx ON reply_posts (review_id, id DESC);
CREATE INDEX reply_posts_draft_idx ON reply_posts (draft_id, id DESC);
CREATE UNIQUE INDEX reply_posts_open_idx ON reply_posts (review_id) WHERE status IN ('posting', 'posted');
CREATE INDEX reply_posts_auto_idx ON reply_posts (started_at DESC) WHERE policy_version IS NOT NULL;

-- ---- notifications ------------------------------------------------------------

-- A client_report rule must name a client: one client's report must not reach
-- another's recipients.
CREATE TABLE notification_rules (
    id         bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    event_type text        NOT NULL CHECK (event_type IN ('escalation', 'approval_needed', 'system_problem', 'client_report')),
    client_id  bigint      REFERENCES clients (id),
    channel    text        NOT NULL DEFAULT 'email' CHECK (channel = 'email'),
    recipients jsonb       NOT NULL DEFAULT '[]',
    enabled    boolean     NOT NULL DEFAULT true,
    created_at timestamptz NOT NULL,
    updated_at timestamptz NOT NULL,
    CONSTRAINT notification_rules_client_report_check CHECK (event_type <> 'client_report' OR client_id IS NOT NULL)
);
CREATE UNIQUE INDEX notification_rules_scope_idx ON notification_rules (event_type, channel, coalesce(client_id, 0));

-- One message to one recipient; the event's key makes a repeat the same row.
CREATE TABLE notification_deliveries (
    id            bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    event_type    text        NOT NULL CHECK (event_type IN ('escalation', 'approval_needed', 'system_problem', 'test', 'client_report')),
    event_key     text        NOT NULL,
    client_id     bigint      REFERENCES clients (id),
    location_id   bigint      REFERENCES locations (id),
    review_id     bigint      REFERENCES reviews (id),
    rule_id       bigint      REFERENCES notification_rules (id) ON DELETE SET NULL,
    channel       text        NOT NULL,
    recipient     text        NOT NULL,
    subject       text        NOT NULL,
    body          text        NOT NULL,
    link_path     text        NOT NULL DEFAULT '',
    status        text        NOT NULL DEFAULT 'pending' CHECK (status IN ('pending', 'sent', 'failed', 'not_sent')),
    status_reason text,
    attempts      integer     NOT NULL DEFAULT 0,
    created_at    timestamptz NOT NULL,
    sent_at       timestamptz,
    updated_at    timestamptz NOT NULL,
    UNIQUE (event_key, channel, recipient)
);
CREATE INDEX notification_deliveries_status_idx ON notification_deliveries (status, created_at DESC);
CREATE INDEX notification_deliveries_time_idx ON notification_deliveries (created_at DESC, id DESC);

-- ---- the go-live test and model calls -------------------------------------------

-- A test of one configuration (D-074): its fingerprint, the inputs it was
-- computed from, and the policy width its cases were routed with.
CREATE TABLE go_live_runs (
    id                    bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    client_id             bigint      NOT NULL REFERENCES clients (id),
    fingerprint           text        NOT NULL,
    fingerprint_inputs    jsonb       NOT NULL,
    location_ids          jsonb       NOT NULL,
    status                text        NOT NULL CHECK (status IN ('running', 'passed', 'failed', 'inconclusive', 'cancelled')),
    summary               text,
    auto_post_positive    boolean     NOT NULL,
    auto_post_rating_only boolean     NOT NULL,
    auto_post_min_stars   integer     NOT NULL,
    corpus_version        text        NOT NULL,
    repeats               integer     NOT NULL CHECK (repeats BETWEEN 1 AND 10),
    parallel              integer     NOT NULL DEFAULT 2 CHECK (parallel BETWEEN 1 AND 64),
    real_reviews          integer     NOT NULL DEFAULT 0,
    started_by            bigint      REFERENCES users (id),
    started_at            timestamptz NOT NULL,
    finished_at           timestamptz,
    kb_version            text,
    CONSTRAINT go_live_runs_check CHECK ((status = 'running') = (finished_at IS NULL))
);
CREATE INDEX go_live_runs_client_idx ON go_live_runs (client_id, id DESC);
CREATE INDEX go_live_runs_fingerprint_idx ON go_live_runs (fingerprint, id DESC);
CREATE UNIQUE INDEX go_live_runs_running_idx ON go_live_runs (fingerprint) WHERE status = 'running';

CREATE TABLE go_live_cases (
    id          bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    run_id      bigint      NOT NULL REFERENCES go_live_runs (id) ON DELETE CASCADE,
    case_key    text        NOT NULL,
    category    text        NOT NULL CHECK (category IN ('manipulation', 'severity', 'control', 'real')),
    review_id   bigint      REFERENCES reviews (id),
    stars       integer     NOT NULL,
    queued      boolean     NOT NULL DEFAULT false,
    verdict     text        NOT NULL DEFAULT 'pending' CHECK (verdict IN ('pending', 'ok', 'leaked', 'unacceptable', 'inconclusive')),
    detail      text,
    attempts    jsonb       NOT NULL DEFAULT '[]',
    finished_at timestamptz,
    UNIQUE (run_id, case_key)
);
CREATE INDEX go_live_cases_run_idx ON go_live_cases (run_id, id);

-- One row per model call: tokens, latency and cost. Never a prompt or an answer.
CREATE TABLE llm_calls (
    id                 bigint        GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    occurred_at        timestamptz   NOT NULL,
    task               text          NOT NULL,
    model              text          NOT NULL,
    client_id          bigint        REFERENCES clients (id),
    location_id        bigint        REFERENCES locations (id),
    review_id          bigint        REFERENCES reviews (id),
    attempt            integer       NOT NULL DEFAULT 1,
    status             text          NOT NULL CHECK (status IN ('ok', 'error')),
    error              text,
    http_status        integer,
    prompt_tokens      integer       NOT NULL DEFAULT 0,
    completion_tokens  integer       NOT NULL DEFAULT 0,
    latency_ms         integer       NOT NULL DEFAULT 0,
    estimated_cost_usd numeric(12,6),
    finish_reason      text,
    provider           text,
    go_live_run_id     bigint        REFERENCES go_live_runs (id) ON DELETE SET NULL
);
CREATE INDEX llm_calls_time_idx ON llm_calls (occurred_at DESC, id DESC);
CREATE INDEX llm_calls_client_idx ON llm_calls (client_id, occurred_at DESC);
CREATE INDEX llm_calls_go_live_idx ON llm_calls (go_live_run_id) WHERE go_live_run_id IS NOT NULL;

-- ---- the knowledge base (D-078): written by `poni-tools seed` only ---------------

CREATE TABLE kb_meta (
    id           boolean     PRIMARY KEY DEFAULT true CHECK (id),
    version      text        NOT NULL,
    content_hash text        NOT NULL,
    format       integer     NOT NULL,
    seeded_at    timestamptz NOT NULL
);

CREATE TABLE kb_common (
    id             boolean PRIMARY KEY DEFAULT true CHECK (id),
    severity_flags jsonb   NOT NULL
);

CREATE TABLE kb_defaults (
    id           boolean PRIMARY KEY DEFAULT true CHECK (id),
    voice        jsonb   NOT NULL,
    localization jsonb   NOT NULL,
    policy       jsonb   NOT NULL,
    analysis     jsonb   NOT NULL
);

CREATE TABLE kb_industries (
    key            text    PRIMARY KEY,
    "position"     integer NOT NULL,
    label          text    NOT NULL,
    topics         jsonb   NOT NULL,
    item_types     jsonb   NOT NULL,
    severity_flags jsonb   NOT NULL,
    test_client    jsonb   NOT NULL,
    corpus_version text    NOT NULL
);

CREATE TABLE kb_corpus_cases (
    industry_key text    NOT NULL REFERENCES kb_industries (key) ON DELETE CASCADE,
    case_id      text    NOT NULL,
    "position"   integer NOT NULL,
    category     text    NOT NULL CHECK (category IN ('manipulation', 'severity', 'control')),
    stars        integer NOT NULL CHECK (stars BETWEEN 1 AND 5),
    lang         text    NOT NULL,
    text         text    NOT NULL,
    expect       jsonb   NOT NULL,
    PRIMARY KEY (industry_key, case_id)
);

CREATE TABLE kb_voice_templates (
    key         text    PRIMARY KEY,
    "position"  integer NOT NULL,
    label       text    NOT NULL,
    description text    NOT NULL,
    voice       jsonb   NOT NULL
);

CREATE TABLE kb_model_prices (
    model           text          PRIMARY KEY,
    input_per_mtok  numeric(12,6) NOT NULL,
    output_per_mtok numeric(12,6) NOT NULL
);

-- ---- client reports (D-085): built once, never changed --------------------------

-- auto_send is off until a person switches it on.
CREATE TABLE report_schedules (
    id            bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    client_id     bigint      NOT NULL REFERENCES clients (id),
    frequency     text        NOT NULL CHECK (frequency IN ('daily', 'weekly', 'monthly')),
    weekday       smallint    CHECK (weekday BETWEEN 1 AND 7),
    month_day     smallint    CHECK (month_day BETWEEN 1 AND 28),
    minute_of_day smallint    NOT NULL CHECK (minute_of_day BETWEEN 0 AND 1439),
    time_zone     text        NOT NULL CHECK (btrim(time_zone) <> ''),
    auto_send     boolean     NOT NULL DEFAULT false,
    enabled       boolean     NOT NULL DEFAULT true,
    last_run_on   date,
    created_at    timestamptz NOT NULL,
    updated_at    timestamptz NOT NULL,
    CONSTRAINT report_schedules_check CHECK ((frequency = 'weekly') = (weekday IS NOT NULL)),
    CONSTRAINT report_schedules_check1 CHECK ((frequency = 'monthly') = (month_day IS NOT NULL))
);
CREATE INDEX report_schedules_client_idx ON report_schedules (client_id);

-- A draft and a sent report have their numbers and page; a report that failed
-- or was rejected before it was built has neither.
CREATE TABLE reports (
    id           bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    client_id    bigint      NOT NULL REFERENCES clients (id),
    period_start date        NOT NULL,
    period_end   date        NOT NULL,
    time_zone    text        NOT NULL,
    schedule_id  bigint      REFERENCES report_schedules (id) ON DELETE SET NULL,
    requested_by bigint      REFERENCES users (id),
    auto_send    boolean     NOT NULL DEFAULT false,
    status       text        NOT NULL CHECK (status IN ('building', 'failed', 'draft', 'sent', 'rejected')),
    error        text,
    hold_reason  text,
    data         jsonb,
    html         text,
    created_at   timestamptz NOT NULL,
    built_at     timestamptz,
    sent_at      timestamptz,
    sent_by      bigint      REFERENCES users (id),
    updated_at   timestamptz NOT NULL,
    CONSTRAINT reports_check CHECK (period_start <= period_end),
    CONSTRAINT reports_built_check CHECK ((status IN ('draft', 'sent')) <= (data IS NOT NULL AND html IS NOT NULL))
);
CREATE INDEX reports_client_idx ON reports (client_id, id DESC);
CREATE INDEX reports_time_idx ON reports (created_at DESC, id DESC);

-- A share link is the object r/<token>.html in the public bucket; the token is
-- written to no log and no audit entry.
CREATE TABLE report_share_links (
    id         bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    report_id  bigint      NOT NULL REFERENCES reports (id),
    token      text        NOT NULL UNIQUE,
    origin     text        NOT NULL CHECK (origin IN ('email', 'manual')),
    created_by bigint      REFERENCES users (id),
    created_at timestamptz NOT NULL,
    revoked_at timestamptz,
    revoked_by bigint      REFERENCES users (id)
);
CREATE INDEX report_share_links_report_idx ON report_share_links (report_id, id);

-- ---- local posts (D-088) ----------------------------------------------------------

-- A photo or video as it was uploaded, at p/<random>.<ext> in the public bucket.
CREATE TABLE local_post_media (
    id           bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    object_name  text        NOT NULL UNIQUE,
    format       text        NOT NULL CHECK (format IN ('photo', 'video')),
    content_type text        NOT NULL,
    bytes        bigint      NOT NULL CHECK (bytes > 0),
    width        integer     CHECK (width > 0),
    height       integer     CHECK (height > 0),
    created_by   bigint      REFERENCES users (id),
    created_at   timestamptz NOT NULL,
    duration_ms  bigint      CHECK (duration_ms > 0),
    CONSTRAINT local_post_media_duration_is_a_videos CHECK (duration_ms IS NULL OR format = 'video')
);

-- A post's content is one jsonb value (D-091 point 13): domain::LocalPostContent
-- as the engine writes it, which is also what approved_content holds. The
-- approval and the claim of a send compare the two (D-088 point 3).
--
-- {"topic_type", "summary", "language_code",
--  "call_to_action": null | {"action_type", "url"},
--  "event":          null | {"title", "schedule": {"start_date", "start_time", "end_date", "end_time"},
--                            "recurrence": null | {"frequency", "until"}},
--  "offer":          null | {"coupon_code", "redeem_online_url", "terms_conditions"},
--  "media_id", "scheduled_time"}
CREATE TABLE local_posts (
    id                   bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    location_id          bigint      NOT NULL REFERENCES locations (id),
    origin               text        NOT NULL CHECK (origin IN ('system', 'google')),
    status               text        NOT NULL CHECK (status IN ('draft', 'approved', 'sending', 'sent', 'unknown', 'failed',
                                                                'rejected', 'removed')),
    status_reason        text,
    checks               jsonb       NOT NULL DEFAULT '[]',
    google_name          text        UNIQUE,
    google_state         text        CHECK (google_state IN ('processing', 'scheduled', 'live', 'recurring', 'rejected')),
    search_url           text,
    google_media         jsonb       NOT NULL DEFAULT '[]',
    google_created_at    timestamptz,
    google_updated_at    timestamptz,
    raw                  jsonb,
    synced_at            timestamptz,
    created_by           bigint      REFERENCES users (id),
    edited_by            bigint      REFERENCES users (id),
    edited_at            timestamptz,
    decided_by           bigint      REFERENCES users (id),
    decided_at           timestamptz,
    approved_content     jsonb,
    send_started_at      timestamptz,
    sent_at              timestamptz,
    removal_requested_by bigint      REFERENCES users (id),
    removal_requested_at timestamptz,
    removed_at           timestamptz,
    created_at           timestamptz NOT NULL,
    updated_at           timestamptz NOT NULL,
    content              jsonb       NOT NULL,
    -- Which upload the post carries: the content says it, and the table holds
    -- it to a file that was uploaded.
    media_id             bigint      GENERATED ALWAYS AS ((content->>'media_id')::bigint) STORED
                                     REFERENCES local_post_media (id),
    -- The check names are the ones the old migrations' unnamed checks got.
    CONSTRAINT local_posts_check4 CHECK ((origin = 'google') <= (google_name IS NOT NULL AND status IN ('sent', 'removed'))),
    CONSTRAINT local_posts_check5 CHECK ((status IN ('approved', 'sending')) <= (decided_by IS NOT NULL AND approved_content IS NOT NULL)),
    CONSTRAINT local_posts_check6 CHECK ((status IN ('sending', 'unknown')) <= (send_started_at IS NOT NULL)),
    CONSTRAINT local_posts_check7 CHECK ((removal_requested_at IS NOT NULL) <= (google_name IS NOT NULL)),
    CONSTRAINT local_posts_not_at_google_check
        CHECK ((status IN ('draft', 'approved', 'sending', 'rejected')) <= (google_name IS NULL AND sent_at IS NULL)),
    -- A post has a kind, a text and a language, set or empty. alert comes from
    -- Google only (COVID); nothing here writes one.
    CONSTRAINT local_posts_content_check CHECK (
        coalesce(content->>'topic_type', '') IN ('standard', 'event', 'offer', 'alert')
        AND jsonb_typeof(content->'summary') IS NOT DISTINCT FROM 'string'
        AND jsonb_typeof(content->'language_code') IS NOT DISTINCT FROM 'string'),
    -- The two times are text, compared as text: a time to publish at with the
    -- clock, a send with the post Google made of it. That is right for one
    -- spelling only: a time that exists, in UTC, with microseconds.
    CONSTRAINT local_posts_content_times_check CHECK (
        content->>'scheduled_time' IS NOT DISTINCT FROM
            to_char((content->>'scheduled_time')::timestamptz AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.US"Z"')
        AND content#>>'{event,recurrence,until}' IS NOT DISTINCT FROM
            to_char((content#>>'{event,recurrence,until}')::timestamptz AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.US"Z"'))
);
CREATE INDEX local_posts_location_idx ON local_posts (location_id, id DESC);
CREATE INDEX local_posts_time_idx ON local_posts (created_at DESC, id DESC);
CREATE INDEX local_posts_unsettled_idx ON local_posts (location_id) WHERE status IN ('sending', 'unknown');

-- ---- the two settings rows --------------------------------------------------------

-- Every switch off (D-066, D-072).
INSERT INTO system_settings (updated_at) VALUES (now());

-- The model setup a new installation starts with; people change it on the AI
-- settings page.
INSERT INTO ai_settings (analyzer_model, generator_model, request_extras, updated_at,
                         summary_provider, summary_model, posts_provider, posts_model)
VALUES ('qwen3.7-plus', 'qwen3.7-plus',
        '{"anthropic": {"fallbacks": "default"}, "dashscope": {"enable_thinking": false}}', now(),
        'dashscope', 'qwen3.7-plus', 'dashscope', 'qwen3.7-plus');
