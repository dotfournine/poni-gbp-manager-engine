"""Migrations against a real PostgreSQL: each test gets its own empty schema, as the engine's tests do."""

from __future__ import annotations

import hashlib
import shutil

import psycopg
import pytest

from poni_tools import cli, migrate
from poni_tools.settings import REPOSITORY

SHIPPED = REPOSITORY / "engine/migrations"


def migration(version: int, name: str, sql: str) -> migrate.Migration:
    return migrate.Migration(version, name, sql, hashlib.sha256(sql.encode()).hexdigest())


def one(conn, sql: str):
    return conn.execute(sql).fetchone()[0]


def test_shipped_migrations_apply_once_and_are_recorded(db):
    shipped = migrate.load_migrations(SHIPPED)
    before = migrate.migration_status(db, shipped)
    assert before.applied == [] and len(before.pending) == len(shipped)

    assert len(migrate.apply_migrations(db, shipped)) == len(shipped)
    assert migrate.apply_migrations(db, shipped) == [], "a second run must change nothing"

    after = migrate.migration_status(db, shipped)
    assert len(after.applied) == len(shipped) and after.pending == []
    assert one(db, "SELECT count(*) FROM schema_migrations") == len(shipped)
    assert one(db, "SELECT count(*) FROM jobs") == 0


def test_the_checksum_is_the_one_the_engine_checks(db):
    # The engine compares the same value at start: SHA-256 of the file's bytes.
    shipped = migrate.load_migrations(SHIPPED)
    first = (SHIPPED / "0001_baseline.sql").read_bytes()
    assert shipped[0].checksum == hashlib.sha256(first).hexdigest()
    assert shipped[0].name == "baseline"


def test_status_does_not_change_the_database(db):
    migrate.migration_status(db, migrate.load_migrations(SHIPPED))
    assert one(db, "SELECT to_regclass('schema_migrations')") is None


def test_applies_only_what_is_pending(db):
    files = [migration(1, "a", "CREATE TABLE a (id int)")]
    assert migrate.apply_migrations(db, files) == [1]
    files.append(migration(2, "b", "CREATE TABLE b (id int); CREATE INDEX b_id ON b (id)"))
    assert migrate.apply_migrations(db, files) == [2]


def test_a_failed_migration_rolls_back_and_keeps_earlier_ones(db):
    files = [
        migration(1, "good", "CREATE TABLE good (id int)"),
        migration(2, "bad", "CREATE TABLE half_done (id int); SELECT * FROM no_such_table"),
    ]
    with pytest.raises(migrate.MigrationError, match="0002_bad failed"):
        migrate.apply_migrations(db, files)
    status = migrate.migration_status(db, files)
    assert status.applied == [1] and status.pending == [2]
    assert one(db, "SELECT to_regclass('half_done')") is None


def test_an_edited_migration_is_rejected(db):
    migrate.apply_migrations(db, [migration(1, "a", "CREATE TABLE a (id int)")])
    with pytest.raises(migrate.MigrationError, match="changed after it was applied"):
        migrate.apply_migrations(db, [migration(1, "a", "CREATE TABLE a (id bigint)")])


def test_a_database_newer_than_the_files_is_rejected(db):
    migrate.apply_migrations(db, [migration(1, "a", "CREATE TABLE a (id int)"), migration(2, "b", "SELECT 1")])
    with pytest.raises(migrate.MigrationError, match="has migration 2"):
        migrate.migration_status(db, [migration(1, "a", "CREATE TABLE a (id int)")])


def test_a_percent_sign_in_a_migration_is_not_a_placeholder(db):
    migrate.apply_migrations(db, [migration(1, "a", "CREATE TABLE a (t text); INSERT INTO a VALUES ('100%s')")])
    assert one(db, "SELECT t FROM a") == "100%s"


# ---- a database migrated before the baseline (D-091 point 10) ---------------------------


def recorded(conn) -> list[tuple]:
    return conn.execute("SELECT version, name, checksum FROM schema_migrations ORDER BY version").fetchall()


