"""poni-tools: one command line for the operations that are not the engine's (D-079).

    poni-tools migrate                                   apply pending database migrations
    poni-tools migrate --status                          show applied and pending migrations, change nothing
    poni-tools check [--knowledge DIR]                   check the knowledge base
    poni-tools seed [--knowledge DIR]                    check it, then replace the kb_* tables with it
    poni-tools seed --archive NAME [--bucket BUCKET]     the same from a published archive (production)
    poni-tools pack [--out DIR]                          check it and pack it into one archive
    poni-tools publish --bucket BUCKET                   pack it and upload the archive (from a clean main)
    poni-tools test-db [--name NAME]                     prepare the database the engine's tests start from
    poni-tools mock-google [--port PORT] [--reset]       a local mock of Google with a test portal (development)

Settings are the engine's PONI_* names (PONI_DATABASE_URL, PONI_MIGRATIONS_DIR,
PONI_KNOWLEDGE_DIR, and PONI_KNOWLEDGE_BUCKET with PONI_KNOWLEDGE_ARCHIVE
for a published archive), from the environment or the file named by
--config or PONI_CONFIG; without either, ~/.config/poni-gbp/engine.env if it
exists. A bucket is a Cloud Storage bucket's name; a path stands in for one
in development.

Exit status: 0 when the command did what it was asked, 1 when it found a
problem or could not finish, 2 for a wrong command line.
"""

from __future__ import annotations

import argparse
import sys
import tempfile
from pathlib import Path

import psycopg

from . import knowledge, migrate, publish, seed, settings, storage, testdb
from .check import check


def _knowledge_dir(args: argparse.Namespace, config: settings.Settings) -> Path:
    return args.knowledge or config.knowledge_dir


def cmd_migrate(args: argparse.Namespace, config: settings.Settings) -> int:
    migrations = migrate.load_migrations(config.migrations_dir)
    with migrate.connect(config.database_url) as conn:
        if args.status:
            status = migrate.migration_status(conn, migrations)
            if status.marked_baseline:
                print("The database has migrations 0001 to 0026, which `migrate` marks as 0001_baseline.")
            print(f"{len(status.applied)} migration(s) applied, {len(status.pending)} pending.")
            for version in status.pending:
                print(f"  pending: {version:04}_{migrations[version - 1].name}")
            return 0
        applied = migrate.apply_migrations(
            conn,
            migrations,
            on_baseline_marked=lambda: print(
                "The database had migrations 0001 to 0026: it is marked as having 0001_baseline, nothing else changed."
            ),
        )
        for version in applied:
            print(f"applied migration {version:04}_{migrations[version - 1].name}")
        print(f"The database is up to date: {len(applied)} applied now, {len(migrations)} in total.")
    return 0


def cmd_check(args: argparse.Namespace, config: settings.Settings) -> int:
    folder = _knowledge_dir(args, config)
    kb = knowledge.load(folder)
    problems = check(kb)
    if problems:
        for problem in problems:
            print(problem, file=sys.stderr)
        print(f"\n{len(problems)} problem(s) in {folder}. Nothing can be seeded or published from it.", file=sys.stderr)
        return 1
    cases = sum(len(pack.corpus["cases"]) for pack in kb.industries.values())
    print(
        f"The knowledge base is complete: version {kb.version()}, {len(kb.industries)} industries "
        f"({', '.join(sorted(kb.industries))}), {cases} corpus cases, {len(kb.voices)} voice templates, "
        f"{len(kb.models['prices'])} model prices."
    )
    return 0


