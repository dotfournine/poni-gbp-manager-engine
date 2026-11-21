# Decisions

The decisions in force, by number. Code comments and `CLAUDE.md` cite these numbers, some with a point ("D-088 point 6"), so numbers and point numbers are never reused; a point missing from a numbered list was superseded. Each entry says what holds today, with any later amendment already applied. `CLAUDE.md` has the operational rules in full.

A change of direction is proposed to the owner first and recorded as a new entry with the next number. The entry it replaces becomes a one-line "Superseded by D-xxx" pointer.

## Stack and build

### D-001 · The engine is written in C++23
The owner and AI maintain it; C++26 compiler support is still partial.

### D-002 · Networking uses Boost.Asio and Boost.Beast
Coroutines keep async code readable.

### D-003 · The engine owns every external integration
The dashboard and the phone app talk only to the engine: one place for credentials, rate limits and audit.

### D-004 · The API is REST/JSON, spec-first in OpenAPI 3.1
`api/openapi.yaml` is the one contract; clients are generated from it.

### D-005 · PostgreSQL is the system of record and the job queue
One datastore to run; `SKIP LOCKED` queues are enough at this scale.

### D-011 · The dashboard is React-admin, TypeScript and Vite, a static SPA
Use React-admin's and Material UI's components; no hand-built components, no other UI library.

### D-014 · libpqxx on a dedicated thread pool
There is no maintained Boost-native PostgreSQL client. Blocking calls go through `Database`, never on the `io_context` thread.

### D-028 · Toolchain
Homebrew LLVM on macOS; Ubuntu 26.04 with apt.llvm.org clang in containers (`linux-clang`, the only Linux preset, built against libstdc++); vcpkg manifest with a pinned baseline and overlay triplets. Engine code uses only C++23 that works in both libc++ and libstdc++ (`CLAUDE.md` lists what is not available). GCC is not supported (D-091 point 8).

### D-029 · Time zones come from a library, not `std::chrono`
libc++ has no `std::chrono` time zones. The library is cctz (D-071).

### D-034 · The engine builds for arm64 and x86_64 Linux
CI builds and tests both (D-091 point 9).

### D-079 · The engine is a daemon and nothing else
`poni-gbp-engine` takes no command. (1) Operations are `poni-tools` in `tools/` (`migrate`, `check`, `seed`, `pack`, `publish`, `test-db`); in Compose, `migrate` and `seed` run as one-shot services before the engine, which at start only checks: no pending migration, a seeded knowledge base, the Google target (D-087). (2) The GBP managing account (D-043) and the sending mailbox (D-066) are connected by a signed-in person from the dashboard (`http/connect.hpp`). The account's checks run, and the token is stored encrypted with an audit entry in that person's name; secret, token and key never reach the browser or a log. (3) Tests start from a copy of one database `poni-tools test-db` prepared per build; the engine applies no migration.
Why: one process with one job is easier to reason about, and no second codebase handles the client secret and the encryption.

### D-091 · The engine simplified
(1) Pub/Sub stays beside polling. (2) A report's JSON is kept in `reports.data` only. (3) API surface no client used was removed. (5) The voice field `other_business_names` is removed: nothing read it. (6) Every setting stays. (7) No column is dropped only because nothing reads it. (8) GCC support is deleted. (9) CI builds both architectures. (10) Migrations start from `0001_baseline.sql`, which replaced 26 earlier ones once, while nothing was deployed; `ReplySettingsGolden` (never regenerated) proves a code change alters no prompt. An applied migration is never edited. (12) The sign-in ID token's signature is not verified: it is the token endpoint's answer to the engine's own code exchange over verified TLS (OpenID Connect Core 3.1.3.7). Its claims come only from `google::exchange_code` and are still checked; no other token is read without a signature check. (13) A local post's content is one `jsonb` column beside `approved_content`. (15) D-077 is accepted as built.

### D-092 · A request body is read exactly as the spec says
One parser (`core/body.hpp`): an unlisted member is a 400 naming it; `null` only where allowed; PATCH is JSON Merge Patch (RFC 7396); `100.0` is an integer; no route has its own rule.
Why: a body silently cut down makes a person believe a setting was saved. So the engine ships before a client that sends a new request member.

## Data, sync and jobs

### D-012 · Pub/Sub over REST, not `google-cloud-cpp`
Pull and acknowledge only; no gRPC, Protobuf or Abseil for two HTTP calls.

