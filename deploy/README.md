# Deployment

One Linux VM runs everything with Docker Compose (architecture section 7):

| Service | What it does |
| ------- | ------------ |
| `caddy` | HTTPS with automatic certificates, the dashboard's files, `/api` forwarded to the engine. The only service reachable from outside |
| `engine` | The API, the scheduler and the job workers: a daemon that takes no command (D-079) |
| `migrate` | `poni-tools migrate`: applies database migrations, then exits |
| `seed` | `poni-tools seed`: downloads the knowledge archive that `poni.env` names, checks it and seeds it, then exits (FR-13). The engine waits for both |
| `db` | PostgreSQL 17, data in a Docker volume |
| `backup` | `pg_dump` every night, kept on the server for 14 days and copied to a Cloud Storage bucket |

Files: [`docker-compose.prod.yml`](docker-compose.prod.yml), [`Caddyfile`](Caddyfile) (mounted into the web image), [`backup/`](backup/), [`poni.env.example`](poni.env.example), [`build-images.sh`](build-images.sh). The `migrate` and `seed` services run the tools image ([`tools/Dockerfile`](../tools/Dockerfile)). `docker-compose.dev.yml` is for local development only, and `docker-compose.trial.yml` for the local trial below.

Status: ready to deploy, not deployed. Nothing has been created in Google Cloud. The owner postponed the first deployment on 2026-11-11 (D-052); the VM type and the web address are decided.

- Tried on a development Mac (arm64) with `PONI_SITE_ADDRESS=localhost` on 2026-11-11: the images build, migrations run, the engine becomes healthy, Caddy serves the dashboard and the API over HTTPS with the headers above, and a backup is written and checked.
- Not tried yet: the multi-architecture build and push (`build-images.sh --push`; CI already builds and tests the engine on x86_64 with the same Dockerfile), a real certificate from Let's Encrypt, the upload to Cloud Storage, Pub/Sub and the metadata server from inside a container on a VM, and the dashboard in a browser under the Content-Security-Policy (the built page has no inline script, so it should pass).

## Decided (D-052)

- **The VM**: `e2-medium` (x86_64, 2 shared vCPU, 4 GB of memory). Region: close to the clients, for example `us-east4`.
- **The dashboard's web address**: `gbp.xvisionmediaus.com`. The domain's DNS is hosted at Wix; the owner adds an A record there that points the name at the VM's static IP address.

## Decisions the owner still has to make

1. **When to deploy**, and who creates the Google Cloud resources (the owner in the console, or an AI session with `gcloud` after the owner has signed in).
2. **Where images are stored.** Suggested: an Artifact Registry Docker repository in the `xvisionmedia-gbp` project (`us-docker.pkg.dev/xvisionmedia-gbp/poni`).
3. **The backup bucket**: a name (suggested: `xvisionmedia-gbp-backups`), and how long backups are kept (suggested: a lifecycle rule that deletes objects after 90 days).
4. **Who can reach the dashboard.** It is built to face the internet (Google sign-in limited to `xvisionmediaus.com` and `xvisionus.com`, sessions, attempt limits). Putting it behind Cloudflare Access or Tailscale as well is an optional extra layer (architecture section 8).
5. **The two Google OAuth clients** below have to be created by someone who can open the Google Cloud console of the project.
6. **The knowledge bucket**: a name (suggested: `xvisionmedia-gbp-knowledge`). Archives are a few kilobytes each and are never replaced, so no lifecycle rule is needed.
7. **The public bucket** (D-085, D-091 point 2, D-104): for report pages and the photos and videos of local posts (suggested: `xvisionmedia-gbp-public`; the owner chose a `-public` name because other public artifacts may follow). A report's numbers are kept in the database, not in a bucket. The bucket needs an exception to the project's domain-restricted sharing, which the owner accepted.

## One-time setup

### Google sign-in for staff (dashboard)

In the Google Cloud console, project `xvisionmedia-gbp`: **Google Auth Platform → Clients → Create client**.

- Application type: **Web application**. Name: `PONI GBP Manager dashboard sign-in`.
- Authorized redirect URIs:
  - `https://<the dashboard address>/api/auth/google/callback` (production)
  - `http://localhost:5173/api/auth/google/callback` (local development)
- Authorized JavaScript origins: none needed.

The client ID and the client secret go into `PONI_GOOGLE_SIGNIN_CLIENT_ID` and `PONI_GOOGLE_SIGNIN_CLIENT_SECRET`: on the server in `/etc/poni-gbp/poni.env`, locally in `~/.config/poni-gbp/engine.env`. Never into the repository.

