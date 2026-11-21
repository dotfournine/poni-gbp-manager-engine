"""Database migrations: numbered SQL files in engine/migrations/, named NNNN_description.sql.

Each file runs once, in its own transaction, and is recorded in the
schema_migrations table together with a checksum. A file that has been
applied must never be edited; add a new one instead. The engine applies
nothing: at start it only checks that nothing is pending (D-079).
"""

from __future__ import annotations

import contextlib
import hashlib
import re
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

import psycopg

# Shared by every process that migrates this database: "ponigbp" + 1, as the engine used it.
ADVISORY_LOCK_KEY = 0x706F6E69_67627001

_FILE_NAME = re.compile(r"^(\d{4})_([a-z0-9_]+)\.sql$")


# The migrations that 0001_baseline replaced (D-091 point 10), as a database
# that had every one of them recorded them. Such a database is marked as having
# the baseline, which makes the same schema. Delete this, `_mark_baseline` and
# their two calls once no database is left that was migrated before the baseline.
BEFORE_BASELINE = frozenset(
    {
        (1, "jobs", "d140e4b66bd718263da9e8256705c77141df1abaa3c1de7acac3ea582a1abe43"),
        (2, "users_sessions_audit", "17e6097524ab71b9aba737d44d6e1aeaf9ba079e5a9e0973ffe6c8ac59c1ae60"),
        (3, "credentials_and_status", "5161524ae2340534c0ea60f049bd3a20962309039e9060366bece0cf4c84b2ed"),
        (4, "clients_locations", "4fca678fcbdf3cd2ece767a5c0e6754309f0bdfc18a80fc39a710782d0fa4799"),
        (5, "reviews", "ee466b21e1d2a6ea1617c756c165fedd965ac1d19a53e27f71de43e10c2665d4"),
        (6, "analysis_and_drafts", "bfaf18ea3ea54a87d1d8d5c6a4d3e3e356d69d75afb3ca3ef14442414f102c50"),
        (7, "seed_profiles_and_voices", "c0fcec16befa2aaae7e681991b000de4ddc123a3dda527b8cdd7504e37447c47"),
        (8, "seed_mvp_client_voices", "b4ff4881abf69a49fb3176bff5350e0a2006e9226e02e67b51b718cf194f7271"),
        (
            9,
            "analysis_start_and_no_script_conversion",
            "497cef2dcb5609e49b2c8899b292f3b79fb114b88d96179c6d70174e99fee018",
        ),
        (10, "no_keyword_rules", "aaee988de176eae0b8e36aab794a32b6b3a6687f1e65955c314cd20428267dca"),
        (11, "llm_providers", "ce0ad530c77a72b1f66a964b6fc7d2594057140e5aebba6326a4439ec0ee25da"),
        (12, "posting_escalations_notifications", "883ad0bf06b569de1b86796e6d7d9d72530d55a3c36298c84e91e45e58857f92"),
        (13, "ignored_reviews", "9ac646871baf671bb153ba588a93f4e3a2cb387bd4bb891ab154249096cb12f2"),
        (14, "auto_posting", "5588c008f8e24ff56d88997b29f2ca70709c06ec4e52cfd157956cf44ea5b2f0"),
        (15, "go_live_test", "c4254a6155df07b96a19b0758d45e3458f3abc27a0e7ca9cb0bdcb45b5f22be4"),
        (16, "knowledge_base", "7c93d45102b75fce195e936d5ebbeb4994d076857bbf6727c0884e0f24e6d3e8"),
        (17, "reports", "056ff6373bb2b895f8e1eebb4534e26d7e5a5ac8da55a23711a2ff19a17088f0"),
        (18, "google_target", "287e49bc968cff0dfa64f6f99a92522e047e0e1c253f5815a27d55ce3f6240c5"),
        (19, "local_posts", "7b61dee4821d43a9c8bea64b38549eff37a81280640db5479ad22ce5957d53d5"),
        (20, "local_post_guards", "eaa71018193612da99c392eaed81b539d308eddf6059ec8241a45242503cb535"),
        (21, "posts_model_task", "811a69e0368c4d0ab662dc6c9901e75d54eede2b27ed609952049a46201b026f"),
        (22, "local_post_media_duration", "01051a2f2858a244dbda7a36981fea34c6eccff2e26d64599c42db417872fc9a"),
        (23, "local_post_content", "bc53c384a1009924d3094696c466a09ad9f672de0f824536b073fc6aac7c6d42"),
        (24, "reports_rejected_unbuilt", "0dc499af2d07c651e2b44601f5c96367f50d637fed29a83953f9498b03451b18"),
        (25, "voice_without_other_business_names", "a7cee6c30ffe6971540fd6b7bf5806864f04ce6ebc0533ab4dc19f114f2c2bdb"),
        (26, "reply_settings", "f3697e08a5f5e7161adc7539c8cb5c7f151b53cd17d4044b63925f29cb4b83cd"),
    }
)


class MigrationError(Exception):
    pass


@dataclass
class Migration:
    version: int
    name: str  # the file name without the number and the extension
    sql: str
    checksum: str  # SHA-256 of the file


@dataclass
class Status:
    applied: list[int]
    pending: list[int]
    marked_baseline: bool = False  # migrate replaces the old migrations' rows by the baseline's


