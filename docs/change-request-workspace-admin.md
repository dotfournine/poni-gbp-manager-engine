# Change request: a Google Workspace admin connection

**Status:** the owner chose this approach on 2026-11-23. It is not yet a decision: the planning session turns it into D-105 in `docs/decisions.md` and items in `docs/work-items.md`, after the owner has answered the open questions below, and then deletes this file.

**For the planning session:** read `CLAUDE.md`, then D-007, D-033, D-043, D-079, D-087, D-095 and D-100 in `docs/decisions.md`, and `engine/src/http/connect.hpp`. Plan the work, propose the decision entry to the owner, and split the work into packages that separate sessions can build (a suggested split is at the end). Nothing here is built.

## The problem

1. **Nobody ever leaves.** A row in `users` is created at first sign-in (D-033, `repo_auth.cpp`, an upsert on `google_sub`), and nothing anywhere sets `users.status = 'disabled'`. The column and every check that reads it already exist (`list_users`, `assign_escalation`, both kinds of session, `repo::push_devices`), but no route, job or tool writes it. A former employee stays in `GET /api/users`, can still be assigned escalations, and **keeps their sessions**: up to 7 days for the dashboard and 90 days for the phone app (D-095). Losing their Workspace account only stops their next sign-in, because the engine talks to Google only at sign-in.
2. **Only people who have signed in can be assigned.** `repo::assign_escalation` refuses anyone who is not an active row of `users` (409 `user_not_found`), so a new hire cannot get an escalation before their first sign-in.
3. **Names and photos go stale.** `users.name` and `picture_url` are what the ID token said at the last sign-in.

Two approaches were considered and set aside:
- A live directory call behind `GET /api/users`: it gives no engine ids for assignment or the audit log, and it leaves every session of a former employee valid.
- Google sign-in libraries in the clients (GIS, the iOS SDK) listing the directory themselves: the engine must still check who may be assigned. The engine would also have to read ID tokens from clients, which needs a signature check and a second sign-in path (against D-091 point 12 and D-095). Both clients would build the same directory code. Removing `/api/users` would break the app's contract (D-095 point 8).

## The proposal

A **third connection**, `workspace`, beside `gbp` and `gmail` (D-079, D-100). A signed-in person connects it from the dashboard's status page, signing in with a Workspace admin account. Its refresh token is stored encrypted (D-043) with the account's address. The engine uses it for **read-only** Admin SDK Directory calls.

### What it is used for

The first release covers A to D; E and F are later and listed so that the design does not shut them out.

- **A. Offboarding.** A recurring job (`Registry::every`, hourly is a starting point) asks Google about each `active` engine user. It disables those Google says are gone, which ends their sessions, app tokens and push devices from the next request on.
- **B. Disabling and enabling by hand.** `PATCH /api/users/{id}` with `status`, audited. This works without the connection and is the floor: it is built first.
- **C. Assigning people who have not signed in.** The assign picker can search the domain. Choosing someone creates their `users` row (audited) before their first sign-in, and that sign-in then finds the row.
- **D. Fresh names and photos**, refreshed by the same job.
- **E. Later: roles from Google Groups.** D-007 has one role, and every route goes through the one hook so that roles can be added. Membership of a group such as `gbp-approvers@` could decide who may approve, flip switches or connect accounts.
- **F. Later: staff tied to locations.** A group per client or per area narrows the assign picker, and later suggests an assignee. Assigning automatically would be a decision of its own.
- **Possible small extras** (the planning session decides whether they belong in the first release):
  - flag a notification rule whose staff recipient was disabled;
  - check the sign-in domains against the Workspace's own domains (`domains.list`).

It does not help with recipients outside PONI's Workspace: `location_report` recipients are often the client's own staff.

### Rules to settle in the decision, and to add to `CLAUDE.md` when built

1. **Read-only at Google, by scope and by address.** The scopes are `https://www.googleapis.com/auth/admin.directory.user.readonly` (A, C, D), plus `admin.directory.group.readonly` and `admin.directory.group.member.readonly` only when E or F is built. No scope that can write to Workspace is ever asked for.
   - The outbound client takes only GETs on `admin.googleapis.com`. This needs a rule in `http::why_refused`, plain ids, and lines in `tests/unit/write_path_test.cpp`.
   - The engine never suspends, creates, edits or deletes anything in Workspace.
2. **The engine only disables.** A job may set `disabled` and never sets `active`. Enabling again is a signed-in person's action (B), as with the auto-post pause (D-072).
3. **Missing from a list proves nothing** (in the spirit of D-037). A user is disabled by the job only on one of these:
   - a get of that user that says `suspended: true` or `archived: true`;
   - a get that answers 404 for that user's id.

   A list that lacks someone, a failed read, a 403, a token error or a partial page disables nobody. A job run that would disable more than a set share of active users at once stops and raises a `system_problem` instead. The share is for the owner to decide (question 6).
