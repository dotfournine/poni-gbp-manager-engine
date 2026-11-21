"""Packing, publishing and seeding from a published archive. A folder stands in for the bucket."""

from __future__ import annotations

import gzip
import io
import subprocess
import tarfile

import pytest

from poni_tools import cli, migrate, publish, storage
from poni_tools.settings import REPOSITORY


def one(conn, sql: str):
    return conn.execute(sql).fetchone()[0]


@pytest.fixture
def bucket(tmp_path):
    return tmp_path / "bucket"


@pytest.fixture
def production(db_url, db, bucket, tmp_path, monkeypatch):
    """A migrated database and the settings of an installation that seeds from the bucket, with no knowledge folder."""
    migrate.apply_migrations(db, migrate.load_migrations(REPOSITORY / "engine/migrations"))
    monkeypatch.setenv("PONI_DATABASE_URL", db_url)
    monkeypatch.setenv("PONI_KNOWLEDGE_BUCKET", str(bucket))
    monkeypatch.setenv("PONI_KNOWLEDGE_DIR", str(tmp_path / "no-knowledge-folder-on-the-server"))
    monkeypatch.setenv("HOME", str(tmp_path))
    return db


def test_the_same_content_packs_to_the_same_bytes_under_a_name_with_its_version(kb):
    kb.set_version("20261114-2")
    first = publish.pack(kb.root)
    assert first.name == "knowledge-20261114-2.tar.gz" and first.version == "20261114-2"
    assert publish.pack(kb.root).data == first.data
    with tarfile.open(fileobj=io.BytesIO(first.data)) as tar:
        names = tar.getnames()
    assert "knowledge/manifest.json" in names
    assert "knowledge/industries/food_beverage/corpus.json" in names
    assert "knowledge/schema/corpus.schema.json" in names, (
        "the receiving side checks against the schemas it was packed with"
    )


def test_a_knowledge_base_with_a_problem_is_not_packed(kb):
    kb.drop_ordinary_cases()
    with pytest.raises(publish.PublishError, match="0 ordinary .control. case.s., needs 3"):
        publish.pack(kb.root)


def test_publishing_then_seeding_what_the_installation_names(kb, bucket, production, capsys):
    archive = publish.publish(kb.root, storage.store_for(str(bucket)))
    assert (bucket / archive.name).read_bytes() == archive.data

    # The server: one line in poni.env, one command.
    assert cli.main(["seed", "--archive", archive.name]) == 0
    out = capsys.readouterr().out
    assert f"Read {archive.name} from {bucket}." in out
    assert f"Seeded the knowledge base, version {archive.version}." in out
    assert one(production, "SELECT version FROM kb_meta") == archive.version
    assert one(production, "SELECT count(*) FROM kb_industries") == 2


def test_an_update_is_a_new_archive_named_in_the_settings(kb, bucket, production, monkeypatch, capsys):
    store = storage.store_for(str(bucket))
    first = publish.publish(kb.root, store)
    monkeypatch.setenv("PONI_KNOWLEDGE_ARCHIVE", first.name)
    assert cli.main(["seed"]) == 0

    kb.edit("models.json", lambda m: m["prices"].update({"new-model": {"input_per_mtok": 1, "output_per_mtok": 2}}))
    kb.set_version("20270101")
    second = publish.publish(kb.root, store)
    assert second.name != first.name
    assert cli.main(["seed"]) == 0
    assert "already at version" in capsys.readouterr().out, (
        "nothing changes until the installation names the new archive"
    )

    monkeypatch.setenv("PONI_KNOWLEDGE_ARCHIVE", second.name)
    assert cli.main(["seed"]) == 0
    assert f"version {second.version} (was {first.version})" in capsys.readouterr().out
    assert one(production, "SELECT count(*) FROM kb_model_prices") == 5

    # Going back is naming the older archive again.
    monkeypatch.setenv("PONI_KNOWLEDGE_ARCHIVE", first.name)
    assert cli.main(["seed"]) == 0
    assert one(production, "SELECT version FROM kb_meta") == first.version


