# Architecture

What the system must do and how the engine is built. `D-0NN` points to [decisions.md](decisions.md). The rules that protect clients (what may be written to Google, who must approve, which switches exist) are stated once, in [`CLAUDE.md`](../CLAUDE.md) under "Rules that protect clients", with the code symbol that holds each; this document points to them and does not repeat them. The requirements, with the ids `FR-1` onward that code cites, are section 12. The API is `api/openapi.yaml`; the repository layout is CLAUDE.md's "Layout" table.

## 1. System overview

```
 dashboard (browser)          phone app (iOS)
   │ cookie session             │ bearer token
   ▼                            ▼
 Caddy: TLS, the dashboard's static files
   │ /api/*
   ▼
 Engine (C++23 daemon): HTTP API, scheduler, job workers
   │                         │
   ▼                         ▼
 PostgreSQL                Google: GBP APIs, Performance API, Pub/Sub,
 (system of record,        Gmail, Cloud Storage; LLM providers
  job queue)

 A client opens a report link: a page in a public Cloud Storage bucket, no login.
```

The engine owns every external integration (D-003). Its API has two clients, each in its own repository:

- **The dashboard** (React-admin on Material UI, TypeScript, Vite; D-011), in a repository of its own: static files served by Caddy, calling only `/api` through a client generated from the spec, with the cookie session and a CSRF token. It works at phone width.
- **The iOS app**: the same Google sign-in, then a bearer token (D-095, section 4.5). Responses stay compatible with it (CLAUDE.md, "API changes are spec-first").

## 2. The engine

### 2.1 Process and layers

One binary, `poni-gbp-engine`, a daemon with no commands (D-079): API, scheduler, job workers and Pub/Sub subscriber. At start it checks that no migration is pending, a knowledge base is seeded and the database belongs to the Google it is configured for (D-087). Migrations and seeding are `poni-tools` (section 9). A signed-in person connects the managing account and the sending mailbox with the "Connect" buttons of the status page (`http/connect`). One `io_context` thread runs all coroutines and database calls go to the `Database`'s pool, so engine state needs no locks. A rate limiter per external API is shared by every caller of that API, because Google's quotas are per project. The layers (`core`, `db`, `jobs`, `connectors`, `domain`, `http`) are described in CLAUDE.md's "Layout".

### 2.2 Jobs

A job is a row in `jobs` (`run_at`, backoff, idempotency keys, `FOR UPDATE SKIP LOCKED`, at least once). Recurring work is registered with `Registry::every`.

| Kind | Area |
| ---- | ---- |
| `locations.discover`, `reviews.poll`, `reviews.full_sync`, `notifications.pull`, `reviews.ingest_notification` | Sync (4.2) |
| `reviews.analyze_pending`, `reviews.analyze`, `replies.post`, `replies.confirm` | Review to reply (4.1) |
| `notifications.deliver` | One delivery |
| `golive.sweep`, `golive.case` | Go-live test (section 10) |
| `reports.schedule`, `reports.build` | Reports (4.3) |
| `local_posts.sync_due`, `local_posts.sync`, `local_posts.publish`, `local_posts.remove`, `local_posts.edit` | Local posts (4.4) |
| `system.heartbeat`, `system.housekeeping` | Liveness; purging finished jobs, ended sessions, unused app codes |

## 3. External integrations

| Service | Used for | Auth |
| ------- | -------- | ---- |
| GBP Account Management, Business Information (v1) | Accounts and locations | OAuth of the managing account, `business.manage` |
| Google My Business API (v4) | Reviews (list, batch get, reply); local posts (list, get, create, delete, and the update of the fields an approved edit names) | Same |
| Business Profile Performance API (v1) | Daily metrics and monthly search keywords, read for a report or post help | Same |
| Cloud Pub/Sub (REST) | Review notifications, by pull | Locally gcloud application-default credentials; in production the VM's service account |
| Cloud Storage (JSON API) | Report pages and post media in one public bucket; uploads only | As Pub/Sub |
| LLM providers | Analysis, replies, go-live judge, report summaries, post help | An API key per provider |
| Gmail API | Notifications and reports | OAuth of the sending mailbox, `gmail.send` only (D-066) |
| Apple Push Notification service | Push to the phone app, over HTTP/2: one POST per device (D-097) | A provider token (ES256 JWT) signed with the APNs auth key from `PONI_APNS_KEY` |
| Google OpenID Connect | Sign-in, dashboard and app | The engine's OAuth client |

