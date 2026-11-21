from __future__ import annotations

import os
import uuid

import psycopg
import psycopg.conninfo
import pytest

from poni_tools import cli, testdb
from poni_tools.settings import REPOSITORY

from .conftest import base_database_url


@pytest.fixture
def name():
    name = f"poni_test_tools_{os.getpid()}_{uuid.uuid4().hex[:6]}"
    yield name
    with psycopg.connect(base_database_url(), autocommit=True) as admin:
        admin.execute(f"DROP DATABASE IF EXISTS {name} WITH (FORCE)")


def test_the_prepared_database_is_migrated_seeded_and_can_be_copied(name, kb, monkeypatch, tmp_path, capsys):
    monkeypatch.setenv("PONI_TEST_DATABASE_URL", base_database_url())
    monkeypatch.setenv("PONI_KNOWLEDGE_DIR", str(kb.root))
    monkeypatch.setenv("HOME", str(tmp_path))
    assert cli.main(["test-db", "--name", name]) == 0
    shipped = len(list((REPOSITORY / "engine/migrations").glob("*.sql")))
    assert f"Prepared the test database {name}: {shipped} migration(s), knowledge base " in capsys.readouterr().out

    copy = name + "_c1_1"
    with psycopg.connect(base_database_url(), autocommit=True) as admin:
        admin.execute(f"CREATE DATABASE {copy} TEMPLATE {name}")
        try:
            with psycopg.connect(psycopg.conninfo.make_conninfo(base_database_url(), dbname=copy)) as conn:
                assert conn.execute("SELECT count(*) FROM schema_migrations").fetchone()[0] == shipped
                assert conn.execute("SELECT count(*) FROM kb_industries").fetchone()[0] == 2
        finally:
            # Left behind, as by a test run that crashed: the next preparation clears it away.
            pass

    # A second run starts over: what a test left in the template is gone.
    with psycopg.connect(psycopg.conninfo.make_conninfo(base_database_url(), dbname=name), autocommit=True) as conn:
        conn.execute("CREATE TABLE left_behind (id int)")
    assert cli.main(["test-db", "--name", name]) == 0
    with psycopg.connect(psycopg.conninfo.make_conninfo(base_database_url(), dbname=name)) as conn:
        assert conn.execute("SELECT to_regclass('left_behind')").fetchone()[0] is None
    with psycopg.connect(base_database_url(), autocommit=True) as admin:
        assert admin.execute("SELECT count(*) FROM pg_database WHERE datname = %s", (copy,)).fetchone()[0] == 0


def test_an_incomplete_pack_stops_the_preparation_with_the_reason(name, kb, monkeypatch, tmp_path, capsys):
    kb.drop_ordinary_cases()
    monkeypatch.setenv("PONI_TEST_DATABASE_URL", base_database_url())
    monkeypatch.setenv("PONI_KNOWLEDGE_DIR", str(kb.root))
    monkeypatch.setenv("HOME", str(tmp_path))
    assert cli.main(["test-db", "--name", name]) == 1
    assert "0 ordinary (control) case(s), needs 3" in capsys.readouterr().err


@pytest.mark.parametrize(
    "name", ["poni", "poni_phase3", "postgres", "poni_test_", 'poni_test_x"; DROP DATABASE poni; --']
)
def test_only_a_test_database_is_ever_dropped(name):
    with pytest.raises(testdb.TestDbError, match="is named poni_test_"):
        testdb.prepare(
            "postgresql://nobody@127.0.0.1:1/none", name, REPOSITORY / "engine/migrations", REPOSITORY / "knowledge"
        )
