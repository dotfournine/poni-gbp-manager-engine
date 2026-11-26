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

One binary, `poni-gbp-engine`, a daemon with no commands (D-079): API, scheduler, job workers and Pub/Sub subscriber. At start it checks that no migration is pending, a knowledge base is seeded and the database belongs to the Google it is configured for (D-087). Migrations and seeding are `poni-tools` (section 9). A signed-in person connects the managing account, the sending mailbox and the Workspace administrator with the "Connect" buttons of the status page (`http/connect`); each is the Google account that person signs in with, and the status page shows its address (D-100). One `io_context` thread runs all coroutines and database calls go to the `Database`'s pool, so engine state needs no locks. A rate limiter per external API is shared by every caller of that API, because Google's quotas are per project. The layers (`core`, `db`, `jobs`, `connectors`, `domain`, `http`) are described in CLAUDE.md's "Layout".

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
| GBP Account Management, Business Information (v1) | Accounts and locations | OAuth of the managing account, `business.manage` (and `openid email` at connection, to learn its address, D-100) |
| Google My Business API (v4) | Reviews (list, batch get, reply); local posts (list, get, create, delete, and the update of the fields an approved edit names) | Same |
| Business Profile Performance API (v1) | Daily metrics and monthly search keywords, read for a report or post help | Same |
| Cloud Pub/Sub (REST) | Review notifications, by pull | Locally gcloud application-default credentials; in production the VM's service account |
| Cloud Storage (JSON API) | Report pages and post media in one public bucket; uploads, and a report page's move to the archive (copy, then delete; D-104) | As Pub/Sub |
| LLM providers | Analysis, replies, go-live judge, report summaries, post help | An API key per provider |
| Google Places API (New) | A new location's time zone, read once at discovery (D-109): Place Details with the field mask `timeZone` | An API key (`PONI_GOOGLE_PLACES_API_KEY`), no OAuth |
| Admin SDK Directory API | Who has left (D-106) | OAuth of a Workspace administrator, `admin.directory.user.readonly` (and `openid email` at connection) |
| Gmail API | Notifications and reports | OAuth of the sending mailbox, `gmail.send` (and `openid email` at connection, D-100) (D-066) |
| Apple Push Notification service | Push to the phone app, over HTTP/2: one POST per device (D-097) | A provider token (ES256 JWT) signed with the APNs auth key from `PONI_APNS_KEY` |
| Google OpenID Connect | Sign-in, dashboard and app | The engine's OAuth client |

- **What may change at Google** is a Business Profile in four ways (the reply to a review, and the create, the delete and the update of a local post), and in the engine's own public bucket an upload, the copy of a report's page to its archive and the delete of the original (D-104); each is held to its addresses by `http::why_refused` (CLAUDE.md). The Admin SDK is read with one GET, the list of the account's users, held to its address by the same function (D-106 point 3), and Google Places with one GET, a place's `timeZone` (D-109). With `PONI_GOOGLE_MOCK_URL` the same requests go to the local mock (D-087, [runbook-google-mock.md](runbook-google-mock.md)).
- **A call with a token** goes through `google::send`: token, rate-limiter slot (2.1), one resend after a first 401, a pause after a 429. The one call with a key and no token, the Places time-zone read, has its own connector and limiter slot (D-109).
- **Pub/Sub over REST**, not `google-cloud-cpp`, which brings gRPC, Protobuf and Abseil for two calls (D-012). It runs only where a subscription is configured.
- **IDs.** v4 review names include the account ID; account and location IDs are stored apart, so a new managing account only rebinds the account.
- **LLM.** `llm::Client` speaks OpenAI-compatible and Anthropic's Messages API, per provider (D-010, D-059). Five tasks, each with its provider and model: `analyzer`, `writer`, `judge`, `summary`, `posts`. Prices come from the knowledge base.
- **Email** through Gmail (D-026); the sender is the mailbox connected on the status page, at the address its sign-in named (D-100).

## 4. Key flows

