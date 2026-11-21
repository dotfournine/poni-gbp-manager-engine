"""The Google mock (D-087) over HTTP, as the engine and the portal use it. No database is needed."""

from __future__ import annotations

import base64
import json
import threading
import time
import urllib.error
import urllib.request
from collections.abc import Iterator
from email.message import EmailMessage
from pathlib import Path
from urllib.parse import parse_qs, urlencode, urlsplit

import pytest

from poni_tools import cli
from poni_tools.mock_google.server import make_server
from poni_tools.mock_google.state import MockGoogle

from .conftest import REPOSITORY

GBP = "/mybusiness.googleapis.com/v4"
SCOPE = "https://www.googleapis.com/auth/business.manage"


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):  # noqa: ANN002, ANN003
        return None


class Mock:
    def __init__(self, base: str, mock: MockGoogle) -> None:
        self.base = base
        self.mock = mock
        self.token = ""
        self.opener = urllib.request.build_opener(NoRedirect)

    def call(self, method: str, path: str, body=None, *, form=None, headers=None, auth=True):
        data = None
        all_headers = dict(headers or {})
        if form is not None:
            data = urlencode(form).encode()
            all_headers["Content-Type"] = "application/x-www-form-urlencoded"
        elif body is not None:
            data = json.dumps(body).encode()
            all_headers["Content-Type"] = "application/json"
        if auth and self.token:
            all_headers["Authorization"] = f"Bearer {self.token}"
        request = urllib.request.Request(self.base + path, data=data, method=method, headers=all_headers)
        try:
            with self.opener.open(request) as response:
                raw = response.read()
                return response.status, dict(response.headers), (json.loads(raw) if raw.startswith(b"{") else raw)
        except urllib.error.HTTPError as error:
            raw = error.read()
            return error.code, dict(error.headers), (json.loads(raw) if raw.startswith(b"{") else raw)

    def connect(self, scope: str = SCOPE, login_hint: str | None = None) -> dict:
        """The engine's way: the consent page, then the code at the token endpoint."""
        redirect = "http://localhost:5173/api/connections/gbp/callback"
        query = {"client_id": "mock", "redirect_uri": redirect, "response_type": "code", "scope": scope, "state": "s1"}
        if login_hint:
            query["login_hint"] = login_hint
        status, headers, _ = self.call("GET", "/accounts.google.com/o/oauth2/v2/auth?" + urlencode(query), auth=False)
        assert status == 302
        back = urlsplit(headers["Location"])
        assert f"{back.scheme}://{back.netloc}{back.path}" == redirect
        params = {k: v[0] for k, v in parse_qs(back.query).items()}
        assert params["state"] == "s1"
        form = {
            "grant_type": "authorization_code",
            "code": params["code"],
            "client_id": "mock",
            "client_secret": "mock",
            "redirect_uri": redirect,
        }
        status, _, tokens = self.call("POST", "/oauth2.googleapis.com/token", form=form, auth=False)
        assert status == 200, tokens
        self.token = tokens["access_token"]
        return tokens

    def location_ids(self) -> list[str]:
        status, _, body = self.call(
            "GET", f"/mybusinessbusinessinformation.googleapis.com/v1/accounts/{ACCOUNT}/locations"
        )
        assert status == 200
        return [loc["name"].removeprefix("locations/") for loc in body["locations"]]

    def reviews(self, location_id: str) -> list[dict]:
        status, _, body = self.call("GET", f"{GBP}/accounts/{ACCOUNT}/locations/{location_id}/reviews?pageSize=50")
        assert status == 200
        return body.get("reviews", [])

    def portal(self, path: str, body: dict, *, header: bool = True):
        return self.call("POST", path, body, headers={"X-Mock-Portal": "1"} if header else {}, auth=False)


ACCOUNT = "900000000000000001"


