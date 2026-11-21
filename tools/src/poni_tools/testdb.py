"""The prepared database of a test run (D-079 point 3).

The engine's tests cannot call a script for every test. So a test run
prepares one database with the real scripts, migrated and seeded, and every
test starts from a copy of it (`CREATE DATABASE ... TEMPLATE`), which
PostgreSQL makes in a few milliseconds.
"""

from __future__ import annotations

import re
from pathlib import Path

import psycopg
import psycopg.conninfo
from psycopg import sql

from . import knowledge, migrate, seed

# Only names this code made up are ever dropped.
PREFIX = "poni_test_"
_NAME = re.compile(r"^poni_test_[a-z0-9_]{1,40}$")


class TestDbError(Exception):
    __test__ = False  # not a pytest class, whatever its name starts with


def copy_prefix(name: str) -> str:
    """What the copies of the prepared database `name` are called: the tests name them `<name>_c<pid>_<n>`."""
    return f"{name}_c"


def prepare(server_url: str, name: str, migrations_dir: Path, knowledge_dir: Path) -> tuple[int, str]:
    """Drops and recreates the database `name` on the server of `server_url`, migrates it and seeds it.

    Returns the number of migrations applied and the knowledge base's
    version. Copies a crashed test run left behind are dropped too. Nothing
    may be connected to the old database: tests connect only to their copies.
    """
    if not _NAME.match(name):
        raise TestDbError(f"a prepared test database is named {PREFIX}<lower_case_name>, not '{name}'")
    migrations = migrate.load_migrations(migrations_dir)
    kb = knowledge.load(knowledge_dir)
    with psycopg.connect(server_url, autocommit=True, connect_timeout=5) as admin:
        stale = admin.execute(
            "SELECT datname FROM pg_database WHERE datname = %s OR starts_with(datname, %s)", (name, copy_prefix(name))
        ).fetchall()
        for (database,) in stale:
            admin.execute(sql.SQL("DROP DATABASE IF EXISTS {} WITH (FORCE)").format(sql.Identifier(database)))
        admin.execute(sql.SQL("CREATE DATABASE {}").format(sql.Identifier(name)))
    with psycopg.connect(psycopg.conninfo.make_conninfo(server_url, dbname=name), autocommit=True) as conn:
        applied = migrate.apply_migrations(conn, migrations)
        seeded = seed.seed(conn, kb)
    return len(applied), seeded.version