def migrated_before_the_baseline(conn, rows) -> None:
    """schema_migrations as the old chain left it; the schema itself does not matter to the mark."""
    migrate._ensure_table(conn)
    for row in rows:
        conn.execute("INSERT INTO schema_migrations (version, name, checksum) VALUES (%s, %s, %s)", row)


def test_a_database_with_every_old_migration_is_marked_as_having_the_baseline(db):
    # With the baseline alone: this made-up database has the old chain's rows and none of its tables, which
    # shows that the baseline is not run. A migration after the baseline would find nothing to change here.
    baseline = migrate.load_migrations(SHIPPED)[:1]
    migrated_before_the_baseline(db, sorted(migrate.BEFORE_BASELINE))

    status = migrate.migration_status(db, baseline)
    assert status.marked_baseline and status.pending == []
    assert len(recorded(db)) == 26, "--status changes nothing"

    assert migrate.apply_migrations(db, baseline) == []
    assert recorded(db) == [(1, "baseline", baseline[0].checksum)]
    assert one(db, "SELECT to_regclass('jobs')") is None, "the baseline itself is not run"
    assert not migrate.migration_status(db, baseline).marked_baseline


def test_a_marked_database_then_gets_the_migrations_after_the_baseline(db):
    shipped = migrate.load_migrations(SHIPPED)
    # The tables as the old chain left them are the tables the baseline creates.
    assert migrate.apply_migrations(db, shipped[:1]) == [1]
    db.execute("DELETE FROM schema_migrations")
    migrated_before_the_baseline(db, sorted(migrate.BEFORE_BASELINE))

    later = [m.version for m in shipped[1:]]
    status = migrate.migration_status(db, shipped)
    assert status.marked_baseline and status.applied == [1] and status.pending == later
    assert len(recorded(db)) == 26, "--status changes nothing"

    assert migrate.apply_migrations(db, shipped) == later
    assert [row[:2] for row in recorded(db)] == [(m.version, m.name) for m in shipped]


def test_the_sessions_there_are_become_browser_sessions(db):
    # 0002 (D-095): a session is of a kind, and every session from before is the dashboard's.
    shipped = migrate.load_migrations(SHIPPED)
    assert migrate.apply_migrations(db, shipped[:1]) == [1]
    db.execute(
        "INSERT INTO users (google_sub, email, created_at, last_login_at) VALUES ('sub', 'staff@example.com', now(), now())"
    )
    db.execute(
        "INSERT INTO sessions (token_hash, user_id, csrf_token, created_at, last_seen_at, expires_at) "
        "SELECT 'a-hash', id, 'a-csrf-token', now(), now(), now() FROM users"
    )
    assert migrate.apply_migrations(db, shipped[:2]) == [2]
    assert db.execute("SELECT kind, csrf_token FROM sessions").fetchall() == [("browser", "a-csrf-token")]
    assert one(db, "SELECT count(*) FROM app_sign_in_codes") == 0


def old_report(conn, location: int, status: str, *, auto_send=False, built=False, error=None, sent_by=None) -> int:
    """A report as 0009 left them; a draft's and a sent report's page was in the database."""
    return conn.execute(
        "INSERT INTO reports (location_id, period_start, period_end, time_zone, auto_send, status, error, data, html, "
        "created_at, built_at, sent_at, sent_by, updated_at) VALUES (%s, '2026-10-01', '2026-10-31', 'UTC', %s, %s, %s, "
        "CASE WHEN %s THEN '{\"version\": 3}'::jsonb END, CASE WHEN %s THEN '<html></html>' END, now(), "
        "CASE WHEN %s THEN now() END, CASE WHEN %s::bigint IS NOT NULL OR %s = 'sent' THEN now() END, %s, now()) RETURNING id",
        (location, auto_send, status, error, built, built, built, sent_by, status, sent_by),
    ).fetchone()[0]