4. **What a disable does.** It is audited as `system`, in the transaction that writes it, with the reason Google gave. People are told through a notification (`repo::emit`); the planning session decides whether a new `system_problem` event is needed. Sessions end through the existing check of `users.status`: no new session logic.
5. **A user created before sign-in** (C) is keyed by the Directory user's `id` as `google_sub`.
   - **To verify:** that the Directory API's user `id` equals the `sub` claim of that person's ID token. If it does not, that person's first sign-in creates a second row. Check it against the mock first and then with one real account before C ships. If it cannot be shown, fall back to keying by email at first sign-in, and settle what happens when the two disagree.
   - Only addresses in the allowed sign-in domains (`http::domain_allowed`) are created.
   - Such a row has no session until its person signs in. A sign-in into a `disabled` row is refused as now.
6. **Least privilege for the connecting account.** The account should hold a custom admin role with only Users read (and Groups read for E and F), not a super admin. The status page shows the connected address (D-100) and whether the token works. The connect flow checks that the granted scopes include the user-read scope, as it checks `gmail.send` today (`connect.cpp`).
7. **No secrets anywhere else.** As for the other two connections: the token is encrypted in `credentials`, and nothing reaches the browser or a log.
8. **The mock (D-087).** `tools/src/poni_tools/mock_google` gets the Directory endpoints the engine reads, with made-up users, so the runbook can show offboarding end to end. Against the mock, the job runs only on the mock's data. The address check stays as it is for Google's other hosts.
9. **Pagination and limits.** Calls go through `google::send` with a limiter slot. A get per user is fine at this team's size; the planning session decides whether `users.list` with a `query` is needed for the picker's search (C).

### Likely changes, for scoping (the planning session confirms)

- **Spec first** (`api/openapi.yaml`), additions only (D-095 point 8):
  - `workspace` as a third `{account}` of `/api/connections/{account}/start` and `/callback`;
  - the integration in the status schema;
  - `PATCH /api/users/{id}` (B);
  - `GET /api/users/directory?q=` (C), which returns directory people, each marked with whether an engine user exists;
  - a way to create a user from the directory (C): `POST /api/users`, or an assign that takes an email; the planning session picks one;
  - `User.status`, and a filter so that the dashboard can list disabled users (B).
- **Migration 0010** if a column is needed: when the job last checked a user, the reason for a disable, how a row came to be (`signed_in` or `directory`). Possibly none for B alone.
- **Engine:**
  - `connectors/workspace` (a directory reader behind an interface, with a fake in `tests/fakes.hpp`);
  - `kConnectWorkspace` in `http/connect`;
  - a job class under `jobs/` for A and D;
  - the user routes;
  - `repo_users` (or the existing user code in `repo_auth.cpp` and `repo_cases.cpp`).
- **Tools:** the mock's Directory endpoints, and the runbook section.
- **Clients:** the dashboard's status page (Connect), the user list with disable and enable, and the picker's directory search. The app gets the picker search if the owner wants it there.

## Open questions for the owner

1. Is B (disable by hand) built and shipped first, on its own, ahead of the connection? Recommended: yes. It closes the session gap now.
2. Does the job disable on its own (A as written), or only flag the user for a person to disable? The first closes the gap within the hour; the second never disables by mistake.
3. Does C (assign before first sign-in) belong in the first release, given the `id`/`sub` check in rule 5?
4. Is the picker's directory search in the phone app too, or in the dashboard only?
5. Which admin account connects it, and can it be a custom role with Users read only?
6. What is the upper bound on how many users one run may disable before it stops (rule 3)?
7. E and F (groups): planned now with their scopes left out, or not planned at all?

## Suggested split into packages

The order matters where marked. Every package follows the "Before calling work done" list in `CLAUDE.md`.

1. **P1, disable by hand (B).** Spec, route, audit, tests, and the dashboard and app told. It can start at once and is independent of the rest.
2. **P2, the contract for the connection** (spec only: connection account, status, directory search, create from directory). It is merged before P4 and P6 start building against it.
3. **P3, the mock's Directory endpoints and runbook.** In the tools; it can run in parallel with P2.
4. **P4, the connection and connector.** `workspace` in `http/connect`, the scope check, `connectors/workspace` with its fake, and the `why_refused` rule with `write_path_test.cpp` lines. It needs P2.
5. **P5, the offboarding and refresh job (A, D).** Rules 2 to 4, with integration tests for every way it disables, refuses to disable or stops. It needs P1 (the disable path) and P4.
6. **P6, assigning from the directory (C).** Rule 5 verified first against the mock (P3) and a real account. It needs P2 and P4.
7. **P7, the clients.** Messages to the dashboard's builder and the app's side after each merged contract change, as `CLAUDE.md` step 3 asks.