### 4.1 New review to reply

1. **Analysis.** Every minute `reviews.analyze_pending` finds reviews whose current version has no analysis, or with no reply and no live draft; `reviews.analyze` asks the model, with the industry profile, for FR-3's analysis. Never from Google's translation.
2. **Policy** (`domain/reply_policy`): auto reply, approval or escalate. A flag on an unreplied review opens an escalation and its alert in the same transaction.
3. **Draft** from the location's content settings and reply settings (D-107); the checks and the policy gate (section 10) decide whether it may auto reply.
4. **Posting.** Only `repo::approve_draft` and `repo::schedule_auto_reply` enqueue `replies.post`; an auto reply waits a random delay inside the window (D-110). The job reads the review from Google again; `repo::claim_post` checks it, both switches, `auto_verdict` and the open-post rule in one transaction (D-061, D-069, D-072, D-110); then the PUT.

```
draft ─approve─▶ approved ─claim─▶ posting ─200─▶ posted ─seen on Google─▶ published
  ▲                 │                 │              │
  │                 │ review gone,    │ 4xx: back    │ not seen in 30 minutes:
  └─────────────────┘ edited, replied │ to draft     │ unconfirmed, back to draft
   a switch is off    → cancelled or  │
                        superseded    └─ unknown (timeout, 5xx): stays posting, never resent
```

An auto reply is `scheduled` before `approved`; one that is stopped returns to the queue with a notification. Google shows a reply minutes after accepting it: `replies.confirm` reads reviews with an open post every minute, and the sync knows the engine's reply by its text; any other reply is from outside and cancels the drafts. An auto reply's audit actor is `auto-reply`, and its post carries the policy version instead of an approver.

### 4.2 Sync

Only enabled locations are synced (D-102): a location is switched on by a person, and discovery lists it switched off. Every change ends in one store routine that records edits, outside replies and removals in `review_events` (D-039) and does nothing when nothing changed (D-037).

- **Discover:** accounts and locations every 15 minutes. A new location's time zone is read once from Google Places by its place id before the location is stored, and never changes (D-109); a location not on Google Maps yet is added when it is, and the status page says so.
- **Poll:** every minute (`PONI_REVIEW_POLL_TICK_S`), the locations whose poll is due (every 2 minutes by default, one engine-wide setting, `PONI_REVIEW_POLL_INTERVAL_S`, D-112), one `batchGetReviews` per account, newest first, stopping at a page with nothing new.
- **Full reconciliation:** on switching a location on, then hourly, every review listed and compared; it finds outside replies, which do not change a review's update time. Only `NOT_FOUND` for a review marks it removed.
- **Notifications** (Pub/Sub, every 30 seconds) name a review, fetched and stored the same way.

### 4.3 Reports