def test_reports_become_ready_failed_or_discarded_and_their_sends_name_them(db):
    # 0010 (D-104): no draft, sent or rejected report, no page in the database, no share link.
    shipped = migrate.load_migrations(SHIPPED)
    assert migrate.apply_migrations(db, shipped[:9]) == list(range(1, 10))
    user = one(
        db,
        "INSERT INTO users (google_sub, email, created_at, last_login_at) "
        "VALUES ('sub', 'staff@example.com', now(), now()) RETURNING id",
    )
    location = one(
        db,
        "INSERT INTO locations (gbp_location_id, gbp_account_id, title, raw, first_seen_at, last_seen_at, updated_at) "
        "VALUES ('9001', '111', 'A Spa', '{}', now(), now(), now()) RETURNING id",
    )
    building_auto = old_report(db, location, "building", auto_send=True)
    building = old_report(db, location, "building")
    failed = old_report(db, location, "failed", error="broken")
    draft_auto = old_report(db, location, "draft", auto_send=True, built=True)
    draft = old_report(db, location, "draft", built=True)
    sent_by_person = old_report(db, location, "sent", built=True, sent_by=user)
    sent_by_schedule = old_report(db, location, "sent", auto_send=True, built=True)
    rejected_draft = old_report(db, location, "rejected", built=True)
    rejected_failed = old_report(db, location, "rejected", error="broken")
    db.execute(
        "INSERT INTO report_share_links (report_id, token, origin, created_at) VALUES (%s, 'a-token', 'manual', now())",
        (draft,),
    )
    deliveries = {}
    for report in (sent_by_person, sent_by_schedule, 999999):
        deliveries[report] = db.execute(
            "INSERT INTO notification_deliveries (event_type, event_key, location_id, channel, recipient, subject, body, "
            "created_at, updated_at) VALUES ('location_report', %s, %s, 'email', 'owner@example.com', 'Report', 'Body', "
            "now(), now()) RETURNING id",
            (f"report:{report}:sent", location),
        ).fetchone()[0]

    assert migrate.apply_migrations(db, shipped[:10]) == [10]
    rows = {
        row[0]: row[1:]
        for row in db.execute("SELECT id, status, error, auto_send_status, data IS NOT NULL, page_object FROM reports")
    }
    gone = "Built before D-104; generate it again."
    assert rows == {
        building_auto: ("building", None, "pending", False, None),
        building: ("building", None, "off", False, None),
        failed: ("failed", "broken", "off", False, None),
        draft_auto: ("failed", gone, "off", True, None),
        draft: ("failed", gone, "off", True, None),
        sent_by_person: ("failed", gone, "off", True, None),
        sent_by_schedule: ("failed", gone, "done", True, None),
        rejected_draft: ("discarded", None, "off", True, None),
        rejected_failed: ("discarded", "broken", "off", False, None),
    }
    sends = {row[0]: row[1:] for row in db.execute("SELECT id, report_id, requested_by FROM notification_deliveries")}
    assert {report: sends[delivery] for report, delivery in deliveries.items()} == {
        sent_by_person: (sent_by_person, user),
        sent_by_schedule: (sent_by_schedule, None),
        999999: (None, None),
    }
    assert one(db, "SELECT to_regclass('report_share_links')") is None
    with pytest.raises(psycopg.errors.CheckViolation):
        db.execute("UPDATE reports SET status = 'ready' WHERE id = %s", (draft,))


@pytest.mark.parametrize(
    "rows",
    [
        sorted(migrate.BEFORE_BASELINE)[:20],
        sorted(migrate.BEFORE_BASELINE)[:25] + [(26, "reply_settings", "an-earlier-version-of-the-file")],
        sorted(migrate.BEFORE_BASELINE) + [(27, "a_later_one", "checksum")],
    ],
    ids=["some", "one-changed", "one-more"],
)
def test_a_database_with_only_some_of_them_is_refused(db, rows):
    migrated_before_the_baseline(db, rows)
    with pytest.raises(migrate.MigrationError, match="migrate it with the release before the baseline first"):
        migrate.apply_migrations(db, migrate.load_migrations(SHIPPED))
    assert recorded(db) == sorted(rows)


