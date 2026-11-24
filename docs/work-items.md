# Work items

The engine's backlog: what is not built or not done yet, by area, the most urgent first. What was built is described in [architecture.md](architecture.md) and [decisions.md](decisions.md).

An item is removed when it ships. A change of direction is proposed to the owner and recorded as a new entry in [decisions.md](decisions.md); this file then follows it. Each item says what and why, what done looks like, and which rules in `CLAUDE.md` and which decisions it touches. "The owner's" marks an item only the owner can do or decide.

## Next: the phone app

The app's requests are in the app repository (`poni-gbp-manager-ios/docs/design/engine-requests-app-v1.md`). Sign-in with a one-time code and PKCE, the bearer token, the developer sign-in that returns a token, and sign-out of a token are built (D-095, migration `0002_app_sign_in.sql`). Copying a post, deleting a draft and the list filters are built in the form D-094 settled; replacing a post in one step was declined there. The second round's list order, `google_state=none` and the link from a copy to its source are built (D-098), and so are a location's time zone and the time of its last failed read of posts, a report's average rating and response rate, an escalation's deliveries by its id, and the contract's account of what a push says (D-098 points 6 to 9). The app's sign-in answers the attempt limit with a redirect to the app (D-098 point 10). Push is settled by D-097. Editing a post at Google in place is built (D-099, migration `0005_local_post_edits.sql`), from Google's documentation and without a trial: the first real edits are watched (see "The first real local posts").

### Push notifications to the phone app (APNs)

**What and why.** The app has alerts in v1: a new escalation, auto-posting paused at a location, and a local post that Google refused, that failed, or that could not be removed. Without push the app only refreshes when a screen appears, when it returns to the foreground and on pull to refresh. The app's request 4 asks for push; D-097 settles how, with every question this item used to leave open answered there.

**What to build** (D-097):

