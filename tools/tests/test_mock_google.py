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


DIRECTORY = "/admin.googleapis.com/admin/directory/v1/users"


def owner_id(google: Mock) -> str:
    return next(u["id"] for u in google.mock.state["directory"] if u["primaryEmail"] == "mock-owner@example.com")


def staff_id(google: Mock, index: int = 1) -> str:
    return google.mock.state["directory"][index]["id"]


def id_token_claims(tokens: dict) -> dict:
    payload = tokens["id_token"].split(".")[1]
    return json.loads(base64.urlsafe_b64decode(payload + "=" * (-len(payload) % 4)))


def test_a_directory_user_is_read_by_id_and_the_owner_is_a_delegated_administrator(google: Mock) -> None:
    google.connect()
    owner = owner_id(google)
    assert len(owner) == 21 and owner.isdigit()
    status, _, user = google.call("GET", f"{DIRECTORY}/{owner}")
    assert status == 200
    assert set(user) == {
        "kind",
        "id",
        "primaryEmail",
        "name",
        "isAdmin",
        "isDelegatedAdmin",
        "suspended",
        "archived",
        "customerId",
        "orgUnitPath",
    }
    assert user["kind"] == "admin#directory#user" and user["id"] == owner
    assert user["primaryEmail"] == "mock-owner@example.com"
    assert set(user["name"]) == {"givenName", "familyName", "fullName"}
    assert user["name"]["fullName"] == f"{user['name']['givenName']} {user['name']['familyName']}"
    assert user["isDelegatedAdmin"] is True and user["isAdmin"] is False
    assert user["suspended"] is False and user["archived"] is False
    assert user["customerId"] == "C0mock0001" and user["orgUnitPath"] == "/"
    assert google.call("GET", f"{DIRECTORY}/mock-owner@example.com")[0] == 404, "the mock knows ids only"


def test_the_directory_is_made_up_and_only_the_owner_is_an_administrator(google: Mock) -> None:
    users = google.mock.state["directory"]
    assert len(users) == 4
    assert all(u["primaryEmail"].endswith("@example.com") for u in users)
    assert all(not u["isAdmin"] for u in users)
    assert [u["isDelegatedAdmin"] for u in users] == [True, False, False, False]


def test_the_directory_wants_a_token(google: Mock) -> None:
    assert google.call("GET", f"{DIRECTORY}/{owner_id(google)}")[0] == 401


def test_an_unknown_user_is_not_found_in_the_directory_apis_own_error_shape(google: Mock) -> None:
    google.connect()
    status, _, body = google.call("GET", f"{DIRECTORY}/999999999999999999999")
    assert status == 404
    assert body["error"]["code"] == 404
    assert body["error"]["errors"][0]["reason"] == "notFound"
    assert "status" not in body["error"]


def test_a_suspended_user_says_so_and_unsuspending_clears_it(google: Mock) -> None:
    google.connect()
    user = staff_id(google)
    assert google.portal(f"/portal/directory/{user}/suspend", {"suspended": True})[0] == 200
    assert google.call("GET", f"{DIRECTORY}/{user}")[2]["suspended"] is True
    assert google.portal(f"/portal/directory/{user}/suspend", {"suspended": False})[0] == 200
    assert google.call("GET", f"{DIRECTORY}/{user}")[2]["suspended"] is False
    status, _, error = google.portal(f"/portal/directory/{user}/suspend", {"suspended": "yes"})
    assert status == 400 and error["error"]


def test_a_deleted_user_is_not_found_and_a_restored_one_answers_again_with_the_same_id(google: Mock) -> None:
    google.connect()
    user = staff_id(google, 2)
    assert google.portal(f"/portal/directory/{user}/remove", {})[0] == 200
    status, _, body = google.call("GET", f"{DIRECTORY}/{user}")
    assert status == 404 and body["error"]["errors"][0]["reason"] == "notFound"
    assert google.portal(f"/portal/directory/{user}/suspend", {"suspended": True})[0] == 400, "deleted: restore first"
    assert google.portal(f"/portal/directory/{user}/restore", {})[0] == 200
    status, _, again = google.call("GET", f"{DIRECTORY}/{user}")
    assert status == 200 and again["id"] == user