@pytest.fixture
def google(tmp_path: Path) -> Iterator[Mock]:
    mock = MockGoogle(tmp_path / "state.json", REPOSITORY / "knowledge")
    server = make_server(mock, 0)
    thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True)
    thread.start()
    try:
        yield Mock(f"http://127.0.0.1:{server.server_address[1]}", mock)
    finally:
        server.shutdown()
        server.server_close()


def test_it_listens_on_loopback_only(google: Mock) -> None:
    assert urlsplit(google.base).hostname == "127.0.0.1"


def test_the_businesses_are_the_knowledge_bases_test_clients(google: Mock) -> None:
    google.connect()
    status, _, body = google.call("GET", "/mybusinessaccountmanagement.googleapis.com/v1/accounts")
    assert status == 200
    assert body["accounts"][0]["name"] == f"accounts/{ACCOUNT}"
    names = {
        json.loads(p.read_text(encoding="utf-8"))["name"]
        for p in (REPOSITORY / "knowledge/industries").glob("*/test_client.json")
    }
    _, _, locations = google.call(
        "GET", f"/mybusinessbusinessinformation.googleapis.com/v1/accounts/{ACCOUNT}/locations"
    )
    titles = [loc["title"] for loc in locations["locations"]]
    assert {title.split(" - ")[0] for title in titles} == names
    for loc in locations["locations"]:
        assert loc["websiteUri"].endswith(".example")
        assert google.reviews(loc["name"].removeprefix("locations/")), "every location starts with reviews"


def test_connecting_gives_a_refresh_token_that_works_and_unknown_ones_are_refused(google: Mock) -> None:
    tokens = google.connect()
    assert tokens["refresh_token"].startswith("mock-refresh-")
    assert "id_token" not in tokens, "only an openid scope gets an ID token"
    form = {
        "grant_type": "refresh_token",
        "refresh_token": tokens["refresh_token"],
        "client_id": "c",
        "client_secret": "s",
    }
    status, _, refreshed = google.call("POST", "/oauth2.googleapis.com/token", form=form, auth=False)
    assert status == 200 and refreshed["access_token"].startswith("mock-access-")
    status, _, refused = google.call(
        "POST", "/oauth2.googleapis.com/token", form={**form, "refresh_token": "made-up"}, auth=False
    )
    assert status == 400 and refused["error"] == "invalid_grant"


def test_a_code_is_used_once(google: Mock) -> None:
    redirect = "http://localhost:5173/api/connections/gbp/callback"
    query = urlencode({"client_id": "c", "redirect_uri": redirect, "response_type": "code", "scope": SCOPE})
    _, headers, _ = google.call("GET", "/accounts.google.com/o/oauth2/v2/auth?" + query, auth=False)
    code = parse_qs(urlsplit(headers["Location"]).query)["code"][0]
    form = {"grant_type": "authorization_code", "code": code, "redirect_uri": redirect}
    assert google.call("POST", "/oauth2.googleapis.com/token", form=form, auth=False)[0] == 200
    assert google.call("POST", "/oauth2.googleapis.com/token", form=form, auth=False)[0] == 400


def test_the_consent_page_sends_the_browser_back_to_this_machine_only(google: Mock) -> None:
    for redirect in ("https://evil.example/callback", "http://localhost.evil.example/x", "javascript:alert(1)"):
        query = urlencode({"client_id": "c", "redirect_uri": redirect, "response_type": "code", "scope": SCOPE})
        status, headers, _ = google.call("GET", "/accounts.google.com/o/oauth2/v2/auth?" + query, auth=False)
        assert status == 400 and "Location" not in headers, redirect


def test_the_mailbox_gets_an_id_token_with_the_address_it_signed_in_as(google: Mock) -> None:
    tokens = google.connect(
        scope="https://www.googleapis.com/auth/gmail.send openid email", login_hint="host@x.example"
    )
    payload = tokens["id_token"].split(".")[1]
    claims = json.loads(base64.urlsafe_b64decode(payload + "=" * (-len(payload) % 4)))
    assert claims["email"] == "host@x.example"
    assert "gmail.send" in tokens["scope"]