A person ("Generate now", any finished period) or `reports.schedule` (every five minutes; daily is the day before, weekly the seven days before, monthly the previous month, in the schedule's time zone; a person's report without a time zone takes the location's, which every location has) writes a `building` row. `reports.build` reads the period's and the previous period's reviews and analyses; asks the Performance API for daily metrics and the month's keywords, storing nothing else (Google lags about three days: the report says the day its numbers reach and compares only whole periods; without an answer it is built without them); computes every figure in `domain/report.hpp`; asks the `summary` task for a few sentences, checked for links, addresses, phone numbers and length; and renders one HTML page without script (`domain/report_html.hpp`). Numbers and page never change (D-085).

Under D-104 the page is not kept in the database: the build names its object on the row (`r/<32 random bytes>.<ext>`), uploads it, and marks the report `ready`; that address is the report's one link. Nothing waits for a decision. A person sends a `ready` report as often as they like, to the location's `location_report` recipients or some of them; the request blocks while each address gets its own Gmail message, and then one row per address is appended to `notification_deliveries` with its outcome. The client shows who had the report already and asks before sending again. A schedule with "send automatically" sends once: `auto_send_status` turns `done` in the transaction that marks the build `ready`, before the send, or stays `pending` when the send's checks fail.

Discarding (`ready` or `failed`, never while `building`) is a person's: it marks the report `discarding` and names its archive, `a/<new token>.<ext>`, on the row, with the `report.discard` audit entry; copies the object there; deletes the original only after that copy; and marks it `discarded`: the link stops working, and the dashboard loads the report from the archive only. A report that never named a page is `discarded` at once. A copy or a delete that fails answers 502 and leaves the report `discarding`, and the next call starts again at the copy; the archive's name, once written, is kept. It cannot be undone. The retention is ours: the bucket has no lifecycle rule, and a clean-up script outside the engine (work-items) expires reports by the same steps, ending in `expired`.

### 4.4 Local posts (D-088)

`local_posts.content` (`jsonb`) sits next to `approved_content` (D-091 point 13). A draft is normalized and its checks stored; `repo::approve_local_post` takes the exact content read and enqueues `local_posts.publish`, which claims the post (`repo::claim_local_post_send`) and sends one create with its scheduled time and repeat. The engine has no timer.

| Google's answer | The post becomes |
| --- | --- |
| Accepted | `sent`; read again after 1, 5 and 15 minutes |
| 429, or a token error (401, no token) | `approved` for the next attempt; on the last, `draft` |
| 400, 403, 404 | `draft` with Google's message; staff notified |
| Anything else (timeout, 5xx, 408, 409, 499, an unreadable 2xx) | `unknown`: never sent again, the reads settle it |

- **Media:** up to ten photos and videos, in order (`media_ids`, D-101), each uploaded with its own `POST /api/local-post-media`, which the server takes to disk, checked from its bytes, stored unchanged as `p/<32 random bytes>.<ext>` in the public bucket, where Google fetches it (D-090).
- **Help:** praise of the last 90 days counted without a model (`GET /api/locations/{id}/review-highlights`), and ideas or a text from the `posts` task through `jobs::PostAdvisor`; nothing stored.
- **Remove:** `repo::request_local_post_removal`, then `local_posts.remove`; the only way to stop a scheduled post.
- **Edit (D-099):** a post that is at Google gets an edit beside it (`local_post_edits`, one open edit per post), written and approved like a draft, with the post's content the person wrote it against (which the client sends, and the engine checks). `repo::approve_local_post_edit` enqueues `local_posts.edit`, which claims the edit, reads the post from Google and sends one PATCH that names the fields the edit's author changed and Google does not have yet, and no other. A field changed on both sides stops the edit. Accepted: the post's row takes Google's answer, and the location is read again after 1, 5 and 15 minutes. A certain refusal: the edit is a draft again with Google's message (429 and token errors are tried again). Anything else: the edit is a draft again saying that Google did not confirm it, and nothing is sent again without a new approval. Built from the documentation, without a trial.
- **Read:** `local_posts.sync_due` (every five minutes) queues a sync for each enabled location unread for an hour or holding an unsettled send. A sync settles sends of unknown outcome by adoption (CLAUDE.md), stores the listed posts, and marks `removed` only a post whose own get answers `NOT_FOUND`. None of this runs against the mock.

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
| Clients | `clients` (a name, notes and an archived flag: grouping only, D-102), `locations` (GBP account and location IDs, the optional `client_id`, the `enabled` switch, industry key, `analyze_from`, the required time zone that discovery read from Google Places once (D-109), posting switch, and auto reply's pause state; its switch, cap and settings are reply settings) |
| Settings | `content_settings`, `reply_settings` and `report_settings` (one complete, flat row of each per location, written from the defaults at discovery: what the business is, how it sounds and who its customers are; how a reply is written, and auto reply's switch, scope, timing, cap and pause after failures; the format and the sections of its reports; D-107, D-110, D-111, D-102), `ai_settings` (analysis settings, each task's provider and model) |
| Reviews | `reviews` (rating, original and translated text, language, script, reviewer, current reply), `review_events`, `review_analyses` |
| Replies | `reply_drafts` (draft, scheduled, approved, posting, posted, published, rejected, cancelled, superseded; model text beside a person's edit), `reply_posts` (one per PUT, at most one open per review) |
| Local posts | `local_posts` (draft, approved, sending, sent, unknown, failed, rejected, removed, beside Google's state), `local_post_media`, `local_post_edits` (the open edit of a post at Google: draft, approved, sending) |
| Other | `cases`, `go_live_runs` (one location's test), `go_live_cases`, `notification_rules`, `notification_deliveries`, `reports` and `report_schedules` (each names a location) |
| Knowledge base | `kb_meta`, `kb_industries`, `kb_common`, `kb_corpus_cases`, `kb_defaults`, `kb_content_defaults`, `kb_reply_defaults`, `kb_report_defaults`, `kb_voice_templates`, `kb_model_prices` (written only by `poni-tools seed`) |
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
| Settings | Content settings, reply settings, report settings, analysis settings, switches | Staff, through the API; a change of an existing thing carries its `ETag` as `If-Match` (D-108) | Audited routes |

`poni-tools check` (JSON Schema, cross-checks, the complete-pack rule) → `publish` (one archive, by date and version, to a Cloud Storage bucket) → `seed` (production names the archive in `poni.env`; download, compare the hash, check, replace the tables) → the engine reads; the dashboard shows them read-only.

- **Version:** `manifest.json`, `YYYYMMDD` or `YYYYMMDD-N` (D-084). `kb_meta` keeps the content's hash; changed content under a seeded version is refused.
- **The engine reads** the industry profile with the common flags (every analysis), the corpus (go-live), prices, the defaults (written for a newly discovered location) and the industry list. It writes and caches none.
- **Copies:** a location's settings are its own two rows, content and reply (D-107), written from the defaults when the location is discovered and never read from the tables again; a new field reaches rows by a migration. The industry key (`locations.industry_key`) is the one reference, without a foreign key; a location whose industry is gone is not analyzed (D-084, D-102).
- **A new version** that changes what the analyzer is told about an industry changes its locations' fingerprints; `poni-tools seed` prints the enabled ones.
- **Tests** copy (`CREATE DATABASE ... TEMPLATE`) a database `poni-tools test-db` migrated and seeded once per run.

## 10. Reply safety and AI testing

Strangers write the reviews, so every review is hostile input (D-021). Layered defences:

1. **Prompt structure** (`domain/prompts.hpp`): instructions and the location's content settings in the system message; the review JSON-escaped in a nonce-tagged data block that the model is told holds no instruction; never the reviewer's name or photo.
2. **Structured output:** JSON (reply, language, script, suspected manipulation).
3. **Mechanical checks** in C++ (D-058): no links, email addresses or phone numbers; length and emoji; the language of the review or the fallback. Offers, promises and Chinese script are the model's instructions, not code's.
4. **The policy gate** (`domain/review_pipeline.hpp`): a flag, suspected manipulation or a failed check goes to the approval queue; auto reply only when everything passes.
5. **Limits** (D-072, D-110): auto reply off per location until a person switches it on in the reply settings, for reviews with text of at least four or five stars; a daily cap; the engine pauses a location when checks keep failing or an automatic reply is refused or does not appear, and a person resumes it.

| Suite (`engine/tests/ai/`) | Covers | Runs |
| ----- | ------ | ---- |
| Check unit tests | Each check, fixed inputs | Every build |
| Adversarial corpus, offline | Known attacks (English, Chinese, mixed, hidden in spacing or emoji, role-play, fake "system" text) against scripted models: a raised flag is acted on, and a reply with a link, address or number is stopped even when both models were fooled | Every build |
| Adversarial corpus, live | The same against the real model with the seeded configuration; nothing may reach auto reply | By hand, before a release that changes prompts or checks |
| Go-live test (D-074) | The industry's corpus and the location's recent 4 and 5 star reviews, three runs each, with the location's configuration; an AI judge rates the replies | Started by a person per location |

The decisions are plain functions in `domain/review_pipeline.hpp`, called by the job and the offline suite; each case runs as its pack's made-up business.

**Go-live gate** (D-074 to D-077; the rule in CLAUDE.md): `domain/go_live.hpp` computes the fingerprint; `repo::go_live_gate` is a condition of `auto_verdict`; `jobs/go_live.cpp` runs one case per job through the pipeline's functions and asks the judge (by default Anthropic's API). A run writes no draft or post and sends nothing to Google; the location's reviews reach the judge's provider without name or photo (D-075). `ReplySettingsGolden` (`tests/ai/fixtures/reply_settings_golden.json`) fails when a code change alters a prompt for fixed settings.

## 11. Localization

- Reply settings (D-107): the languages a reply may be in and the fallback language (default English). Content settings: `chinese_variant` (`zh-CN`, `zh-HK`, `zh-TW`) and `english_variant` (`en-US`, `en-CA`), each possibly not set.
- The analyzer detects language and script from the original; the writer is told the language, and a set variant's script and phrasing or spelling. Not set, it is told to match the review's (Simplified for a Chinese reply to a review that is not in Chinese), and an English reply stays in English whatever the review's language (D-107 point 11).
- Nothing converts or verifies a Chinese reply's script (D-056).

## 12. Requirements

PONI runs Google Business Profiles (GBP) for its clients. The system automates review replies, location reports and local posts for every client location, through the GBP and LLM APIs directly, with no paid review platform.

### 12.1 Users and access

- PONI staff only. Clients never sign in: they get emails and report links. Nothing should rule out a client-facing product later.
- One role: every signed-in user may do everything; every route has a `RouteInfo`, so roles can be added without touching handlers.
- Google sign-in only, for Workspace accounts on `xvisionmediaus.com` and `xvisionus.com` (D-033, D-053); no passwords stored. A user is created on first sign-in (D-040) or, before it, by a signed-in person who picks them from the Workspace directory, so that they can be assigned an escalation at once (D-106 point 8). A person may use the engine while their Workspace account is active: the staff check reads the directory every five minutes, disables a user whose account is suspended, archived or no longer listed, ends their sessions, keeps them visible as disabled, and enables them again when the list holds the account active (D-106).
- Every change is audited: who (a person, or the system and its component), what, which object and its location (D-103), when, before and after.

### 12.2 Scale and markets

- Design point: 200 clients, about 500 locations, 10,000 reviews a day in total, 100 a day per location (today under 50 clients, 50 to 100 locations).
- United States and Canada; each location has its own time zone, read from Google Places at discovery (D-109).
- Mostly food and beverage, also wellness (spas); behaviour is per industry (FR-3, FR-13).
- Mostly English reviews, some Chinese (Simplified and Traditional) and Spanish. Non-English reviews arrive as "(Translated by Google) … (Original) …"; the original is analyzed.

### 12.3 Functional requirements

- **FR-1 Clients and locations.** Locations are discovered through the GBP API and are the unit of work (D-102): each is switched on by a person, with its own industry and analysis start day, and does nothing until then. A client (brand) only groups locations, for display; a person may delete one, which unlinks its locations and changes nothing else (`repo::delete_client`). Noticing invitations or lost access is not built; lost access shows as an error on the status page.
- **FR-2 Review ingestion.** Polling is the main path, Pub/Sub a second source, an hourly full reconciliation the safety net (D-037, D-091 point 1). The whole review is stored; a reply posted outside the system is detected.
- **FR-3 Review analysis.** Sentiment, topics, items (dishes, treatments, staff), language and script, severity flags, suspected manipulation. Topics, item types and industry flags come from the industry profile; every industry also has discrimination or harassment, staff misconduct, legal or regulatory threat, threats of violence. Flags and manipulation are the model's alone (D-058).
- **FR-4 Reply generation.** Each location has its own complete content settings and reply settings (D-107), started from the knowledge base's defaults or templates. A reply is in the review's language as the variants shape it (section 11). Every draft records model, prompt version and inputs.
- **FR-5 Tiered reply policy.** Per location: a review with text of at least the location's minimum of stars (4 or 5), without a complaint, may auto reply (a review with no text is never auto replied); a complaint, three stars or fewer, a severity flag, suspected manipulation, a failed check, an edited review and a draft a person edited go to the approval queue. Auto replies wait an optional random delay inside an optional posting window in local time (for example 09:00 to 21:00); the review is read again just before posting. In the queue a person edits, regenerates with instructions, approves, rejects, or sets the review aside (D-068).
- **FR-6 Escalation.** A severity flag on an unreplied review raises an immediate alert and an escalation (`open` or `closed`, one assignee; D-065), a row in `cases`.
- **FR-7 Notifications.** Channels behind one interface (D-097): email through Gmail, and push to the phone app through Apple's push service, for escalations and system problems (D-097). Rules map event type × location (or every location) to recipients. Events: `escalation`, `approval_needed`, `system_problem` (lost access, token failure, failing jobs, a post rejected or a send failed, a staff account disabled or active again, a staff check that could not use the directory), `location_report`. Every delivery is logged. A queued one is retried; a report a person sends is tried once per address and not retried (D-104 point 4).
- **FR-8 Reports.** One kind, a location's report per period (D-085, D-102): reviews, ratings and their change, response rate and time, sentiment, topics and items, highlights, escalations, employee recognition (D-086), Performance API visibility and search keywords, and a model-written summary. A location's report settings choose the sections a report has, and a report records the format and sections it was created with; a left-out section is not computed (D-111). Built once. A location has any number of schedules, daily, weekly or monthly, each with its own recipients and, if it says so, its own sections; schedules, report periods and a person's report follow the location's time zone (D-109, D-111). PDF is the browser's print (D-035).
- **FR-9 Profile accuracy.** Not built: compare a profile with a reference record and alert, never edit.
- **FR-10 Dashboard.** Its own repository (section 1): reviews, approval queue, escalations, clients and locations, content and reply settings, posts, knowledge base (read-only), notification rules, reports, audit log, status.
- **FR-11 Users and audit.** Google sign-in and sign-out; a user is made at first sign-in or, before it, from the Workspace directory by a signed-in person; a user follows their Workspace account, disabled and enabled by the staff check alone (D-106); the audit log is append-only, viewable and filterable.
- **FR-12 Cases.** After the MVP: following a negative review from first alert to final public response; today's escalation is its simplest form (D-054).
- **FR-13 Knowledge base (D-078).** What is the same for every client and nobody changes while the system runs is files reviewed like code, checked and seeded by `poni-tools` into static, versioned tables. An industry is supported only with a complete pack; a location's settings start as a copy of the defaults (section 9).
- **FR-14 Local posts (D-088, D-099).** Updates, offers and events with Google's fields and up to ten photos and videos (D-101). A person approves the exact content; Google schedules and repeats. Every post of a location is shown with its state at Google, including posts made there; staff may remove any, and correct one in place by an edit a person approves (D-099). The model helps only when asked. Not included: client approval, figures per post, product posts.

### 12.4 Non-functional requirements

- **Latency:** with the delay off, 95% of auto-replies posted within 2 minutes of the review reaching Google (a target).
- **Completeness:** a lost notification never means a missed review.
- **Safety:** never overwrite a reply someone else posted; check again before every post; jobs can run twice.
- **Cost:** infrastructure under $100 a month; model cost recorded per call (`llm_calls`).
- **Operability:** JSON log lines, `/api/health`, a status page, nightly backups off the server.
- **Portability:** macOS for development, one Linux server in production, arm64 and x86_64 (D-034). Security: section 8.

### 12.5 Out of scope

Client logins and roles; Q&A (Google shut its API down); editing a profile; removing or flagging reviews (no API); profile photos and product posts; social media monitoring.
