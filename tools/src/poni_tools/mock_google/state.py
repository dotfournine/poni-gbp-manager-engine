"""What the mock knows and what it answers, without HTTP: the server and the tests both call this.

Everything lives in one JSON file outside the repository, written after each change, so the engine and
the portal see the same data after a restart. One lock guards it: the server answers on several threads.
"""

from __future__ import annotations

import base64
import email
import email.policy
import hashlib
import json
import os
import re
import secrets
import threading
import time
from dataclasses import dataclass, field
from datetime import UTC, date, datetime, timedelta
from pathlib import Path
from urllib.parse import parse_qs, urlencode, urlsplit, urlunsplit

from .data import directory_users, initial_state, rfc3339

STARS = {1: "ONE", 2: "TWO", 3: "THREE", 4: "FOUR", 5: "FIVE"}
PAGE_SIZE = 50
OUTBOX_LIMIT = 200
GMAIL_SCOPE = "https://www.googleapis.com/auth/gmail.send"
# The consent page sends the browser back only to the engine or the dashboard on this machine.
LOCAL_HOSTS = {"localhost", "127.0.0.1"}


@dataclass
class Answer:
    status: int
    body: object = None  # dict or list: JSON; str: HTML
    headers: dict[str, str] = field(default_factory=dict)


def google_error(status: int, code: str, message: str) -> Answer:
    """Google's error shape, which the engine's describe_failure reads."""
    return Answer(status, {"error": {"code": status, "message": message, "status": code}})


def directory_error(status: int, reason: str, message: str) -> Answer:
    """The Admin SDK Directory API's error shape, which has no status word.

    From memory of Google's answers, not from its documentation, which describes no 404 for users.get. So the
    engine must tell "gone" from the HTTP status alone, never from a member of this body.
    """
    return Answer(
        status,
        {
            "error": {
                "code": status,
                "message": message,
                "errors": [{"domain": "global", "reason": reason, "message": message}],
            }
        },
    )


def not_found(what: str = "Requested entity was not found.") -> Answer:
    return google_error(404, "NOT_FOUND", what)


class PortalError(Exception):
    """A request of the portal that can't be done; the message is shown to the person."""


def _now() -> datetime:
    return datetime.now(UTC)


