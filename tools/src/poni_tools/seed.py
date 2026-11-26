"""Seeds the knowledge base into the kb_* tables (D-078).

The tables are static: nothing but this writes them, and it replaces all of
them in one transaction, so the engine sees the old knowledge base or the
new one and never a mix. Seeding the version that is already there does
nothing. The files are checked first; a knowledge base with a problem is
never seeded.

The engine keeps nothing of the knowledge base in memory and watches for
nothing (D-084): the next read finds the new version. A version that changes
what the analyzer is told about an industry changes the fingerprint of that
industry's locations, so none of them auto replies until a new go-live test
passes. The seed is where that is said: it returns those locations, and the
command prints them for the person who seeds.
"""

from __future__ import annotations

import datetime
import hashlib
from dataclasses import dataclass, field

import psycopg
from psycopg.types.json import Jsonb

from .check import check
from .knowledge import Knowledge, canonical

# Every table of the knowledge base, children before parents.
TABLES = (
    "kb_corpus_cases",
    "kb_industries",
    "kb_common",
    "kb_voice_templates",
    "kb_defaults",
    "kb_content_defaults",
    "kb_reply_defaults",
    "kb_report_defaults",
    "kb_model_prices",
    "kb_meta",
)

# One seed at a time ("ponigbp" + 2; migrations hold + 1).
ADVISORY_LOCK_KEY = 0x706F6D69_67627002


class SeedError(Exception):
    pass


@dataclass
class Location:
    title: str
    industry: str


@dataclass
class Seeded:
    version: str
    # The version that was there before; None on a database never seeded.
    previous: str | None
    changed: bool
    # Enabled locations whose industry's profile, as the analyzer is told it, changed: each needs a new go-live test.
    retest: list[Location] = field(default_factory=list)
    # Enabled locations whose industry is no longer there: not analyzed until they are given another.
    stranded: list[Location] = field(default_factory=list)


def told(label: str, topics: list, item_types: list, flags: list, common: list) -> str:
    """What the analyzer is told about an industry, as one comparable value: its label, its topics' ids, its item
    types' ids and descriptions, and the enabled severity flags, its own and the common ones. A topic's label is
    shown to people only, and whether an item type is a person is for the report only.
    If in doubt this says "changed": the Go-live test page is what decides, this only tells whom to look at."""
    return canonical(
        {
            "label": label,
            "topics": [t["id"] for t in topics],
            "item_types": [{"id": t["id"], "description": t["description"]} for t in item_types],
            "flags": [f for f in [*flags, *common] if f.get("enabled", True)],
        }
    )


def _locations(conn: psycopg.Connection, industries: list[str]) -> list[Location]:
    if not industries:
        return []
    rows = conn.execute(
        "SELECT title, industry_key FROM locations WHERE enabled AND industry_key = ANY(%s) ORDER BY lower(title), id",
        (industries,),
    ).fetchall()
    return [Location(title, industry) for title, industry in rows]


def corpus_version(corpus: dict) -> str:
    """Which corpus a go-live test ran against: a hash of the industry's cases."""
    return hashlib.sha256(canonical(corpus["cases"]).encode()).hexdigest()[:12]


def _insert_templates_and_defaults(conn: psycopg.Connection, kb: Knowledge) -> None:
    """The voice templates and the defaults, each bundle a row of its own (D-107)."""
    for position, (key, template) in enumerate(sorted(kb.voices.items())):
        conn.execute(
            "INSERT INTO kb_voice_templates (key, position, label, description, content, reply) "
            "VALUES (%s, %s, %s, %s, %s, %s)",
            (
                key,
                position,
                template["label"],
                template["description"],
                Jsonb(template["content"]),
                Jsonb(template["reply"]),
            ),
        )
    defaults = kb.defaults
    conn.execute("INSERT INTO kb_defaults (analysis) VALUES (%s)", (Jsonb(defaults["analysis"]),))
    # One typed column per member, as a location's own rows have them (D-107); a list is a text[].
    for table, bundle in (
        ("kb_content_defaults", defaults["content"]),
        ("kb_reply_defaults", defaults["reply"]),
        ("kb_report_defaults", defaults["report"]),
    ):
        columns = list(bundle)
        values = [Jsonb(bundle[c]) if c == "length" else bundle[c] for c in columns]
        conn.execute(
            f"INSERT INTO {table} ({', '.join(columns)}) VALUES ({', '.join(['%s'] * len(columns))})",
            values,
        )


