"""The made-up businesses and reviews a new mock starts with. Nothing here is a real business, person or review."""

from __future__ import annotations

import json
from datetime import UTC, datetime, timedelta
from pathlib import Path

ACCOUNT_ID = "900000000000000001"
ACCOUNT_NAME = "PONI Mock Agency"

# Per industry pack: the locations its test client gets, and the reviews they start with
# (days ago, reviewer, stars, text or None for a rating-only review, a reply by the owner or None).
_LOCATIONS: dict[str, list[dict]] = {
    "food_beverage": [
        {
            "suffix": "",
            "category": "Buffet restaurant",
            "locality": "Fairview",
            "region": "VA",
            "street": "100 Example Plaza",
            "reviews": [
                (
                    40,
                    "Morgan T.",
                    5,
                    "Huge spread and everything was fresh. The sushi and the dim sum were the best part.",
                    "Thank you for the kind words! We're glad you enjoyed the sushi and the dim sum.",
                ),
                (
                    12,
                    "Jamie L.",
                    4,
                    "Good variety for the price. Dessert station was a little empty when we went.",
                    None,
                ),
                (6, "Riley K.", 5, None, None),
                (3, "陈小姐", 5, "海鲜很新鲜，环境干净，服务员很热情。下次还会再来！", None),
                (1, "Casey P.", 2, "The hot food was lukewarm and the trays were not refilled for a long time.", None),
            ],
        }
    ],
    "wellness": [
        {
            "suffix": " - Midtown",
            "category": "Spa",
            "locality": "New York",
            "region": "NY",
            "street": "200 Example Avenue, Floor 3",
            "reviews": [
                (
                    30,
                    "Avery S.",
                    5,
                    "The head spa was so relaxing. Eva was gentle and explained every step.",
                    "Thank you so much! We'll share your kind words with Eva.",
                ),
                (9, "Quinn R.", 5, "Best scalp treatment I have had. The room was calm and smelled lovely.", None),
                (2, "Drew M.", 3, "Treatment was nice but we waited twenty minutes past our booking time.", None),
            ],
        },
        {
            "suffix": " - Flushing",
            "category": "Spa",
            "locality": "Queens",
            "region": "NY",
            "street": "300 Example Street",
            "reviews": [
                (14, "Taylor W.", 4, "Lovely foot massage, a bit pricey but worth it.", None),
                (4, "林女士", 5, "头疗做得很舒服，技师手法很专业，强烈推荐！", None),
                (1, "Jordan B.", 5, None, None),
            ],
        },
    ],
}

# Any other pack gets one location with these.
_GENERIC = {
    "suffix": "",
    "category": "Local business",
    "locality": "Springfield",
    "region": "IL",
    "street": "400 Example Road",
    "reviews": [
        (20, "Sam G.", 5, "Friendly staff and quick service.", None),
        (5, "Alex N.", 4, "Good experience overall.", None),
    ],
}


def rfc3339(moment: datetime) -> str:
    return moment.astimezone(UTC).strftime("%Y-%m-%dT%H:%M:%S.%fZ")


def _test_clients(knowledge_dir: Path) -> list[tuple[str, str]]:
    """(industry key, business name) for every pack that has a made-up test client."""
    out = []
    industries = knowledge_dir / "industries"
    for folder in sorted(p for p in industries.iterdir() if p.is_dir()) if industries.is_dir() else []:
        path = folder / "test_client.json"
        if path.is_file():
            out.append((folder.name, json.loads(path.read_text(encoding="utf-8"))["name"]))
    return out


def initial_state(knowledge_dir: Path, now: datetime) -> dict:
    state: dict = {
        "account": {"id": ACCOUNT_ID, "name": ACCOUNT_NAME},
        "reply_delay_s": 0,
        "next_id": 1,
        "locations": [],
        "reviews": [],
        "outbox": [],
        "codes": {},
        "refresh_tokens": {},
    }
    number = 0
    clients = _test_clients(knowledge_dir) or [("generic", "Example Business")]
    for industry, name in clients:
        for spec in _LOCATIONS.get(industry, [_GENERIC]):
            number += 1
            location_id = f"910000000000000{number:03d}"
            state["locations"].append(
                {
                    "id": location_id,
                    "industry": industry,
                    "title": name + spec["suffix"],
                    "store_code": f"MOCK-{number:03d}",
                    "category": spec["category"],
                    "phone": f"(555) 010-{number:04d}",
                    "website": f"https://{industry.replace('_', '-')}-{number}.example",
                    "address": {
                        "regionCode": "US",
                        "administrativeArea": spec["region"],
                        "locality": spec["locality"],
                        "postalCode": "00000",
                        "addressLines": [spec["street"]],
                    },
                }
            )
            for days, reviewer, stars, text, owner_reply in spec["reviews"]:
                created = now - timedelta(days=days, hours=number)
                review = {
                    "id": f"mock-review-{state['next_id']}",
                    "location_id": location_id,
                    "reviewer": reviewer,
                    "stars": stars,
                    "comment": text,
                    "create_time": rfc3339(created),
                    "update_time": rfc3339(created),
                    "deleted": False,
                    "reply": None,
                }
                state["next_id"] += 1
                if owner_reply:
                    # A reply the owner wrote on Google before the engine: the engine must never post over it.
                    replied = created + timedelta(hours=8)
                    review["reply"] = {"comment": owner_reply, "update_time": rfc3339(replied), "visible_at": 0}
                state["reviews"].append(review)
    return state