def test_an_archive_that_does_not_hold_what_its_name_says_is_refused(kb, bucket, production, capsys):
    archive = publish.publish(kb.root, storage.store_for(str(bucket)))
    kb.set_version("20270101")
    other = publish.pack(kb.root)
    (bucket / archive.name).write_bytes(other.data)  # another version under the published name

    assert cli.main(["seed", "--archive", archive.name]) == 1
    assert "not the version its name says" in capsys.readouterr().err
    assert one(production, "SELECT count(*) FROM kb_meta") == 0


def test_a_damaged_archive_a_missing_one_and_a_wrong_name(kb, bucket, production, capsys):
    archive = publish.publish(kb.root, storage.store_for(str(bucket)))
    (bucket / archive.name).write_bytes(archive.data[: len(archive.data) // 2])
    assert cli.main(["seed", "--archive", archive.name]) == 1
    assert "is not an archive this can read" in capsys.readouterr().err

    missing = publish.archive_name("20990101")
    assert cli.main(["seed", "--archive", missing]) == 1
    assert f"has no archive named {missing}" in capsys.readouterr().err

    assert cli.main(["seed", "--archive", "latest.tar.gz"]) == 1
    assert "is named knowledge-<version>.tar.gz" in capsys.readouterr().err
    assert one(production, "SELECT count(*) FROM kb_meta") == 0


def test_an_archive_cannot_write_outside_its_folder(tmp_path):
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w") as tar:
        info = tarfile.TarInfo("../escaped.json")
        info.size = 2
        tar.addfile(info, io.BytesIO(b"{}"))
    name = publish.archive_name("20990101")
    with pytest.raises(publish.PublishError, match="is not an archive this can read"):
        publish.unpack(name, gzip.compress(buffer.getvalue()), tmp_path / "into")
    assert not (tmp_path / "escaped.json").exists()


def test_a_published_archive_is_never_replaced(kb, bucket):
    store = storage.store_for(str(bucket))
    publish.publish(kb.root, store)
    with pytest.raises(storage.StorageError, match="a published archive is never replaced"):
        publish.publish(kb.root, store)


def test_an_archive_without_a_bucket_is_a_wrong_command_line(kb, production, monkeypatch, capsys):
    monkeypatch.delenv("PONI_KNOWLEDGE_BUCKET")
    assert cli.main(["seed", "--archive", publish.archive_name("20990101")]) == 2
    assert "an archive needs a bucket" in capsys.readouterr().err


def test_where_archives_are_kept():
    assert isinstance(storage.store_for("poni-gbp-knowledge"), storage.BucketStore)
    assert storage.store_for("gs://poni-gbp-knowledge/").description == "gs://poni-gbp-knowledge"
    assert isinstance(storage.store_for("/srv/knowledge"), storage.FolderStore)
    assert isinstance(storage.store_for("./published"), storage.FolderStore)
    assert isinstance(storage.store_for("file:///srv/knowledge"), storage.FolderStore)


def git(repository, *args):
    subprocess.run(["git", "-C", str(repository), *args], check=True, capture_output=True)


@pytest.fixture
def checkout(kb, tmp_path):
    """The knowledge folder inside a git checkout of its own, on main, with everything committed."""
    repository = kb.root.parent
    git(repository, "init", "-q", "-b", "main")
    git(repository, "-c", "user.name=Test", "-c", "user.email=test@localhost", "add", "-A")
    git(repository, "-c", "user.name=Test", "-c", "user.email=test@localhost", "commit", "-q", "-m", "knowledge")
    return repository


def test_the_owner_publishes_from_a_clean_checkout_of_main(kb, checkout, bucket, monkeypatch, capsys):
    monkeypatch.setenv("HOME", str(checkout))
    assert cli.main(["publish", "--knowledge", str(kb.root), "--bucket", str(bucket)]) == 0
    out = capsys.readouterr().out
    name = next(bucket.iterdir()).name
    assert f"PONI_KNOWLEDGE_ARCHIVE={name}" in out

    kb.edit("models.json", lambda m: m["prices"].update({"not-committed": {"input_per_mtok": 1, "output_per_mtok": 1}}))
    assert cli.main(["publish", "--knowledge", str(kb.root), "--bucket", str(bucket)]) == 1
    assert "changes that are not committed" in capsys.readouterr().err

    git(checkout, "checkout", "-q", "-b", "a-branch")
    git(checkout, "-c", "user.name=Test", "-c", "user.email=test@localhost", "commit", "-q", "-am", "on a branch")
    assert cli.main(["publish", "--knowledge", str(kb.root), "--bucket", str(bucket)]) == 1
    assert "is on 'a-branch'; the knowledge base is published from main" in capsys.readouterr().err
    assert len(list(bucket.iterdir())) == 1

    assert cli.main(["publish", "--knowledge", str(kb.root)]) == 2
    assert "say where to publish" in capsys.readouterr().err


def test_pack_writes_the_archive_without_publishing(kb, tmp_path, monkeypatch, capsys):
    monkeypatch.setenv("HOME", str(tmp_path))
    assert cli.main(["pack", "--knowledge", str(kb.root), "--out", str(tmp_path / "out")]) == 0
    assert "Packed version" in capsys.readouterr().out
    assert len(list((tmp_path / "out").glob("knowledge-*.tar.gz"))) == 1


# ---- the Cloud Storage bucket, against a stand-in for Google's client ----------------
# No test reaches Google. What is real here is this code's use of the client: which object
# it names, that an upload refuses to replace one, and what a person is told when it fails.


class FakeBlob:
    def __init__(self, objects: dict[str, bytes], name: str) -> None:
        self.objects, self.name = objects, name

    def download_as_bytes(self) -> bytes:
        from google.api_core import exceptions

        if self.name not in self.objects:
            raise exceptions.NotFound(self.name)
        return self.objects[self.name]

    def upload_from_string(self, data: bytes, content_type: str, if_generation_match: int) -> None:
        from google.api_core import exceptions

        assert content_type == "application/gzip"
        assert if_generation_match == 0, "an upload must only succeed where no object exists"
        if self.name in self.objects:
            raise exceptions.PreconditionFailed(self.name)
        self.objects[self.name] = data


class FakeBucket:
    def __init__(self) -> None:
        self.objects: dict[str, bytes] = {}

    def blob(self, name: str) -> FakeBlob:
        return FakeBlob(self.objects, name)


@pytest.fixture
def gcs(monkeypatch):
    fake = FakeBucket()
    monkeypatch.setattr(storage.BucketStore, "_bucket", lambda self: fake)
    return fake


def test_publishing_to_a_bucket_and_seeding_from_it(kb, gcs, production, monkeypatch, capsys):
    monkeypatch.setenv("PONI_KNOWLEDGE_BUCKET", "poni-gbp-knowledge")
    store = storage.store_for("gs://poni-gbp-knowledge")
    archive = publish.publish(kb.root, store)
    assert list(gcs.objects) == [archive.name]

    with pytest.raises(storage.StorageError, match="gs://poni-gbp-knowledge already has an archive"):
        publish.publish(kb.root, store)

    monkeypatch.setenv("PONI_KNOWLEDGE_ARCHIVE", archive.name)
    assert cli.main(["seed"]) == 0
    assert f"Read {archive.name} from gs://poni-gbp-knowledge." in capsys.readouterr().out
    assert one(production, "SELECT version FROM kb_meta") == archive.version

    monkeypatch.setenv("PONI_KNOWLEDGE_ARCHIVE", publish.archive_name("20990101"))
    assert cli.main(["seed"]) == 1
    assert "gs://poni-gbp-knowledge has no archive named" in capsys.readouterr().err


def test_a_bucket_that_cannot_be_reached_is_said_in_one_line(gcs, production, monkeypatch, capsys):
    from google.api_core import exceptions

    def refuse(self):
        raise exceptions.Forbidden("the caller does not have storage.objects.get access")

    monkeypatch.setattr(FakeBlob, "download_as_bytes", refuse)
    assert cli.main(["seed", "--archive", publish.archive_name("20990101"), "--bucket", "poni-gbp-knowledge"]) == 1
    err = capsys.readouterr().err
    assert "could not be read" in err and "storage.objects.get" in err
    assert one(production, "SELECT count(*) FROM kb_meta") == 0
