// Checks that a running engine answers as api/openapi.yaml says.
//
//   npm run check:contract                                   # engine at http://127.0.0.1:8080
//   PONI_API_BASE_URL=http://127.0.0.1:8081 npm run check:contract
//
// The engine must run with development sign-in on (PONI_DEV_LOGIN_EMAIL),
// because the script signs in to reach the endpoints behind the session.
// It writes a little: one sign-in and one client per run, so point it at a
// development database. What it does:
//
//   1. Without a session: open endpoints answer as the spec says, every other
//      endpoint answers 401 with the Error shape.
//   2. Signs in, then calls every GET that needs no parameters and validates
//      the status, content type and JSON body against the spec.
//   3. Creates, reads, changes and deletes a client; follows the first
//      location and review (if any) to the endpoints that take an id; reads
//      and changes the analysis settings, and saves the reply settings of the
//      seeded location and puts them back. It never starts a model call. For Phase 3 it
//      walks the draft, escalation and notification endpoints; it never
//      posts a reply (it approves a draft only to see the refusal while the
//      posting switch is off) and sends no email. It writes, replaces and
//      discards an edit of a seeded post that is "at Google", and never
//      changes a post there (the same refusal, for the same reason).
//   4. Checks the error answers: missing CSRF token, unknown id, unknown path.
//   5. For every route with a JSON body in the spec: a body with a member the
//      schema does not list, and one with null for a member that can't be
//      null, are each refused with a 400 that names the member (D-092). The
//      bodies are made from the schemas and sent to ids that don't exist: a
//      refused body changes nothing.
//   6. The phone app (D-095): the start with `client=ios`, development
//      sign-in with `client=ios`, then a few requests with the bearer token
//      and no CSRF token, and signing the token out. The dashboard's session
//      is still there afterwards.
//
// PONI_CONTRACT_REQUIRE_DATA=1 makes missing locations or reviews a failure
// (CI seeds them with scripts/contract-seed.sql). Exit code 0 means
// everything matched.
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import Ajv2020 from "ajv/dist/2020.js";
import { parse } from "yaml";

const baseUrl = (process.env.PONI_API_BASE_URL ?? "http://127.0.0.1:8080").replace(/\/$/, "");
const requireData = process.env.PONI_CONTRACT_REQUIRE_DATA === "1";
// The finished go-live run contract-seed.sql inserts under a fixed id: no configuration shows its made-up fingerprint.
const SEEDED_GO_LIVE_RUN = 900000001;
const specPath = fileURLToPath(new URL("../openapi.yaml", import.meta.url));
const spec = parse(readFileSync(specPath, "utf8"));

// OpenAPI 3.1 schemas are JSON Schema 2020-12. The whole document is
// registered so "#/components/..." references resolve; strict mode is off
// because OpenAPI adds keywords (such as "example") that Ajv doesn't know,
// and formats aren't checked (no ajv-formats dependency for "date-time").
const ajv = new Ajv2020({ strict: false, allErrors: true, validateFormats: false });
ajv.addSchema(spec, "openapi");
const pointer = (...parts) => "#/" + parts.map((p) => String(p).replaceAll("~", "~0").replaceAll("/", "~1")).join("/");
const validatorFor = (...parts) => ajv.getSchema("openapi" + pointer(...parts));

let failures = 0;
let passed = 0;
const fail = (what, detail) => {
  failures += 1;
  console.error(`FAIL  ${what}\n      ${detail}`);
};
const ok = (what, detail = "") => {
  passed += 1;
  console.log(`ok    ${what}${detail ? " " + detail : ""}`);
};

const session = { cookie: "", csrf: "" };

// `token`: the phone app's bearer token, sent beside whatever else the call carries.
async function call(method, path, { body, signedIn = true, csrf = true, token } = {}) {
  const headers = { accept: "application/json" };
  if (signedIn && session.cookie) headers.cookie = session.cookie;
  if (signedIn && csrf && session.csrf && method !== "GET") headers["x-csrf-token"] = session.csrf;
  if (token) headers.authorization = `Bearer ${token}`;
  if (body !== undefined) headers["content-type"] = "application/json";
  const response = await fetch(baseUrl + path, {
    method,
    headers,
    body: body === undefined ? undefined : JSON.stringify(body),
    redirect: "manual",
  });
  const text = await response.text();
  let json;
  try {
    json = JSON.parse(text);
  } catch {
    json = undefined;
  }
  return { status: response.status, contentType: response.headers.get("content-type") ?? "", text, json, response };
}

// Resolves a response object that may be a "$ref" to components/responses.
function declaredResponse(operation, status) {
  const declared = operation.responses?.[String(status)];
  if (!declared?.$ref) return { declared, schemaPath: undefined };
  const name = declared.$ref.split("/").pop();
  return { declared: spec.components.responses[name], schemaPath: ["components", "responses", name] };
}

// Validates one answer against what the spec declares for (path, method, status).
function check(name, specPath, method, got) {
  const operation = spec.paths[specPath]?.[method.toLowerCase()];
  if (!operation) return fail(name, `the spec has no ${method} ${specPath}`);
  const { declared, schemaPath } = declaredResponse(operation, got.status);
  if (!declared) {
    return fail(name, `status ${got.status} is not in the spec (declared: ${Object.keys(operation.responses ?? {}).join(", ")})\n      ${got.text.slice(0, 300)}`);
  }
  if (!declared.content?.["application/json"]) return ok(name, `-> ${got.status}`);
  if (!got.contentType.startsWith("application/json") || got.json === undefined) {
    return fail(name, `expected a JSON body, got content-type "${got.contentType}": ${got.text.slice(0, 200)}`);
  }
  const base = schemaPath ?? ["paths", specPath, method.toLowerCase(), "responses", got.status];
  const validate = validatorFor(...base, "content", "application/json", "schema");
  if (!validate(got.json)) {
    return fail(name, `status ${got.status} body does not match the spec: ${ajv.errorsText(validate.errors)}\n      ${got.text.slice(0, 600)}`);
  }
  return ok(name, `-> ${got.status}`);
}

function expectStatus(name, got, status, code) {
  const validate = validatorFor("components", "schemas", "Error");
  if (got.status !== status) return fail(name, `expected ${status}, got ${got.status}: ${got.text.slice(0, 200)}`);
  if (!validate(got.json)) return fail(name, `${status} body is not an Error: ${ajv.errorsText(validate.errors)}\n      ${got.text}`);
  if (code && got.json.error.code !== code) return fail(name, `expected error code ${code}, got ${got.json.error.code}`);
  return ok(name, `-> ${status} ${got.json.error.code}`);
}

// ---- request bodies (D-092) ----

const resolve = (schema) => (schema?.$ref ? spec.components.schemas[schema.$ref.split("/").pop()] : schema);
const takesNull = (schema) => {
  const resolved = resolve(schema);
  return [resolved.type].flat().includes("null") || (resolved.oneOf ?? []).some(takesNull);
};
// A value the schema's type takes. Lengths, ranges and patterns don't matter: the body is refused for its shape first.
function sample(schema) {
  const resolved = resolve(schema);
  if (takesNull(resolved)) return null;
  if (resolved.enum) return resolved.enum[0];
  switch (resolved.type) {
    case "object":
      return Object.fromEntries((resolved.required ?? []).map((name) => [name, sample(resolved.properties[name])]));
    case "array":
      return [];
    case "string":
      return "x";
    case "boolean":
      return false;
    default:
      return 1;
  }
}