def cmd_seed(args: argparse.Namespace, config: settings.Settings) -> int:
    archive = args.archive or (None if args.knowledge else config.knowledge_archive or None)
    with tempfile.TemporaryDirectory() as scratch:
        if archive:
            # Production: the archive this installation names, from the bucket, compared with the version in its name.
            bucket = args.bucket or config.knowledge_bucket
            if not bucket:
                print("an archive needs a bucket: --bucket, or PONI_KNOWLEDGE_BUCKET", file=sys.stderr)
                return 2
            store = storage.store_for(bucket)
            kb = publish.fetch(store, archive, Path(scratch))
            print(f"Read {archive} from {store.description}.")
        else:
            kb = knowledge.load(_knowledge_dir(args, config))
        with migrate.connect(config.database_url) as conn:
            result = seed.seed(conn, kb)
    if not result.changed:
        print(f"The knowledge base is already at version {result.version}. Nothing was changed.")
    elif result.previous is None:
        print(f"Seeded the knowledge base, version {result.version}.")
    else:
        print(f"Seeded the knowledge base, version {result.version} (was {result.previous}).")
        _say_whom_it_changes(result)
    return 0


def _say_whom_it_changes(result: seed.Seeded) -> None:
    def lines(locations: list[seed.Location]) -> str:
        return "\n".join(f"  - {x.title} ({x.industry})" for x in locations)

    if result.retest:
        print(
            "\nThis version changes what the analyzer is told about an industry. These locations are analyzed with the "
            "new profile from now on, and none of them auto replies until a new go-live test has passed "
            "(replies go to the approval queue meanwhile). Start the tests on the Go-live test page:\n"
            + lines(result.retest)
        )
    if result.stranded:
        print(
            "\nThis version no longer has the industry of these locations. Their reviews are not analyzed until each "
            "is given another industry in the dashboard, and then passes a go-live test:\n" + lines(result.stranded)
        )
    if not result.retest and not result.stranded:
        print("No location needs a new go-live test because of it.")


def cmd_pack(args: argparse.Namespace, config: settings.Settings) -> int:
    archive = publish.pack(_knowledge_dir(args, config))
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    (out / archive.name).write_bytes(archive.data)
    print(f"Packed version {archive.version}: {out / archive.name} ({len(archive.data)} bytes).")
    return 0


def cmd_publish(args: argparse.Namespace, config: settings.Settings) -> int:
    folder = _knowledge_dir(args, config)
    bucket = args.bucket or config.knowledge_bucket
    if not bucket:
        print("say where to publish: --bucket, or PONI_KNOWLEDGE_BUCKET", file=sys.stderr)
        return 2
    # What production runs on must be what was reviewed and merged.
    if problem := publish.clean_main_checkout(folder.resolve().parent):
        print(f"Not published: {problem}.", file=sys.stderr)
        return 1
    store = storage.store_for(bucket)
    archive = publish.publish(folder, store)
    print(
        f"Published version {archive.version} to {store.description} as {archive.name}.\n"
        f"To run production on it, set in the server's poni.env:\n"
        f"  PONI_KNOWLEDGE_ARCHIVE={archive.name}\n"
        "and run the seed service there (deploy/README.md)."
    )
    return 0


def cmd_test_db(args: argparse.Namespace, config: settings.Settings) -> int:
    applied, version = testdb.prepare(config.test_database_url, args.name, config.migrations_dir, config.knowledge_dir)
    print(f"Prepared the test database {args.name}: {applied} migration(s), knowledge base {version}.")
    return 0


DEFAULT_MOCK_STATE = Path.home() / ".local/state/poni-gbp/mock-google.json"