def test_the_apis_want_a_token(google: Mock) -> None:
    assert google.call("GET", "/mybusinessaccountmanagement.googleapis.com/v1/accounts")[0] == 401
    google.token = "someone-elses"
    assert google.call("GET", "/mybusinessaccountmanagement.googleapis.com/v1/accounts")[0] == 401


def test_a_review_posted_in_the_portal_is_listed_first_and_read_by_its_name(google: Mock) -> None:
    google.connect()
    location = google.location_ids()[0]
    status, _, posted = google.portal(
        "/portal/reviews", {"location_id": location, "reviewer": "Pat Q.", "stars": 2, "comment": "Cold food."}
    )
    assert status == 200
    newest = google.reviews(location)[0]
    assert newest["reviewId"] == posted["id"]
    assert newest["starRating"] == "TWO" and newest["comment"] == "Cold food."
    assert newest["name"] == f"accounts/{ACCOUNT}/locations/{location}/reviews/{posted['id']}"
    assert "reviewReply" not in newest

    status, _, one = google.call("GET", f"{GBP}/{newest['name']}")
    assert status == 200 and one == newest

    # A rating-only review has no comment at all, as at Google.
    _, _, quiet = google.portal(
        "/portal/reviews", {"location_id": location, "reviewer": "Lee", "stars": 5, "comment": ""}
    )
    assert "comment" not in google.reviews(location)[0]
    assert google.reviews(location)[0]["reviewId"] == quiet["id"]


def test_the_portal_refuses_requests_without_its_header(google: Mock) -> None:
    location = google.mock.state["locations"][0]["id"]
    status, _, _ = google.portal(
        "/portal/reviews", {"location_id": location, "reviewer": "X", "stars": 5}, header=False
    )
    assert status == 403
    status, _, error = google.portal("/portal/reviews", {"location_id": location, "reviewer": "X", "stars": 9})
    assert status == 400 and "Stars" in error["error"]


def test_a_reply_put_shows_on_the_review_after_the_delay(google: Mock) -> None:
    google.connect()
    location = google.location_ids()[0]
    _, _, posted = google.portal(
        "/portal/reviews", {"location_id": location, "reviewer": "A", "stars": 5, "comment": "Hi"}
    )
    name = f"accounts/{ACCOUNT}/locations/{location}/reviews/{posted['id']}"

    status, _, accepted = google.call("PUT", f"{GBP}/{name}/reply", {"comment": "Thank you!"})
    assert status == 200 and accepted["comment"] == "Thank you!"
    assert google.call("GET", f"{GBP}/{name}")[2]["reviewReply"]["comment"] == "Thank you!"
    portal = google.call("GET", "/portal/state", auth=False)[2]
    shown = next(r for loc in portal["locations"] for r in loc["reviews"] if r["id"] == posted["id"])
    assert shown["reply"]["by"] == "engine"

    # Google shows a new reply minutes after accepting it.
    assert google.portal("/portal/settings", {"reply_delay_s": 60})[0] == 200
    google.call("PUT", f"{GBP}/{name}/reply", {"comment": "Thanks again!"})
    assert "reviewReply" not in google.call("GET", f"{GBP}/{name}")[2]
    assert all("reviewReply" not in r for r in google.reviews(location) if r["reviewId"] == posted["id"])


def test_an_edited_review_gets_a_new_update_time_and_keeps_its_reply(google: Mock) -> None:
    google.connect()
    location = google.location_ids()[0]
    replied = next(r for r in google.reviews(location) if "reviewReply" in r)
    google.portal(f"/portal/reviews/{replied['reviewId']}/edit", {"stars": 3, "comment": "Changed my mind."})
    after = google.call("GET", f"{GBP}/{replied['name']}")[2]
    assert after["updateTime"] > replied["updateTime"]
    assert after["createTime"] == replied["createTime"]
    assert after["starRating"] == "THREE" and after["comment"] == "Changed my mind."
    assert after["reviewReply"] == replied["reviewReply"]
    assert google.reviews(location)[0]["reviewId"] == replied["reviewId"], "most recently updated first"


