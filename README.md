# PONI GBP Manager

Automates Google Business Profile (GBP) work for PONI's clients: review replies, client reports and local posts.

- **Engine** (`engine/`): one C++23 binary, a daemon: HTTP API, scheduler and job workers; PostgreSQL is the system of record and the job queue.
- **Tools** (`tools/`): `poni-tools`, Python scripts for what is not the daemon's job: applying migrations, and checking, seeding and publishing the knowledge base.
- **Knowledge base** (`knowledge/`): what is the same for every client (industry packs with their test corpora, voice templates, defaults, model prices), as reviewed files that are seeded into tables nobody edits.
- **API contract** (`api/openapi.yaml`): the source of truth for the API.
- **Deployment** (`deploy/`): Docker Compose for development and production, the Caddyfile, backups.

Two clients use the API and live in repositories of their own: the dashboard (React-admin), whose client is generated from `api/openapi.yaml`, and the iOS app (`poni-gbp-manager-ios`).

Documents are in [`docs/`](docs/): [architecture](docs/architecture.md) (with the requirements), [decisions](docs/decisions.md) in force, [work items](docs/work-items.md) still to build, and the [Google mock runbook](docs/runbook-google-mock.md). Conventions and the rules that protect clients are in [`CLAUDE.md`](CLAUDE.md).

Status: built and tried locally: the review mirror, analysis and drafts, approved and automatic replies, the go-live test, reports and local posts. Nothing is deployed yet ([`deploy/`](deploy/README.md) has the files), and nothing auto-posts: it is off until a person switches it on for a location, and no configuration has a passing go-live test.

## Getting started (macOS, Apple silicon)

### 1. Tools

```bash
brew install cmake ninja llvm node uv
```

```bash
git clone https://github.com/microsoft/vcpkg ~/vcpkg && ~/vcpkg/bootstrap-vcpkg.sh -disableMetrics
```

You also need Docker Desktop (for PostgreSQL and the Linux build) and Python 3.10 or newer on `PATH` ahead of Apple's 3.9 (`brew install python`); vcpkg uses it to build libpq. `uv` runs `poni-tools` and fetches the Python it needs (3.12 or newer) by itself.

Every engine command below assumes:

```bash
export VCPKG_ROOT=~/vcpkg PATH="/opt/homebrew/bin:$PATH"
```

### 2. Database

```bash
docker compose -f deploy/docker-compose.dev.yml up -d --wait db
```

This starts PostgreSQL 17 on `127.0.0.1:55432` (database, user and password are all `poni`; local use only).

### 3. Engine

```bash
cd engine
cmake --preset macos-llvm
cmake --build --preset macos-llvm
ctest --preset macos-llvm
```

The first configure builds the dependencies (about 4 minutes); after that they come from vcpkg's binary cache. The integration tests need the database server from step 2. `ctest` first has `poni-tools` prepare one migrated and seeded database (`poni_test_macos_llvm`); each test then works in its own copy of it and leaves the development data alone.

The engine is a daemon and does not set up its database. `poni-tools` does, from `tools/`:

```bash
cd ../tools
uv run poni-tools migrate
uv run poni-tools seed
```

`migrate` applies the numbered files in `engine/migrations/`; `seed` checks the files in `knowledge/` and puts them into the `kb_*` tables (see [knowledge/README.md](knowledge/README.md)). Both read the same settings as the engine, so they work on the same database. Then start the engine, which takes no command:

```bash
cd ../engine
./build/macos-llvm/poni-gbp-engine
```

```bash
curl -s http://127.0.0.1:8080/api/health
```

The engine refuses to start while migrations are pending or no knowledge base is seeded, and says which. Within a second or two of starting it logs a `heartbeat` job: the scheduler enqueued it and a worker ran it through the queue.

### 4. Local secrets and the GBP account

Secrets live outside the repository, in `~/.config/poni-gbp/`. The engine reads `~/.config/poni-gbp/engine.env` by itself when no `--config` is given. Create it once (mode 600):

```bash
umask 077 && printf 'PONI_SECRET_KEY=%s\nPONI_DEV_LOGIN_EMAIL=dev@localhost\n' "$(openssl rand -base64 32)" > ~/.config/poni-gbp/engine.env
```

- `PONI_SECRET_KEY` encrypts the GBP token in the database.
- `PONI_DEV_LOGIN_EMAIL` switches on development sign-in, so the dashboard works locally without a Google OAuth client. The engine only accepts it while `PONI_PUBLIC_URL` is plain `http://localhost`.