### D-037 · Review sync: three paths into one idempotent store
(1) Every minute one `batchGetReviews` per account for the locations whose poll is due. (2) A full reconciliation per location on linking, then hourly. (3) Pub/Sub notifications. A review is marked removed only when Google answers `NOT_FOUND` for it.
Why: notifications lag about 15 minutes, a reply added elsewhere does not move `updateTime`, and Google's lists are not reliably complete.

### D-038 · Only linked locations are synced
Reviews are synced only for locations linked to a client that is not archived; others are listed only. Linking is the step that says "we manage this one".

### D-039 · What the sync notices goes to `review_events`
Not to the audit log, which holds what people do and what the engine does to its own objects. Mirroring reviews would bury the audit log.

### D-044 · A review's language is first guessed from its script
Reviews carry no language field. The analyzer's answer replaces the guess and tells Simplified from Traditional.

### D-046 · Which reviews need work is read from the data
A review needs work when its current version has no analysis, or it has no reply and no live draft; a job every minute queues `reviews.analyze` for them, a batch at a time. A review with a reply gets an analysis, no draft.
Why: the pipeline is idempotent and self-healing; an edited review or a dead job is picked up by the next pass.

### D-055 · A client may set the day its analysis starts from
`analyze_from`: only reviews written on or after it are analyzed and drafted for; none means every review. Staff can still analyze an older review by hand.

## Google writes and posting safety

### D-042 · The outbound client is narrow by construction
No PATCH. PUT for one address (D-061), DELETE for one (D-088 point 4), a POST to `mybusiness.googleapis.com` only for `batchGetReviews` and the create of a local post. One function, `http::why_refused`, decides before a name is resolved, and `FakeHttp` asks it too.
Why: "the engine writes only these things to Google" is a property of the code, not a habit.

### D-061 · Replies are written by one PUT
`…/reviews/{r}/reply`, behind `gbp::ReplyWriter` (one method), called only by `jobs/posting.cpp`. The engine's writes at Google are exactly three: this PUT and the create and delete of a local post. A wrong id cannot turn the call into an update of anything else.

### D-062 · How a reply is posted
(1) A post starts from a signed-in person's approval of the exact stored text, or from the policy (D-069). (2) The job reads the review from Google again; `repo::claim_post` checks in one transaction that it is unchanged with no reply but the engine's own, reads both switches, and opens a row in `reply_posts` (one open post per review). (3) One PUT. A 4xx means nothing was posted; a timeout, dropped connection or 5xx leaves the post open and is never retried. (4) `posted` on 200, `published` when a read shows it. (5) While a post is open nothing else is posted to that review; after `PONI_POST_CONFIRM_WINDOW_S` an unseen post is closed as unconfirmed and a person decides.
Why: Google shows a reply minutes after accepting it, and a second PUT replaces whatever is there.

### D-063 · Two kill switches
`system_settings.posting_enabled` (off after installation) and `locations.posting_enabled`. Both must be on to approve and are read again at the claim. A draft a switch stopped returns to the queue; switching back on posts nothing. They cover auto-posts and local posts. Only people flip them.

### D-069 · Auto-posting
A draft routed `auto_post` is scheduled by `repo::schedule_auto_post`; it and `repo::approve_draft` are the only two that enqueue `replies.post`, and both end in `repo::claim_post`. The conditions, in `auto_verdict`, run at scheduling and again at the claim: the switches (D-063, D-070), no pause and the cap (D-072), a go-live pass (D-074), route `auto_post`, the pipeline's own words (D-067), an unedited review with no earlier draft or attempt, no open escalation, checks passing, inside the window (D-071). A draft that fails one later goes to the approval queue and is never scheduled again by code. A post is stood behind by a person or by the policy (`policy_version`), never both and never neither.
Why: one job and one claim mean every guard of D-062 holds for auto-posts too.

### D-070 · Auto-posting is switched on per location and star rating
`auto_post_enabled`, `auto_post_five_star`, `auto_post_four_star`, off by default; three stars or fewer never auto-post. Only a signed-in person changes them (audited). Switching on needs a star rating, a linked and accessible location and usable timing. Linking to another client or unlinking switches them off. Code, migrations and tests never switch them on.