def test_a_renamed_user_shows_the_new_name(google: Mock) -> None:
    google.connect()
    user = staff_id(google)
    names = {"given_name": "  Robin ", "family_name": "Fairweather"}
    assert google.portal(f"/portal/directory/{user}/rename", names)[0] == 200
    name = google.call("GET", f"{DIRECTORY}/{user}")[2]["name"]
    assert name == {"givenName": "Robin", "familyName": "Fairweather", "fullName": "Robin Fairweather"}
    for bad in ({"given_name": " ", "family_name": "X"}, {"given_name": "X", "family_name": "y" * 61}):
        status, _, error = google.portal(f"/portal/directory/{user}/rename", bad)
        assert status == 400 and error["error"], bad
    assert google.call("GET", f"{DIRECTORY}/{user}")[2]["name"]["fullName"] == "Robin Fairweather"


def test_a_portal_action_on_an_unknown_user_is_refused_and_needs_the_portals_header(google: Mock) -> None:
    actions = (("suspend", {"suspended": True}), ("rename", {"given_name": "A", "family_name": "B"}), ("remove", {}))
    for action, body in (*actions, ("restore", {})):
        status, _, error = google.portal(f"/portal/directory/999999999999999999999/{action}", body)
        assert status == 400 and error["error"] == "No such user.", action
        assert google.portal(f"/portal/directory/{staff_id(google)}/{action}", body, header=False)[0] == 403, action


def test_the_connections_id_token_names_the_owners_directory_id(google: Mock) -> None:
    tokens = google.connect(scope=f"{SCOPE} openid email")
    assert id_token_claims(tokens)["sub"] == owner_id(google)
    tokens = google.connect(scope=f"{SCOPE} openid email", login_hint="host@x.example")
    assert id_token_claims(tokens)["sub"] == "mock"


def test_a_state_file_without_a_directory_gains_one_and_keeps_its_reviews(tmp_path: Path) -> None:
    path = tmp_path / "state.json"
    first = MockGoogle(path, REPOSITORY / "knowledge")
    location = first.state["locations"][0]["id"]
    first.post_review({"location_id": location, "reviewer": "Kept", "stars": 4, "comment": "Still here"})
    old = json.loads(path.read_text(encoding="utf-8"))
    del old["directory"]
    path.write_text(json.dumps(old), encoding="utf-8")

    again = MockGoogle(path, REPOSITORY / "knowledge")
    assert [u["primaryEmail"] for u in again.state["directory"]][0] == "mock-owner@example.com"
    assert {k: v for k, v in again.state.items() if k != "directory"} == old
    assert "directory" in json.loads(path.read_text(encoding="utf-8")), "and it is saved"

    again.remove_directory_user(again.state["directory"][1]["id"])
    fresh = MockGoogle(path, REPOSITORY / "knowledge", reset=True)
    assert len(fresh.state["directory"]) == 4 and not any(u["deleted"] for u in fresh.state["directory"])


def test_the_portals_state_lists_the_directory_with_the_deleted_user_marked(google: Mock) -> None:
    gone = staff_id(google, 2)
    google.portal(f"/portal/directory/{gone}/remove", {})
    directory = google.call("GET", "/portal/state", auth=False)[2]["directory"]
    assert len(directory) == 4
    assert all(
        set(u) == {"id", "primary_email", "full_name", "suspended", "deleted", "administrator"} for u in directory
    )
    assert [u["deleted"] for u in directory] == [False, False, True, False]
    assert [u["administrator"] for u in directory] == [True, False, False, False]
    assert directory[0]["primary_email"] == "mock-owner@example.com"