- **Spec first.** A `Device` (`id`, `environment`, `app_version`, `created_at`, `last_seen_at`, the last two read from the session), `POST /api/devices` (`apns_token`, `environment`, `app_version`; 201, or 400 for the other environment) and `DELETE /api/devices/{id}` (204, or 404), and `push` in `NotificationRule.channel`. All are additions, compatible with released apps (D-095 point 8).
- **Migration 0003.** `devices` (`id`, `session` referencing the session and deleted with it, unique per session; `apns_token` unique; `app_version`; `created_at`), `notification_rules.channel` widened to `email` or `push`, and a check that `push` is only for `escalation` and `system_problem`. `notification_deliveries` keeps one row per user for push too, and gains the two ids a push opens (`escalation_id`, `local_post_id`).
- **Config.** `PONI_APNS_ENVIRONMENT`, the key (`.p8`), the key id, the team id and the bundle id, all or none, in `load_config`; in `poni.env` and `engine.env`, never in the repository, a test, a fixture, a log or the browser, and neither is the provider token.
- **Transport.** One function in `http/` that opens a TLS connection with ALPN `h2` to the configured host, sends one stream per request through nghttp2, and returns each status and body. Its rule in `http::why_refused` (a `POST` to `/3/device/<hex token>` on that host and nothing else) gets lines in `tests/unit/write_path_test.cpp`. A slot from `core::RateLimiter` before every request. nghttp2 goes into `engine/vcpkg.json`.
- **Connector.** `notify::Apns`: the provider token (ES256 with OpenSSL, kept 50 minutes, dropped on `ExpiredProviderToken`), the request headers (`apns-topic`, `apns-push-type: alert`, `apns-priority`, `apns-expiration`, `apns-collapse-id` from the event's key), and Apple's answers mapped to `notify::SendError`. `FakeApns` in `tests/fakes.hpp`; tests never reach Apple.
- **Events and the notifier.** `NotificationEvent` gains the escalation's and the local post's id, set where those events are raised. A pure payload builder in `domain/` takes the subject, the event type and the ids, and nothing else, so no body text can reach it; a unit test says so. The notifier sends a push delivery to the user's devices at that moment, on one connection, drops a device on `410` or `400 BadDeviceToken`, and raises one `system_problem` when Apple refuses the key, the team or the topic.
- **Device routes.** Through `api.change`; a registration that changes nothing writes nothing.
- **Dashboard and app.** The dashboard's rule form gets the `push` channel (dashboard repository). The app's side is told what differs from its request: a registration for the other environment is a 400; the alert's line is the event's subject; a push delivery is one row per user, and the delivery's reason says how many devices it reached.

**Owner's steps.** Create two APNs keys in the Apple Developer account, each environment-specific and topic-specific for the app's bundle id: the sandbox key goes into the Mac's `engine.env`, the production key only into production's environment.

**Done when:** a device registers and unregisters through the API, and a logout or an ended session stops its pushes; a push rule for `escalation` or `system_problem` delivers to every device of the users it names, with the subject and ids only; an invalid token is dropped; deliveries show in the log with `channel: push`; without the APNs settings push rules are refused; tests cover all of it against `FakeApns`, and one push reaches a build run from Xcode from a local engine with the sandbox key. After the first deployment, one push reaches a TestFlight build.

**Touches:** D-066, D-095 points 5 and 8, D-097, the rules on secrets, notifications, outbound HTTPS, audit and spec-first API changes in `CLAUDE.md`, which gets the rule for Apple's address when this is built.

### The app's spec follows this repository's contract

**What and why.** The app builds its client from its own `docs/api/openapi.yaml`, merged from a projection in the dashboard's repository (`docs/ios-app/openapi-projected.yaml`) and the app's requests. The projection's order 15 differs from what was built: `POST /api/local-posts/{id}/copy` takes no body (the projection sends `replace`, an unknown member under D-092), there is no `replaces_id` or `replaced_by_id`, and a `failed` post cannot be deleted (D-094).

**Done when:** the app's spec is rebuilt from this repository's `api/openapi.yaml`, with only push still merged in. The work is the app repository's; the engine's part is to tell it.

**Touches:** D-092, D-094, D-095.

## Next: reports

### Reports: one page, repeated sends, discard

**What and why.** D-104 replaces the draft, send-once and reject workflow and the share links: a report is one page in the public bucket with one link, sent as often as a person likes, and withdrawn by a discard that moves the page to an archive. Neither the dashboard nor the phone app has shipped, so the contract changes in place, without a compatibility path.

**What to build** (D-104):

- **Spec first** (`api/openapi.yaml`). `ReportStatus`: `building`, `failed`, `ready`, `discarding`, `discarded`, `expired`. `Report` loses `sent_at`, `sent_by`, `hold_reason` and `share_links`, and gains `url` (while `ready`), `archive_url` (once discarded or expired), `auto_send_status` (`off`, `pending`, `done`), `recipients` (the addresses of the location's enabled `location_report` rules) and `deliveries` (its rows of the delivery log, newest first). `POST /api/reports/{id}/send` takes an optional body `{"recipients": [...]}`, a subset of `recipients` (any other address is a 400), blocks, and answers 200 with the report, this send's rows included, whatever each address's outcome; its 409s are the report not `ready`, the location switched off, sending off or no sender, and no recipient. `POST /api/reports/{id}/discard` answers 200 with the report, or 409 while it is `building` or once it is `discarded` or `expired`. Removed: `POST /api/reports/{id}/reject`, `GET /api/reports/{id}/html`, `POST /api/reports/{id}/share-links`, `POST /api/report-share-links/{id}/revoke` and `ReportShareLink`. `NotificationDelivery` gains `report_id` and `requested_by` (the person's email, null for a schedule), and the delivery list a `report_id` filter.
- **Migration 0010.** `reports`: the new status check; `page_object` and `archive_object`; `auto_send_status` in place of `auto_send`; `html`, `hold_reason`, `sent_at` and `sent_by` dropped, and `reports_built_check` rewritten (`ready` needs `data` and `page_object`). `report_share_links` dropped; the audit entries that name a `report_share_link` stay, so the object types of `0009_audit_location.sql` keep it. `notification_deliveries`: `report_id` referencing `reports` and `requested_by` referencing `users`. Nothing is deployed: a development report without an object becomes `failed`, saying it was built before D-104.
- **Storage.** `storage::Store` gains the copy of an `r/` object to an `a/` object and the delete of an `r/` object, in the bucket and in the local folder. `http::why_refused` takes the two requests on the configured public bucket only (the object name's `/` is the one `%2F` the address may hold), with lines in `tests/unit/write_path_test.cpp` for every refused neighbour: another bucket, another prefix, a DELETE of an `a/` or a `p/` object, a copy the other way.
- **Build.** The object name on the row first, then the upload, then `ready`; in the transaction that marks it `ready`, the send's checks decide `auto_send_status` (`done`, then the send; or `pending`). `fail_stalled_reports` stays.
- **Send.** The checks with the row locked; one Gmail message per address, one after another, through the Gmail channel; afterwards the rows, `sent` or `failed` with Gmail's message, under the key `report:{id}:send:<one per send>`, and a `report.send` audit entry naming how many addresses. Not through `repo::emit` or the delivery job. The Gmail limit in `main.cpp` becomes 120 a minute with a burst of 5.
- **Discard.** `discarding` with the row locked, the copy, the delete, then `discarded` with the archive's name and a `report.discard` audit entry; a call on a `discarding` report finishes it.
- **Tests.** `integration/reports_test.cpp` for the build's order, every refusal of a send and of a discard, a send repeated to the same address, a send with some addresses failing, auto-send at most once (a build run twice), a discard finished by a second call; the API tests for the new routes; `write_path_test.cpp`.
- **Tell the clients.** Once merged, the web app's builder and the phone app get the list above in a message they can act on: the members and routes removed and added, the statuses, that the dashboard frames `url` or `archive_url` directly, and that the client asks the person before sending to an address with a `sent` row.
- **The notes in `CLAUDE.md` and `architecture.md`** that say D-104 is not built yet go.

**Done when:** a report is uploaded when built, sent twice to the same address with two rows in its log, auto-sent once by its schedule, and discarded, its old link answering Cloud Storage's error page and its archive loading in the dashboard; tests cover all of it; the contract check passes.

**Touches:** D-066, D-085, D-102 point 6, D-104; the rules on reports, on the public bucket and on what the outbound client may write in `CLAUDE.md`; spec-first API changes.

## First deployment

The system runs locally only. The owner postponed the first deployment (D-052); the VM type (`e2-medium`) and the address (`gbp.xvisionmediaus.com`) are decided.

### The owner's decisions for the deployment

**What.** The open list in `deploy/README.md`, "Decisions the owner still has to make": when to deploy and who creates the Google Cloud resources; where images are stored (suggested: Artifact Registry in `xvisionmedia-gbp`); the backup bucket and its retention; whether the dashboard sits behind an extra layer such as Cloudflare Access or Tailscale; the two Google OAuth web clients (staff sign-in, and connecting the GBP account and the mailbox) with the production redirect addresses; the knowledge bucket (exists: `xvisionmedia-gbp-knowledge`) and the public bucket for shared reports and post media. The organization admin also still has to restore the domain-sharing policy on the Google Cloud project, with the exception the public bucket needs.

**Done when:** each point has an answer in `deploy/README.md` and the resources exist.

**Touches:** D-008, D-052, D-085, D-091 point 2.

### Bring up production

**What.** On the VM: the Compose stack of `deploy/docker-compose.prod.yml` with images built for x86_64 (`build-images.sh --push`, never tried), `migrate` and then `seed` from the knowledge bucket (the first real `publish` and the first `seed` from a bucket were tested only against a stand-in), read access to the knowledge bucket for the VM's service account, the report buckets and the public bucket without a lifecycle rule (D-104 point 8), nightly backups, and the DNS record at Wix. Then, on each real database once it is migrated: connect the GBP managing account and the sending mailbox in the dashboard.

**Done when:** the dashboard is served at its address with a real certificate, the System status page is green (knowledge base, GBP token, discovery, Pub/Sub), a backup is written and restored once, and staff sign in from a phone.

**Touches:** D-078 (seed only), D-079 (connect in the dashboard), D-085 (buckets), D-087 (no mock in any image or Compose file).

### What only a deployment can prove

**What.** Things tried only locally or against stand-ins: a real Let's Encrypt certificate; the dashboard under the Content-Security-Policy in a browser, where it frames a report's page from the public bucket and another bucket's object is blocked; Pub/Sub and the metadata server from a container on the VM; the Cloud Storage upload, a public read without listing, the move of a report's page to `a/` (D-104) and the service account's role; a post with a video sent by the engine itself (needs the public bucket); sign-in from a phone with a real account.

**Done when:** each is seen working once and `deploy/README.md` says so.

### Watching the running system

**What.** From `deploy/README.md`, "Not done yet": nothing watches the VM from outside (`GET /api/health` is there for an uptime check), logs stay in Docker's local files (the Ops Agent can forward them), and a failed backup is only logged, although the notification module now exists to raise it as a system problem.

**Done when:** an outage, a failed backup and the engine's logs reach someone without logging in to the VM.

## Going live with the MVP clients

The owner's steps, in this order, once the system is deployed. The engine switches nothing on by itself: a location (`enabled`, D-102), the posting switches, email sending, auto-posting and "send automatically" are a person's (`CLAUDE.md`, rules that protect clients).

### The first real reply, and a week of replies through the system

**What.** The owner posts the first real reply through the dashboard on a review they choose and sees it confirmed as published. Then staff answer every new review of the three MVP locations through the dashboard for a week. Regenerating with instructions has never been tried against the real model.

**Done when:** a week of replies went out through the dashboard and none was posted twice.

**Touches:** D-062, D-063.

### Email notifications on

**What.** Connect `host@xvisionmediaus.com` on the production database, add rules, switch on "Send notification emails" and send the test message. Then measure the time from a review's sync to the escalation email in the inbox (target: 5 minutes; the alert is written in the same run as the analysis).

**Done when:** a real escalation email arrived and its delay is known.

**Touches:** D-066; no real email from tests or scripts.

### The live adversarial suite on the current prompts

**What.** The analyzer's and the writer's prompts changed since the last live run, which changes every location's fingerprint. Run `engine/scripts/linux.sh ai-live` (or the AI live workflow) once with the seeded configuration.

**Done when:** a live run passes in which no case reaches auto-post, before the first go-live test is started.

**Touches:** architecture section 10; the rule on prompt changes in `CLAUDE.md`.

### Go-live tests and the auto-posting rollout

**What.** Before anything auto-posts: the judge's provider in the engine's environment (`PONI_LLM_ANTHROPIC_BASE_URL`, `_API_KEY`, `_PROTOCOL=anthropic`; the judge has never been called from this code); the owner's rating of the 58 replies of the spike, compared with the judge's; a first real go-live test of each MVP location, started by the owner, which also shows what a test costs and how long it takes. Then a go-ahead per location: Gangnam AYCE Buffet 5★ first, then all three locations, then 4★, and two weeks of watching. Two known costs are accepted until then and stay open: nothing checks the Chinese script of a reply (D-056), and an offer or a promise in a reply is noticed only by the model (D-058).

**Done when:** two weeks of auto-posting with no incident, and the typical reply time for 4 and 5★ reviews is under an hour.

**Touches:** D-069 to D-077; a test is started only by a person; code never switches auto-posting on or lifts a pause.

### The first real reports

**What.** The owner approves the report layout (two PDFs of the local trial were handed over). The first report built with the models on shows what the summary says and costs (one call per report; it has never been asked for). The first reports are sent once sending is on and each location has recipients.

**Done when:** a monthly report of each MVP location was sent to its recipients.

**Touches:** D-085, D-086; a report is sent only by a person or a schedule a person set to send.

### The first real local posts

**What.** A post for each MVP location drafted, approved and published through the dashboard; a scheduled one removed again; ideas and texts from the model used on a real location. Posts were run once by hand on X Vision Media's own profile only.

The first edits of a post at Google (D-099) are made on X Vision Media's own profile and watched, one of a live post and one of a scheduled post that is not published yet, because the edit was built without a trial. Three things nobody knows until then: which of the six fields Google takes in `updateMask` (try each: the text, the button, an event's title and dates, an offer's parts, another photo, the language); whether an edit sends a live post back to `PROCESSING` or can get it `REJECTED`; and whether a scheduled post can be edited at all. A field Google refuses comes back as a draft edit with Google's message and leaves the post as it was. What is found is written into D-099, and a field Google never takes is taken out of the mask.

Several photos and videos on one post (D-101) were built from what Google's own interface takes, without a trial through the API. Also on X Vision Media's own profile: a post with two photos, one with a photo and a video, one with ten files, and an edit that puts a post's files in another order; see what Google accepts, whether it keeps the order, and what the post shows. A number or mix Google refuses comes back as a draft with Google's message. What is found is written into D-101, and `domain::kLocalPostMediaMax` follows it.

**Done when:** all three are done on the production system, the two edits were made, the posts with several files were sent, and what they showed is written down.

**Touches:** D-088, D-090, D-094, D-099, D-101.

### Questions for the owner

- **A US-region DashScope key.** The local key is a cn-beijing workspace; D-010 makes the US region the primary.
- **The 5-minute target for new reviews.** Google's API returns a new review about 5 minutes after it is written (once after 95 minutes) and notifies after 10 to 16 minutes. Suggested: count the target from the moment Google's API returns the review, which the 2-minute poll meets.
- **Outbound POSTs to Google's other hosts.** Raised by the reviews of local posts: whether the outbound client should limit POSTs to the engine's known addresses on every Google host (OAuth token, Pub/Sub, Gmail, Cloud Storage), not only on `mybusiness.googleapis.com`. Today that is a rule for the calling code (`CLAUDE.md`, the four changes to a profile).

## After the MVP

In the suggested order.

### Move to a PONI organization account for GBP access

**What and why.** The personal Gmail is the GBP managing account (D-009), and one account holds all client access. Every client has to re-invite the new account, so it is done once, before more clients are onboarded.

**Done when:** every MVP location is managed through the organization account and the engine is connected to it.

**Touches:** D-009, D-043, D-079.

### Onboard the other clients

**What.** The other 50 to 100 locations, in batches. A client of a new business type needs a complete industry pack first: profile, test corpus with the cases the complete-pack rule asks for, a made-up business, and a live run of the suite (FR-13). Each location is then switched on, given its industry, and needs its reply settings reviewed and a go-live test before it auto-posts.

**Done when:** each batch is switched on, has settings, and runs through the approval queue.

**Touches:** D-078, D-082, D-093; `knowledge/README.md`; an industry is supported only with a complete pack.

### Profile accuracy (FR-9)

**What.** Reference records per location, alerts when Google changes a profile, a daily comparison, website scans and holiday reminders. Alert only: the engine never edits a profile (D-017). Nothing of it is built.

**Touches:** D-017; the three changes to a profile in `CLAUDE.md` stay three.

### Cases (FR-12)

**What.** Tracked follow-up on negative reviews (statuses Open, Investigating, Responded, Closed, the "communicating with customer" flag, rules over the analyzer's output), built on the escalations, which are already the simplest form of a case.

**Touches:** D-054, D-065.

### More notification channels and roles

**What.** As the need appears: channels beyond email and push (D-097), and roles and permissions beyond the single admin role (D-007). Every endpoint already has the authorization hook roles would use.

### Clean-up of old reports, posts and uploads

**What.** A script outside the engine, run by hand or by the VM's cron once in a while, that replaces Cloud Storage's lifecycle rule (D-104 point 8). It works from the database's records, not from the bucket: it expires every report built more than the retention period ago (by `reports.built_at`, a period to be chosen) by discard's steps (`discarding`, the copy to `a/`, the delete of the original) with `expired` at the end; it marks old `draft` and `rejected` local posts `expired` (a new status of a post; nothing that was ever sent is touched, D-094 point 2) and deletes their uploads, but only an upload that no open post or edit names in its `content`, `approved_content` or `base_content`, because copies and edits share uploads (D-101 point 3); and it lists the bucket with credentials of its own and deletes an object no row names once it is a day old (an upload is written before its row). It takes the same row locks as the engine, never touches a report that is `building` or `discarding`, and writes audit entries as `system:cleanup`. The archive (`a/`) is kept for good. `kMediaUsableFor` in `db/repo_local_posts.cpp` (360 days, and its message that the file "may be gone") follows the period the script deletes uploads after.

**Touches:** D-085, D-094, D-101, D-104; `deploy/README.md` (how and when it runs, and the role that may list and delete).

### Report pages rendered outside the engine

**What.** The owner's follow-up to D-085: render the report page from the stored JSON outside the C++ engine.

**Touches:** D-085; the report page's rules (`html_escape`, no script) in `CLAUDE.md`.

## Finishing the repository split

The engine has a repository of its own and the dashboard another (D-096). These items finish the split.

### The dashboard takes the contract from this repository

**What.** `api/openapi.yaml` here is the source of truth for the API. The dashboard repository has its own copy, from which it generates `src/api/schema.d.ts` (`npm run generate:api`, never edited by hand). It must take the file from this repository instead, at a known version, so that a change here reaches the dashboard and the dashboard's `check:api` checks against the contract the engine is built to. The contract check (`api/`, `npm run check:contract`) stays here, against a running engine.

**Done when:** the dashboard repository has no contract of its own and its generated client comes from this repository's file.

### The web image and the Caddyfile

**What.** The production web image (Caddy with the built dashboard) is built in the dashboard repository (`deploy/web.Dockerfile`); this repository's `deploy/Caddyfile` is mounted over the image's by `docker-compose.prod.yml`, and the image name is `${PONI_IMAGE_PREFIX}-web` with `PONI_WEB_IMAGE_TAG` when its tag differs. The dashboard's build must produce an image under the new prefix, and the two repositories must agree on where the Caddyfile lives and who changes it (its Content-Security-Policy follows what the dashboard loads).

**Done when:** a web image from the dashboard repository runs with this repository's Compose file and Caddyfile in the local trial (`deploy/README.md`, "Trying the stack locally").

### Remove the engine from the dashboard's repository

**What.** Once this repository is the one in use, delete `engine/`, `tools/`, `knowledge/`, `api/` (after the item above), the engine's `deploy/` files and docs, and the engine's CI from the dashboard's repository, so that there is one engine and one contract.

**Done when:** the dashboard's repository holds the dashboard and what it alone needs, and its CI builds only that.

### The dashboard uses the product's name throughout

**What.** The dashboard's repository still has its own names for the product: its page title, package name and the variable its dev server reads for the engine's address. This repository's runbook already starts it with `PONI_ENGINE_URL`, the name the dashboard's `vite.config.ts` is to read.

**Done when:** the dashboard's names match this repository's (`PONI`), and the runbook's command works as written.

### CI in this repository

**What.** The workflows here read the DashScope secrets as `PONI_LLM_DASHSCOPE_BASE_URL` and `PONI_LLM_DASHSCOPE_API_KEY` (`ai-live.yml`); they must be set on this repository under those names.

**Done when:** CI is green here and the manually started AI live suite runs.

### Remove the code for databases from before the baseline

**What.** `poni-tools migrate` still marks a database that has exactly the 26 earlier migrations as having the baseline (`BEFORE_BASELINE` in `migrate.py`). It goes once no such database is left.

**Done when:** every development database records `0001_baseline` and the code and its tests are removed.

**Touches:** D-091 point 10.