### D-071 · Timing of an auto-post
A random delay and a local posting window are policy fields; the location's IANA time zone is entered by a person (Google gives none). The time is now plus the delay, moved into the window (next opening plus the delay again); a window may cross midnight. No time zone or impossible timing means no auto-post; a job that runs late outside the window does not post. cctz reads the OS zone files, so images need `tzdata`.

### D-072 · Daily cap and pauses
(1) At most `auto_post_daily_cap` automatic replies per location in 24 hours (default 100); scheduled drafts count. (2) The engine pauses a location after N drafts in a row failed a check (`auto_post_pause_after_failures`, default 3), on an unconfirmed automatic reply, or when Google refuses one for good; everything, when the refusal concerns the shared credentials. Only a person lifts a pause, and resuming posts nothing that was stopped.

### D-076 · Amendments to auto-posting
No earlier human-approved reply is needed before switching on (the go-live gate is the safeguard); the cap defaults to 100; the pause counts failures in a row, per location. D-070 and D-072 are written with these applied.

## AI replies and go-live

### D-006 · Tiered reply policy with escalation
Every review ends as `auto_post`, `approval_queue` or `escalate` (FR-5, FR-6). Auto-posting a serious complaint is the biggest business risk.

### D-010 · One LLM client, DashScope first
`llm::Client` is the one way to a model, with two protocols (D-074 point 8). DashScope (Qwen) through its US-region endpoint is the first provider; a key works only with its own region's address. The model is chosen per task.

### D-021 · Layered defences against manipulative reviews
Structured prompts, mechanical checks and the policy gate, with a permanent adversarial suite (`ctest -L ai`). Strangers write reviews; replies are public under the client's name. The layers today: D-058.

### D-022 · Localization per location
Part of a location's reply settings (D-093): reply languages (English, Chinese, Spanish), Chinese script and regional phrasing, English variant, fallback language. The script is not verified by code (D-056).

### D-024 · Reply quality is rated by an AI judge from another model family
A judge of the writer's own family favours its writing. The rubric is in `domain/prompts.cpp`; the judge is D-031.

### D-030 · Default models
`qwen3.7-plus` for every DashScope task in `knowledge/defaults.json`; `qwen3.8-flash` is priced as the cost fallback. The model is a setting per task.

### D-031 · Claude Opus 5.5 is the judge
`claude-opus-5-5` through the `anthropic` provider by default; a different family from the writers.

### D-048 · How model failures are handled
A wrong address or key (`Error::configuration()`) fails the job and marks no review. A transient failure is retried and, on the last attempt, stored as a failed analysis or an empty draft for the approval queue (not escalated). A content-filter refusal is stored as failed at once. Every request is a row in `llm_calls` with an estimated cost.

### D-051 · Prompts are in code and versioned
Every analysis and draft records `prompt_version()` (a revision plus a hash of the prompt texts), the model and the inputs.

### D-056 · No conversion or check of Chinese script
The writer is told the script and phrasing; its text is stored as written. The analyzer says a review's script; without one the reply is Simplified.

### D-057 · Model cost is summed per location
And per client, model, provider, task and day.

### D-058 · Severity and manipulation are detected by the model alone
No keyword rules or pattern library. Code checks a reply only for links, email addresses, phone numbers, emoji, length and language (plain scans after `text::fold`). The offline suite proves a raised flag is always acted on and contact details are stopped even when both models were fooled; the live suite (`engine/scripts/linux.sh ai-live`, by hand) measures how often the model is fooled, and a prompt or check change needs a live run with no case reaching auto-post. Offers, promises and spelled-out contact details are not caught.

### D-059 · Several model providers
A provider is `PONI_LLM_<NAME>_BASE_URL`, `_API_KEY` and `_PROTOCOL` (`openai` or `anthropic`, required), any name. Each task names its provider and model; every call records its provider.

### D-064 · A draft with a failed check cannot be approved
Checks run on every edit and at approval, with no override: change the text, or the voice setting the check reads.

### D-067 · Regenerate with instructions
The staff member's words follow the rules in the writer's system message, and a rule wins a conflict. Such a draft goes to the approval queue (`regenerated_with_instructions`) and never auto-posts: the adversarial suite never ran with added instructions.

### D-068 · "No reply needed"
A signed-in person can mark a review with no reply and none on its way: its draft is cancelled (kept), none is written, and it leaves the queue. Audited, undoable, optional reason; it ends when the reviewer edits the review. An escalation is untouched.

