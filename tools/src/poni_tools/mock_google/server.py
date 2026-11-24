"""The mock's HTTP side: Google's paths, each under the Google host it stands in for, and the portal.

The engine rewrites https://mybusiness.googleapis.com/v4/... to http://127.0.0.1:<port>/mybusiness.googleapis.com/v4/...
(http::google_mock_address), so the first segment says which API a request is for. GET, POST and the one PUT of
a reply are answered; there is no PATCH and no DELETE.
"""

from __future__ import annotations

import json
import re
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from importlib import resources
from urllib.parse import parse_qs, unquote, urlsplit

from .state import Answer, MockGoogle, PortalError, google_error, parse_form

LOOPBACK = "127.0.0.1"
# The portal's changing requests carry this header: a page of another site can't add it without asking first,
# and the mock answers no such question (CORS preflight), so no other site can post to the portal.
PORTAL_HEADER = "X-Mock-Portal"
# The portal is one page with its script and styles inline; it loads nothing else.
PORTAL_POLICY = "default-src 'self'; style-src 'unsafe-inline'; script-src 'unsafe-inline'"

_ID = r"([^/]+)"
# Boost.URL may leave the colon of "locations:batchGetReviews" as it is or encode it.
_COLON = r"(?::|%3[Aa])"
_GET = [
    (re.compile(r"^/accounts\.google\.com/o/oauth2/v2/auth$"), "consent"),
    (re.compile(r"^/mybusinessaccountmanagement\.googleapis\.com/v1/accounts$"), "accounts"),
    (re.compile(rf"^/mybusinessbusinessinformation\.googleapis\.com/v1/accounts/{_ID}/locations$"), "locations"),
    (re.compile(rf"^/mybusiness\.googleapis\.com/v4/accounts/{_ID}/locations/{_ID}/reviews$"), "list_reviews"),
    (re.compile(rf"^/mybusiness\.googleapis\.com/v4/accounts/{_ID}/locations/{_ID}/reviews/{_ID}$"), "get_review"),
    (
        re.compile(
            rf"^/businessprofileperformance\.googleapis\.com/v1/locations/([^/:%]+){_COLON}fetchMultiDailyMetricsTimeSeries$"
        ),
        "daily_metrics",
    ),
    (
        re.compile(
            rf"^/businessprofileperformance\.googleapis\.com/v1/locations/{_ID}/searchkeywords/impressions/monthly$"
        ),
        "search_keywords",
    ),
    (re.compile(r"^/admin\.googleapis\.com/admin/directory/v1/users$"), "list_directory_users"),
    (re.compile(rf"^/admin\.googleapis\.com/admin/directory/v1/users/{_ID}$"), "directory_user"),
]
_POST = [
    (re.compile(r"^/oauth2\.googleapis\.com/token$"), "token"),
    (re.compile(rf"^/mybusiness\.googleapis\.com/v4/accounts/{_ID}/locations{_COLON}batchGetReviews$"), "batch"),
    (re.compile(r"^/gmail\.googleapis\.com/gmail/v1/users/me/messages/send$"), "gmail"),
]
_PUT_REPLY = re.compile(rf"^/mybusiness\.googleapis\.com/v4/accounts/{_ID}/locations/{_ID}/reviews/{_ID}/reply$")
_PORTAL = [
    (re.compile(r"^/portal/reviews$"), "post_review"),
    (re.compile(rf"^/portal/reviews/{_ID}/edit$"), "edit_review"),
    (re.compile(rf"^/portal/reviews/{_ID}/remove$"), "remove_review"),
    (re.compile(r"^/portal/settings$"), "set_reply_delay"),
    (re.compile(r"^/portal/directory$"), "add_directory_user"),
    (re.compile(rf"^/portal/directory/{_ID}/suspend$"), "suspend_directory_user"),
    (re.compile(rf"^/portal/directory/{_ID}/rename$"), "rename_directory_user"),
    (re.compile(rf"^/portal/directory/{_ID}/remove$"), "remove_directory_user"),
    (re.compile(rf"^/portal/directory/{_ID}/restore$"), "restore_directory_user"),
]


def _match(table, path: str):
    for pattern, name in table:
        if found := pattern.match(path):
            return name, [unquote(group) for group in found.groups()]
    return None, []