def test_a_removed_review_is_not_found_and_leaves_every_list(google: Mock) -> None:
    google.connect()
    location = google.location_ids()[0]
    gone = google.reviews(location)[0]
    assert google.portal(f"/portal/reviews/{gone['reviewId']}/remove", {})[0] == 200
    status, _, body = google.call("GET", f"{GBP}/{gone['name']}")
    assert status == 404 and body["error"]["status"] == "NOT_FOUND"
    assert gone["reviewId"] not in {r["reviewId"] for r in google.reviews(location)}
    assert google.call("PUT", f"{GBP}/{gone['name']}/reply", {"comment": "Hi"})[0] == 404


def test_batch_get_reviews_covers_several_locations_and_pages(google: Mock) -> None:
    google.connect()
    ids = google.location_ids()
    body = {"locationNames": [f"accounts/{ACCOUNT}/locations/{i}" for i in ids], "pageSize": 2}
    seen, token = [], None
    while True:
        status, _, page = google.call(
            "POST",
            f"{GBP}/accounts/{ACCOUNT}/locations:batchGetReviews",
            {**body, **({"pageToken": token} if token else {})},
        )
        assert status == 200
        seen += [entry["review"]["reviewId"] for entry in page.get("locationReviews", [])]
        token = page.get("nextPageToken")
        if not token:
            break
    expected = sum(len(google.reviews(i)) for i in ids)
    assert len(seen) == len(set(seen)) == expected
    # The engine's client may send the colon encoded.
    assert google.call("POST", f"{GBP}/accounts/{ACCOUNT}/locations%3AbatchGetReviews", body)[0] == 200


def test_the_performance_api_gives_the_same_plausible_numbers_every_time(google: Mock) -> None:
    google.connect()
    location = google.location_ids()[0]
    query = urlencode(
        [
            ("dailyMetrics", "BUSINESS_IMPRESSIONS_MOBILE_MAPS"),
            ("dailyMetrics", "CALL_CLICKS"),
            ("dailyRange.start_date.year", "2026"),
            ("dailyRange.start_date.month", "10"),
            ("dailyRange.start_date.day", "1"),
            ("dailyRange.end_date.year", "2026"),
            ("dailyRange.end_date.month", "10"),
            ("dailyRange.end_date.day", "31"),
        ]
    )
    path = (
        f"/businessprofileperformance.googleapis.com/v1/locations/{location}:fetchMultiDailyMetricsTimeSeries?{query}"
    )
    status, _, body = google.call("GET", path)
    assert status == 200
    series = body["multiDailyMetricTimeSeries"][0]["dailyMetricTimeSeries"]
    assert [s["dailyMetric"] for s in series] == ["BUSINESS_IMPRESSIONS_MOBILE_MAPS", "CALL_CLICKS"]
    maps = series[0]["timeSeries"]["datedValues"]
    assert len(maps) == 31 and maps[0]["date"] == {"year": 2026, "month": 10, "day": 1}
    assert all(100 < int(v["value"]) < 600 for v in maps)
    assert google.call("GET", path)[2] == body

    keywords = google.call(
        "GET",
        f"/businessprofileperformance.googleapis.com/v1/locations/{location}/searchkeywords/impressions/monthly?"
        "monthlyRange.start_month.year=2026&monthlyRange.start_month.month=10",
    )[2]["searchKeywordsCounts"]
    assert any("threshold" in k["insightsValue"] for k in keywords)
    assert all(k["searchKeyword"] for k in keywords)


