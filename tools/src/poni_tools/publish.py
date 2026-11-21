"""Packs the knowledge base into one archive and publishes it (D-078 point 8).

    knowledge-<version>.tar.gz        for example knowledge-20261115.tar.gz

The version is the one the files name (manifest.json), which starts with the
day it was made (D-084). The owner publishes by hand, from a clean checkout
of `main`, and an archive is never replaced; production names the archive it
wants in poni.env, and its seed step downloads it, compares the version
inside with the name, checks it again and seeds.
"""

from __future__ import annotations

import gzip
import io
import re
import subprocess
import tarfile
import tempfile
from dataclasses import dataclass
from pathlib import Path

from . import knowledge
from .check import check
from .storage import Store

_NAME = re.compile(r"^knowledge-(\d{8}(?:-[1-9]\d?)?)\.tar\.gz$")

# What is packed: the data, and the schemas the receiving side checks it against.
_ROOT = "knowledge"


class PublishError(Exception):
    pass


@dataclass
class Archive:
    name: str
    version: str
    data: bytes


def archive_name(version: str) -> str:
    return f"knowledge-{version}.tar.gz"


def version_in(name: str) -> str:
    match = _NAME.match(name)
    if not match:
        raise PublishError(f"an archive is named knowledge-<version>.tar.gz, not '{name}'")
    return match[1]


def pack(folder: Path) -> Archive:
    """Checks the folder and packs it. The same content packs to the same bytes, whoever packs it and when."""
    kb = knowledge.load(folder)
    problems = check(kb)
    if problems:
        raise PublishError(
            "\n".join(problems) + f"\n\n{len(problems)} problem(s) in the knowledge base. Nothing was packed."
        )
    files = sorted(
        path for path in Path(folder).rglob("*") if path.is_file() and not any(p.startswith(".") for p in path.parts)
    )
    buffer = io.BytesIO()
    # No timestamps, owners or compression dates: two packs of one version are byte for byte the same.
    with tarfile.open(fileobj=buffer, mode="w", format=tarfile.PAX_FORMAT) as tar:
        for path in files:
            info = tarfile.TarInfo(f"{_ROOT}/{path.relative_to(folder).as_posix()}")
            data = path.read_bytes()
            info.size = len(data)
            info.mode = 0o644
            tar.addfile(info, io.BytesIO(data))
    data = gzip.compress(buffer.getvalue(), mtime=0)
    version = kb.version()
    return Archive(archive_name(version), version, data)


def unpack(archive_name_: str, data: bytes, into: Path) -> knowledge.Knowledge:
    """Unpacks an archive and returns the knowledge base in it, after comparing its content with its name."""
    expected = version_in(archive_name_)
    try:
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tar:
            # "data": no absolute paths, no links out of the folder, no devices.
            tar.extractall(into, filter="data")
    except (tarfile.TarError, OSError, EOFError) as error:
        raise PublishError(f"{archive_name_} is not an archive this can read ({error})") from None
    kb = knowledge.load(into / _ROOT)
    if kb.version() != expected:
        raise PublishError(
            f"{archive_name_} holds version {kb.version()}, not the version its name says. Nothing was seeded."
        )
    return kb


def clean_main_checkout(repository: Path) -> str | None:
    """Why this checkout must not be published from, or nothing. The owner publishes from a clean checkout of main."""

    def git(*args: str) -> str:
        return subprocess.run(
            ["git", "-C", str(repository), *args], check=True, capture_output=True, text=True
        ).stdout.strip()

    try:
        branch = git("rev-parse", "--abbrev-ref", "HEAD")
        dirty = git("status", "--porcelain")
    except (subprocess.CalledProcessError, FileNotFoundError):
        return f"{repository} is not a git checkout"
    if branch != "main":
        return f"this checkout is on '{branch}'; the knowledge base is published from main"
    if dirty:
        return "this checkout has changes that are not committed; the knowledge base is published from a clean checkout"
    return None


def publish(folder: Path, store: Store) -> Archive:
    archive = pack(folder)
    # Read back through the same door production uses, before anything is uploaded.
    with tempfile.TemporaryDirectory() as scratch:
        unpack(archive.name, archive.data, Path(scratch))
    store.write_new(archive.name, archive.data)
    return archive


def fetch(store: Store, name: str, into: Path) -> knowledge.Knowledge:
    version_in(name)  # a wrong name is said before anything is downloaded
    return unpack(name, store.read(name), into)