def make_handler(mock: MockGoogle) -> type[BaseHTTPRequestHandler]:
    class Handler(BaseHTTPRequestHandler):
        server_version = "poni-mock-google"

        def log_message(self, format: str, *args) -> None:  # noqa: A002 - the base class's name
            # The method and path only: a query may hold a code, and the bodies hold tokens.
            print(f"mock-google: {self.command} {urlsplit(self.path).path} -> {args[1] if len(args) > 1 else ''}")

        # ---- plumbing ----

        def _send(self, answer: Answer) -> None:
            body = b""
            content_type = "application/json; charset=utf-8"
            if isinstance(answer.body, str):
                body, content_type = answer.body.encode("utf-8"), "text/html; charset=utf-8"
            elif answer.body is not None:
                body = json.dumps(answer.body, ensure_ascii=False).encode("utf-8")
            self.send_response(answer.status)
            for name, value in answer.headers.items():
                self.send_header(name, value)
            if body:
                self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def _body(self) -> bytes:
            length = int(self.headers.get("Content-Length") or 0)
            return self.rfile.read(length) if length else b""

        def _json_body(self) -> dict | None:
            try:
                value = json.loads(self._body() or b"{}")
            except json.JSONDecodeError:
                return None
            return value if isinstance(value, dict) else None

        def _authorized(self) -> bool:
            # Any token the mock handed out; it checks the shape, not which one.
            return (self.headers.get("Authorization") or "").startswith("Bearer mock-access-")

        def _unauthorized(self) -> None:
            self._send(google_error(401, "UNAUTHENTICATED", "Request had invalid authentication credentials."))

        # ---- methods ----

        def do_GET(self) -> None:  # noqa: N802 - the base class's name
            url = urlsplit(self.path)
            if url.path in ("/", "/portal"):
                page = resources.files(__package__).joinpath("portal.html").read_text(encoding="utf-8")
                return self._send(Answer(200, page, {"Content-Security-Policy": PORTAL_POLICY}))
            if url.path == "/portal/state":
                return self._send(Answer(200, mock.portal_state()))
            name, ids = _match(_GET, url.path)
            if name is None:
                return self._send(google_error(404, "NOT_FOUND", "The mock has no such address."))
            query = parse_qs(url.query, keep_blank_values=True)
            single = {key: values[0] for key, values in query.items()}
            if name == "consent":
                return self._send(mock.consent(single))
            if not self._authorized():
                return self._unauthorized()
            match name:
                case "accounts":
                    answer = mock.accounts()
                case "locations":
                    answer = mock.locations(ids[0])
                case "list_reviews":
                    answer = mock.list_reviews(ids[0], ids[1], single)
                case "get_review":
                    answer = mock.get_review(*ids)
                case "daily_metrics":
                    answer = mock.daily_metrics(ids[0], query)
                case "list_directory_users":
                    answer = mock.list_directory_users(single)
                case "directory_user":
                    answer = mock.directory_user(ids[0])
                case _:
                    answer = mock.search_keywords(ids[0], query)
            self._send(answer)

        def do_POST(self) -> None:  # noqa: N802
            path = urlsplit(self.path).path
            name, ids = _match(_PORTAL, path)
            if name is not None:
                return self._portal(name, ids)
            name, ids = _match(_POST, path)
            if name is None:
                return self._send(google_error(404, "NOT_FOUND", "The mock has no such address."))
            if name == "token":
                return self._send(mock.token(parse_form(self._body())))
            if not self._authorized():
                return self._unauthorized()
            payload = self._json_body()
            if payload is None:
                return self._send(google_error(400, "INVALID_ARGUMENT", "The body is not a JSON object."))
            self._send(mock.batch_get_reviews(ids[0], payload) if name == "batch" else mock.gmail_send(payload))

        def do_PUT(self) -> None:  # noqa: N802
            found = _PUT_REPLY.match(urlsplit(self.path).path)
            if not found:
                return self._send(google_error(404, "NOT_FOUND", "The only PUT is a reply to a review."))
            if not self._authorized():
                return self._unauthorized()
            payload = self._json_body()
            if payload is None:
                return self._send(google_error(400, "INVALID_ARGUMENT", "The body is not a JSON object."))
            self._send(mock.put_reply(*[unquote(group) for group in found.groups()], payload))

        def _portal(self, name: str, ids: list[str]) -> None:
            if self.headers.get(PORTAL_HEADER) != "1":
                return self._send(Answer(403, {"error": "the portal's page sends this request"}))
            form = self._json_body()
            if form is None:
                return self._send(Answer(400, {"error": "the body is not a JSON object"}))
            try:
                # The removals and the restore take no body.
                bodiless = {
                    "remove_review": mock.remove_review,
                    "remove_directory_user": mock.remove_directory_user,
                    "restore_directory_user": mock.restore_directory_user,
                }
                result = bodiless[name](*ids) if name in bodiless else getattr(mock, name)(*ids, form)
            except PortalError as error:
                return self._send(Answer(400, {"error": str(error)}))
            self._send(Answer(200, result))

    return Handler


def make_server(mock: MockGoogle, port: int) -> ThreadingHTTPServer:
    """Listens on loopback only: the mock is for this machine."""
    server = ThreadingHTTPServer((LOOPBACK, port), make_handler(mock))
    server.daemon_threads = True
    return server