The existing app is "External" and "In production" with only the `business.manage` scope. Sign-in adds the scopes `openid`, `email` and `profile`, which need no Google review. The engine itself refuses every account that isn't a verified Google Workspace account of `xvisionmediaus.com` or `xvisionus.com` (`PONI_SIGNIN_DOMAINS`, D-053), so the app's audience setting doesn't have to change, and a second domain needs nothing new in the Google Cloud console.

### Connecting the GBP managing account and the sending mailbox (dashboard)

Both accounts are connected in the dashboard, on **System status**, by a signed-in person (D-079). **Connect** sends the browser to Google; the engine receives Google's redirect, exchanges the code, checks the account, encrypts the refresh token and stores it, with an audit entry in the name of the person who clicked. The client secret, the token and the encryption key never reach the browser. Nothing is stored unless every check passed: Google must name the account's verified address; the GBP account must be able to list its locations; the mailbox must have granted the permission to send. Each connection is the account the person picks in Google's account chooser, and no setting names it (D-100): the status page shows the address of each, the GBP one being the address clients invite to manage their profiles. To move to another account, connect that one: there is one connection of each kind, and the new one replaces the old token. Locations the new GBP account does not manage show as lost after two discoveries.

This needs one OAuth client of type **Web application** in the same Google Auth Platform (**Clients → Create client**), with these **Authorized redirect URIs**:

- `https://<the dashboard address>/api/connections/gbp/callback`
- `https://<the dashboard address>/api/connections/gmail/callback`
- `http://localhost:5173/api/connections/gbp/callback` and `http://localhost:5173/api/connections/gmail/callback` (local development)

Its ID and secret go into `PONI_GBP_OAUTH_CLIENT_ID` and `PONI_GBP_OAUTH_CLIENT_SECRET`. The dashboard's sign-in client above could carry these addresses too, but a client of its own keeps the two purposes apart: a refresh token belongs to the client that issued it, so replacing this client means connecting both accounts again, and nothing else. A "Desktop app" client, as the removed `auth` command used, does not work here. A token that is already stored in a database stays valid as long as its client exists; the token the spike obtained through the OAuth Playground can no longer be imported (`auth --import-env` is gone): connect the account in the dashboard instead.

Until the status page's **Connect** was used, the engine syncs nothing and the status page says so. Any signed-in staff member can connect or replace an account; each time is in the audit log (`credential.store`).

#### The mailbox notifications are sent from

Notifications (escalations, approvals, system problems) are sent as email through the Gmail API from the mailbox connected on the status page, at the address its sign-in named (D-066, D-100); today that is `host@xvisionmediaus.com`. A mailbox connected before the engine kept the address sends nothing until it is connected again. The engine needs a token of that mailbox with one permission, sending mail. Until the steps below are done, and after them until sending is switched on in the dashboard, every notification is recorded in the dashboard's **Delivery log** as not sent and no email leaves.

In the Google Cloud console of the project (`xvisionmedia-gbp`), signed in as someone who can edit it:

1. **APIs & Services → Library**: find **Gmail API** and enable it.
2. **Google Auth Platform → Data access → Add or remove scopes**: add `https://www.googleapis.com/auth/gmail.send` and save. Google lists it as a sensitive scope. The app is "External" and "In production", so until Google has verified the app for this scope, the consent screen shows "Google hasn't verified this app" and the sign-in goes on through **Advanced → Go to … (unsafe)**. That is acceptable for one mailbox of our own; verification is only needed to remove the warning. If the Workspace admin has restricted third-party apps, the app's client ID has to be marked as trusted in the Admin console (**Security → API controls → App access control**).

Then, in the dashboard: **System status → Notification email → Connect**. Pick `host@xvisionmediaus.com` (or whichever mailbox should send) in Google's account chooser and allow "Send email on your behalf". The engine takes the account that signed in, checks that the permission was granted, and stores the token encrypted under its own name (the GBP token is not touched). Connecting switches no sending on.

Last, in the dashboard: add at least one rule under **Notification rules** (for example event "System problems", all clients, your own address), switch on **Send notification emails** under **Posting and sending**, and press **Send test email** on the **Delivery log** page. The test message should arrive within a few seconds and show as sent in the log.

The `reports@` send-as alias (FR-7) is not set up: mail comes from `host@` itself. Mail is sent from the connected mailbox's own address; an alias would need a way to name it, which is not built.