### D-074 · The go-live test gates auto-posting
(1) A location's fingerprint (`domain::fingerprint`) hashes what changes what the models are asked and what code checks: rendered prompts, each task's provider, model, temperature, token limit and extras, the check limits, and `kGateVersion` (raised by hand when the gate or checks change). (2) A location auto-posts only while its fingerprint has a pass covering its policy's width (`auto_verdict`). A pass switches nothing on. (3) A new fingerprint or a wider policy needs a new test; cap, timing and corpus additions do not. (4) Until a pass, the location does not auto-post. (5) Only a signed-in person starts a run (`repo::start_go_live`), never code or a test against a real database. One job per case, with the real settings and the pipeline's prompts: the industry's corpus and the client's reviews (D-075), 3 times each; replies routed to auto-post are judged. It fails when an attack or severity case reaches auto-post or a reply is "unacceptable"; only "passed" opens the gate. (6) The fingerprint sits beside D-069's policy version. (7) The writer's temperature is `Voice::temperature`; each task has a "send a temperature" switch. (8) `llm::Client` also speaks Anthropic's Messages API (`_PROTOCOL=anthropic`); the judge is its own task. (9) Nothing in CI calls a model.

### D-075 · A go-live test includes the client's own reviews
Up to 20 recent 4–5-star reviews with text and 2 without, through `domain/prompts.hpp` (never the reviewer's name or photo); so real review text reaches the judge's provider during a test, and only then. Replies are kept with the run in the database only.

### D-077 · Details of the go-live test
(1) The newest pass or fail of a configuration decides. (2) A failed test runs to its end. (3) A setting changed under a running test cancels it and the engine starts a new test of the new configuration in the submitter's name: the one case in which the engine starts a test. (5) Only "unacceptable" fails. (8) The Anthropic protocol is never sent a temperature. (12) Cases use the widest policy among the locations sharing the fingerprint. (14) A provider's address and protocol are not in the fingerprint. (15) `PONI_LLM_<NAME>_PROTOCOL` is required.

## Knowledge base and reply settings

### D-023 · Industry profiles define topics, item types and severity flags
Two industries, `food_beverage` and `wellness`, each a pack in `knowledge/industries/<key>/`.

### D-078 · The knowledge base
(1) What is the same for every client: industry packs (profile, corpus, made-up test business), common flags, voice templates, defaults and model prices. Prompts, the rubric, checks and limits stay in code. (2) JSON files in `knowledge/`, a JSON Schema per kind (unknown keys refused), and `manifest.json` (format and version). (3) `poni-tools seed` checks the files and replaces every `kb_*` table in one transaction; nothing else writes them. The version (`YYYYMMDD[-N]`) is set by hand with every change. The engine needs a seeded base to start and reads the tables on each use, caching nothing. (5) A client's industry is a reference by key (`clients.industry_key`); a client whose industry is missing is not analyzed. (6) Supported means a complete pack (`poni-tools check`): 2 cases per applicable severity flag, 10 manipulation cases (one Chinese, one mixed language), 3 ordinary ones. (7) The go-live test reads the corpus from `kb_corpus_cases`; a version that changes what the analyzer is told changes that industry's fingerprints. (8) `pack` and `publish` put `knowledge-<version>.tar.gz` in `PONI_KNOWLEDGE_BUCKET`, only from a clean `main`; production names `PONI_KNOWLEDGE_ARCHIVE` and runs the seed, with no new image or restart. (9) Tests seed from `knowledge/` with the same tool, and a test reads every pack through the engine's structs.
Why: a profile and its corpus must agree, and a reviewed file change cannot silently change every client's fingerprint as a dashboard edit could.

### D-080 · A voice's base style; the tools package
(2) A voice has `base_style`, filled from a template's label and description when the template is taken and rendered into the writer's prompt. (4) The tools are one Python 3.12+ package run with `uv run poni-tools`, dependencies pinned in `uv.lock`; CI runs ruff, pytest and `check`.

### D-082 · A corpus is not signed
A pack is complete by its numbers alone (D-078 point 6). The review of the cases is the pull request's review, which says which severity and manipulation cases are new. A signature recorded a claim, not a reading.

### D-084 · The rule for copies, and no watcher
What a location can change (voice, localization, policy, a taken template) is copied into its own row (D-093); what nobody changes (profile, flags, corpus, prices) is read from the tables. Nothing watches for a new version or notifies: `poni-tools seed` prints the clients and locations that need a new go-live test and the clients whose industry is gone. The dashboard compares `industry_key` with `GET /api/knowledge`.

### D-093 · A location's reply settings are its own complete row
(1) `reply_settings` holds one complete row (`voice`, `localization`, `policy`) per linked location; nothing is merged when read, and a missing field is an error. A client has none. (2) `GET` and `PUT /api/locations/{id}/reply-settings`; a save sends every field and replaces the row. Taking a template is the dashboard filling the form. (3) A newly linked location copies the client's earliest-linked location, or the defaults for the first; an unlink or a move deletes the row into its audit entry; only `repo::update_location` does either. (7) A field added later reaches stored rows by a migration, never from a default at run time. (8) No PATCH merges deeply. Saves and copies start no go-live test.
Why: a voice describes one business, and every reader already asks by location.

## Cases and notifications

### D-020 · Superseded by D-097

### D-026 · Email goes through the Gmail API from a Workspace mailbox
`PONI_EMAIL_FROM` on `xvisionmediaus.com`; it reuses the engine's Google OAuth code, with no new vendor.

### D-054 · Escalations are the simplest form of a case
A row of `cases`: kind `escalation`, opened by a severity flag on a review with no reply, `open` or `closed`, one assignee, a closing reason; one case per review, reopened when a newer analysis is still flagged. The full case workflow (FR-12) comes later and grows out of this table.

### D-066 · Notifications
`repo::emit` raises an event in the transaction that caused it: one row per recipient in `notification_deliveries`, sent by a job, unique per recipient by its key. Rules map event type and client to recipients. While sending is off or no sender is connected, a delivery is recorded as not sent and never sent later. The mailbox has its own `gmail.send` token. Review text is in the body only.
Why: an alert exists exactly when its cause was committed; a late alert without context is worse than a logged miss.

### D-097 · Push notifications to the phone app
(1) `push` is a second notification channel beside email; channels stay plug-ins (`connectors/notify`). (2) The engine sends to Apple's push service (APNs) itself, over HTTP/2: nghttp2 (a vcpkg dependency) frames the bytes on the engine's own TLS stream, with ALPN `h2`. No relay (Firebase or another vendor) and no sidecar. One connection per delivery: opened, used for the user's devices, closed. (3) One environment per engine: `PONI_APNS_ENVIRONMENT` is `production` or `sandbox`, with one token-based key (`.p8`, environment-specific and topic-specific for the app's bundle id), its key id, the team id and the bundle id, all or none, from the environment only and checked in `load_config`. A registration for the other environment is a 400. The provider token (an ES256 JWT signed with OpenSSL) is kept 50 minutes and dropped on `ExpiredProviderToken`; the job's retry then sends again. (4) The outbound client takes one request to Apple: a `POST` to `/3/device/<token>` on the configured environment's host, the token hex only, refused by `http::why_refused` in any other shape. (5) A device belongs to the app session that registered it (`devices.session`, deleted with the session) and has no user, environment or last-seen of its own: a logout, an ended session and a disabled user stop its pushes. `POST /api/devices` stores the token for the calling session, one device per session, and a token another session had moves to this one; it writes an audit entry only when something changed. `DELETE /api/devices/{id}` removes the device. A device token is in no log. (6) A rule may have the channel `push` only for `escalation` and `system_problem`; its recipients are staff emails. A push rule is refused while no APNs key is configured. Push has no global switch: an enabled push rule is the person's switch (D-066's switch stays for email). (7) One delivery per user, as for email, which keeps the ids a push opens (`escalation_id`, `local_post_id`): the job sends to every device the user has at that moment. It succeeds when every device accepted or was dropped, any temporary failure retries the whole delivery, and the reason says how many devices were reached. A resend is harmless, because `apns-collapse-id`, from the event's key, replaces the alert already shown. (8) The alert is the event's `subject` under a fixed title per event type, never its body. The payload carries `event_type` and ids only (escalation, review, location, local post; `NotificationEvent` gains the escalation's and the post's id), a category (`ESCALATION` or `SYSTEM`), a thread id per kind (`escalation`, `system`, as the app asked) and a sound. (9) A device is dropped when APNs answers `410 Unregistered` or `400 BadDeviceToken`; a refused key, team or topic raises one `system_problem`. Supersedes D-020.
Why: Apple speaks only HTTP/2 and Beast only HTTP/1.1. nghttp2 is a small C library without dependencies that works on the stream the engine already owns, so the one thread, the TLS context and the write rules stay as they are, and only Apple sees an alert. At our volume (hundreds a day) every push service costs about nothing; what differs is work and parts: Firebase would change the app and add a Google host, a sidecar would add an image. Apple's advice to keep connections open does not matter at this volume, and pooling can come later. Tying a device to its session answers who may receive a push with the rules sessions already have. Push needs no switch of its own because it reaches only staff's own signed-in phones, while email can reach clients.