def list_directory(google: Mock, query: str = "customer=my_customer&maxResults=500"):
    return google.call("GET", f"{DIRECTORY}?{query}")


def test_the_directory_lists_its_users(google: Mock) -> None:
    google.connect()
    status, _, body = list_directory(google)
    assert status == 200
    assert set(body) == {"kind", "users"}, "no nextPageToken"
    assert body["kind"] == "admin#directory#users"
    assert [u["id"] for u in body["users"]] == [u["id"] for u in google.mock.state["directory"]]
    for user in body["users"]:
        assert user == google.call("GET", f"{DIRECTORY}/{user['id']}")[2]


def test_the_directory_list_is_paged(google: Mock) -> None:
    google.connect()
    status, _, first = list_directory(google, "customer=my_customer&maxResults=2")
    assert status == 200 and len(first["users"]) == 2 and first["nextPageToken"]
    status, _, rest = list_directory(google, f"customer=my_customer&maxResults=2&pageToken={first['nextPageToken']}")
    assert status == 200 and len(rest["users"]) == 2 and "nextPageToken" not in rest
    ids = [u["id"] for u in first["users"] + rest["users"]]
    assert ids == [u["id"] for u in google.mock.state["directory"]]


def test_an_empty_directory_page_has_no_users_member(google: Mock) -> None:
    google.connect()
    for user in list(google.mock.state["directory"]):
        assert google.portal(f"/portal/directory/{user['id']}/remove", {})[0] == 200
    status, _, body = list_directory(google)
    assert status == 200 and body == {"kind": "admin#directory#users"}


def test_the_directory_list_wants_my_customer_and_a_token(google: Mock) -> None:
    assert list_directory(google)[0] == 401
    google.connect()
    for query in ("maxResults=500", "customer=C0mock0001", "customer=", "customer=my_customer&maxResults=many"):
        status, _, body = list_directory(google, query)
        assert status == 400, query
        assert body["error"]["code"] == 400 and body["error"]["errors"][0]["reason"] == "invalid", query
        assert "status" not in body["error"], query


def test_a_deleted_user_leaves_the_list_and_a_suspended_one_stays_flagged(google: Mock) -> None:
    google.connect()
    gone, away = staff_id(google, 2), staff_id(google, 3)
    assert google.portal(f"/portal/directory/{gone}/remove", {})[0] == 200
    assert google.portal(f"/portal/directory/{away}/suspend", {"suspended": True})[0] == 200
    users = {u["id"]: u for u in list_directory(google)[2]["users"]}
    assert gone not in users and len(users) == 3
    assert users[away]["suspended"] is True


def new_hire(**changes: object) -> dict:
    return {"given_name": "Robin", "family_name": "Newhire", "address": "robin.newhire@example.com", **changes}


def test_a_person_added_in_the_portal_is_listed_and_read_by_id(google: Mock) -> None:
    google.connect()
    before = {u["id"] for u in google.mock.state["directory"]}
    status, _, added = google.portal("/portal/directory", new_hire(address="  Robin.Newhire@Example.com "))
    assert status == 200
    new_id = added["id"]
    assert len(new_id) == 21 and new_id.isdigit() and new_id not in before
    listed = list_directory(google)[2]["users"]
    assert listed[-1]["id"] == new_id and len(listed) == 5
    status, _, user = google.call("GET", f"{DIRECTORY}/{new_id}")
    assert status == 200 and user == listed[-1]
    assert user["primaryEmail"] == "robin.newhire@example.com"
    assert user["name"] == {"givenName": "Robin", "familyName": "Newhire", "fullName": "Robin Newhire"}
    assert not any(user[flag] for flag in ("isAdmin", "isDelegatedAdmin", "suspended", "archived"))
    portal = google.call("GET", "/portal/state", auth=False)[2]["directory"]
    assert portal[-1] == {
        "id": new_id,
        "primary_email": "robin.newhire@example.com",
        "full_name": "Robin Newhire",
        "suspended": False,
        "deleted": False,
        "administrator": False,
    }