def seed(conn: psycopg.Connection, kb: Knowledge, now: datetime.datetime | None = None) -> Seeded:
    problems = check(kb)
    if problems:
        raise SeedError(
            "\n".join(problems) + f"\n\n{len(problems)} problem(s) in the knowledge base. Nothing was seeded."
        )
    now = now or datetime.datetime.now(datetime.UTC)
    version = kb.version()

    with conn.transaction():
        if conn.execute("SELECT to_regclass('kb_meta')").fetchone()[0] is None:
            raise SeedError("the database has no knowledge base tables yet: run `poni-tools migrate` first")
        conn.execute("SELECT pg_advisory_xact_lock(%s)", (ADVISORY_LOCK_KEY,))
        row = conn.execute("SELECT version, format, content_hash FROM kb_meta").fetchone()
        previous = row[0] if row else None
        content_hash = kb.content_hash()
        if row and row[0] == version and row[1] == kb.manifest["format"]:
            if row[2] != content_hash:
                raise SeedError(
                    f"the knowledge base changed but its version is still {version}, which is seeded with other "
                    'content: set a new "version" in manifest.json (today\'s date, YYYYMMDD; a second one on the '
                    "same day ends in -2). Nothing was seeded."
                )
            return Seeded(version, previous, changed=False)

        # What the analyzer was told about each industry until now, to say whose go-live test no longer counts.
        before: dict[str, str] = {}
        if row:
            was_common = conn.execute("SELECT severity_flags FROM kb_common").fetchone()
            for key, label, topics, item_types, flags in conn.execute(
                "SELECT key, label, topics, item_types, severity_flags FROM kb_industries"
            ):
                before[key] = told(label, topics, item_types, flags, was_common[0] if was_common else [])
        now_told = {
            key: told(
                pack.profile["label"],
                pack.profile["topics"],
                pack.profile["item_types"],
                pack.profile["severity_flags"],
                kb.common["severity_flags"],
            )
            for key, pack in kb.industries.items()
        }
        retest = _locations(conn, sorted(key for key in before if key in now_told and before[key] != now_told[key]))
        stranded = _locations(conn, sorted(key for key in before if key not in now_told))

        for table in TABLES:
            conn.execute(f"DELETE FROM {table}")  # the names are the constants above

        conn.execute("INSERT INTO kb_common (severity_flags) VALUES (%s)", (Jsonb(kb.common["severity_flags"]),))
        for position, (key, pack) in enumerate(sorted(kb.industries.items())):
            conn.execute(
                "INSERT INTO kb_industries (key, position, label, topics, item_types, severity_flags, test_client, "
                "corpus_version) VALUES (%s, %s, %s, %s, %s, %s, %s, %s)",
                (
                    key,
                    position,
                    pack.profile["label"],
                    Jsonb(pack.profile["topics"]),
                    Jsonb(pack.profile["item_types"]),
                    Jsonb(pack.profile["severity_flags"]),
                    Jsonb(pack.test_client),
                    corpus_version(pack.corpus),
                ),
            )
            for case_position, case in enumerate(pack.corpus["cases"]):
                conn.execute(
                    "INSERT INTO kb_corpus_cases (industry_key, case_id, position, category, stars, lang, text, "
                    "expect) VALUES (%s, %s, %s, %s, %s, %s, %s, %s)",
                    (
                        key,
                        case["id"],
                        case_position,
                        case["category"],
                        case["stars"],
                        case["lang"],
                        case["text"],
                        Jsonb(case["expect"]),
                    ),
                )
        _insert_templates_and_defaults(conn, kb)
        for model, price in sorted(kb.models["prices"].items()):
            conn.execute(
                "INSERT INTO kb_model_prices (model, input_per_mtok, output_per_mtok) VALUES (%s, %s, %s)",
                (model, price["input_per_mtok"], price["output_per_mtok"]),
            )
        conn.execute(
            "INSERT INTO kb_meta (version, content_hash, format, seeded_at) VALUES (%s, %s, %s, %s)",
            (version, content_hash, kb.manifest["format"], now),
        )
    return Seeded(version, previous, changed=True, retest=retest, stranded=stranded)