### The VM

1. Create the VM with a static external IP address, a 20 GB disk or more, and firewall rules for TCP 80 and 443 (and UDP 443 for HTTP/3). Give it its own service account.
2. Grant that service account:
   - `roles/pubsub.subscriber` on the subscription `gbp-notifications-engine`
   - `roles/storage.objectCreator` on the backup bucket
   - `roles/storage.objectViewer` on the knowledge bucket (the `seed` service downloads the archive `poni.env` names)
   - `roles/storage.objectUser` on the public bucket ("The public bucket" below)
   - `roles/artifactregistry.reader` on the image repository

   The project has domain-restricted sharing; a service account of the same project is inside the organization, so these grants should not need a policy exception. To confirm when it is done.
3. Give the VM the access scope `cloud-platform` (or the Pub/Sub and Storage scopes), so the service account's roles apply.
4. Install Docker Engine with the Compose plugin.
5. Point the DNS record at the static IP address.
6. Create `/etc/poni-gbp/poni.env` from [`poni.env.example`](poni.env.example), owned by root with mode 600.

### The LLM (review analysis and drafts)

Set a model provider's three lines in `poni.env`, for example `PONI_LLM_DASHSCOPE_BASE_URL`, `PONI_LLM_DASHSCOPE_API_KEY` and `PONI_LLM_DASHSCOPE_PROTOCOL=openai`: the address up to the version segment, its key, and which API it speaks (`openai` for any OpenAI-compatible provider, `anthropic` for Anthropic's own). The protocol is required. Several providers can be set at once; staff choose the one each task uses in the dashboard under Analysis settings (D-059). A provider other than the two listed in `poni.env.example` also needs its three lines in `docker-compose.prod.yml`. A DashScope key only works with the address of its own region (D-010). Without them the engine syncs reviews and analyzes nothing; the System status page says so.

The go-live test (D-074), which a client's configuration must pass before anything is auto-posted with it, also needs the judge's provider: `PONI_LLM_ANTHROPIC_BASE_URL=https://api.anthropic.com/v1`, `PONI_LLM_ANTHROPIC_API_KEY` and `PONI_LLM_ANTHROPIC_PROTOCOL=anthropic`. During a test the client's recent reviews (the star rating and the text, never the reviewer's name) are sent to that provider with the replies it rates (D-075). A test is started by a person on the dashboard's Go-live test page and costs model calls; the page shows an estimate first.