- **What may change at Google** is three things, held to their addresses by `http::why_refused` (CLAUDE.md). With `PONI_GOOGLE_MOCK_URL` the same requests go to the local mock (D-087, [runbook-google-mock.md](runbook-google-mock.md)).
- **A call with a token** goes through `google::send`: token, rate-limiter slot (2.1), one resend after a first 401, a pause after a 429.
- **Pub/Sub over REST**, not `google-cloud-cpp`, which brings gRPC, Protobuf and Abseil for two calls (D-012). It runs only where a subscription is configured.
- **IDs.** v4 review names include the account ID; account and location IDs are stored apart, so a new managing account only rebinds the account.
- **LLM.** `llm::Client` speaks OpenAI-compatible and Anthropic's Messages API, per provider (D-010, D-059). Five tasks, each with its provider and model: `analyzer`, `writer`, `judge`, `summary`, `posts`. Prices come from the knowledge base.
- **Email** through Gmail (D-026); the sender is `PONI_EMAIL_FROM`.

## 4. Key flows

### 4.1 New review to reply

1. **Analysis.** Every minute `reviews.analyze_pending` finds reviews whose current version has no analysis, or with no reply and no live draft; `reviews.analyze` asks the model, with the industry profile, for FR-3's analysis. Never from Google's translation.
2. **Policy** (`domain/reply_policy`): auto-post, approval or escalate. A flag on an unreplied review opens an escalation and its alert in the same transaction.
3. **Draft** in the location's voice and localization; the checks and the policy gate (section 10) decide whether it may auto-post.
4. **Posting.** Only `repo::approve_draft` and `repo::schedule_auto_post` enqueue `replies.post`; an auto-post waits a random delay inside the window (D-071). The job reads the review from Google again; `repo::claim_post` checks it, both switches, `auto_verdict` and the open-post rule in one transaction (D-061, D-069 to D-072); then the PUT.

```
draft ─approve─▶ approved ─claim─▶ posting ─200─▶ posted ─seen on Google─▶ published
  ▲                 │                 │              │
  │                 │ review gone,    │ 4xx: back    │ not seen in 30 minutes:
  └─────────────────┘ edited, replied │ to draft     │ unconfirmed, back to draft
   a switch is off    → cancelled or  │
                        superseded    └─ unknown (timeout, 5xx): stays posting, never resent
```

An auto-post is `scheduled` before `approved`; one that is stopped returns to the queue with a notification. Google shows a reply minutes after accepting it: `replies.confirm` reads reviews with an open post every minute, and the sync knows the engine's reply by its text; any other reply is from outside and cancels the drafts. An auto-post's audit actor is `auto-reply`, and its post carries the policy version instead of an approver.

### 4.2 Sync

Only linked locations are synced (D-038). Every change ends in one store routine that records edits, outside replies and removals in `review_events` (D-039) and does nothing when nothing changed (D-037).

- **Discover:** accounts and locations every 15 minutes.
- **Poll:** every minute, the locations whose poll is due (2 minutes by default), one `batchGetReviews` per account, newest first, stopping at a page with nothing new.
- **Full reconciliation:** on link, then hourly, every review listed and compared; it finds outside replies, which do not change a review's update time. Only `NOT_FOUND` for a review marks it removed.
- **Notifications** (Pub/Sub, every 30 seconds) name a review, fetched and stored the same way.

### 4.3 Reports