## Reports

### D-085 · Client reports
(2) Built once by `reports.build`, never changed: `reports.data` and `reports.html`, self-contained HTML with no script. (3) Every text through `html_escape`; figures in `domain/report.hpp`. (4) No report is served to anyone not signed in: a share link is `r/<token>.html` (32 random bytes) in the public bucket, and revoking overwrites it. (6) Performance figures are read at build time and kept in the report only. (7) The model writes only the summary, from the figures, never review text or a name. (8) Highlight reviews show the reviewer's name and photo, as on Google. (9) A `client_report` rule must name a client. (10) A report is sent by a person, or by a schedule with "send automatically" switched on by a person.
Why: a page without scripts is frozen and needs no server of ours; unguessable names in a public bucket take the only public route off the engine.

### D-086 · Employee recognition in the client report
Only staff a reviewer named count (item types with `"person": true`), in two ranked lists per location and name (`ReportData.staff`): praised and named in complaints. People are left out of the item lists, so the summary model gets no staff name. Counts only; spellings are not merged.

## Local posts

### D-088 · Local posts
(1) A local post belongs to one location and has Google's `LocalPost` fields (`standard`, `event` or `offer`). (3) A post reaches Google only after a signed-in person approved that exact content (`repo::approve_local_post`, the only function that enqueues `local_posts.publish`); the claim checks the content, both switches and the checks again. Only a draft can be edited or rejected; nothing is edited at Google. (4) `gbp::LocalPostWriter` creates and deletes, called only by `local_posts.publish` and `local_posts.remove`; the DELETE goes only to `.../locations/{l}/localPosts/{p}`. Nothing else is deleted at Google. (5) Scheduling and recurrence are Google's; the engine has no timer for posts. (6) A create is sent once: only 400, 401, 403, 404, 429 or no token is a refusal (`definitely_refused`); any other answer makes the post `unknown`, never sent again and settled by the sync. An approval expires after 30 minutes. (7) A post is deleted only because a signed-in person asked (`repo::request_local_post_removal`); a `NOT_FOUND` for a post the job could not read first proves nothing. (8) The sync reads posts hourly, marks one removed only after a get answered `NOT_FOUND`, and never writes. (9) One photo or video, checked from its bytes and stored exactly as uploaded as `p/<32 random bytes>.<ext>` in the public bucket, because Google fetches media only from a public address. (10) Checks in code (`domain::run_local_post_checks`): lengths, no contact details, an `https` button, a future time, a fetchable photo. (11) The model helps only when a person presses a button (`jobs::PostAdvisor`), stores nothing, never sees a reviewer's name or photo or a flagged review, and is not in the go-live fingerprint. (13) Not built: editing at Google, automatic posting, posts in the Google mock.
Why: deleting is the only way to stop a scheduled post or take down a wrong one, so the delete rule was lifted for local posts and for nothing else.