def cmd_mock_google(args: argparse.Namespace, config: settings.Settings) -> int:
    try:
        # Not in the production image (.dockerignore): there, the command says so.
        from .mock_google.server import make_server
        from .mock_google.state import MockGoogle
    except ImportError:
        print("the Google mock is a development tool and is not part of this installation", file=sys.stderr)
        return 1
    mock = MockGoogle(args.state, args.knowledge or config.knowledge_dir, reset=args.reset)
    server = make_server(mock, args.port)
    port = server.server_address[1]
    print(
        f"Mock Google on http://127.0.0.1:{port} (state: {args.state}).\n"
        f"  The test portal:  http://127.0.0.1:{port}/\n"
        f"  The engine:       PONI_GOOGLE_MOCK_URL=http://127.0.0.1:{port} with a database of its own "
        "(docs/runbook-google-mock.md)\n"
        "Stop it with Ctrl-C.",
        flush=True,
    )
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="poni-tools", description="Operations scripts of the PONI GBP Manager.")
    parser.add_argument("--config", type=Path, metavar="FILE", help="a KEY=VALUE file of PONI_* settings")
    commands = parser.add_subparsers(dest="command", required=True, metavar="COMMAND")

    def with_knowledge(sub: argparse.ArgumentParser) -> None:
        sub.add_argument(
            "--knowledge",
            type=Path,
            metavar="DIR",
            help="the knowledge base folder (default: PONI_KNOWLEDGE_DIR, or knowledge/ of this checkout)",
        )

    sub = commands.add_parser("migrate", help="apply pending database migrations")
    sub.add_argument(
        "--status", action="store_true", help="show applied and pending migrations without changing anything"
    )
    sub.set_defaults(run=cmd_migrate)

    sub = commands.add_parser("check", help="check the knowledge base: schemas, cross-checks, complete packs")
    with_knowledge(sub)
    sub.set_defaults(run=cmd_check)

    sub = commands.add_parser(
        "seed",
        help="check the knowledge base, then replace the kb_* tables with it",
        description="Seeds from the published archive this installation names (PONI_KNOWLEDGE_ARCHIVE in "
        "PONI_KNOWLEDGE_BUCKET, or --archive and --bucket), else from the knowledge folder.",
    )
    with_knowledge(sub)
    sub.add_argument("--archive", metavar="NAME", help="a published archive, knowledge-<version>.tar.gz")
    sub.add_argument("--bucket", metavar="BUCKET", help="where the archive is: a Cloud Storage bucket, or a folder")
    sub.set_defaults(run=cmd_seed)

    sub = commands.add_parser("pack", help="check the knowledge base and pack it into one archive, without publishing")
    with_knowledge(sub)
    sub.add_argument("--out", default=".", metavar="DIR", help="where to write the archive (default: here)")
    sub.set_defaults(run=cmd_pack)

    sub = commands.add_parser(
        "publish",
        help="pack the knowledge base and upload the archive to the bucket",
        description="For the owner, by hand, from a clean checkout of main. A published archive is never replaced.",
    )
    with_knowledge(sub)
    sub.add_argument("--bucket", metavar="BUCKET", help="a Cloud Storage bucket (default: PONI_KNOWLEDGE_BUCKET)")
    sub.set_defaults(run=cmd_publish)

    sub = commands.add_parser(
        "test-db",
        help="prepare the database the engine's tests start from (ctest runs this before the integration tests)",
        description="Drops and recreates a database on the server of PONI_TEST_DATABASE_URL, migrates it and seeds it. "
        "Every engine integration test starts from a copy of it.",
    )
    sub.add_argument("--name", default="poni_test_template", help="the database's name; must start with poni_test_")
    sub.set_defaults(run=cmd_test_db)

    sub = commands.add_parser(
        "mock-google",
        help="serve a local mock of the Google APIs the engine uses, with a test portal (development only)",
        description="Listens on 127.0.0.1 only. The engine uses it when PONI_GOOGLE_MOCK_URL names it; give it a "
        "database of its own. Starts with the knowledge base's made-up test clients and a few made-up reviews each.",
    )
    with_knowledge(sub)
    sub.add_argument("--port", type=int, default=8090, help="the port on 127.0.0.1 (default 8090; 0: any free one)")
    sub.add_argument(
        "--state",
        type=Path,
        default=DEFAULT_MOCK_STATE,
        metavar="FILE",
        help=f"where the mock keeps its data between runs (default {DEFAULT_MOCK_STATE})",
    )
    sub.add_argument("--reset", action="store_true", help="start again from the made-up businesses and reviews")
    sub.set_defaults(run=cmd_mock_google)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.run(args, settings.load(args.config))
    except (
        knowledge.KnowledgeError,
        migrate.MigrationError,
        publish.PublishError,
        storage.StorageError,
        seed.SeedError,
        settings.SettingsError,
        testdb.TestDbError,
    ) as error:
        print(error, file=sys.stderr)
        return 1
    except psycopg.OperationalError as error:
        # The first line only: libpq's message may go on about hosts and options.
        print(f"database: {str(error).strip().splitlines()[0]}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