// A body the spec does not allow: the answer is a 400 `invalid_request` whose message names `member`.
async function expectRefused(name, method, url, body, member) {
  const got = await call(method.toUpperCase(), url, { body });
  // Against the Google mock the routes that would send a post answer before they read the body.
  if (got.status === 409 && got.json?.error?.code === "local_posts_unavailable") return console.log(`skip  ${name}: the engine works against the Google mock`);
  const sent = `\n      sent ${JSON.stringify(body)}`;
  if (got.status !== 400) return fail(name, `expected 400, got ${got.status}: ${got.text.slice(0, 200)}${sent}`);
  if (!got.json?.error?.message?.includes(member)) return fail(name, `the message does not name "${member}": ${got.text.slice(0, 200)}${sent}`);
  return expectStatus(name, got, 400, "invalid_request");
}

// Every route with a JSON body in the spec refuses a member its schema does not list, and null for a
// member that can't be null. The ids don't exist: a refused body is refused before anything is looked up.
async function checkRefusedBodies() {
  for (const [path, item] of Object.entries(spec.paths ?? {})) {
    for (const [method, operation] of Object.entries(item)) {
      const schema = resolve(operation?.requestBody?.content?.["application/json"]?.schema);
      if (!schema) continue;
      const route = `${method.toUpperCase()} ${path}`;
      const url = path.replaceAll(/\{[a-z_]+\}/g, "999999999");
      const valid = sample(schema);
      await expectRefused(`${route} (a member the spec does not list)`, method, url, { ...valid, not_in_the_spec: true }, "not_in_the_spec");
      const firm = Object.keys(schema.properties ?? {}).find((name) => !takesNull(schema.properties[name]));
      if (firm) await expectRefused(`${route} (null for "${firm}")`, method, url, { ...valid, [firm]: null }, firm);
      else console.log(`note  ${route}: every member of its body may be null, so there is no null to refuse`);
    }
  }
}

const isOpen = (operation) => Array.isArray(operation.security) && operation.security.length === 0;
const plainGets = Object.entries(spec.paths ?? {}).filter(
  ([path, item]) => item.get && !path.includes("{") && !(item.get.parameters ?? []).some((p) => p.required),
);