The GBP managing account is connected in the dashboard: **System status**, **Connect** on the GBP token card. That needs `PONI_GBP_OAUTH_CLIENT_ID` and `PONI_GBP_OAUTH_CLIENT_SECRET` in `engine.env`: a Google OAuth client of type "Web application" whose redirect addresses include `http://localhost:5173/api/connections/gbp/callback`, `http://localhost:5173/api/connections/gmail/callback` and `http://localhost:5173/api/connections/workspace/callback` (creating it: [deploy/README.md](deploy/README.md)). The engine exchanges the code, checks that the account can list its locations, and stores the token encrypted; the status page shows whether Google still accepts it. A database that already holds a token keeps it.

For review analysis and drafts, add a model provider's address, key and protocol to `engine.env` as `PONI_LLM_DASHSCOPE_BASE_URL`, `PONI_LLM_DASHSCOPE_API_KEY` and `PONI_LLM_DASHSCOPE_PROTOCOL=openai`. Any number of providers can be listed this way (`PONI_LLM_OPENAI_…`, and so on); which one each task uses is chosen in the dashboard under **Analysis settings**. Without them the engine runs as before and analyzes nothing; the System status page says so.

Replies are posted only when a person approves a draft in the dashboard's **Approval queue**, and only while **Post replies to Google** is switched on under **Posting and sending** (it is off after installation; each location has its own switch too). Notification emails need the sending mailbox connected once (**Connect** on the Notification email card of **System status**) and **Send notification emails** switched on; until then every notification is in the **Delivery log** as not sent.

With the token stored, the engine discovers the account's locations within a minute. Reviews are synced for locations that are linked to a client, which is done in the dashboard.

Optional: `PONI_PUBSUB_SUBSCRIPTION=projects/xvisionmedia-gbp/subscriptions/gbp-notifications-engine` in `engine.env` adds review notifications. Locally they use gcloud's application-default credentials (`gcloud auth application-default login`), which Workspace expires about once a day; the System status page says so when that happens, and polling carries on regardless.

To sign in with a real Google account locally, add the dashboard sign-in client's `PONI_GOOGLE_SIGNIN_CLIENT_ID` and `PONI_GOOGLE_SIGNIN_CLIENT_SECRET` to `engine.env` (creating the client: [deploy/README.md](deploy/README.md)).

To try every workflow on made-up data instead of real clients' reviews, run the engine against the local mock of Google with a database of its own: [docs/runbook-google-mock.md](docs/runbook-google-mock.md).

### 5. Dashboard

The dashboard is in its own repository: run its dev server there (`npm ci && npm run dev`). Open <http://localhost:5173> (use `localhost`, not `127.0.0.1`: the engine checks the address). The dev server forwards `/api` to this engine on port 8080. Sign in with **Development sign-in**, link a location to a client under **Locations**, and its reviews appear in **Reviews** within a minute or two.

## Engine settings

Settings are environment variables. `--config FILE` (or `PONI_CONFIG`) names an optional `KEY=VALUE` file; the environment overrides the file. Secrets are passed the same way and never live in this repository.