The help with local posts (D-088) is a task of its own, `posts`, with its provider and model on the same page. Only when a person presses "Suggest ideas", "Write a text" or "Improve the text" on a post's page, that provider is sent the location's reviews of the last 90 days (stars, day, language and text; never the reviewer's name or photo, and no review flagged for a serious issue or suspected manipulation), Google's figures and search keywords for the profile, and the profile's recent posts. No job does this, and nothing the model writes is stored or published by itself.

A client has no voice of its own: each of its locations has complete reply settings (D-093). Give the client its industry on its page (one of the supported industries) and link its first location, which starts with a copy of the knowledge base's default voice, localization and policy. On that location's page open **Reply settings**, take a voice template, fill in what the business says about itself and save; a location linked afterwards starts with a copy of these, and **Copy to other locations** brings locations that are already linked in line. The voice drafts of the two MVP clients are kept as a reference in the history of the old migrations (`git log --all -p -- engine/migrations/0008_seed_mvp_client_voices.sql`); that file cannot be run (it names a voice template, which a voice no longer does), so on a new database they are entered in the dashboard. A database that had them before the knowledge base (D-078) keeps them.

### The knowledge base

Industry packs, voice templates, defaults and model prices are files in `knowledge/`, published as an archive to a Cloud Storage bucket and seeded from there (FR-13, D-078). Production does not need the code repository for an update, and no image is rebuilt.

One time: create the bucket (`gcloud storage buckets create gs://<bucket> --location=us --uniform-bucket-level-access`), grant the VM's service account `roles/storage.objectViewer` on it, and put its name into `poni.env` as `PONI_KNOWLEDGE_BUCKET`.

To publish, on your own machine, from a clean checkout of `main`, signed in to Google (`gcloud auth application-default login`) as someone who may write to the bucket:

```bash
cd tools && uv run poni-tools publish --bucket <bucket>
```

It checks the knowledge base, packs it as `knowledge-<version>.tar.gz` (the version is the one in `knowledge/manifest.json`, for example `knowledge-20261115.tar.gz`), uploads it and prints the line for `poni.env`. It refuses a checkout that is not a clean `main`, a knowledge base with a problem (an incomplete pack, for example), and a name that is already in the bucket.

To run production on a published archive, set `PONI_KNOWLEDGE_ARCHIVE=<that name>` in `/etc/poni-gbp/poni.env` on the VM and run the seed service:

```bash
docker compose -f deploy/docker-compose.prod.yml --env-file /etc/poni-gbp/poni.env run --rm seed
```

It downloads the archive as the VM's service account, compares the version inside with its name, checks it again and replaces the knowledge tables in one transaction. The engine keeps nothing of the knowledge base in memory, so it can keep running and reads the new version on its next use. **Read what the seed prints**: when the update changed what the analyzer is told about an industry, it lists the clients and locations that no longer auto-post until a new go-live test has passed, and the clients whose industry is gone. Nothing else reports this (D-084); the Go-live test page shows the same state. Going back is naming the older archive and running `seed` again.

### The public bucket

A report is shared as a page in a public bucket (D-085); its numbers are kept in the database (D-091 point 2). The bucket also holds the photos and videos of local posts (D-088, D-090), under `p/`: Google takes a post's photo or video only from a public address. The engine writes objects, and under D-104 moves a discarded report's page from `r/` to `a/` (a copy, then the delete of the original); it reads and lists nothing, and deletes nothing else. Not tried against real Cloud Storage yet: the first deployment is the test.

The public bucket (`PONI_PUBLIC_BUCKET`):

```bash
gcloud storage buckets create gs://<public-bucket> --location=us --uniform-bucket-level-access
# Anyone may read an object whose name they know; nobody may list the bucket.
gcloud storage buckets add-iam-policy-binding gs://<public-bucket> --member=allUsers --role=roles/storage.legacyObjectReader
```

The bucket has no lifecycle rule (D-104 point 8): the retention is our own. Don't add one, and remove one that an older setup added (`gcloud storage buckets update gs://<public-bucket> --clear-lifecycle`).

- `allUsers` is outside the organization, so the binding needs an exception to domain-restricted sharing for this project (the organization admin), and public access prevention must not be enforced on the bucket.
- Do not grant `roles/storage.objectViewer` to `allUsers`: it includes listing, and a listing would give every report's address away. The names are the secret.
- The retention is the clean-up script's (work-items, "Clean-up of old reports, posts and uploads"): it expires old reports to the archive, deletes the uploads no open post or edit names, and deletes objects no row names. It is not built yet; until it is, nothing leaves the bucket. The archive (`a/`) is kept for good.
- A post's photo or video (`p/<32 random bytes>.jpg`, `.png`, `.mp4` or `.mov`) is stored exactly as it was uploaded: the engine does not resize it, convert it or strip what a phone wrote into it (place and time among it), and it is public to anyone who has its address. Google copies the file when the post is created, so it is only needed until then. The engine refuses an upload older than 360 days for a new post (`kMediaUsableFor`), which the clean-up script's period is to follow.
- With a folder in place of the bucket (development) a photo or video can be uploaded but no post with one can be approved: Google cannot fetch a file from a folder.
- A video may be 75 MB. The engine holds an upload in memory while it checks it and sends it on, about three times the file's size while it goes to the bucket (some 225 MB for the largest video; two people uploading at once need twice that), and only for a signed-in person: who is calling is asked before the body is read. Size the VM's memory with that in mind. An upload that is still coming in when the engine stops is given up, and the person uploads again. Caddy sets no limit on a request's size, and none is needed; a proxy put in front that has one must allow 75 MB on `/api/local-post-media`. A video's upload to the bucket is given a minute and four more seconds for every megabyte.
- The VM's service account needs `roles/storage.objectUser` on the bucket: discarding a report deletes the original of its page after the copy (D-104), and a retried build overwrites its page, for which Cloud Storage asks for the delete permission too.

Without `PONI_PUBLIC_BUCKET` the engine builds no report (a person's "generate" is refused, and schedules start none) and takes no photo or video for a post.

## Building images

From a clean checkout of the commit to deploy, on a machine with Docker:

```bash
deploy/build-images.sh --push us-docker.pkg.dev/xvisionmedia-gbp/poni/poni-gbp
```

This builds `…-engine`, `…-tools` and `…-backup` for both architectures and tags them with the commit. The web image (`…-web`: Caddy with the built dashboard) is built in the dashboard's repository with the same prefix; when its tag differs from the engine's, set `PONI_WEB_IMAGE_TAG` as well. It prints the two lines to put into `poni.env`. Without arguments it builds for the local machine only, which is what the trial below uses.

## Deploying

On the VM, with the repository's `deploy/` folder copied or checked out:

```bash
docker compose -f deploy/docker-compose.prod.yml --env-file /etc/poni-gbp/poni.env pull
```

```bash
docker compose -f deploy/docker-compose.prod.yml --env-file /etc/poni-gbp/poni.env up -d
```

`migrate` runs first, then `seed`; the engine starts when both have finished. Then, once, in the dashboard: sign in, open **System status** and connect the GBP managing account (**Connect** on the GBP token card; see above). The engine then discovers the locations within 15 minutes, or at once with **Look for locations now**.

Check:

- `https://<address>/api/health` answers `{"status":"ok",…}`.
- Sign in to the dashboard, open **System status**: the knowledge base's version, the GBP token, location discovery and Pub/Sub are green.
- Link the locations to their clients; their reviews arrive within a minute or two.

An update of the software is the same `pull` and `up -d` with a new `PONI_IMAGE_TAG`. An update of the knowledge base alone is a new `PONI_KNOWLEDGE_ARCHIVE` and `run --rm seed` (above).

## Backups and restore

The `backup` service writes `/backups/poni-<UTC time>.dump` at `PONI_BACKUP_TIME_UTC` (default 08:30 UTC, about 3:30 at night in New York), checks that the dump is readable, uploads it to `gs://$PONI_BACKUP_BUCKET/` as the VM's service account, and removes local dumps older than `PONI_BACKUP_KEEP_DAYS`. A failed night is logged as `BACKUP FAILED` and the schedule continues; `/backups/last-success` holds the time of the last good run.

Run one now:

```bash
docker compose -f deploy/docker-compose.prod.yml --env-file /etc/poni-gbp/poni.env run --rm backup once
```

Restore into the running database (this replaces what is there; stop the engine first):

```bash
docker compose -f deploy/docker-compose.prod.yml --env-file /etc/poni-gbp/poni.env stop engine
```

```bash
docker compose -f deploy/docker-compose.prod.yml --env-file /etc/poni-gbp/poni.env exec -T backup pg_restore --clean --if-exists --no-owner -d poni /backups/poni-<time>.dump
```

```bash
docker compose -f deploy/docker-compose.prod.yml --env-file /etc/poni-gbp/poni.env start engine
```

The stored GBP token is encrypted with `PONI_SECRET_KEY`. A backup is only fully usable together with that key, so keep a copy of `poni.env` somewhere safe that is not the VM and not the bucket.

## Trying the stack locally

With Docker Desktop, on any machine:

```bash
deploy/build-images.sh
```

Pack the knowledge base into a folder that stands in for the bucket:

```bash
(cd tools && uv run poni-tools pack --out ../deploy/.trial-knowledge)
```

```bash
PONI_SITE_ADDRESS=localhost PONI_IMAGE_TAG=$(git rev-parse --short=12 HEAD) PONI_DB_PASSWORD=localtrial PONI_SECRET_KEY=$(openssl rand -base64 32) PONI_HTTP_PORT=8088 PONI_HTTPS_PORT=8443 PONI_GOOGLE_CREDENTIALS=adc PONI_TRIAL_KNOWLEDGE=$PWD/deploy/.trial-knowledge PONI_KNOWLEDGE_BUCKET=/published PONI_KNOWLEDGE_ARCHIVE=$(ls deploy/.trial-knowledge) docker compose -p poni-gbp-trial -f deploy/docker-compose.prod.yml -f deploy/docker-compose.trial.yml up -d
```

Caddy serves `https://localhost:8443` with its own certificate (the browser warns once). Signing in isn't part of the trial: it needs the Google OAuth client and the real address. The trial shows that the stack starts, migrates, seeds the knowledge base from an archive, serves the dashboard with its security headers and forwards the API (`curl -k https://localhost:8443/api/health`). Remove it with `docker compose -p poni-gbp-trial -f deploy/docker-compose.prod.yml -f deploy/docker-compose.trial.yml down -v`.

## Not done yet

- Nothing watches the VM from outside. `GET /api/health` is there for an uptime check (Google Cloud Monitoring has a free one).
- Logs stay in Docker's local log files. On Compute Engine the Ops Agent can forward them.
- Failed backups are only logged; alerting comes with the notification module (Phase 3).