### D-090 · A local post can carry a video
MP4 or QuickTime, recognised from its bytes (`domain::inspect_video`), at most 30 seconds and 75 MB, 720 px on the short side; no codec check. The upload route alone has its own body limit (75 MB); for it the server checks the caller from the headers before reading the body (`Router::refuse_before_body`), and a file is moved, never copied.

### D-094 · Copy a post, delete a draft, filter the list
(1) `POST /api/local-posts/{id}/copy` writes a new draft from any post and queues nothing; what a draft cannot hold is dropped and named, and a post made at Google brings no media (the engine fetches nothing from Google's media hosts). (2) `DELETE /api/local-posts/{id}` deletes only a `draft` or `rejected` post; never a `failed` one, because a send that was given up is still adopted when its post turns up at Google. (3) The list filters by `status` and `google_state` lists. (4) No one-step replace: it would make an approval cause a delete at Google.

## Sign-in and sessions

### D-007 · Internal users, one role, full audit log
Every route passes the one authorization hook, so roles can be added later.

### D-033 · Sign-in is Google sign-in
Workspace accounts of the listed domains only (D-053); no passwords; server-side sessions; the audit log records the Workspace email. A user row is created at first sign-in.

### D-040 · The dashboard's session
A random token in an `HttpOnly`, `SameSite=Strict` cookie (`__Host-` and `Secure` over HTTPS), its hash in the database; 7 days at most, 24 hours idle. A changing request needs `X-CSRF-Token` and, when sent, our own `Origin`.

### D-041 · Development sign-in
`POST /api/auth/dev-login` works only with `PONI_DEV_LOGIN_EMAIL` set and `PONI_PUBLIC_URL` plain `http://localhost`, so one stray variable cannot enable it in production. It is how CI's contract check and the Google mock sign in.

### D-043 · The stored GBP credential
Client ID, client secret and refresh token together (a refresh token works only with its client), encrypted with AES-256-GCM (`core::SecretBox`) under `PONI_SECRET_KEY`; stored only from the dashboard (D-079 point 2).

### D-053 · Sign-in domains
`PONI_SIGNIN_DOMAINS` (`xvisionmediaus.com`, `xvisionus.com`). An account gets in when its address is verified and both its Workspace organization (`hd`) and its address's domain are listed (`http::domain_allowed`).

### D-095 · Sign-in for the phone app; two kinds of session
(1) One Google sign-in for both, every check in the dashboard's code path: the app starts with `client=ios` and a PKCE S256 challenge, and its callback redirects to the constant `gbpmanager://auth` with a one-time code, setting no cookie. (2) `POST /api/auth/app-token` exchanges code and verifier for a bearer token; a code lives 60 seconds, is stored hashed and is spent by the first request (`repo::take_app_code`); every refusal is the same 400. (3) A session is `browser` (cookie) or `app` (`Authorization: Bearer`, that header only), both through `AuthService::authorize`; a bearer request gets no CSRF or Origin check. (4) An app session lasts 30 days idle, 90 at most. (5) An inactive user has no session. (6) Logout with a token ends it. (8) From the app's first release, removing or renaming a response member or narrowing a response enum is breaking and agreed first; adding is not. No token, code or verifier is logged or put in an address.
Why: the browser sheet cannot hand a cookie to the app, and PKCE (RFC 8252) makes a code caught by another app useless.

## Operations and environment

### D-008 · Hosting on Google Cloud, under $100 a month
One Compute Engine VM running Docker Compose.

### D-009 · A personal Gmail is the GBP managing account for now
Move to a PONI organization account before onboarding beyond the MVP clients; every client then re-invites.

### D-016 · No Q&A features
Google shut down the Q&A API.

### D-017 · Profile accuracy is alert-only
The system never edits a profile.

### D-027 · The GBP API applicant enables the v4 reviews API
The v4 API was allowlisted for the account that applied, not the project; plan for it when moving accounts.

### D-036 · Superseded by D-096

### D-045 · Production images and backups
Images for x86_64 and arm64 from a registry; the web image (Caddy with the dashboard) comes from the dashboard's repository (D-096). A `backup` container runs `pg_dump` nightly, keeps 14 days and uploads to Cloud Storage. The VM's service account replaces key files.

### D-052 · Production VM and address
One `e2-medium` VM (x86_64, 4 GB); the dashboard at `gbp.xvisionmediaus.com`. Not deployed yet (`deploy/README.md`).

### D-087 · A local mock of Google for development
`uv run poni-tools mock-google` on 127.0.0.1, with a test portal and made-up data outside the repository; no Pub/Sub, deletes or posts. The engine's switch is `PONI_GOOGLE_MOCK_URL`, loopback only; only `http::NetClient` rewrites to the mock, after the write rules are checked on Google's address. `system_settings.google_target` marks a database `google` or `mock`, and the engine refuses to start when mark and setting disagree. The mock is in no image or Compose file.
Why: a real database against the mock would see every review as removed, and a mock database against Google would post replies to made-up reviews.

### D-096 · Repositories
The engine, its tools, the knowledge base, the API contract, the deployment files and CI live in this repository (`dotfournine/poni-gbp-manager-engine`). The dashboard lives in a repository of its own: it generates its client from this repository's `api/openapi.yaml` and builds the web image (Caddy with the dashboard). The iOS app is in its own repository. Supersedes D-036.
