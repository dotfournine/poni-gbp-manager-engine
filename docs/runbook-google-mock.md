# Runbook: every workflow on made-up data, against the Google mock

The engine can run against `poni-tools mock-google`, a local mock of the Google APIs it uses (D-087), instead of Google. The mock serves made-up businesses (the knowledge base's test clients) and made-up reviews, takes the engine's replies, gives a report its visibility numbers, and keeps the email the engine sends in an outbox. Its test portal is where a person posts, edits and removes reviews.

Everything below runs next to a normal local setup without touching it: the mock engine gets its own database, its own config file and its own ports (engine 8081, dashboard 5174, mock 8090). Never point the mock engine at the real database or at `~/.config/poni-gbp/engine.env`; the engine refuses to start when a database and the mock setting disagree, in either direction.

What the mock does not do: Pub/Sub (the engine polls), Google sign-in (development sign-in instead), Cloud Storage (a local folder instead), local posts (the dashboard says posts are unavailable), and any of Google's delete endpoints.

The dashboard is not in this repository: it is run from its own repository, with its dev server pointed at this engine.

## Once: a database and a config file

From the repository root, with the development PostgreSQL running (`docker compose -f deploy/docker-compose.dev.yml up -d --wait db`):

```bash
docker compose -f deploy/docker-compose.dev.yml exec db createdb -U poni poni_mock
```

The config file, `~/.config/poni-gbp/mock.env` (mode 600). The OAuth client's id and secret are any text: the mock accepts them. The public bucket is a folder.

```bash
umask 077 && mkdir -p ~/.local/state/poni-gbp/mock-public && cat > ~/.config/poni-gbp/mock.env <<EOF
PONI_DATABASE_URL=postgresql://poni:poni@127.0.0.1:55432/poni_mock
PONI_HTTP_PORT=8081
PONI_PUBLIC_URL=http://localhost:5174
PONI_DEV_LOGIN_EMAIL=dev@localhost
PONI_SECRET_KEY=$(openssl rand -base64 32)
PONI_GOOGLE_MOCK_URL=http://127.0.0.1:8090
PONI_GBP_OAUTH_CLIENT_ID=mock-client
PONI_GBP_OAUTH_CLIENT_SECRET=mock-secret
PONI_LOCATION_DISCOVERY_INTERVAL_S=60
PONI_REVIEW_POLL_TICK_S=10
PONI_REVIEW_FULL_SYNC_INTERVAL_S=300
PONI_ANALYSIS_TICK_S=10
PONI_PUBLIC_BUCKET=$HOME/.local/state/poni-gbp/mock-public
PONI_PUBLIC_FOLDER_URL=http://127.0.0.1:8088
EOF
```

The public folder is served on 127.0.0.1:8088 so that the dashboard can show a report's page (D-105). Start the server once (from the repository's root):

```bash
mkdir -p ~/.local/state/poni-gbp/mock-public
PONI_PUBLIC_FOLDER=$HOME/.local/state/poni-gbp/mock-public docker compose -f deploy/docker-compose.dev.yml --profile public up -d public
```

For analysis, drafts and report summaries, add a model provider as in `engine.env` (`PONI_LLM_<NAME>_BASE_URL`, `_API_KEY`, `_PROTOCOL`). The real model is the point of the exercise; it costs a few cents.

Migrate and seed the mock database (from `tools/`):

```bash
uv run poni-tools --config ~/.config/poni-gbp/mock.env migrate
```

```bash
uv run poni-tools --config ~/.config/poni-gbp/mock.env seed
```

## Start the three programs

Each in its own terminal. The mock (from `tools/`; `--reset` starts again from the made-up data):

```bash
uv run poni-tools mock-google
```

The engine (from `engine/`):

```bash
./build/macos-llvm/poni-gbp-engine --config ~/.config/poni-gbp/mock.env
```

The dashboard, from its own repository's `dashboard/` folder. Its dev server forwards `/api` to the engine named by the variable its `vite.config.ts` reads (here, the Linux build's engine on port 8081):

```bash
PONI_ENGINE_URL=http://127.0.0.1:8081 npm run dev -- --port 5174 --strictPort
```

The test portal is http://127.0.0.1:8090/, the dashboard http://localhost:5174. Every dashboard page shows an orange band saying it runs against the mock. The first start marks `poni_mock` as a mock database; from then on it starts only with `PONI_GOOGLE_MOCK_URL`.

## Walk through

1. **Connect the accounts.** Dashboard, development sign-in, **System status**: **Connect** on the GBP token card. The mock's consent page approves at once and sends the browser back; the card says connected. Do the same on the Notification email card. The mock signs both in as `mock-owner@example.com`, which each card shows as the connected account.
2. **Switch the locations on.** Within a minute **Locations** lists the mock's businesses, all switched off: a location does nothing until a person switches it on (D-102). Give each its industry (`food_beverage` for Golden Harbor Buffet, `wellness` for Willow Moon Head Spa) and switch it on; no client is needed (a client only groups locations, if you want one). Switching on starts a full sync, which brings in the made-up reviews, two of them with a reply the owner wrote, which the engine never posts over.
3. **Post a review** in the portal: location, a made-up name, stars, text (or none). The engine finds it at the location's next poll (its poll interval, one minute at the shortest; set it on the location) and analyzes it and writes a draft (**Reviews**, **Approval queue**).
4. **Approve and post.** Switch **Post replies and posts to Google** on under **Posting and sending** (off after installation, also in a mock database), then approve the draft in the **Approval queue**. The reply shows in the portal under the review, marked as posted by the engine, and the engine confirms it on its next read. To see the engine wait for a reply Google has not shown yet, set the portal's delay to, say, 120 seconds first.
5. **Edit and remove.** In the portal, **Edit** a review (new text or stars): the engine sees an edited review at the next poll, and an edited review always goes to the approval queue. **Delete** one: at the next full sync (5 minutes with the settings above) the engine asks for it, the mock answers `NOT_FOUND`, and the review is marked removed.
6. **Email.** Switch **Send notification emails** on and add a notification rule; what the engine sends lands in the portal's **Outbox**, not in anyone's mailbox.
7. **A report.** **Reports**, **Generate a report**, a location, last month. Its visibility part (views, calls, search keywords) comes from the mock's made-up numbers, which are the same each time for the same days. Its page goes into the public folder when the report is built and opens in the dashboard, served from 127.0.0.1:8088. With a `location_report` rule for the location and sending switched on, **Send** puts one email per recipient into the portal's **Outbox**, each with the page's address (D-104).

## Afterwards

Stop the three programs. The mock keeps its data in `~/.local/state/poni-gbp/mock-google.json`; start it with `--reset` for a fresh set. To start from nothing, drop the database (`docker compose -f deploy/docker-compose.dev.yml exec db dropdb -U poni poni_mock`), create it again and run the mock with `--reset`: a token the engine stored for an old mock is refused by a new one, so connect again on the status page.
