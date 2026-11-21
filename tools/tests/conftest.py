"""Every test works on its own copy of the repository's knowledge base."""

from __future__ import annotations

import json
import os
import shutil
import uuid
from collections.abc import Iterator
from pathlib import Path

import psycopg
import psycopg.conninfo
import pytest

REPOSITORY = Path(__file__).resolve().parents[2]


class Folder:
    """A copy of knowledge/ whose files a test can change."""

    def __init__(self, root: Path) -> None:
        self.root = root

    def read(self, relative: str):
        return json.loads((self.root / relative).read_text(encoding="utf-8"))

    def write(self, relative: str, value) -> None:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    def edit(self, relative: str, change) -> None:
        value = self.read(relative)
        change(value)
        self.write(relative, value)

    def set_version(self, version: str) -> None:
        """A changed knowledge base gets a new version, as a person would give it."""
        self.edit("manifest.json", lambda m: m.update(version=version))

    def drop_ordinary_cases(self, industry: str = "food_beverage") -> None:
        """Makes a pack incomplete: it keeps no ordinary case."""
        self.edit(
            f"industries/{industry}/corpus.json",
            lambda c: c.update(cases=[case for case in c["cases"] if case["category"] != "control"]),
        )


@pytest.fixture
def kb(tmp_path: Path) -> Folder:
    """A copy of the repository's knowledge base that a test may change."""
    root = tmp_path / "knowledge"
    shutil.copytree(REPOSITORY / "knowledge", root)
    return Folder(root)


# ---- a real PostgreSQL ----------------------------------------------------------------


def base_database_url() -> str:
    """PONI_TEST_DATABASE_URL, or the development database of deploy/docker-compose.dev.yml (as the engine's tests)."""
    return os.environ.get("PONI_TEST_DATABASE_URL") or "postgresql://poni:poni@127.0.0.1:55432/poni"


@pytest.fixture
def db_url() -> Iterator[str]:
    """The address of an empty schema of its own; a missing database fails the test, it is never skipped."""
    schema = f"tools_test_{os.getpid()}_{uuid.uuid4().hex[:8]}"
    with psycopg.connect(base_database_url(), autocommit=True, connect_timeout=5) as admin:
        admin.execute(f"CREATE SCHEMA {schema}")
        try:
            yield psycopg.conninfo.make_conninfo(base_database_url(), options=f"-csearch_path={schema}")
        finally:
            admin.execute(f"DROP SCHEMA IF EXISTS {schema} CASCADE")


@pytest.fixture
def db(db_url: str) -> Iterator[psycopg.Connection]:
    with psycopg.connect(db_url, autocommit=True) as conn:
        yield conn