try {
  // 1. Without a session.
  for (const [path, item] of plainGets) {
    const got = await call("GET", path, { signedIn: false });
    if (isOpen(item.get)) check(`GET ${path} (signed out)`, path, "GET", got);
    else expectStatus(`GET ${path} (signed out)`, got, 401, "unauthenticated");
  }

  // 2. Sign in.
  const login = await call("POST", "/api/auth/dev-login", { signedIn: false });
  check("POST /api/auth/dev-login", "/api/auth/dev-login", "POST", login);
  if (login.status !== 200) {
    throw new Error("development sign-in is off: start the engine with PONI_DEV_LOGIN_EMAIL set (see README.md)");
  }
  session.cookie = login.response.headers.getSetCookie().map((c) => c.split(";")[0]).join("; ");
  session.csrf = login.json.csrf_token;

  for (const [path] of plainGets) check(`GET ${path}`, path, "GET", await call("GET", path));

  // 3. A client, start to finish.
  const name = `Contract check ${new Date().toISOString()}`;
  const created = await call("POST", "/api/clients", { body: { name, notes: "Created by check-contract.mjs" } });
  check("POST /api/clients", "/api/clients", "POST", created);
  if (created.status === 201) {
    const path = `/api/clients/${created.json.id}`;
    check(`GET ${path}`, "/api/clients/{id}", "GET", await call("GET", path));
    check(`PATCH ${path}`, "/api/clients/{id}", "PATCH", await call("PATCH", path, { body: { archived: true } }));
    check("POST /api/clients (same name)", "/api/clients", "POST", await call("POST", "/api/clients", { body: { name } }));
    // A client is bookkeeping (D-102): deleting it unlinks its locations, and this one has none.
    check(`DELETE ${path}`, "/api/clients/{id}", "DELETE", await call("DELETE", path));
    check(`DELETE ${path} (again)`, "/api/clients/{id}", "DELETE", await call("DELETE", path));
    expectStatus(`GET ${path} (deleted)`, await call("GET", path), 404, "not_found");
  }
  check("POST /api/clients (bad body)", "/api/clients", "POST", await call("POST", "/api/clients", { body: { name: 5 } }));

  // Locations and reviews exist only after a sync (or the CI seed).
  const locations = await call("GET", "/api/locations?per_page=1");
  const location = locations.json?.items?.[0];
  // The location contract-seed.sql adds, under an account that does not exist. Whatever the check changes or
  // queues about a location, it does to this one and to no other: run by hand, the database may hold real ones.
  const allLocations = await call("GET", "/api/locations?per_page=100");
  const seededLocation = (allLocations.json?.items ?? []).find((item) => item.gbp_location_id === "contract-location-1" && item.gbp_account_id === "contract-account-1");
  if (location) {
    const path = `/api/locations/${location.id}`;
    check(`GET ${path}`, "/api/locations/{id}", "GET", await call("GET", path));
  } else if (requireData) {
    fail("GET /api/locations", "no location to check; was the seed applied?");
  } else {
    console.log("skip  location endpoints: no locations in this database");
  }
  if (seededLocation) {
    const path = `/api/locations/${seededLocation.id}`;
    check(`PATCH ${path} (no change)`, "/api/locations/{id}", "PATCH", await call("PATCH", path, { body: {} }));
    // Its own fields (D-102), sent as the seed left them: a change that changes nothing.
    const own = { enabled: true, industry_key: "wellness", analyze_from: null, audience: "" };
    const same = await call("PATCH", path, { body: own });
    check(`PATCH ${path} (its own fields, as they are)`, "/api/locations/{id}", "PATCH", same);
    if (same.status !== 200 || !same.json?.enabled || same.json?.industry_key !== "wellness") fail(`PATCH ${path}`, `the seeded location is not switched on with its industry: ${same.text.slice(0, 200)}`);
    check(`PATCH ${path} (an industry the knowledge base lacks)`, "/api/locations/{id}", "PATCH", await call("PATCH", path, { body: { industry_key: "no_such_industry" } }));
    // Queues a read of the location's posts. Its account does not exist, so the job reads nothing and says so.
    check(`POST ${path}/local-posts/refresh`, "/api/locations/{id}/local-posts/refresh", "POST", await call("POST", `${path}/local-posts/refresh`));
  } else if (requireData) {
    fail("PATCH /api/locations/{id}", "the seeded location is missing; was the seed applied?");
  } else {
    console.log("skip  a location's PATCH and the read of its posts: this database has no seeded location, and the check changes no other");
  }
  // Local posts (Phase 6): the list with its filters, and each post's page.
  for (const filter of [
    "?status=draft",
    "?location_id=1",
    "?status=posted",
    "?status=draft,rejected",
    "?google_state=live,recurring",
    "?status=sent&google_state=scheduled",
    "?google_state=gone",
    "?google_state=none",
    "?google_state=none,live&status=draft,sent",
    "?order=asc&per_page=2",
  ]) {
    check(`GET /api/local-posts${filter}`, "/api/local-posts", "GET", await call("GET", `/api/local-posts${filter}`));
  }
  expectStatus("GET /api/local-posts?order=sideways", await call("GET", "/api/local-posts?order=sideways"), 400, "invalid_request");
  const localPosts = await call("GET", "/api/local-posts?per_page=100");
  for (const post of localPosts.json?.items ?? []) {
    check(`GET /api/local-posts/${post.id} (${post.origin})`, "/api/local-posts/{id}", "GET", await call("GET", `/api/local-posts/${post.id}`));
  }
  if (requireData && (localPosts.json?.items ?? []).length < 2) {
    fail("GET /api/local-posts", "no local posts to check; was the seed applied?");
  }
  check("GET /api/local-posts/0", "/api/local-posts/{id}", "GET", await call("GET", "/api/local-posts/0"));
  // An approval that is to be refused, and only while posting is off: the switch is read a moment before,
  // every time, and anything but the expected refusal is a failure. The check never sends anything to Google.
  const refusedApproval = async (name, specPath, url, body, code) => {
    const switches = await call("GET", "/api/system/settings");
    if (switches.json?.posting_enabled !== false) return console.log(`skip  ${name}: the posting switch is on in this database, and the check never sends a post to Google or changes one there`);
    const approval = await call("POST", url, { body });
    check(name, specPath, "POST", approval);
    if (approval.status !== 409 || approval.json?.error?.code !== code) fail(name, `expected the refusal ${code}, got ${approval.status} ${approval.json?.error?.code ?? ""}: the check must never send a post to Google or change one there`);
  };
  // A draft, start to finish. Only ever on the location contract-seed.sql adds, under an account that does
  // not exist: never on a real location, whatever database this runs against. Nothing is sent: each
  // approval is tried only right after reading that posting is off. The draft is deleted on every way out.
  if (!seededLocation) {
    if (requireData) fail("POST /api/locations/{id}/local-posts", "the seeded location is missing; was the seed applied?");
    else console.log("skip  a draft of a local post: this database has no seeded location, and the check writes a draft on no other");
  } else {
    const content = { topic_type: "standard", summary: "Written by check-contract.mjs.", language_code: "en", call_to_action: null, event: null, offer: null, media_ids: [], scheduled_time: null };
    const draft = await call("POST", `/api/locations/${seededLocation.id}/local-posts`, { body: content });
    check(`POST /api/locations/${seededLocation.id}/local-posts`, "/api/locations/{id}/local-posts", "POST", draft);
    check("POST /api/locations/{id}/local-posts (not a post)", "/api/locations/{id}/local-posts", "POST", await call("POST", `/api/locations/${seededLocation.id}/local-posts`, { body: { ...content, topic_type: "alert" } }));
    if (draft.status === 201) {
      const path = `/api/local-posts/${draft.json.id}`;
      let copyPath;
      try {
        const edited = { ...content, summary: "Written by check-contract.mjs, then edited.", call_to_action: { action_type: "call", url: null } };
        const saved = await call("PUT", path, { body: edited });
        check(`PUT ${path}`, "/api/local-posts/{id}", "PUT", saved);
        if (saved.status === 200) {
          // The first content is not the stored content any more; the stored one meets the switch.
          await refusedApproval(`POST ${path}/approve (content changed)`, "/api/local-posts/{id}/approve", `${path}/approve`, content, "local_post_changed");
          await refusedApproval(`POST ${path}/approve (posting is off)`, "/api/local-posts/{id}/approve", `${path}/approve`, edited, "posting_disabled");
        } else {
          fail(`PUT ${path}`, `expected 200, got ${saved.status}: no approval is tried`);
        }
        check(`POST ${path}/reject`, "/api/local-posts/{id}/reject", "POST", await call("POST", `${path}/reject`, { body: { reason: "Only a check." } }));
        check(`POST ${path}/reject (again)`, "/api/local-posts/{id}/reject", "POST", await call("POST", `${path}/reject`));
        check(`PUT ${path} (rejected)`, "/api/local-posts/{id}", "PUT", await call("PUT", path, { body: edited }));
        // Only the refusal: the check never asks for a post at Google to be deleted.
        check(`POST ${path}/remove (not at Google)`, "/api/local-posts/{id}/remove", "POST", await call("POST", `${path}/remove`));
        // A copy of the rejected draft is a new draft (D-094); both are then deleted, which touches nothing at Google.
        const copy = await call("POST", `${path}/copy`);
        check(`POST ${path}/copy`, "/api/local-posts/{id}/copy", "POST", copy);
        if (copy.status === 201) {
          copyPath = `/api/local-posts/${copy.json.id}`;
          // The source now lists its copy (D-098).
          check(`GET ${path} (copied)`, "/api/local-posts/{id}", "GET", await call("GET", path));
        }
      } finally {
        // Whatever happened above, no draft of the check's stays behind. (A draft that is not yet rejected is
        // rejected first; one that was approved against every guard cannot be deleted, and that is reported.)
        await call("POST", `${path}/reject`, { body: { reason: "Only a check." } });
        if (copyPath) check(`DELETE ${copyPath} (the copy)`, "/api/local-posts/{id}", "DELETE", await call("DELETE", copyPath));
        const deleted = await call("DELETE", path);
        check(`DELETE ${path} (rejected)`, "/api/local-posts/{id}", "DELETE", deleted);
        if (deleted.status !== 204) fail(`DELETE ${path}`, `the check left its draft behind: ${deleted.status} ${deleted.text.slice(0, 200)}`);
      }
      check(`DELETE ${path} (again)`, "/api/local-posts/{id}", "DELETE", await call("DELETE", path));
    }
  }
  // A post Google has is never deleted here: only the refusal.
  const atGoogle = (localPosts.json?.items ?? []).find((post) => post.status === "sent");
  if (atGoogle) {
    const refused = await call("DELETE", `/api/local-posts/${atGoogle.id}`);
    check(`DELETE /api/local-posts/${atGoogle.id} (at Google)`, "/api/local-posts/{id}", "DELETE", refused);
    if (refused.status !== 409) fail(`DELETE /api/local-posts/${atGoogle.id}`, `expected a refusal for a post at Google, got ${refused.status}`);
  }
  // An edit of a post "at Google" (D-099): written, replaced and discarded, which touches nothing at Google.
  // Only ever of the post contract-seed.sql adds, which belongs to the seed's own client under an account
  // that does not exist: never of a real post, whatever database this runs against. The check never
  // changes a post at Google: each approval is tried only right after reading that posting is off, and
  // anything but the expected refusal is a failure. The edit is discarded on every way out.
  const seededPost = (localPosts.json?.items ?? []).find(
    (post) => post.status === "sent" && post.client_name === "Contract Check Spa" && post.google_name === "accounts/contract-account-1/locations/contract-location-1/localPosts/contract-post-1",
  );
  if (!seededPost) {
    if (requireData) fail("PUT /api/local-posts/{id}/edit", "the seeded post at Google is missing; was the seed applied?");
    else console.log("skip  the edit of a post at Google: this database has no seeded post, and the check edits no other");
  } else {
    const path = `/api/local-posts/${seededPost.id}/edit`;
    const content = Object.fromEntries(["topic_type", "summary", "language_code", "call_to_action", "event", "offer", "media_ids", "scheduled_time"].map((name) => [name, seededPost[name]]));
    // An edit says what the post should say and what the person was looking at: the post's content as just read.
    const written = await call("PUT", path, { body: { content: { ...content, summary: "Edited by check-contract.mjs." }, based_on: content } });
    try {
      check(`PUT ${path}`, "/api/local-posts/{id}/edit", "PUT", written);
      // Against the Google mock the answer is the 409 `local_posts_unavailable`, and there is no edit to go on with.
      if (written.status === 200) {
        if (written.json.edit?.status !== "draft") fail(`PUT ${path}`, `the post does not show its edit as a draft: ${JSON.stringify(written.json.edit)}`);
        if (JSON.stringify(written.json.edit?.fields) !== JSON.stringify(["summary"])) fail(`PUT ${path}`, `the edit does not say that it changes the summary alone: ${JSON.stringify(written.json.edit?.fields)}`);
        check(`GET /api/local-posts/${seededPost.id} (with an edit)`, "/api/local-posts/{id}", "GET", await call("GET", `/api/local-posts/${seededPost.id}`));
        check("GET /api/local-posts (one with an edit)", "/api/local-posts", "GET", await call("GET", "/api/local-posts?per_page=100"));
        // An edit does not change when Google publishes a post: a 400 that names the member.
        const moved = await call("PUT", path, { body: { content: { ...content, scheduled_time: content.scheduled_time === null ? "2031-01-01T00:00:00Z" : null }, based_on: content } });
        check(`PUT ${path} (another time)`, "/api/local-posts/{id}/edit", "PUT", moved);
        if (moved.status !== 400 || !moved.json?.error?.message?.includes("scheduled_time")) fail(`PUT ${path} (another time)`, `expected a 400 that names scheduled_time, got ${moved.status}: ${moved.text.slice(0, 200)}`);
        // Written from a post that is not the post as it is now: refused, and nothing is stored.
        const stale = await call("PUT", path, { body: { content: { ...content, summary: "Edited by check-contract.mjs, from a stale page." }, based_on: { ...content, summary: "What the post said once." } } });
        check(`PUT ${path} (the post changed since)`, "/api/local-posts/{id}/edit", "PUT", stale);
        if (stale.status !== 409 || stale.json?.error?.code !== "local_post_changed") fail(`PUT ${path} (the post changed since)`, `expected the refusal local_post_changed, got ${stale.status}: ${stale.text.slice(0, 200)}`);
        const replaced = await call("PUT", path, { body: { content: { ...content, summary: "Edited by check-contract.mjs, then replaced." }, based_on: content } });
        check(`PUT ${path} (replaced)`, "/api/local-posts/{id}/edit", "PUT", replaced);
        if (replaced.status === 200) {
          // The content of the edit that was replaced is not the stored edit any more; the stored one meets the switch.
          await refusedApproval(`POST ${path}/approve (content changed)`, "/api/local-posts/{id}/edit/approve", `${path}/approve`, written.json.edit.content, "local_post_changed");
          await refusedApproval(`POST ${path}/approve (posting is off)`, "/api/local-posts/{id}/edit/approve", `${path}/approve`, replaced.json.edit.content, "posting_disabled");
        } else {
          fail(`PUT ${path} (replaced)`, `expected 200, got ${replaced.status}: no approval is tried`);
        }
      }
    } finally {
      // Whatever happened above, no edit stays behind. An edit the check wrote is a draft, which a discard takes.
      const discarded = await call("DELETE", path);
      if (written.status === 200) check(`DELETE ${path}`, "/api/local-posts/{id}/edit", "DELETE", discarded);
      const after = await call("GET", `/api/local-posts/${seededPost.id}`);
      if (after.status === 200 && after.json?.edit !== null) fail(`DELETE ${path}`, `the check left an edit behind: ${JSON.stringify(after.json.edit).slice(0, 200)}`);
    }
    if (written.status === 200) {
      check(`DELETE ${path} (again)`, "/api/local-posts/{id}/edit", "DELETE", await call("DELETE", path));
      check(`GET /api/local-posts/${seededPost.id} (the edit discarded)`, "/api/local-posts/{id}", "GET", await call("GET", `/api/local-posts/${seededPost.id}`));
    }
  }
  const onlyACheck = { topic_type: "standard", summary: "Only a check.", language_code: "en", call_to_action: null, event: null, offer: null, media_ids: [], scheduled_time: null };
  check("PUT /api/local-posts/0/edit", "/api/local-posts/{id}/edit", "PUT", await call("PUT", "/api/local-posts/0/edit", { body: { content: onlyACheck, based_on: onlyACheck } }));
  check("DELETE /api/local-posts/0/edit", "/api/local-posts/{id}/edit", "DELETE", await call("DELETE", "/api/local-posts/0/edit"));
  check("POST /api/local-posts/0/edit/approve", "/api/local-posts/{id}/edit/approve", "POST", await call("POST", "/api/local-posts/0/edit/approve", { body: { topic_type: "standard", summary: "Only a check.", language_code: "en", call_to_action: null, event: null, offer: null, media_ids: [], scheduled_time: null } }));
  // The help with writing a post. The highlights are counted from the database. The check never asks the
  // model: an idea or a text costs a call, so only what is refused before one is tried here, and the
  // answers of the model's two routes are checked field by field in the engine's tests.
  if (location) {
    const path = `/api/locations/${location.id}`;
    check(`GET ${path}/review-highlights`, "/api/locations/{id}/review-highlights", "GET", await call("GET", `${path}/review-highlights`));
    check(`POST ${path}/local-post-drafts (nothing to write from)`, "/api/locations/{id}/local-post-drafts", "POST", await call("POST", `${path}/local-post-drafts`, { body: { topic_type: "standard", language_code: "en", brief: "", summary: "", title: null } }));
  }
  check("GET /api/locations/0/review-highlights", "/api/locations/{id}/review-highlights", "GET", await call("GET", "/api/locations/0/review-highlights"));
  check("POST /api/locations/0/local-post-ideas", "/api/locations/{id}/local-post-ideas", "POST", await call("POST", "/api/locations/0/local-post-ideas"));
  check("POST /api/locations/0/local-post-drafts", "/api/locations/{id}/local-post-drafts", "POST", await call("POST", "/api/locations/0/local-post-drafts", { body: { topic_type: "standard", language_code: "en", brief: "Only a check.", summary: "", title: null } }));
  // Not an image: refused before anything is stored (or, without a public bucket, because there is none).
  check("POST /api/local-post-media (not an image)", "/api/local-post-media", "POST", await call("POST", "/api/local-post-media", { body: "not an image" }));
  // The first bytes of a video file and nothing a length or a size could be read from (D-090): refused the same way.
  check("POST /api/local-post-media (not a video)", "/api/local-post-media", "POST", await call("POST", "/api/local-post-media", { body: "\u0000\u0000\u0000\u0018ftypisom\u0000\u0000\u0000\u0000isomavc1 and nothing more" }));
  check("POST /api/local-posts/0/remove", "/api/local-posts/{id}/remove", "POST", await call("POST", "/api/local-posts/0/remove"));
  check("POST /api/local-posts/0/copy", "/api/local-posts/{id}/copy", "POST", await call("POST", "/api/local-posts/0/copy"));
  check("DELETE /api/local-posts/0", "/api/local-posts/{id}", "DELETE", await call("DELETE", "/api/local-posts/0"));
  check("POST /api/locations/0/local-posts/refresh", "/api/locations/{id}/local-posts/refresh", "POST", await call("POST", "/api/locations/0/local-posts/refresh"));
  for (const filter of ["?has_reply=true", "?has_reply=false", "?status=removed"]) {
    check(`GET /api/reviews${filter}`, "/api/reviews", "GET", await call("GET", `/api/reviews${filter}&per_page=100`));
  }
  const reviews = await call("GET", "/api/reviews?status=all&per_page=1");
  const review = reviews.json?.items?.[0];
  if (review) {
    check(`GET /api/reviews/${review.id}`, "/api/reviews/{id}", "GET", await call("GET", `/api/reviews/${review.id}`));
    check(`GET /api/reviews/${review.id}/events`, "/api/reviews/{id}/events", "GET", await call("GET", `/api/reviews/${review.id}/events`));
  } else if (requireData) {
    fail("GET /api/reviews", "no review to check; was the seed applied?");
  } else {
    console.log("skip  review endpoints: no reviews in this database");
  }
  // Phase 2: analysis, drafts and the settings behind them.
  for (const filter of ["?analyzed=true&status=all", "?has_draft=true&status=all&sort=draft_time", "?flagged=true&status=all", "?route=escalate&status=all"]) {
    check(`GET /api/reviews${filter}`, "/api/reviews", "GET", await call("GET", `/api/reviews${filter}&per_page=100`));
  }
  const drafted = (await call("GET", "/api/reviews?has_draft=true&status=all&per_page=1")).json?.items?.[0];
  if (drafted) {
    check(`GET /api/reviews/${drafted.id}/drafts`, "/api/reviews/{id}/drafts", "GET", await call("GET", `/api/reviews/${drafted.id}/drafts`));
    if (!drafted.analysis || !drafted.draft) fail("GET /api/reviews?has_draft=true", "the review carries no analysis or no draft");
  } else if (requireData) {
    fail("GET /api/reviews?has_draft=true", "no drafted review to check; was the seed applied?");
  }
  const removed = (await call("GET", "/api/reviews?status=removed&per_page=1")).json?.items?.[0];
  if (removed) {
    // A removed review can't be analyzed; a live one would start a model call, which the check must not do.
    const path = `/api/reviews/${removed.id}/analyze`;
    check(`POST ${path} (removed review)`, "/api/reviews/{id}/analyze", "POST", await call("POST", path));
  }
  // Connecting a Google account (D-079). The check never goes to Google: an engine without the OAuth client
  // answers 409, one with it answers with the address at Google, and neither stores anything.
  check("POST /api/connections/gbp/start", "/api/connections/{account}/start", "POST", await call("POST", "/api/connections/gbp/start"));
  expectStatus("POST /api/connections/drive/start", await call("POST", "/api/connections/drive/start"), 404, "not_found");

  // The knowledge base (FR-13): read-only, seeded by poni-tools before the engine starts.
  const knowledge = await call("GET", "/api/knowledge");
  check("GET /api/knowledge", "/api/knowledge", "GET", knowledge);
  const industry = knowledge.json?.industries?.[0]?.key;
  if (industry) {
    const path = `/api/knowledge/industries/${industry}/corpus`;
    check(`GET ${path}`, "/api/knowledge/industries/{key}/corpus", "GET", await call("GET", path));
  } else {
    fail("GET /api/knowledge", "no industry; the engine does not start without a seeded knowledge base");
  }
  expectStatus("GET /api/knowledge/industries/no_such_industry/corpus", await call("GET", "/api/knowledge/industries/no_such_industry/corpus"), 404, "not_found");
  expectStatus("PATCH /api/knowledge", await call("PATCH", "/api/knowledge", { body: {} }), 405, "method_not_allowed");

  // A location's reply settings (D-093): its own complete voice, localization and policy. A save sends all of
  // them; here they are read, saved back as they are, saved with a change, and put back. Every location has
  // settings from its discovery (D-102), and only the seeded location's are saved: between the change and the
  // putting back, a real location's replies would be drafted under the check's name, and a go-live test
  // running there would be cancelled and started again (D-077).
  const settingsSpec = "/api/locations/{id}/reply-settings";
  if (seededLocation) {
    const path = `/api/locations/${seededLocation.id}/reply-settings`;
    const stored = await call("GET", path);
    check(`GET ${path}`, settingsSpec, "GET", stored);
    if (stored.status === 200) {
      const { voice, localization, policy } = stored.json;
      const save = async (what, body, expected) => {
        const got = await call("PUT", path, { body });
        check(`PUT ${path} (${what})`, settingsSpec, "PUT", got);
        if (got.status !== expected) fail(`PUT ${path} (${what})`, `expected ${expected}, got ${got.status}: ${got.text.slice(0, 200)}`);
        return got;
      };
      await save("no change", { voice, localization, policy }, 200);
      const changed = await save("a new name", { voice: { ...voice, reply_name: "Contract Check" }, localization, policy }, 200);
      if (changed.json?.voice?.reply_name !== "Contract Check") fail(`PUT ${path}`, "the answer does not hold what was saved");
      await save("a value outside its limits", { voice: { ...voice, emoji: { ...voice.emoji, max: 99 } }, localization, policy }, 400);
      await save("a field left out", { voice: { ...voice, sign_off: undefined }, localization, policy }, 400);
      await save("put back", { voice, localization, policy }, 200);
    } else {
      fail(`GET ${path}`, `every location has reply settings, got ${stored.status}: ${stored.text.slice(0, 200)}`);
    }
  } else if (requireData) {
    fail(`PUT ${settingsSpec}`, "the seeded location is missing, so there are no settings to save; was the seed applied?");
  } else {
    console.log("skip  reply settings: this database has no seeded location, and the check saves no other location's settings");
  }
  expectStatus("GET /api/locations/999999999/reply-settings", await call("GET", "/api/locations/999999999/reply-settings"), 404, "not_found");
  check("PATCH /api/ai/settings (no change)", "/api/ai/settings", "PATCH", await call("PATCH", "/api/ai/settings", { body: {} }));
  check("PATCH /api/ai/settings (bad value)", "/api/ai/settings", "PATCH", await call("PATCH", "/api/ai/settings", { body: { batch_size: 1000 } }));
  check("GET /api/llm/usage?from&to", "/api/llm/usage", "GET", await call("GET", "/api/llm/usage?from=2026-01-01&to=2026-12-31"));
  check("GET /api/llm/calls?per_page=5", "/api/llm/calls", "GET", await call("GET", "/api/llm/calls?per_page=5"));
  expectStatus("GET /api/llm/usage?from=nonsense", await call("GET", "/api/llm/usage?from=nonsense"), 400, "invalid_request");

  // Phase 3: the approval queue, escalations, notifications and the switches.
  // The check never posts a reply: it approves only to see the refusal, and
  // only while the posting switch is off.
  const switches = await call("GET", "/api/system/settings");
  check("PATCH /api/system/settings (no change)", "/api/system/settings", "PATCH", await call("PATCH", "/api/system/settings", { body: {} }));
  check("PATCH /api/system/settings (bad value)", "/api/system/settings", "PATCH", await call("PATCH", "/api/system/settings", { body: { posting_enabled: "yes" } }));
  if (drafted?.draft) {
    const path = `/api/drafts/${drafted.draft.id}`;
    const draft = await call("GET", path);
    check(`GET ${path}`, "/api/drafts/{id}", "GET", draft);
    check(`PATCH ${path}`, "/api/drafts/{id}", "PATCH", await call("PATCH", path, { body: { text: draft.json?.text ?? "x" } }));
    check(`POST ${path}/reject (no reason)`, "/api/drafts/{id}/reject", "POST", await call("POST", `${path}/reject`, { body: {} }));
    check(`POST ${path}/regenerate (no instructions)`, "/api/drafts/{id}/regenerate", "POST", await call("POST", `${path}/regenerate`, { body: {} }));
    if (switches.json?.posting_enabled === false) {
      const approval = await call("POST", `${path}/approve`, { body: { text: draft.json?.text ?? "" } });
      check(`POST ${path}/approve (posting is off)`, "/api/drafts/{id}/approve", "POST", approval);
      if (approval.status !== 409) fail(`POST ${path}/approve`, `expected a refusal while posting is off, got ${approval.status}`);
    } else {
      console.log("skip  approve: the posting switch is on in this database, and the check never posts");
    }
  }
  // Phase 4: auto-posting. The check never switches it on and never lifts a
  // pause the engine set: it reads, sends changes that change nothing or
  // are refused, and resumes only what is not paused.
  const autoPosting = await call("GET", "/api/auto-posting");
  check("GET /api/auto-posting", "/api/auto-posting", "GET", autoPosting);
  if (autoPosting.json?.paused === false) {
    check("POST /api/auto-posting/resume (not paused)", "/api/auto-posting/resume", "POST", await call("POST", "/api/auto-posting/resume"));
  } else {
    console.log("skip  resume: auto-posting is paused in this database, and the check never lifts a pause");
  }
  check("GET /api/auto-posts", "/api/auto-posts", "GET", await call("GET", "/api/auto-posts?per_page=5"));
  if (location) {
    const path = `/api/locations/${location.id}/auto-posting`;
    const settings = await call("PATCH", path, { body: {} });
    check(`PATCH ${path} (no change)`, "/api/locations/{id}/auto-posting", "PATCH", settings);
    check(`PATCH ${path} (bad value)`, "/api/locations/{id}/auto-posting", "PATCH", await call("PATCH", path, { body: { enabled: "yes" } }));
    check(`PATCH ${path} (bad cap)`, "/api/locations/{id}/auto-posting", "PATCH", await call("PATCH", path, { body: { daily_cap: 0 } }));
    check(`PATCH ${path} (unknown time zone)`, "/api/locations/{id}/auto-posting", "PATCH", await call("PATCH", path, { body: { time_zone: "Mars/Olympus" } }));
    if (settings.json?.paused === false) {
      check(`POST ${path}/resume (not paused)`, "/api/locations/{id}/auto-posting/resume", "POST", await call("POST", `${path}/resume`));
    }
  }
  expectStatus("PATCH /api/locations/999999999/auto-posting", await call("PATCH", "/api/locations/999999999/auto-posting", { body: { daily_cap: 5 } }), 404, "not_found");
  if (drafted?.draft) {
    const path = `/api/drafts/${drafted.draft.id}/unschedule`;
    check(`POST ${path} (not scheduled)`, "/api/drafts/{id}/unschedule", "POST", await call("POST", path));
  }
  expectStatus("POST /api/drafts/999999999/unschedule", await call("POST", "/api/drafts/999999999/unschedule"), 404, "not_found");

  // Phase 4.1: the go-live test. The check never starts one: a test calls the
  // models and costs money. It reads, and asks for things that don't exist.
  const goLive = await call("GET", "/api/go-live");
  check("GET /api/go-live", "/api/go-live", "GET", goLive);
  // A finished run the overview shows, or else the one contract-seed.sql adds under a fixed id.
  const shown = (goLive.json?.locations ?? [])
    .flatMap((location) => [location.latest_run, location.pass])
    .find((run) => run && run.status !== "running");
  const run = await call("GET", `/api/go-live-runs/${shown?.id ?? SEEDED_GO_LIVE_RUN}`);
  if (run.status === 200) {
    const path = `/api/go-live-runs/${run.json.id}`;
    check(`GET ${path}`, "/api/go-live-runs/{id}", "GET", run);
    check(`POST ${path}/cancel (not running)`, "/api/go-live-runs/{id}/cancel", "POST", await call("POST", `${path}/cancel`));
  } else if (requireData) {
    fail("GET /api/go-live-runs/{id}", "no finished go-live test to check; was the seed applied?");
  }
  expectStatus("GET /api/go-live-runs/999999999", await call("GET", "/api/go-live-runs/999999999"), 404, "not_found");
  expectStatus("POST /api/go-live-runs/999999999/cancel", await call("POST", "/api/go-live-runs/999999999/cancel"), 404, "not_found");
  expectStatus("POST /api/locations/999999999/go-live-test", await call("POST", "/api/locations/999999999/go-live-test"), 404, "not_found");

  for (const action of ["approve", "reject", "regenerate"]) {
    const body = action === "reject" ? { reason: "x" } : action === "approve" ? { text: "x" } : { instructions: "x" };
    expectStatus(`POST /api/drafts/999999999/${action}`, await call("POST", `/api/drafts/999999999/${action}`, { body }), 404, "not_found");
  }
  expectStatus("POST /api/reviews/999999999/ignore", await call("POST", "/api/reviews/999999999/ignore"), 404, "not_found");
  expectStatus("POST /api/reviews/999999999/unignore", await call("POST", "/api/reviews/999999999/unignore"), 404, "not_found");
  check("GET /api/reviews?ignored=true", "/api/reviews", "GET", await call("GET", "/api/reviews?ignored=true&status=all"));
  if (removed) {
    // A removed review can't be set aside; the answers are still checked against the spec.
    check(`POST /api/reviews/${removed.id}/ignore (removed review)`, "/api/reviews/{id}/ignore", "POST", await call("POST", `/api/reviews/${removed.id}/ignore`, { body: { reason: "x" } }));
    check(`POST /api/reviews/${removed.id}/unignore`, "/api/reviews/{id}/unignore", "POST", await call("POST", `/api/reviews/${removed.id}/unignore`));
  }
  check("GET /api/escalations?status=open", "/api/escalations", "GET", await call("GET", "/api/escalations?status=open"));
  expectStatus("GET /api/escalations?status=odd", await call("GET", "/api/escalations?status=odd"), 400, "invalid_request");
  const escalation = (await call("GET", "/api/escalations?per_page=1")).json?.items?.[0];
  if (escalation) {
    const path = `/api/escalations/${escalation.id}`;
    check(`GET ${path}`, "/api/escalations/{id}", "GET", await call("GET", path));
    check(`POST ${path}/assign (bad body)`, "/api/escalations/{id}/assign", "POST", await call("POST", `${path}/assign`, { body: {} }));
    check(`POST ${path}/assign (unknown user)`, "/api/escalations/{id}/assign", "POST", await call("POST", `${path}/assign`, { body: { assignee_id: 999999999 } }));
    check(`POST ${path}/close (bad reason)`, "/api/escalations/{id}/close", "POST", await call("POST", `${path}/close`, { body: { reason: "bored" } }));
  } else if (requireData) {
    fail("GET /api/escalations", "no escalation to check; was the seed applied?");
  }
  if (seededLocation) {
    const body = { event_type: "approval_needed", location_id: seededLocation.id, recipients: ["contract-check@example.com"] };
    const rule = await call("POST", "/api/notification-rules", { body });
    check("POST /api/notification-rules", "/api/notification-rules", "POST", rule);
    check("POST /api/notification-rules (again)", "/api/notification-rules", "POST", await call("POST", "/api/notification-rules", { body }));
    check("POST /api/notification-rules (bad address)", "/api/notification-rules", "POST", await call("POST", "/api/notification-rules", { body: { ...body, recipients: ["nobody"] } }));
    if (rule.status === 201) {
      const path = `/api/notification-rules/${rule.json.id}`;
      check(`GET ${path}`, "/api/notification-rules/{id}", "GET", await call("GET", path));
      check(`PATCH ${path}`, "/api/notification-rules/{id}", "PATCH", await call("PATCH", path, { body: { enabled: false } }));
      check(`DELETE ${path}`, "/api/notification-rules/{id}", "DELETE", await call("DELETE", path));
      check(`DELETE ${path} (again)`, "/api/notification-rules/{id}", "DELETE", await call("DELETE", path));
    }
  }
  // A push rule (D-097): made where the engine has the APNs settings, refused where it has none.
  const pushRule = await call("POST", "/api/notification-rules", { body: { event_type: "system_problem", channel: "push", recipients: ["contract-check@example.com"] } });
  check("POST /api/notification-rules (push)", "/api/notification-rules", "POST", pushRule);
  if (pushRule.status === 201) {
    check("DELETE the push rule", "/api/notification-rules/{id}", "DELETE", await call("DELETE", `/api/notification-rules/${pushRule.json.id}`));
  } else {
    expectStatus("POST /api/notification-rules (push, no APNs settings)", pushRule, 409, "push_not_configured");
  }
  check("POST /api/notifications/test (bad address)", "/api/notifications/test", "POST", await call("POST", "/api/notifications/test", { body: { recipient: "nobody" } }));
  if (switches.json?.email_sending_enabled === false) {
    // Logged as not sent: no email leaves while sending is off.
    check("POST /api/notifications/test", "/api/notifications/test", "POST", await call("POST", "/api/notifications/test", { body: { recipient: "contract-check@example.com" } }));
  }
  check("GET /api/notification-deliveries?status=not_sent", "/api/notification-deliveries", "GET", await call("GET", "/api/notification-deliveries?status=not_sent"));
  check("GET /api/notification-deliveries?escalation_id=1", "/api/notification-deliveries", "GET", await call("GET", "/api/notification-deliveries?escalation_id=1"));
  expectStatus("GET /api/notification-deliveries?escalation_id=x", await call("GET", "/api/notification-deliveries?escalation_id=x"), 400, "invalid_request");

  // Phase 5: reports, a location's (D-102): the seeded location's. What keeps
  // the model out is the period, not the engine's settings (a local engine may
  // have a model): January 2026 holds none of its reviews, and its made-up
  // account has no visibility figures, so the summary stops at "nothing to
  // summarize". A period with reviews would start a model call. Nothing is
  // sent: the build puts the page into the engine's public bucket, a folder
  // here (D-104), and sends nothing. The check then discards the report it
  // built, which moves its page to the archive in that folder.
  check("GET /api/reports?status=ready", "/api/reports", "GET", await call("GET", "/api/reports?status=ready&per_page=5"));
  expectStatus("GET /api/reports?location_id=x", await call("GET", "/api/reports?location_id=x"), 400, "invalid_request");
  if (seededLocation) {
    const locationId = seededLocation.id;
    const period = { period_start: "2026-01-01", period_end: "2026-01-31", time_zone: "America/New_York" };
    const made = await call("POST", `/api/locations/${locationId}/reports`, { body: period });
    check("POST /api/locations/{id}/reports", "/api/locations/{id}/reports", "POST", made);
    if (made.status === 409 && made.json?.error?.code === "no_public_bucket") {
      fail("POST /api/locations/{id}/reports", "the engine has no public bucket: give it a folder as PONI_PUBLIC_BUCKET, a report's page goes there");
    }
    check("POST /api/locations/{id}/reports (upside down)", "/api/locations/{id}/reports", "POST", await call("POST", `/api/locations/${locationId}/reports`, { body: { ...period, period_end: "2025-12-01" } }));
    check("POST /api/locations/{id}/reports (bad zone)", "/api/locations/{id}/reports", "POST", await call("POST", `/api/locations/${locationId}/reports`, { body: { ...period, time_zone: "Mars/Base" } }));
    if (made.status === 201) {
      const path = `/api/reports/${made.json.id}`;
      // Its job runs within a second or two.
      let report = await call("GET", path);
      for (let i = 0; i < 40 && report.json?.status === "building"; i += 1) {
        await new Promise((resolve) => setTimeout(resolve, 500));
        report = await call("GET", path);
      }
      check(`GET ${path}`, "/api/reports/{id}", "GET", report);
      const built = report.json ?? {};
      if (built.status !== "ready") fail(`GET ${path}`, `the report was not built: ${report.text.slice(0, 300)}`);
      else if (typeof built.url !== "string" || built.url === "" || built.archive_url !== null || built.auto_send_status !== "off") {
        fail(`GET ${path}`, `a ready report has its page's address and nothing else: ${report.text.slice(0, 300)}`);
      }
      check(`GET /api/reports?location_id=${locationId}`, "/api/reports", "GET", await call("GET", `/api/reports?location_id=${locationId}`));
      // Only while sending is off, as in the bare engine: then both are a declared 409 and no email leaves (D-104).
      // With sending on, a rule of this location would get the report: the check sends no email.
      if (switches.json?.email_sending_enabled === false) {
        for (const [name, body] of [[`POST ${path}/send`, undefined], [`POST ${path}/send (some recipients)`, { recipients: ["contract-check@example.com"] }]]) {
          const sent = await call("POST", `${path}/send`, { body });
          check(name, "/api/reports/{id}/send", "POST", sent);
          if (sent.status !== 409) fail(name, `expected a refusal, got ${sent.status}: ${sent.text.slice(0, 300)}`);
        }
      } else {
        console.log(`skip  POST ${path}/send: email sending is switched on in this database, and the check sends no email`);
      }
      check(`GET /api/notification-deliveries?report_id=${made.json.id}`, "/api/notification-deliveries", "GET", await call("GET", `/api/notification-deliveries?report_id=${made.json.id}`));
      // Withdrawn for good (D-104): the page moves to the archive in the folder that stands in for the bucket.
      const discarded = await call("POST", `${path}/discard`);
      check(`POST ${path}/discard`, "/api/reports/{id}/discard", "POST", discarded);
      if (discarded.status !== 200 || discarded.json?.status !== "discarded" || discarded.json?.url !== null || typeof discarded.json?.archive_url !== "string") {
        fail(`POST ${path}/discard`, `a discarded report has its archive's address and no url: ${discarded.text.slice(0, 300)}`);
      }
      for (const [name, route, code] of [[`POST ${path}/discard (again)`, "discard", "report_discarded"], [`POST ${path}/send (discarded)`, "send", "report_not_ready"]]) {
        const refused = await call("POST", `${path}/${route}`);
        check(name, `/api/reports/{id}/${route}`, "POST", refused);
        if (refused.status !== 409 || refused.json?.error?.code !== code) fail(name, `expected the refusal ${code}, got ${refused.status}: ${refused.text.slice(0, 300)}`);
      }
    }
    const schedule = await call("POST", "/api/report-schedules", { body: { location_id: locationId, frequency: "monthly", month_day: 5, time: "09:00", time_zone: "America/New_York" } });
    check("POST /api/report-schedules", "/api/report-schedules", "POST", schedule);
    check("POST /api/report-schedules (no weekday)", "/api/report-schedules", "POST", await call("POST", "/api/report-schedules", { body: { location_id: locationId, frequency: "weekly", time: "09:00", time_zone: "America/New_York" } }));
    if (schedule.status === 201) {
      const path = `/api/report-schedules/${schedule.json.id}`;
      check(`PATCH ${path}`, "/api/report-schedules/{id}", "PATCH", await call("PATCH", path, { body: { frequency: "weekly", weekday: 1, enabled: false } }));
      check(`PATCH ${path} (bad time)`, "/api/report-schedules/{id}", "PATCH", await call("PATCH", path, { body: { time: "noon" } }));
      check("GET /api/report-schedules", "/api/report-schedules", "GET", await call("GET", "/api/report-schedules"));
      check(`DELETE ${path}`, "/api/report-schedules/{id}", "DELETE", await call("DELETE", path));
      check(`DELETE ${path} (again)`, "/api/report-schedules/{id}", "DELETE", await call("DELETE", path));
    }
    // A location's recipients are a rule; one for every location is refused.
    const recipients = await call("POST", "/api/notification-rules", { body: { event_type: "location_report", location_id: locationId, recipients: ["contract-check@example.com"] } });
    check("POST /api/notification-rules (location_report)", "/api/notification-rules", "POST", recipients);
    if (recipients.status === 201) check("DELETE the location_report rule", "/api/notification-rules/{id}", "DELETE", await call("DELETE", `/api/notification-rules/${recipients.json.id}`));
    expectStatus("POST /api/notification-rules (location_report, no location)", await call("POST", "/api/notification-rules", { body: { event_type: "location_report", recipients: ["contract-check@example.com"] } }), 400, "invalid_request");
  } else if (requireData) {
    fail("POST /api/locations/{id}/reports", "the seeded location is missing; was the seed applied?");
  }
  expectStatus("GET /api/reports/999999999", await call("GET", "/api/reports/999999999"), 404, "not_found");
  expectStatus("POST /api/reports/999999999/send", await call("POST", "/api/reports/999999999/send"), 404, "not_found");
  expectStatus("POST /api/reports/999999999/discard", await call("POST", "/api/reports/999999999/discard"), 404, "not_found");
  expectStatus("GET /api/notification-deliveries?report_id=x", await call("GET", "/api/notification-deliveries?report_id=x"), 400, "invalid_request");
  expectStatus("POST /api/locations/999999999/reports", await call("POST", "/api/locations/999999999/reports", { body: { period_start: "2026-01-01", period_end: "2026-01-31" } }), 404, "not_found");

  check("POST /api/locations/discover", "/api/locations/discover", "POST", await call("POST", "/api/locations/discover"));
  check("GET /api/audit-log?action=client.create", "/api/audit-log", "GET", await call("GET", "/api/audit-log?action=client.create"));
  expectStatus("GET /api/audit-log?location_id=x", await call("GET", "/api/audit-log?location_id=x"), 400, "invalid_request");
  if (seededLocation) {
    // The location's own entries (its switch, its report), each naming it (D-103).
    const located = await call("GET", `/api/audit-log?location_id=${seededLocation.id}`);
    check("GET /api/audit-log?location_id={id}", "/api/audit-log", "GET", located);
    const items = located.json?.items ?? [];
    if (items.length === 0 && requireData) fail("GET /api/audit-log?location_id={id}", "no entry names the seeded location");
    if (items.some((entry) => entry.location_id !== seededLocation.id)) fail("GET /api/audit-log?location_id={id}", "an entry of another location");
  }

  // 4. Errors.
  expectStatus("POST /api/clients without the CSRF token", await call("POST", "/api/clients", { body: { name: "x" }, csrf: false }), 403, "csrf_token_invalid");
  expectStatus("GET /api/clients/999999999", await call("GET", "/api/clients/999999999"), 404, "not_found");
  expectStatus("GET /api/reviews?rating=9", await call("GET", "/api/reviews?rating=9"), 400, "invalid_request");
  expectStatus("DELETE /api/clients/999999999", await call("DELETE", "/api/clients/999999999"), 404, "not_found");
  expectStatus("PUT /api/clients/1", await call("PUT", "/api/clients/1", { body: {} }), 405, "method_not_allowed");
  expectStatus("GET unknown path", await call("GET", "/api/this-path-does-not-exist"), 404, "not_found");

  // 5. Request bodies: what the spec does not allow is refused, on every route with a body.
  await checkRefusedBodies();

  // 6. The phone app: a token instead of the cookie, and no CSRF token.
  const challenge = "A".repeat(43); // shaped like the S256 challenge of a PKCE verifier
  const appStart = await call("GET", `/api/auth/google/start?client=ios&code_challenge=${challenge}`, { signedIn: false });
  check("GET /api/auth/google/start (the app)", "/api/auth/google/start", "GET", appStart);
  const offersGoogle = (await call("GET", "/api/session", { signedIn: false })).json?.sign_in?.google;
  const sentTo = appStart.response.headers.get("location") ?? "";
  if (offersGoogle === false && sentTo !== "gbpmanager://auth?error=not_configured") {
    fail("GET /api/auth/google/start (the app)", `without Google sign-in the app is told so at its own address, not at "${sentTo}"`);
  }
  if (appStart.response.headers.getSetCookie().some((c) => c.includes("poni_session="))) {
    fail("GET /api/auth/google/start (the app)", "the app's sign-in sets no session cookie");
  }
  expectStatus("GET /api/auth/google/start (another client)", await call("GET", `/api/auth/google/start?client=web&code_challenge=${challenge}`, { signedIn: false }), 400, "invalid_request");
  expectStatus("GET /api/auth/google/start (the app, no challenge)", await call("GET", "/api/auth/google/start?client=ios", { signedIn: false }), 400, "invalid_request");
  expectStatus("POST /api/auth/app-token (a code nobody was given)", await call("POST", "/api/auth/app-token", { signedIn: false, body: { code: "not-a-code", code_verifier: "not-a-verifier" } }), 400, "invalid_request");
  expectStatus("POST /api/auth/dev-login (another client)", await call("POST", "/api/auth/dev-login?client=android", { signedIn: false }), 400, "invalid_request");

  const appLogin = await call("POST", "/api/auth/dev-login?client=ios", { signedIn: false });
  check("POST /api/auth/dev-login?client=ios", "/api/auth/dev-login", "POST", appLogin);
  const isAppToken = validatorFor("components", "schemas", "AppToken");
  if (!isAppToken(appLogin.json)) {
    fail("POST /api/auth/dev-login?client=ios", `the answer is not an AppToken: ${ajv.errorsText(isAppToken.errors)}`);
  } else if (appLogin.response.headers.getSetCookie().length > 0) {
    fail("POST /api/auth/dev-login?client=ios", "the app gets a token and no cookie");
  } else {
    const asApp = { signedIn: false, token: appLogin.json.token };
    const appSession = await call("GET", "/api/session", asApp);
    check("GET /api/session (bearer)", "/api/session", "GET", appSession);
    if (appSession.json?.authenticated !== true || appSession.json?.csrf_token !== null) {
      fail("GET /api/session (bearer)", `expected a signed-in session without a CSRF token: ${appSession.text.slice(0, 200)}`);
    }
    check("GET /api/clients (bearer)", "/api/clients", "GET", await call("GET", "/api/clients", asApp));
    // The app's device (D-097): registered where the engine pushes to sandbox, refused where it can't push.
    const registration = { apns_token: "ab".repeat(32), environment: "sandbox", app_version: "contract-check" };
    const device = await call("POST", "/api/devices", { ...asApp, body: registration });
    check("POST /api/devices (bearer)", "/api/devices", "POST", device);
    if (device.status === 201) {
      check("DELETE /api/devices/{id}", "/api/devices/{id}", "DELETE", await call("DELETE", `/api/devices/${device.json.id}`, asApp));
    }
    expectStatus("DELETE /api/devices/{id} (none)", await call("DELETE", "/api/devices/999999999", asApp), 404, "not_found");
    expectStatus("POST /api/devices (the dashboard)", await call("POST", "/api/devices", { body: registration }), 400, "invalid_request");
    // A changing request without a CSRF token reaches the handler, which refuses the body: nothing is written.
    expectStatus("POST /api/clients (bearer, no CSRF token, bad body)", await call("POST", "/api/clients", { ...asApp, body: { name: 5 } }), 400, "invalid_request");
    // A request with a bearer token stands on the token alone: the dashboard's good cookie beside it does not help.
    expectStatus("GET /api/clients (a wrong token beside the cookie)", await call("GET", "/api/clients", { token: "not-a-token" }), 401, "unauthenticated");
    check("POST /api/auth/logout (bearer)", "/api/auth/logout", "POST", await call("POST", "/api/auth/logout", asApp));
    expectStatus("GET /api/clients after the app signed out", await call("GET", "/api/clients", asApp), 401, "unauthenticated");
    check("GET /api/clients (the dashboard is still signed in)", "/api/clients", "GET", await call("GET", "/api/clients"));
  }

  const out = await call("POST", "/api/auth/logout");
  check("POST /api/auth/logout", "/api/auth/logout", "POST", out);
  expectStatus("GET /api/clients after signing out", await call("GET", "/api/clients"), 401, "unauthenticated");
} catch (error) {
  fail("contract check", `${error.cause?.message ?? error.message} (engine at ${baseUrl})`);
}

if (failures > 0) {
  console.error(`\n${failures} contract check(s) failed against ${baseUrl} (${passed} passed)`);
  process.exit(1);
}
console.log(`\nThe engine at ${baseUrl} matches api/openapi.yaml (${passed} checks).`);