def test_an_added_persons_id_is_new_even_after_the_last_user_was_deleted(google: Mock) -> None:
    last = google.mock.state["directory"][-1]["id"]
    assert google.portal(f"/portal/directory/{last}/remove", {})[0] == 200
    new_id = google.portal("/portal/directory", new_hire())[2]["id"]
    assert int(new_id) == int(last) + 1


def test_adding_a_person_is_refused_with_a_reason(google: Mock) -> None:
    deleted = google.mock.state["directory"][3]
    assert google.portal(f"/portal/directory/{deleted['id']}/remove", {})[0] == 200
    refused = [
        new_hire(address="avery.stone@example.com"),
        new_hire(address=deleted["primaryEmail"]),
        new_hire(address="robin@other.example"),
        new_hire(address="robin newhire@example.com"),
        new_hire(address="@example.com"),
        new_hire(address="r" * 41 + "@example.com"),
        new_hire(given_name="  "),
        new_hire(family_name="y" * 61),
        new_hire(given_name=7),
        new_hire(address=None),
        {"given_name": "Robin", "family_name": "Newhire"},
    ]
    for body in refused:
        status, _, error = google.portal("/portal/directory", body)
        assert status == 400 and isinstance(error["error"], str) and error["error"], body
    assert len(google.mock.state["directory"]) == 4, "nothing was added"
    assert google.portal("/portal/directory", new_hire())[0] == 200
    status, _, error = google.portal("/portal/directory", new_hire())
    assert status == 400 and error["error"] == "A user of the directory has or had that address."
    assert google.portal("/portal/directory", new_hire(address="x@example.com"), header=False)[0] == 403


def test_an_added_person_survives_a_restart(tmp_path: Path) -> None:
    path = tmp_path / "state.json"
    first = MockGoogle(path, REPOSITORY / "knowledge")
    new_id = first.add_directory_user(new_hire())["id"]
    again = MockGoogle(path, REPOSITORY / "knowledge")
    assert again.directory_user(new_id).body["primaryEmail"] == "robin.newhire@example.com"


PLACES = "/places.googleapis.com/v1/places"
PLACES_HEADERS = {"X-Goog-Api-Key": "test-places-key", "X-Goog-FieldMask": "timeZone"}


def place_id(google: Mock, title_start: str) -> str:
    """The placeId the locations list gives the location whose title starts so."""
    google.connect()
    _, _, body = google.call("GET", f"/mybusinessbusinessinformation.googleapis.com/v1/accounts/{ACCOUNT}/locations")
    return next(loc["metadata"]["placeId"] for loc in body["locations"] if loc["title"].startswith(title_start))


def test_a_place_answers_its_time_zone_with_a_key_and_the_mask(google: Mock) -> None:
    place = place_id(google, google.mock.state["locations"][0]["title"])
    status, _, body = google.call("GET", f"{PLACES}/{place}", headers=PLACES_HEADERS, auth=False)
    assert status == 200
    assert body == {"timeZone": {"id": google.mock.state["locations"][0]["time_zone"], "version": "2025b"}}


def test_a_place_wants_a_key_not_a_token(google: Mock) -> None:
    place = place_id(google, google.mock.state["locations"][0]["title"])
    for headers in ({"X-Goog-FieldMask": "timeZone"}, {"X-Goog-Api-Key": "", "X-Goog-FieldMask": "timeZone"}):
        status, _, body = google.call("GET", f"{PLACES}/{place}", headers=headers)
        assert status == 403 and body["error"]["status"] == "PERMISSION_DENIED", headers