def load_migrations(directory: Path) -> list[Migration]:
    """Reads and validates the directory: versions must run 1, 2, 3, ... with no gaps or duplicates."""
    directory = Path(directory)
    if not directory.is_dir():
        raise MigrationError(f"migrations directory {directory} not found")
    found: list[Migration] = []
    for path in directory.iterdir():
        if not path.is_file() or path.suffix != ".sql":
            continue
        match = _FILE_NAME.match(path.name)
        if not match or int(match[1]) < 1:
            raise MigrationError(f"{path.name}: migration files are named NNNN_lower_case_name.sql, starting at 0001")
        raw = path.read_bytes()
        found.append(Migration(int(match[1]), match[2], raw.decode("utf-8"), hashlib.sha256(raw).hexdigest()))
    found.sort(key=lambda m: m.version)
    for index, migration in enumerate(found):
        expected = index + 1
        if migration.version < expected:
            raise MigrationError(f"two migrations have number {migration.version:04}")
        if migration.version > expected:
            raise MigrationError(f"migration {expected:04} is missing")
    return found


def _ensure_table(conn: psycopg.Connection) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS schema_migrations (
            version    integer     PRIMARY KEY,
            name       text        NOT NULL,
            checksum   text        NOT NULL,
            applied_at timestamptz NOT NULL DEFAULT now()
        )"""
    )


def _record(conn: psycopg.Connection, migration: Migration) -> None:
    conn.execute(
        "INSERT INTO schema_migrations (version, name, checksum) VALUES (%s, %s, %s)",
        (migration.version, migration.name, migration.checksum),
    )


def _mark_baseline(conn: psycopg.Connection, migrations: list[Migration]) -> bool:
    """Replaces the rows of BEFORE_BASELINE by the baseline's when the table holds exactly those, refuses some."""
    if not migrations or (migrations[0].version, migrations[0].name) != (1, "baseline"):
        return False
    recorded = set(conn.execute("SELECT version, name, checksum FROM schema_migrations").fetchall())
    if recorded == BEFORE_BASELINE:
        conn.execute("DELETE FROM schema_migrations")
        _record(conn, migrations[0])
        return True
    # Every database migrated before the baseline has 0001_jobs; no other has.
    if any((version, name) == (1, "jobs") for version, name, _ in recorded):
        raise MigrationError(
            "the database has some of the migrations that 0001_baseline replaced, but not exactly 0001_jobs to "
            "0026_reply_settings; migrate it with the release before the baseline first, then with this one"
        )
    return False


def _status(conn: psycopg.Connection, migrations: list[Migration]) -> Status:
    applied = dict(conn.execute("SELECT version, checksum FROM schema_migrations").fetchall())
    status = Status([], [])
    for migration in migrations:
        checksum = applied.pop(migration.version, None)
        if checksum is None:
            status.pending.append(migration.version)
        elif checksum != migration.checksum:
            raise MigrationError(
                f"migration {migration.version:04}_{migration.name} was changed after it was applied; "
                "add a new migration instead"
            )
        else:
            status.applied.append(migration.version)
    if applied:
        raise MigrationError(
            f"the database has migration {min(applied)} but the migration files don't; "
            "are these files older than the database?"
        )
    return status


def migration_status(conn: psycopg.Connection, migrations: list[Migration]) -> Status:
    """What is applied and what is not. Changes nothing: the table it may create, and the mark, are rolled back."""
    try:
        with conn.transaction():
            _ensure_table(conn)
            marked = _mark_baseline(conn, migrations)
            status = _status(conn, migrations)
            status.marked_baseline = marked
            raise _Rollback
    except _Rollback:
        return status


class _Rollback(Exception):
    pass


def apply_migrations(
    conn: psycopg.Connection, migrations: list[Migration], on_baseline_marked: Callable[[], None] | None = None
) -> list[int]:
    """Applies every pending migration in order and returns the versions applied.

    A database migrated before the baseline is first marked as having it, in
    the transaction of the check, and on_baseline_marked is called. Safe to
    run from two processes at once (advisory lock). Migrations applied before
    a failing one stay applied.
    """
    conn.execute("SELECT pg_advisory_lock(%s)", (ADVISORY_LOCK_KEY,))
    try:
        with conn.transaction():
            _ensure_table(conn)
            marked = _mark_baseline(conn, migrations)
            status = _status(conn, migrations)
        if marked and on_baseline_marked:
            on_baseline_marked()
        applied: list[int] = []
        for migration in migrations:
            if migration.version not in status.pending:
                continue
            try:
                with conn.transaction():
                    conn.execute(migration.sql.encode("utf-8"))
                    _record(conn, migration)
            except psycopg.Error as error:
                detail = str(error).strip().splitlines()[0] if str(error).strip() else type(error).__name__
                raise MigrationError(f"migration {migration.version:04}_{migration.name} failed: {detail}") from error
            applied.append(migration.version)
        return applied
    finally:
        # The lock goes away with the session if this fails.
        with contextlib.suppress(psycopg.Error):
            conn.execute("SELECT pg_advisory_unlock(%s)", (ADVISORY_LOCK_KEY,))


def connect(database_url: str) -> psycopg.Connection:
    # autocommit: every transaction above is opened explicitly. A dead host would otherwise be waited on for minutes.
    return psycopg.connect(database_url, autocommit=True, connect_timeout=5)