def test_a_database_with_none_of_them_gets_the_baseline(db):
    assert migrate.apply_migrations(db, migrate.load_migrations(SHIPPED))[0] == 1
    assert recorded(db)[0][:2] == (1, "baseline")
    assert one(db, "SELECT count(*) FROM system_settings WHERE NOT posting_enabled AND NOT email_sending_enabled") == 1


def test_the_command_says_it_marked_the_database(db_url, db, tmp_path, monkeypatch, capsys):
    migrated_before_the_baseline(db, sorted(migrate.BEFORE_BASELINE))
    # The baseline alone, as above: this database has none of the tables a later migration changes.
    only_the_baseline = tmp_path / "migrations"
    only_the_baseline.mkdir()
    shutil.copy(SHIPPED / "0001_baseline.sql", only_the_baseline)
    monkeypatch.setenv("PONI_DATABASE_URL", db_url)
    monkeypatch.setenv("PONI_MIGRATIONS_DIR", str(only_the_baseline))
    monkeypatch.setenv("HOME", str(tmp_path))  # no local engine.env

    assert cli.main(["migrate", "--status"]) == 0
    assert "marks as 0001_baseline" in capsys.readouterr().out
    assert cli.main(["migrate"]) == 0
    out = capsys.readouterr().out
    assert "it is marked as having 0001_baseline, nothing else changed" in out
    assert "applied migration" not in out


@pytest.mark.parametrize(
    ("files", "message"),
    [
        (["0001_a.sql", "0003_c.sql"], "migration 0002 is missing"),
        (["0001_a.sql", "0001_b.sql"], "two migrations have number 0001"),
        (["0001_Bad-Name.sql"], "named NNNN_lower_case_name.sql"),
        (["1_a.sql"], "named NNNN_lower_case_name.sql"),
    ],
)
def test_file_names_are_checked(tmp_path, files, message):
    for name in files:
        (tmp_path / name).write_text("SELECT 1")
    with pytest.raises(migrate.MigrationError, match=message):
        migrate.load_migrations(tmp_path)


def test_other_files_in_the_folder_are_left_alone(tmp_path):
    (tmp_path / "0001_a.sql").write_text("SELECT 1")
    (tmp_path / "README.md").write_text("notes")
    assert [m.name for m in migrate.load_migrations(tmp_path)] == ["a"]


def test_the_command_applies_and_reports(db_url, db, tmp_path, monkeypatch, capsys):
    (tmp_path / "0001_a.sql").write_text("CREATE TABLE a (id int)")
    monkeypatch.setenv("PONI_DATABASE_URL", db_url)
    monkeypatch.setenv("PONI_MIGRATIONS_DIR", str(tmp_path))
    monkeypatch.setenv("HOME", str(tmp_path))  # no local engine.env

    assert cli.main(["migrate", "--status"]) == 0
    assert "0 migration(s) applied, 1 pending" in capsys.readouterr().out
    assert cli.main(["migrate"]) == 0
    assert "applied migration 0001_a" in capsys.readouterr().out
    assert cli.main(["migrate"]) == 0
    assert "0 applied now, 1 in total" in capsys.readouterr().out
    assert one(db, "SELECT count(*) FROM a") == 0


def test_an_unreachable_database_is_one_line_and_exit_1(tmp_path, monkeypatch, capsys):
    (tmp_path / "0001_a.sql").write_text("SELECT 1")
    monkeypatch.setenv("PONI_DATABASE_URL", "postgresql://poni:poni@127.0.0.1:1/poni")
    monkeypatch.setenv("PONI_MIGRATIONS_DIR", str(tmp_path))
    monkeypatch.setenv("HOME", str(tmp_path))
    assert cli.main(["migrate"]) == 1
    err = capsys.readouterr().err
    assert err.startswith("database: ") and err.count("\n") == 1