class MockGoogle:
    def __init__(self, path: Path, knowledge_dir: Path, *, reset: bool = False, clock=_now) -> None:
        self.path = path
        self.clock = clock
        self.lock = threading.Lock()
        if reset or not path.is_file():
            self.state = initial_state(knowledge_dir, clock())
            self._save()
        else:
            self.state = json.loads(path.read_text(encoding="utf-8"))
            # A state file from before the directory: it gains one, and nothing else of it changes.
            if "directory" not in self.state:
                self.state["directory"] = directory_users()
                self._save()

    # ---- storage ------------------------------------------------------------------------

    def _save(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        scratch = self.path.with_suffix(".tmp")
        scratch.write_text(json.dumps(self.state, ensure_ascii=False, indent=1), encoding="utf-8")
        os.replace(scratch, self.path)

    def _location(self, location_id: str) -> dict | None:
        return next((loc for loc in self.state["locations"] if loc["id"] == location_id), None)

    def _review(self, location_id: str, review_id: str) -> dict | None:
        return next(
            (
                r
                for r in self.state["reviews"]
                if r["id"] == review_id and r["location_id"] == location_id and not r["deleted"]
            ),
            None,
        )

    def _directory_find(self, key: str, value: str) -> dict | None:
        """The directory user whose member `key` is `value`, deleted or not. Takes no lock: the caller holds it."""
        return next((user for user in self.state["directory"] if user[key] == value), None)

    # ---- Google's JSON ------------------------------------------------------------------

    def _location_json(self, loc: dict) -> dict:
        return {
            "name": f"locations/{loc['id']}",
            "title": loc["title"],
            "storeCode": loc["store_code"],
            "languageCode": "en",
            "phoneNumbers": {"primaryPhone": loc["phone"]},
            "categories": {"primaryCategory": {"name": "categories/gcid:mock", "displayName": loc["category"]}},
            "storefrontAddress": loc["address"],
            "websiteUri": loc["website"],
            "metadata": {"placeId": f"mock-place-{loc['id']}", "mapsUri": "https://maps.example/?mock=" + loc["id"]},
        }

    def _review_json(self, review: dict) -> dict:
        out = {
            "name": f"accounts/{self.state['account']['id']}/locations/{review['location_id']}/reviews/{review['id']}",
            "reviewId": review["id"],
            "reviewer": {"displayName": review["reviewer"], "isAnonymous": False},
            "starRating": STARS[review["stars"]],
            "createTime": review["create_time"],
            "updateTime": review["update_time"],
        }
        if review["comment"]:
            out["comment"] = review["comment"]
        reply = review["reply"]
        # Google shows a reply minutes after accepting it; until then the review has none.
        if reply and reply["visible_at"] <= time.time():
            out["reviewReply"] = {"comment": reply["comment"], "updateTime": reply["update_time"]}
        return out

    def _visible_reviews(self, location_ids: set[str]) -> list[dict]:
        reviews = [r for r in self.state["reviews"] if r["location_id"] in location_ids and not r["deleted"]]
        return sorted(reviews, key=lambda r: r["update_time"], reverse=True)

    @staticmethod
    def _page(items: list, token: str | None, size: int) -> tuple[list, str | None]:
        start = int(token) if token and token.isdigit() else 0
        size = max(1, min(size, PAGE_SIZE))
        rest = items[start + size :]
        return items[start : start + size], (str(start + size) if rest else None)

    # ---- OAuth ------------------------------------------------------------------------

    def consent(self, query: dict[str, str]) -> Answer:
        """The consent page: approves at once and sends the browser back with a code."""
        redirect = query.get("redirect_uri", "")
        parts = urlsplit(redirect)
        if parts.scheme not in ("http", "https") or parts.hostname not in LOCAL_HOSTS:
            return Answer(400, "<p>The mock only sends the browser back to this machine (localhost or 127.0.0.1).</p>")
        if query.get("response_type") != "code" or not query.get("client_id"):
            return Answer(400, "<p>Not an authorization request: response_type=code and client_id are needed.</p>")
        code = "mock-code-" + secrets.token_urlsafe(16)
        with self.lock:
            self.state["codes"][code] = {
                "scope": query.get("scope", ""),
                "email": query.get("login_hint") or "mock-owner@example.com",
                "redirect_uri": redirect,
            }
            self._save()
        params = {"code": code, "scope": query.get("scope", "")}
        if "state" in query:
            params["state"] = query["state"]
        back = urlunsplit(parts._replace(query=(parts.query + "&" if parts.query else "") + urlencode(params)))
        return Answer(302, None, {"Location": back})

    def token(self, form: dict[str, str]) -> Answer:
        grant = form.get("grant_type")
        with self.lock:
            if grant == "authorization_code":
                grant_info = self.state["codes"].pop(form.get("code", ""), None)
                if grant_info is None or grant_info["redirect_uri"] != form.get("redirect_uri"):
                    self._save()
                    return Answer(400, {"error": "invalid_grant", "error_description": "Bad Request"})
                refresh = "mock-refresh-" + secrets.token_urlsafe(24)
                self.state["refresh_tokens"][refresh] = {"scope": grant_info["scope"], "email": grant_info["email"]}
                self._save()
                body = self._access(grant_info["scope"])
                body["refresh_token"] = refresh
                if "openid" in grant_info["scope"].split():
                    # Google's sub is the account's directory id; an address the directory does not hold is "mock".
                    user = self._directory_find("primaryEmail", grant_info["email"])
                    sub = user["id"] if user and not user["deleted"] else "mock"
                    body["id_token"] = _id_token(grant_info["email"], sub)
                return Answer(200, body)
            if grant == "refresh_token":
                known = self.state["refresh_tokens"].get(form.get("refresh_token", ""))
                if known is None:
                    return Answer(
                        400, {"error": "invalid_grant", "error_description": "Token has been expired or revoked."}
                    )
                return Answer(200, self._access(known["scope"]))
        return Answer(400, {"error": "unsupported_grant_type"})

    @staticmethod
    def _access(scope: str) -> dict:
        return {
            "access_token": "mock-access-" + secrets.token_urlsafe(16),
            "expires_in": 3599,
            "token_type": "Bearer",
            "scope": scope,
        }

    # ---- Business Profile -------------------------------------------------------------------

    def accounts(self) -> Answer:
        account = self.state["account"]
        return Answer(
            200,
            {
                "accounts": [
                    {"name": f"accounts/{account['id']}", "accountName": account["name"], "type": "LOCATION_GROUP"}
                ]
            },
        )

    def locations(self, account_id: str) -> Answer:
        if account_id != self.state["account"]["id"]:
            return not_found()
        with self.lock:
            return Answer(200, {"locations": [self._location_json(loc) for loc in self.state["locations"]]})

    def list_reviews(self, account_id: str, location_id: str, query: dict[str, str]) -> Answer:
        with self.lock:
            if account_id != self.state["account"]["id"] or not self._location(location_id):
                return not_found()
            all_reviews = self._visible_reviews({location_id})
            page, token = self._page(all_reviews, query.get("pageToken"), int(query.get("pageSize") or PAGE_SIZE))
            body: dict = {"totalReviewCount": len(all_reviews)}
            if all_reviews:
                body["averageRating"] = round(sum(r["stars"] for r in all_reviews) / len(all_reviews), 1)
            if page:
                body["reviews"] = [self._review_json(r) for r in page]
            if token:
                body["nextPageToken"] = token
            return Answer(200, body)

    def batch_get_reviews(self, account_id: str, payload: dict) -> Answer:
        with self.lock:
            if account_id != self.state["account"]["id"]:
                return not_found()
            prefix = f"accounts/{account_id}/locations/"
            wanted = {name[len(prefix) :] for name in payload.get("locationNames") or [] if name.startswith(prefix)}
            if not wanted or any(self._location(loc) is None for loc in wanted):
                return google_error(400, "INVALID_ARGUMENT", "Request contains an invalid argument.")
            reviews = self._visible_reviews(wanted)
            page, token = self._page(reviews, payload.get("pageToken"), int(payload.get("pageSize") or PAGE_SIZE))
            body: dict = {}
            if page:
                body["locationReviews"] = [
                    {"name": f"{prefix}{r['location_id']}", "review": self._review_json(r)} for r in page
                ]
            if token:
                body["nextPageToken"] = token
            return Answer(200, body)

    def get_review(self, account_id: str, location_id: str, review_id: str) -> Answer:
        with self.lock:
            review = self._review(location_id, review_id) if account_id == self.state["account"]["id"] else None
            return Answer(200, self._review_json(review)) if review else not_found()

    def put_reply(self, account_id: str, location_id: str, review_id: str, payload: dict) -> Answer:
        comment = payload.get("comment")
        if not isinstance(comment, str) or not comment.strip():
            return google_error(400, "INVALID_ARGUMENT", "Request contains an invalid argument.")
        with self.lock:
            review = self._review(location_id, review_id) if account_id == self.state["account"]["id"] else None
            if review is None:
                return not_found()
            updated = rfc3339(self.clock())
            review["reply"] = {
                "comment": comment,
                "update_time": updated,
                "visible_at": time.time() + self.state["reply_delay_s"],
                "by": "engine",
            }
            self._save()
            return Answer(200, {"comment": comment, "updateTime": updated})

    # ---- the Admin SDK Directory API ----------------------------------------------------------

    @staticmethod
    def _directory_user_json(user: dict) -> dict:
        """One user as users.get and users.list answer it."""
        return {
            "kind": "admin#directory#user",
            "id": user["id"],
            "primaryEmail": user["primaryEmail"],
            "name": {
                "givenName": user["givenName"],
                "familyName": user["familyName"],
                "fullName": f"{user['givenName']} {user['familyName']}",
            },
            "isAdmin": user["isAdmin"],
            "isDelegatedAdmin": user["isDelegatedAdmin"],
            "suspended": user["suspended"],
            "archived": user["archived"],
            "customerId": "C0mock0001",
            "orgUnitPath": "/",
        }

    def directory_user(self, user_id: str) -> Answer:
        """users.get by id only: Google also takes an address, but the engine never sends one."""
        with self.lock:
            user = self._directory_find("id", user_id)
            if user is None or user["deleted"]:
                return directory_error(404, "notFound", "Resource Not Found: userKey")
            return Answer(200, self._directory_user_json(user))

    def list_directory_users(self, query: dict[str, str]) -> Answer:
        """users.list of the connected account's own customer, the one way the engine asks it.

        Suspended and archived users are listed with their flags, as at Google; deleted ones are not.
        """
        if query.get("customer") != "my_customer":
            return directory_error(400, "invalid", "Invalid Input")
        try:
            size = int(query.get("maxResults") or 100)
        except ValueError:
            return directory_error(400, "invalid", "Invalid Input")
        with self.lock:
            users = [user for user in self.state["directory"] if not user["deleted"]]
            page, token = self._page(users, query.get("pageToken"), size)
            body: dict = {"kind": "admin#directory#users"}
            # Google leaves an empty list out.
            if page:
                body["users"] = [self._directory_user_json(user) for user in page]
            if token:
                body["nextPageToken"] = token
            return Answer(200, body)

    # ---- the Performance API ------------------------------------------------------------------

    def daily_metrics(self, location_id: str, query: dict[str, list[str]]) -> Answer:
        if not self._location(location_id):
            return not_found()
        try:
            start = _query_date(query, "dailyRange.start_date")
            end = _query_date(query, "dailyRange.end_date")
        except (KeyError, ValueError):
            return google_error(400, "INVALID_ARGUMENT", "dailyRange is missing or not a date.")
        series = []
        for metric in query.get("dailyMetrics", []):
            values = []
            day = start
            while day <= end:
                value = _plausible(location_id, metric, day)
                entry: dict = {"date": {"year": day.year, "month": day.month, "day": day.day}}
                if value:  # Google leaves the value out for a zero
                    entry["value"] = str(value)
                values.append(entry)
                day += timedelta(days=1)
            series.append({"dailyMetric": metric, "timeSeries": {"datedValues": values}})
        return Answer(200, {"multiDailyMetricTimeSeries": [{"dailyMetricTimeSeries": series}]} if series else {})

    def search_keywords(self, location_id: str, query: dict[str, list[str]]) -> Answer:
        loc = self._location(location_id)
        if not loc:
            return not_found()
        month = "-".join(
            query.get(k, ["0"])[0] for k in ("monthlyRange.start_month.year", "monthlyRange.start_month.month")
        )
        words = [
            loc["title"].lower(),
            loc["category"].lower() + " near me",
            loc["category"].lower() + " " + loc["address"]["locality"].lower(),
            loc["title"].split(" - ")[0].lower(),
            loc["category"].lower(),
            "best " + loc["category"].lower(),
        ]
        counts = []
        for rank, word in enumerate(words):
            value = max(1, int(_hash_number(location_id, word, month) % 900 / (rank + 1)) + 20)
            # Google gives only "fewer than 15" for rare keywords.
            insight = {"value": str(value)} if rank < len(words) - 1 else {"threshold": "15"}
            counts.append({"searchKeyword": word, "insightsValue": insight})
        return Answer(200, {"searchKeywordsCounts": counts})

    # ---- Gmail ------------------------------------------------------------------------------

    def gmail_send(self, payload: dict) -> Answer:
        raw = payload.get("raw")
        if not isinstance(raw, str):
            return google_error(400, "INVALID_ARGUMENT", "'raw' RFC822 payload message string is required.")
        try:
            message = email.message_from_bytes(
                base64.urlsafe_b64decode(raw + "=" * (-len(raw) % 4)), policy=email.policy.default
            )
            body = message.get_content() if not message.is_multipart() else ""
        except (ValueError, LookupError) as error:
            return google_error(400, "INVALID_ARGUMENT", f"the message could not be read: {error}")
        with self.lock:
            message_id = f"mock-mail-{self.state['next_id']}"
            self.state["next_id"] += 1
            self.state["outbox"].insert(
                0,
                {
                    "id": message_id,
                    "sent_at": rfc3339(self.clock()),
                    "from": str(message.get("From", "")),
                    "to": str(message.get("To", "")),
                    "subject": str(message.get("Subject", "")),
                    "body": body,
                },
            )
            del self.state["outbox"][OUTBOX_LIMIT:]
            self._save()
        return Answer(200, {"id": message_id, "threadId": message_id, "labelIds": ["SENT"]})

    # ---- the portal ---------------------------------------------------------------------------

    def portal_state(self) -> dict:
        with self.lock:
            now = time.time()
            locations = []
            for loc in self.state["locations"]:
                reviews = []
                for r in self._visible_reviews({loc["id"]}):
                    reply = r["reply"]
                    reviews.append(
                        {
                            "id": r["id"],
                            "reviewer": r["reviewer"],
                            "stars": r["stars"],
                            "comment": r["comment"] or "",
                            "create_time": r["create_time"],
                            "update_time": r["update_time"],
                            "edited": r["update_time"] != r["create_time"],
                            "reply": None
                            if not reply
                            else {
                                "comment": reply["comment"],
                                "update_time": reply["update_time"],
                                "by": reply.get("by", "owner"),
                                "visible_in": max(0, round(reply["visible_at"] - now)),
                            },
                        }
                    )
                locations.append(
                    {"id": loc["id"], "title": loc["title"], "industry": loc["industry"], "reviews": reviews}
                )
            return {
                "account": self.state["account"],
                "reply_delay_s": self.state["reply_delay_s"],
                "locations": locations,
                "outbox": self.state["outbox"],
                "directory": [
                    {
                        "id": user["id"],
                        "primary_email": user["primaryEmail"],
                        "full_name": f"{user['givenName']} {user['familyName']}",
                        "suspended": user["suspended"],
                        "deleted": user["deleted"],
                        "administrator": user["isAdmin"] or user["isDelegatedAdmin"],
                    }
                    for user in self.state["directory"]
                ],
            }

    def post_review(self, form: dict) -> dict:
        location_id = str(form.get("location_id", ""))
        reviewer = str(form.get("reviewer", "")).strip()
        stars = _stars(form.get("stars"))
        comment = str(form.get("comment") or "").strip() or None
        if not reviewer:
            raise PortalError("Give the reviewer a name.")
        with self.lock:
            if not self._location(location_id):
                raise PortalError("No such location.")
            now = rfc3339(self.clock())
            review = {
                "id": f"mock-review-{self.state['next_id']}",
                "location_id": location_id,
                "reviewer": reviewer,
                "stars": stars,
                "comment": comment,
                "create_time": now,
                "update_time": now,
                "deleted": False,
                "reply": None,
            }
            self.state["next_id"] += 1
            self.state["reviews"].append(review)
            self._save()
            return {"id": review["id"]}

    def edit_review(self, review_id: str, form: dict) -> dict:
        with self.lock:
            review = next((r for r in self.state["reviews"] if r["id"] == review_id and not r["deleted"]), None)
            if review is None:
                raise PortalError("No such review.")
            review["stars"] = _stars(form.get("stars", review["stars"]))
            review["comment"] = str(form.get("comment") or "").strip() or None
            # Google keeps the reply when the reviewer edits; the new updateTime is what the engine notices.
            review["update_time"] = rfc3339(
                max(self.clock(), _parse(review["update_time"]) + timedelta(microseconds=1))
            )
            self._save()
            return {"id": review_id}

    def remove_review(self, review_id: str) -> dict:
        """The reviewer (or Google) took the review down: it leaves every list, and a read answers NOT_FOUND."""
        with self.lock:
            review = next((r for r in self.state["reviews"] if r["id"] == review_id and not r["deleted"]), None)
            if review is None:
                raise PortalError("No such review.")
            review["deleted"] = True
            self._save()
            return {"id": review_id}

    def add_directory_user(self, form: dict) -> dict:
        """A new hire: a made-up person at example.com, under an id no user of the directory has or had."""
        given, family, address = form.get("given_name"), form.get("family_name"), form.get("address")
        if not all(isinstance(value, str) for value in (given, family, address)):
            raise PortalError("A given name, a family name and an address are text.")
        given, family, address = given.strip(), family.strip(), address.strip().lower()
        if not (1 <= len(given) <= 60 and 1 <= len(family) <= 60):
            raise PortalError("A given name and a family name are 1 to 60 characters each.")
        if not re.fullmatch(r"[a-z0-9._-]{1,40}@example\.com", address):
            raise PortalError(
                "The address is at example.com, with 1 to 40 letters, digits, dots, dashes or underscores before the @."
            )
        with self.lock:
            if self._directory_find("primaryEmail", address) is not None:
                raise PortalError("A user of the directory has or had that address.")
            # Not next_id: the reviews and the Outbox share it, so it could give a made-up user's id again.
            user_id = str(max(int(user["id"]) for user in self.state["directory"]) + 1)
            self.state["directory"].append(
                {
                    "id": user_id,
                    "primaryEmail": address,
                    "givenName": given,
                    "familyName": family,
                    "suspended": False,
                    "archived": False,
                    "deleted": False,
                    "isAdmin": False,
                    "isDelegatedAdmin": False,
                }
            )
            self._save()
            return {"id": user_id}

    def suspend_directory_user(self, user_id: str, form: dict) -> dict:
        suspended = form.get("suspended")
        if not isinstance(suspended, bool):
            raise PortalError("suspended is true or false.")
        with self.lock:
            user = self._directory_find("id", user_id)
            if user is None:
                raise PortalError("No such user.")
            if user["deleted"]:
                raise PortalError("The user is deleted; restore them first.")
            user["suspended"] = suspended
            self._save()
            return {"id": user_id}

    def rename_directory_user(self, user_id: str, form: dict) -> dict:
        given = str(form.get("given_name") or "").strip()
        family = str(form.get("family_name") or "").strip()
        if not (1 <= len(given) <= 60 and 1 <= len(family) <= 60):
            raise PortalError("A given name and a family name are 1 to 60 characters each.")
        with self.lock:
            user = self._directory_find("id", user_id)
            if user is None:
                raise PortalError("No such user.")
            user["givenName"], user["familyName"] = given, family
            self._save()
            return {"id": user_id}

    def remove_directory_user(self, user_id: str) -> dict:
        """An administrator deleted the account: the get answers 404, and it leaves the list."""
        with self.lock:
            user = self._directory_find("id", user_id)
            if user is None:
                raise PortalError("No such user.")
            user["deleted"] = True
            self._save()
            return {"id": user_id}

    def restore_directory_user(self, user_id: str) -> dict:
        """The account was undeleted: it answers again under the same id, as at Google."""
        with self.lock:
            user = self._directory_find("id", user_id)
            if user is None:
                raise PortalError("No such user.")
            user["deleted"] = False
            self._save()
            return {"id": user_id}

    def set_reply_delay(self, form: dict) -> dict:
        try:
            delay = int(form.get("reply_delay_s", 0))
        except (TypeError, ValueError):
            raise PortalError("The delay is a whole number of seconds.") from None
        if not 0 <= delay <= 3600:
            raise PortalError("The delay is between 0 and 3600 seconds.")
        with self.lock:
            self.state["reply_delay_s"] = delay
            self._save()
            return {"reply_delay_s": delay}


def _stars(value) -> int:
    try:
        stars = int(value)
    except (TypeError, ValueError):
        raise PortalError("Stars are 1 to 5.") from None
    if stars not in STARS:
        raise PortalError("Stars are 1 to 5.")
    return stars


def _parse(text: str) -> datetime:
    return datetime.fromisoformat(text.replace("Z", "+00:00"))


def _query_date(query: dict[str, list[str]], prefix: str) -> date:
    return date(int(query[f"{prefix}.year"][0]), int(query[f"{prefix}.month"][0]), int(query[f"{prefix}.day"][0]))


def _hash_number(*parts: str) -> int:
    return int.from_bytes(hashlib.sha256("|".join(parts).encode()).digest()[:8], "big")


# Rough daily sizes per metric, so a report looks like a small business's.
_SCALE = {
    "BUSINESS_IMPRESSIONS_MOBILE_SEARCH": 140,
    "BUSINESS_IMPRESSIONS_DESKTOP_SEARCH": 45,
    "BUSINESS_IMPRESSIONS_MOBILE_MAPS": 260,
    "BUSINESS_IMPRESSIONS_DESKTOP_MAPS": 60,
    "CALL_CLICKS": 6,
    "BUSINESS_DIRECTION_REQUESTS": 14,
    "WEBSITE_CLICKS": 9,
    "BUSINESS_FOOD_MENU_CLICKS": 3,
    "BUSINESS_BOOKINGS": 1,
}


def _plausible(location_id: str, metric: str, day: date) -> int:
    """The same number for the same location, metric and day every time; busier at weekends."""
    scale = _SCALE.get(metric, 5)
    weekend = 1.3 if day.weekday() >= 5 else 1.0
    jitter = (_hash_number(location_id, metric, day.isoformat()) % 1000) / 1000  # 0..1
    return int(scale * weekend * (0.6 + 0.8 * jitter))


def _id_token(email_address: str, sub: str) -> str:
    """An unsigned ID token: the engine reads its claims from what its own token request returned."""

    def part(value: dict) -> str:
        return base64.urlsafe_b64encode(json.dumps(value).encode()).rstrip(b"=").decode()

    claims = {"iss": "https://accounts.google.com", "email": email_address, "email_verified": True, "sub": sub}
    return f"{part({'alg': 'none', 'typ': 'JWT'})}.{part(claims)}.mock"


def parse_form(body: bytes) -> dict[str, str]:
    return {key: values[0] for key, values in parse_qs(body.decode("utf-8"), keep_blank_values=True).items()}