| Variable | Default | Meaning |
| -------- | ------- | ------- |
| `PONI_DATABASE_URL` | `postgresql://poni:poni@127.0.0.1:55432/poni` | libpq connection URL |
| `PONI_DB_POOL_SIZE` | `8` | Database threads and connections |
| `PONI_HTTP_HOST` / `PONI_HTTP_PORT` | `127.0.0.1` / `8080` | Listen address |
| `PONI_LOG_LEVEL` | `info` | `debug`, `info`, `warn` or `error` |
| `PONI_LOG_FORMAT` | `json` | `json` (one object per line) or `text` |
| `PONI_JOB_WORKERS` | `8` | Job worker loops, shared by every kind of job; `0` runs none. A go-live test runs this many cases at a time |
| `PONI_JOB_POLL_INTERVAL_MS` | `1000` | How often an idle worker checks the queue |
| `PONI_JOB_LOCK_TIMEOUT_S` | `2400` | A running job older than this is handed to another worker; longer than the longest job (a go-live case) |
| `PONI_HEARTBEAT_INTERVAL_S` | `300` | How often the built-in heartbeat job runs |
| `PONI_JOB_RETENTION_DAYS` | `7` | Succeeded jobs are deleted after this |
| `PONI_MIGRATIONS_DIR` | `engine/migrations` of the build | Where the migration files are |
| `PONI_SECRET_KEY` | none | Base64 of 32 random bytes; encrypts the stored GBP token. Without it nothing syncs |
| `PONI_PUBLIC_URL` | `http://localhost:5173` | The dashboard's address as staff type it. Decides the cookie flags and the Google sign-in redirect address. Must be `https://…` except on localhost |
| `PONI_SIGNIN_DOMAINS` | `xvisionmediaus.com,xvisionus.com` | Only Google Workspace accounts of these domains may sign in (comma-separated, D-053) |
| `PONI_GOOGLE_SIGNIN_CLIENT_ID` / `_SECRET` | none | Google OAuth web client for staff sign-in. Without it Google sign-in is off |
| `PONI_DEV_LOGIN_EMAIL` | none | Local development only: sign in as this address without Google. Refused with an `https` public URL |
| `PONI_SESSION_TTL_HOURS` / `PONI_SESSION_IDLE_HOURS` | `168` / `24` | A dashboard session ends after this long, or after this long without use |
| `PONI_APP_SESSION_TTL_HOURS` / `PONI_APP_SESSION_IDLE_HOURS` | `2160` / `720` | The same for the phone app's session (D-095): 90 days, or 30 days without use |
| `PONI_TRUST_FORWARDED_FOR` | `false` | `true` behind a reverse proxy: the visitor's address is taken from `X-Forwarded-For` |
| `PONI_GBP_OAUTH_CLIENT_ID` / `_SECRET` | none | The Google OAuth client (a web client) through which the GBP managing account, the sending mailbox and the Workspace administrator are connected on the status page. Its redirect addresses must include `<PONI_PUBLIC_URL>/api/connections/gbp/callback`, `.../gmail/callback` and `.../workspace/callback` |
| `PONI_LOCATION_DISCOVERY_INTERVAL_S` | `900` | How often locations are discovered |
| `PONI_REVIEW_POLL_TICK_S` | `60` | How often the review poll job runs. Due polls are picked up at this tick; how often a location is polled is `PONI_REVIEW_POLL_INTERVAL_S` |
| `PONI_REVIEW_POLL_INTERVAL_S` | `120` | How often each location's reviews are polled (D-112); at least 60 |
| `PONI_REVIEW_FULL_SYNC_INTERVAL_S` | `3600` | How often every review of a location is reconciled |
| `PONI_STAFF_CHECK_INTERVAL_S` | `300` | How often the staff check reads the Workspace directory's list and disables or enables users (D-106); at least 60 |
| `PONI_PUBSUB_SUBSCRIPTION` | none | `projects/…/subscriptions/…`; empty means polling only |
| `PONI_GOOGLE_CREDENTIALS` | `adc` | Pub/Sub credentials: `adc` (gcloud, development) or `metadata` (the VM's service account) |
| `PONI_PUBSUB_PULL_INTERVAL_S` | `30` | How often notifications are pulled |
| `PONI_LLM_<NAME>_BASE_URL` / `PONI_LLM_<NAME>_API_KEY` | none | A model provider for review analysis and drafts, any number of them (D-059): an OpenAI-compatible address up to the version segment (the engine posts to `…/chat/completions`) and its key. `<NAME>` is letters, digits and `_`, for example `DASHSCOPE` or `OPENAI`; the dashboard shows it in lower case. Without any, reviews are synced but not analyzed |
| `PONI_LLM_<NAME>_PROTOCOL` | none, required | How that provider is spoken to: `openai` (any OpenAI-compatible provider, for example DashScope) or `anthropic` (Anthropic's Messages API; the engine posts to `…/messages`). The engine refuses to start if a provider with an address and a key has no protocol. The judge of the go-live test is by default the model `claude-opus-5-5` at a provider named `anthropic` (D-074): `PONI_LLM_ANTHROPIC_BASE_URL=https://api.anthropic.com/v1`, its key, and `PONI_LLM_ANTHROPIC_PROTOCOL=anthropic` |
| `PONI_LLM_TIMEOUT_S` / `PONI_LLM_MAX_ATTEMPTS` | `60` / `3` | Timeout of each stage of one model request, and requests per call before the job is retried later |
| `PONI_ANALYSIS_TICK_S` | `60` | How often reviews that still need an analysis or a draft are picked up |
| `PONI_POST_CONFIRM_WINDOW_S` | `1800` | How long a reply that was sent may stay invisible on Google before the engine stops waiting and returns the draft to the approval queue (D-062). At least 600 |
| `PONI_EMAIL_FROM_NAME` | `PONI GBP Manager` | The name shown beside the address notifications are sent from. The address is the mailbox connected on the status page, whichever account signed in there (D-100); no setting names it |
| `PONI_GOOGLE_MOCK_URL` | none | Local only: the address of the mock of Google, a loopback address, which every call to Google then goes to instead (D-087; [docs/runbook-google-mock.md](docs/runbook-google-mock.md)) |
| `PONI_APNS_ENVIRONMENT`, `PONI_APNS_KEY`, `PONI_APNS_KEY_ID`, `PONI_APNS_TEAM_ID`, `PONI_APNS_BUNDLE_ID` | none | Push to the phone app (D-097), all five or none: `production` (builds from TestFlight and the App Store) or `sandbox` (builds run from Xcode; the only one allowed with the Google mock), the APNs auth key as its `.p8` file holds it between the BEGIN and END lines (`grep -v -- ----- AuthKey_XXXXXXXXXX.p8 \| tr -d '\n'`; a secret), its ten-character key id, the team id, and the app's bundle id. None: nothing is pushed and push rules are refused |
| `PONI_PUBLIC_BUCKET` | empty | Where the pages of reports are put when they are built (D-104), and the photos and videos of local posts: a Cloud Storage bucket that anyone can read an object of and nobody can list, or, with a slash in the value, a local folder for development (`/tmp/poni-public`). Empty: no report is built, and no photo or video can be uploaded for a post |
| `PONI_PUBLIC_FOLDER_URL` | none | Local only: the address the folder of `PONI_PUBLIC_BUCKET` is served at, so that the dashboard can show a report's page (D-105): `http://127.0.0.1:8088`, a loopback address; only with a folder, and refused with an `https` public URL. Start the server with `PONI_PUBLIC_FOLDER=<the folder> docker compose -f deploy/docker-compose.dev.yml --profile public up -d public`. Without it an object's address is the folder's `file://` one, which a browser does not show in the dashboard |

The model per task and the batch size are not environment settings: they live in the database and are edited in the dashboard under **Analysis settings**, each with its default from the knowledge base beside it. Model prices are part of the knowledge base (`knowledge/models.json`). The two switches under **Posting and sending**, posting replies to Google and sending notification emails, are both off after installation.

The engine has no subcommands: `poni-gbp-engine [--config FILE]` runs it, `--version` prints its version.

## poni-tools

From `tools/`, with the same settings as the engine (`PONI_DATABASE_URL` and so on, from the environment, `--config FILE`, `PONI_CONFIG` or `~/.config/poni-gbp/engine.env`):

| Command | What it does |
| ------- | ------------ |
| `uv run poni-tools migrate` | Applies pending migrations; `--status` only shows them |
| `uv run poni-tools check` | Checks `knowledge/`: every file against its schema, the files against each other, and that every industry's pack is complete |
| `uv run poni-tools seed` | Checks, then replaces the `kb_*` tables in one transaction. With `PONI_KNOWLEDGE_ARCHIVE` and `PONI_KNOWLEDGE_BUCKET` (production) it seeds from that published archive instead |
| `uv run poni-tools pack` / `publish --bucket <bucket>` | Packs the knowledge base into one archive; `publish` uploads it, from a clean checkout of `main` |
| `uv run poni-tools test-db` | Prepares the database the engine's tests start from (`ctest` runs it) |

| Setting | Default | Meaning |
| ------- | ------- | ------- |
| `PONI_KNOWLEDGE_DIR` | `knowledge/` of the checkout | The knowledge base's files |
| `PONI_KNOWLEDGE_BUCKET` / `PONI_KNOWLEDGE_ARCHIVE` | none | Where published archives are kept (a Cloud Storage bucket; a folder path stands in for one in development), and the archive this installation runs on |
| `PONI_MIGRATIONS_DIR` | `engine/migrations` of the checkout | The migration files |
| `PONI_TEST_DATABASE_URL` | the development database | The server the tests make their databases on |

## Checks

| What | Command |
| ---- | ------- |
| Engine build and tests, macOS | `cmake --build --preset macos-llvm && ctest --preset macos-llvm` (in `engine/`) |
| Tools and knowledge base | `uv run ruff check . && uv run ruff format --check . && uv run pytest -q && uv run poni-tools check` (in `tools/`; the tests need the database server) |
| Engine build and tests, Linux container | `engine/scripts/linux.sh test` |
| Engine answers match the API contract | `npm run check:contract` (in `api/`, engine running with development sign-in and a public bucket or a folder for one; it signs in and creates a test client), or `engine/scripts/linux.sh contract` for the Linux build, which uses its own database `poni_linux` |
| API contract lint | `npm run lint` (in `api/`) |

CI (`.github/workflows/ci.yml`) runs all of these on every push and pull request: the engine on Linux arm64 and x86_64 with clang, through the same `engine/scripts/linux.sh`, the contract check and lint, and the tools with the knowledge base check. `.github/workflows/ai-live.yml` runs the adversarial suite against the real model, by hand only.

## Repository layout

```
api/          openapi.yaml (the API contract), its lint rules and the contract check
engine/       C++23 engine (a daemon): src/{core,db,jobs,connectors,domain,http}, migrations/, tests/
tools/        poni-tools (Python): migrations, and checking, seeding and publishing the knowledge base
knowledge/    the knowledge base: industry packs, voice templates, defaults, model prices, and their schemas
deploy/       Docker Compose for development and production, Caddyfile, backups; see deploy/README.md
docs/         architecture (with the requirements), decisions in force, work items, the Google mock runbook
```