def test_a_place_wants_the_time_zone_mask_and_no_other(google: Mock) -> None:
    place = place_id(google, google.mock.state["locations"][0]["title"])
    for mask in (None, "", "timeZone,displayName", "*", "displayName"):
        headers = {"X-Goog-Api-Key": "test-places-key"} | ({"X-Goog-FieldMask": mask} if mask is not None else {})
        status, _, body = google.call("GET", f"{PLACES}/{place}", headers=headers, auth=False)
        assert status == 400 and body["error"]["status"] == "INVALID_ARGUMENT", mask


def test_an_unknown_place_is_not_found_and_no_answer_shows_the_key(google: Mock) -> None:
    status, _, body = google.call("GET", f"{PLACES}/mock-place-000", headers=PLACES_HEADERS, auth=False)
    assert status == 404 and body["error"]["status"] == "NOT_FOUND"
    place = place_id(google, google.mock.state["locations"][0]["title"])
    for headers in (PLACES_HEADERS, {"X-Goog-Api-Key": "test-places-key"}, {}):
        _, answer_headers, answer = google.call("GET", f"{PLACES}/{place}", headers=headers, auth=False)
        assert "test-places-key" not in json.dumps(answer) + json.dumps(answer_headers)
    _, _, answer = google.call("GET", f"{PLACES}/mock-place-000", headers=PLACES_HEADERS, auth=False)
    assert "test-places-key" not in json.dumps(answer)


def test_the_log_shows_the_places_path_and_no_header(google: Mock, capsys) -> None:
    place = place_id(google, google.mock.state["locations"][0]["title"])
    google.call("GET", f"{PLACES}/{place}", headers=PLACES_HEADERS, auth=False)
    printed = capsys.readouterr().out
    assert f"GET {PLACES}/{place} -> 200" in printed
    assert "test-places-key" not in printed and "timeZone" not in printed


def test_the_made_up_businesses_are_in_their_own_time_zones(google: Mock) -> None:
    zones = {(loc["address"]["locality"], loc["time_zone"]) for loc in google.mock.state["locations"]}
    expected = {
        "Fairview": "America/Los_Angeles",
        "New York": "America/New_York",
        "Queens": "America/New_York",
        "Springfield": "America/Chicago",
    }
    assert zones <= set(expected.items())
    for loc in google.mock.state["locations"]:
        status, _, body = google.call("GET", f"{PLACES}/mock-place-{loc['id']}", headers=PLACES_HEADERS, auth=False)
        assert status == 200 and body["timeZone"]["id"] == expected[loc["address"]["locality"]]


def test_the_made_up_businesses_are_not_all_in_one_zone(google: Mock) -> None:
    # A hand run must be able to tell a zone read from Places from the engine's own fallback.
    assert len({loc["time_zone"] for loc in google.mock.state["locations"]}) >= 2


def test_a_state_file_without_time_zones_gains_them_and_keeps_its_reviews(tmp_path: Path) -> None:
    path = tmp_path / "state.json"
    first = MockGoogle(path, REPOSITORY / "knowledge")
    location = first.state["locations"][0]["id"]
    first.post_review({"location_id": location, "reviewer": "Kept", "stars": 4, "comment": "Still here"})
    zones = [loc["time_zone"] for loc in first.state["locations"]]
    old = json.loads(path.read_text(encoding="utf-8"))
    for loc in old["locations"]:
        del loc["time_zone"]
    path.write_text(json.dumps(old), encoding="utf-8")

    again = MockGoogle(path, REPOSITORY / "knowledge")
    assert [loc["time_zone"] for loc in again.state["locations"]] == zones
    assert {k: v for k, v in again.state.items() if k != "locations"} == {
        k: v for k, v in old.items() if k != "locations"
    }
    assert [{k: v for k, v in loc.items() if k != "time_zone"} for loc in again.state["locations"]] == old["locations"]
    assert any(r["reviewer"] == "Kept" for r in again.state["reviews"])
    saved = json.loads(path.read_text(encoding="utf-8"))
    assert all("time_zone" in loc for loc in saved["locations"]), "and it is saved"