A person ("Generate now", any finished period) or `reports.schedule` (every five minutes; daily is the day before, weekly the seven days before, monthly the previous month, in the schedule's time zone) writes a `building` row. `reports.build` reads the period's and the previous period's reviews and analyses; asks the Performance API for daily metrics and the month's keywords, storing nothing else (Google lags about three days: the report says the day its numbers reach and compares only whole periods; without an answer it is built without them); computes every figure in `domain/report.hpp`; asks the `summary` task for a few sentences, checked for links, addresses, phone numbers and length; and renders one HTML page without script (`domain/report_html.hpp`). Numbers and page (`reports.data`, `reports.html`) never change (D-085).

A person reads the draft in a sandboxed frame and sends, regenerates or rejects it. Sending uploads `r/<token>.html` to the public bucket and raises `client_report`; revoking overwrites the object. "noindex" and the content security policy are `<meta>` elements; the bucket's lifecycle rule removes pages after 365 days.

### 4.4 Local posts (D-088)

`local_posts.content` (`jsonb`) sits next to `approved_content` (D-091 point 13). A draft is normalized and its checks stored; `repo::approve_local_post` takes the exact content read and enqueues `local_posts.publish`, which claims the post (`repo::claim_local_post_send`) and sends one create with its scheduled time and repeat. The engine has no timer.

| Google's answer | The post becomes |
| --- | --- |
| Accepted | `sent`; read again after 1, 5 and 15 minutes |
| 429, or a token error (401, no token) | `approved` for the next attempt; on the last, `draft` |
| 400, 403, 404 | `draft` with Google's message; staff notified |
| Anything else (timeout, 5xx, 408, 409, 499, an unreadable 2xx) | `unknown`: never sent again, the reads settle it |

- **Media:** one photo or video, `POST /api/local-post-media`, checked from its bytes, stored unchanged as `p/<32 random bytes>.<ext>` in the public bucket, where Google fetches it (D-090).
- **Help:** praise of the last 90 days counted without a model (`GET /api/locations/{id}/review-highlights`), and ideas or a text from the `posts` task through `jobs::PostAdvisor`; nothing stored.
- **Remove:** `repo::request_local_post_removal`, then `local_posts.remove`; the only way to stop a scheduled post.
- **Edit (D-099):** a post that is at Google gets an edit beside it (`local_post_edits`, one open edit per post), written and approved like a draft. `repo::approve_local_post_edit` enqueues `local_posts.edit`, which claims the edit, reads the post from Google and sends one PATCH that names exactly the fields that differ. Accepted: the post's row takes Google's answer. A certain refusal: the edit is a draft again with Google's message (429 and token errors are tried again). Anything else: the edit is a draft again saying that Google did not confirm it, and nothing is sent again without a new approval. Built from the documentation, without a trial.
- **Read:** `local_posts.sync_due` (every five minutes) queues a sync for each linked location unread for an hour or holding an unsettled send. A sync settles sends of unknown outcome by adoption (CLAUDE.md), stores the listed posts, and marks `removed` only a post whose own get answers `NOT_FOUND`. None of this runs against the mock.

### 4.5 Sign-in, sessions and audit

- **Sign-in:** Google OpenID Connect. The ID token is the token endpoint's answer to the engine's own exchange over verified TLS, so its signature is not checked; issuer, audience, nonce, expiry and domain are (D-091 point 12). The redirect is tied to its start by a short-lived Lax cookie and the attempt the engine keeps: one browser, one account, one use, ten minutes (`http/browser_flow.hpp`). Attempts are rate-limited.
- **Dashboard sessions:** a hash in `sessions`; cookie `HttpOnly`, `Secure`, `SameSite=Strict`; 7 days, or 24 hours unused; `X-CSRF-Token` on every changing request.
- **The app** (D-095) starts with `client=ios` and a PKCE S256 challenge; the callback redirects to `gbpmanager://auth` with a 60-second one-time code, and `POST /api/auth/app-token` exchanges code and verifier for a bearer token: a session of kind `app`, 90 days at most, 30 unused, read from `Authorization: Bearer` only, no CSRF. Both kinds' lifetimes are `domain::SessionLifetimes`.
- **Authorization:** the router asks one hook (`AuthService::authorize`) before every handler.
- **Audit:** `db.audited(...)` or `api.change(...)` refuses to commit a change without an audit entry in the same transaction; the engine's own work uses `audit::Actor::system("component")`.

## 5. Data model

Migrations in `engine/migrations/`, applied by `poni-tools migrate`, from `0001_baseline.sql` (D-091 point 10). Raw API payloads are `jsonb` next to the typed columns.

| Area | Tables |
| ---- | ------ |
| People | `users`, `sessions` (kind `browser` or `app`), `app_sign_in_codes`, `audit_log` |
| Clients | `clients` (industry key), `locations` (GBP account and location IDs, time zone, posting switch, auto-posting switches per star rating, daily cap, pause) |
| Settings | `reply_settings` (one complete row per linked location: voice, localization, policy, delay, window; D-093), `ai_settings` (analysis settings, each task's provider and model) |
| Reviews | `reviews` (rating, original and translated text, language, script, reviewer, current reply), `review_events`, `review_analyses` |
| Replies | `reply_drafts` (draft, scheduled, approved, posting, posted, published, rejected, cancelled, superseded; model text beside a person's edit), `reply_posts` (one per PUT, at most one open per review) |
| Local posts | `local_posts` (draft, approved, sending, sent, unknown, failed, rejected, removed, beside Google's state), `local_post_media`, `local_post_edits` (the open edit of a post at Google: draft, approved, sending) |
| Other | `cases`, `go_live_runs`, `go_live_cases`, `notification_rules`, `notification_deliveries`, `reports`, `report_share_links`, `report_schedules` |
| Knowledge base | `kb_meta`, `kb_industries`, `kb_common`, `kb_corpus_cases`, `kb_defaults`, `kb_voice_templates`, `kb_model_prices` (written only by `poni-tools seed`) |
| Operations | `jobs`, `credentials` (encrypted, D-043), `integration_status`, `llm_calls`, `system_settings` (posting and email switches, the Google the database belongs to) |

## 6. Technology

C++23, CMake, Ninja, vcpkg manifest (D-001); clang only, Homebrew LLVM on macOS and clang with libstdc++ on Linux, so only C++ that works with both libraries (D-028, D-089, D-091 point 8); Boost.Asio and Beast with OpenSSL (D-002); Boost.JSON with Describe, Boost.URL; libpqxx on a thread pool (D-014); GoogleTest with fakes; OpenAPI 3.1 (D-004); PostgreSQL (D-005); Python with `uv` for `poni-tools` (D-079); Caddy; Docker Compose on one Compute Engine VM (D-008).

## 7. Deployment

- **Development.** The engine builds natively on macOS; PostgreSQL runs in Docker; the Linux container (`engine/scripts/linux.sh`) is what CI runs, on arm64 and x86_64. The dashboard's dev server, in its own repository, forwards `/api` to a local engine.
- **Production.** One Compute Engine VM with Docker Compose: Caddy (HTTPS, the dashboard's static files, `/api` to the engine), the engine, PostgreSQL, and the one-shot `migrate` and `seed` services of the tools image, run before the engine. Google Cloud services authenticate as the VM's service account, so there are no key files. A nightly `pg_dump` goes to Cloud Storage. Guide: [deploy/README.md](../deploy/README.md) (D-045).

## 8. Security baseline

- HTTPS only, with HSTS. Google sign-in only, no passwords, rate-limited attempts. The site faces the internet and can post replies for every client, so sessions, CSRF and section 4.5 are done properly; Cloudflare Access or Tailscale in front is an optional extra layer.
- Secrets stay out of the repository; stored tokens are encrypted with a key from the environment (D-043); no token, code or verifier is logged.
- Review text, and what a model writes, is untrusted input everywhere (section 10).

## 9. Knowledge base and operations scripts (D-078, D-079)

| Kind | What | Changed by | How |
| ---- | ---- | ---------- | --- |
| Schema | Tables and columns | A release | Numbered migrations, each once, in order |
| Knowledge | Industry packs, common flags, voice templates, defaults, model prices | A reviewed change to `knowledge/` | Seeded: all `kb_*` tables replaced in one transaction |
| Settings | Reply settings, analysis settings, switches, caps | Staff, through the API | Audited routes |

`poni-tools check` (JSON Schema, cross-checks, the complete-pack rule) → `publish` (one archive, by date and version, to a Cloud Storage bucket) → `seed` (production names the archive in `poni.env`; download, compare the hash, check, replace the tables) → the engine reads; the dashboard shows them read-only.

- **Version:** `manifest.json`, `YYYYMMDD` or `YYYYMMDD-N` (D-084). `kb_meta` keeps the content's hash; changed content under a seeded version is refused.
- **The engine reads** the industry profile with the common flags (every analysis), the corpus (go-live), prices, the defaults (a client's first link) and the industry list. It writes and caches none.
- **Copies:** a location's settings are its own row (D-093), copied on link, never read from the tables again; a new field reaches rows by a migration. The industry key is the one reference, without a foreign key; a client whose industry is gone is not analyzed (D-084).
- **A new version** that changes what the analyzer is told about an industry changes its clients' fingerprints; `poni-tools seed` prints them.
- **Tests** copy (`CREATE DATABASE ... TEMPLATE`) a database `poni-tools test-db` migrated and seeded once per run.

## 10. Reply safety and AI testing

Strangers write the reviews, so every review is hostile input (D-021). Layered defences:

1. **Prompt structure** (`domain/prompts.hpp`): instructions and voice in the system message; the review JSON-escaped in a nonce-tagged data block that the model is told holds no instruction; never the reviewer's name or photo.
2. **Structured output:** JSON (reply, language, script, suspected manipulation).
3. **Mechanical checks** in C++ (D-058): no links, email addresses or phone numbers; length and emoji; the language of the review or the fallback. Offers, promises and Chinese script are the model's instructions, not code's.
4. **The policy gate** (`domain/review_pipeline.hpp`): a flag, suspected manipulation or a failed check goes to the approval queue; auto-post only when everything passes.
5. **Limits** (D-070, D-072): auto-posting off per location and star rating until a person switches it on; a daily cap; the engine pauses a location when checks keep failing or an automatic reply is refused or does not appear, and a person resumes it.

| Suite (`engine/tests/ai/`) | Covers | Runs |
| ----- | ------ | ---- |
| Check unit tests | Each check, fixed inputs | Every build |
| Adversarial corpus, offline | Known attacks (English, Chinese, mixed, hidden in spacing or emoji, role-play, fake "system" text) against scripted models: a raised flag is acted on, and a reply with a link, address or number is stopped even when both models were fooled | Every build |
| Adversarial corpus, live | The same against the real model with the seeded configuration; nothing may reach auto-post | By hand, before a release that changes prompts or checks |
| Go-live test (D-074) | The industry's corpus and the client's recent 4 and 5 star reviews, three runs each, with the client's configuration; an AI judge rates the replies | Started by a person per client |

The decisions are plain functions in `domain/review_pipeline.hpp`, called by the job and the offline suite; each case runs as its pack's made-up business.

**Go-live gate** (D-074 to D-077; the rule in CLAUDE.md): `domain/go_live.hpp` computes the fingerprint; `repo::go_live_gate` is a condition of `auto_verdict`; `jobs/go_live.cpp` runs one case per job through the pipeline's functions and asks the judge (by default Anthropic's API). A run writes no draft or post and sends nothing to Google; the client's reviews reach the judge's provider without name or photo (D-075). `ReplySettingsGolden` (`tests/ai/fixtures/reply_settings_golden.json`) fails when a code change alters a prompt for fixed settings.

## 11. Localization

- Part of a location's reply settings (D-093): Chinese script (match the reviewer, Simplified, Traditional), regional style (Mainland, Taiwan, Hong Kong), English variant (US, Canadian), and a fallback language (default English).
- The analyzer detects language and script from the original; the writer is told target language, script and style.
- Nothing converts or verifies a Chinese reply's script (D-056).

## 12. Requirements

PONI runs Google Business Profiles (GBP) for its clients. The system automates review replies, client reports and local posts for every client location, through the GBP and LLM APIs directly, with no paid review platform.

### 12.1 Users and access

- PONI staff only. Clients never sign in: they get emails and report links. Nothing should rule out a client-facing product later.
- One role: every signed-in user may do everything; every route has a `RouteInfo`, so roles can be added without touching handlers.
- Google sign-in only, for Workspace accounts on `xvisionmediaus.com` and `xvisionus.com` (D-033, D-053); no passwords stored. A user is created on first sign-in (D-040) and disabled in Google Workspace.
- Every change is audited: who (a person, or the system and its component), what, which object, when, before and after.

### 12.2 Scale and markets

- Design point: 200 clients, about 500 locations, 10,000 reviews a day in total, 100 a day per location (today under 50 clients, 50 to 100 locations).
- United States and Canada; each location has its own time zone.
- Mostly food and beverage, also wellness (spas); behaviour is per industry (FR-3, FR-13).
- Mostly English reviews, some Chinese (Simplified and Traditional) and Spanish. Non-English reviews arrive as "(Translated by Google) … (Original) …"; the original is analyzed.

### 12.3 Functional requirements

- **FR-1 Clients and locations.** A client (brand) owns one or more locations, discovered through the GBP API and linked by staff. Noticing invitations or lost access is not built; lost access shows as an error on the status page.
- **FR-2 Review ingestion.** Polling is the main path, Pub/Sub a second source, an hourly full reconciliation the safety net (D-037, D-091 point 1). The whole review is stored; a reply posted outside the system is detected.
- **FR-3 Review analysis.** Sentiment, topics, items (dishes, treatments, staff), language and script, severity flags, suspected manipulation. Topics, item types and industry flags come from the industry profile; every industry also has discrimination or harassment, staff misconduct, legal or regulatory threat, threats of violence. Flags and manipulation are the model's alone (D-058).
- **FR-4 Reply generation.** Each linked location has its own complete voice and localization (D-093), started from the knowledge base's defaults or templates. A reply is in the review's language as localization shapes it (section 11). Every draft records model, prompt version and inputs.
- **FR-5 Tiered reply policy.** Per location: 4 and 5 stars without a complaint may auto-post (no text: a short, varied thank-you); a complaint, three stars or fewer, a severity flag, suspected manipulation, a failed check, an edited review and a draft a person edited go to the approval queue. Auto-posts wait an optional random delay inside an optional posting window in local time (for example 09:00 to 21:00); the review is read again just before posting. In the queue a person edits, regenerates with instructions, approves, rejects, or sets the review aside (D-068).
- **FR-6 Escalation.** A severity flag on an unreplied review raises an immediate alert and an escalation (`open` or `closed`, one assignee; D-065), a row in `cases`.
- **FR-7 Notifications.** Channels behind one interface (D-097): email through Gmail, and push to the phone app through Apple's push service, for escalations and system problems (D-097). Rules map event type × client to recipients. Events: `escalation`, `approval_needed`, `system_problem` (lost access, token failure, failing jobs, a post rejected or a send failed), `client_report`. Every delivery is logged and retried.
- **FR-8 Reports.** One kind, the client report per client and period, per location (D-085): reviews, ratings and their change, response rate and time, sentiment, topics and items, highlights, escalations, employee recognition (D-086), Performance API visibility and search keywords, and a model-written summary. Built once; schedules daily, weekly or monthly; PDF is the browser's print (D-035).
- **FR-9 Profile accuracy.** Not built: compare a profile with a reference record and alert, never edit.
- **FR-10 Dashboard.** Its own repository (section 1): reviews, approval queue, escalations, clients and locations, reply settings, posts, knowledge base (read-only), notification rules, reports, audit log, status.
- **FR-11 Users and audit.** Google sign-in and sign-out; the audit log is append-only, viewable and filterable.
- **FR-12 Cases.** After the MVP: following a negative review from first alert to final public response; today's escalation is its simplest form (D-054).
- **FR-13 Knowledge base (D-078).** What is the same for every client and nobody changes while the system runs is files reviewed like code, checked and seeded by `poni-tools` into static, versioned tables. An industry is supported only with a complete pack; settings start as copies (section 9).
- **FR-14 Local posts (D-088, D-099).** Updates, offers and events with Google's fields and one photo or video. A person approves the exact content; Google schedules and repeats. Every post of a location is shown with its state at Google, including posts made there; staff may remove any, and correct one in place by an edit a person approves (D-099). The model helps only when asked. Not included: client approval, figures per post, product posts, several photos.

### 12.4 Non-functional requirements

- **Latency:** with the delay off, 95% of auto-replies posted within 2 minutes of the review reaching Google (a target).
- **Completeness:** a lost notification never means a missed review.
- **Safety:** never overwrite a reply someone else posted; check again before every post; jobs can run twice.
- **Cost:** infrastructure under $100 a month; model cost recorded per call (`llm_calls`).
- **Operability:** JSON log lines, `/api/health`, a status page, nightly backups off the server.
- **Portability:** macOS for development, one Linux server in production, arm64 and x86_64 (D-034). Security: section 8.

### 12.5 Out of scope

Client logins and roles; Q&A (Google shut its API down); editing a profile; removing or flagging reviews (no API); profile photos and product posts; social media monitoring.