def test_gmail_send_lands_in_the_outbox_and_goes_nowhere(google: Mock) -> None:
    google.connect()
    message = EmailMessage()
    message["From"] = "PONI <host@x.example>"
    message["To"] = "owner@x.example"
    message["Subject"] = "Escalation: Golden Harbor Buffet"
    message.set_content("A review needs a person.")
    raw = base64.urlsafe_b64encode(message.as_bytes()).decode().rstrip("=")
    status, _, sent = google.call("POST", "/gmail.googleapis.com/gmail/v1/users/me/messages/send", {"raw": raw})
    assert status == 200 and sent["id"]
    outbox = google.call("GET", "/portal/state", auth=False)[2]["outbox"]
    assert outbox[0]["subject"] == "Escalation: Golden Harbor Buffet"
    assert outbox[0]["to"] == "owner@x.example"
    assert outbox[0]["body"].strip() == "A review needs a person."


def test_there_is_no_delete_and_no_other_put(google: Mock) -> None:
    google.connect()
    location = google.location_ids()[0]
    review = google.reviews(location)[0]
    for method in ("DELETE", "PATCH"):
        for path in (f"{GBP}/{review['name']}", f"{GBP}/{review['name']}/reply"):
            assert google.call(method, path)[0] == 501, (method, path)
    assert google.call("PUT", f"{GBP}/{review['name']}", {"comment": "x"})[0] == 404
    assert google.call("PUT", f"/mybusinessbusinessinformation.googleapis.com/v1/locations/{location}", {})[0] == 404
    assert google.call("GET", f"{GBP}/{review['name']}")[0] == 200, "nothing was removed"


def test_the_data_survives_a_restart_and_reset_starts_again(tmp_path: Path) -> None:
    path = tmp_path / "state.json"
    first = MockGoogle(path, REPOSITORY / "knowledge")
    location = first.state["locations"][0]["id"]
    first.post_review({"location_id": location, "reviewer": "Kept", "stars": 4, "comment": "Still here"})
    again = MockGoogle(path, REPOSITORY / "knowledge")
    assert any(r["reviewer"] == "Kept" for r in again.state["reviews"])
    fresh = MockGoogle(path, REPOSITORY / "knowledge", reset=True)
    assert not any(r["reviewer"] == "Kept" for r in fresh.state["reviews"])


def test_the_mock_is_kept_out_of_production_images() -> None:
    ignored = (REPOSITORY / ".dockerignore").read_text(encoding="utf-8").splitlines()
    assert "tools/src/poni_tools/mock_google" in ignored
    assert "mock" not in (REPOSITORY / "deploy/docker-compose.prod.yml").read_text(encoding="utf-8")


def test_the_command_starts_the_mock(tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys) -> None:
    started = {}

    def serve_once(self) -> None:  # noqa: ANN001
        started["address"] = self.server_address

    monkeypatch.setattr("socketserver.BaseServer.serve_forever", serve_once)
    assert cli.main(["mock-google", "--port", "0", "--state", str(tmp_path / "s.json")]) == 0
    assert started["address"][0] == "127.0.0.1"
    assert "PONI_GOOGLE_MOCK_URL=http://127.0.0.1:" in capsys.readouterr().out
    assert (tmp_path / "s.json").is_file()


def test_a_reply_becomes_visible_once_the_delay_has_passed(tmp_path: Path) -> None:
    mock = MockGoogle(tmp_path / "state.json", REPOSITORY / "knowledge")
    location = mock.state["locations"][0]["id"]
    posted = mock.post_review({"location_id": location, "reviewer": "B", "stars": 5, "comment": "ok"})
    mock.set_reply_delay({"reply_delay_s": 1})
    assert mock.put_reply(ACCOUNT, location, posted["id"], {"comment": "Thanks"}).status == 200
    assert "reviewReply" not in mock.get_review(ACCOUNT, location, posted["id"]).body
    time.sleep(1.1)
    assert mock.get_review(ACCOUNT, location, posted["id"]).body["reviewReply"]["comment"] == "Thanks"
