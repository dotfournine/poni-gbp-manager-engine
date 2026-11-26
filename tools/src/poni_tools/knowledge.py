"""The knowledge base as files (D-078): reading the folder, and its version.

Layout of the folder:

    manifest.json                       {"format": 3}
    common/profile.json                 the severity flags of every industry
    industries/<key>/profile.json       topics, item types, severity flags
    industries/<key>/corpus.json        the test corpus
    industries/<key>/test_client.json   a made-up business the suites run as
    voices/<key>.json                   voice templates
    defaults.json                       content, reply, report and analysis settings
    models.json                         model prices
    schema/                             a JSON Schema per kind of file

Reading never fails on content: what is wrong with the files is `check`'s to say.
"""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

# The shape of the files this code, and the engine built with it, can read.
FORMAT = 3

PACK_FILES = ("profile.json", "corpus.json", "test_client.json")


class KnowledgeError(Exception):
    """The folder cannot be read at all: a file is missing or is not JSON."""


@dataclass
class Pack:
    key: str
    profile: dict[str, Any]
    corpus: dict[str, Any]
    test_client: dict[str, Any]


@dataclass
class Knowledge:
    root: Path
    manifest: dict[str, Any]
    common: dict[str, Any]
    industries: dict[str, Pack]
    voices: dict[str, dict[str, Any]]
    defaults: dict[str, Any]
    models: dict[str, Any]
    # Files in the folder that are not part of the layout, relative to it.
    strays: list[str] = field(default_factory=list)

    def content(self) -> dict[str, Any]:
        """Everything that is seeded, as one value."""
        return {
            "manifest": self.manifest,
            "common": self.common,
            "industries": {
                key: {"profile": pack.profile, "corpus": pack.corpus, "test_client": pack.test_client}
                for key, pack in sorted(self.industries.items())
            },
            "voices": dict(sorted(self.voices.items())),
            "defaults": self.defaults,
            "models": self.models,
        }

    def version(self) -> str:
        """The version the files name: the day they were made, "YYYYMMDD" or "YYYYMMDD-N" (D-084)."""
        return str(self.manifest["version"])

    def content_hash(self) -> str:
        """A hash of the content, whatever the files' whitespace. One version never holds two contents."""
        return hashlib.sha256(canonical(self.content()).encode()).hexdigest()[:16]


def canonical(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def _read(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        raise KnowledgeError(f"{path}: the file is missing") from None
    except (json.JSONDecodeError, UnicodeDecodeError) as error:
        raise KnowledgeError(f"{path}: not valid JSON ({error})") from None


def load(root: Path) -> Knowledge:
    root = Path(root)
    if not root.is_dir():
        raise KnowledgeError(f"{root}: no such folder")
    known: set[Path] = set()

    def read(relative: str) -> Any:
        known.add(root / relative)
        return _read(root / relative)

    manifest = read("manifest.json")
    common = read("common/profile.json")
    defaults = read("defaults.json")
    models = read("models.json")

    industries: dict[str, Pack] = {}
    industries_dir = root / "industries"
    for folder in sorted(p for p in industries_dir.iterdir() if p.is_dir()) if industries_dir.is_dir() else []:
        prefix = f"industries/{folder.name}"
        industries[folder.name] = Pack(
            key=folder.name,
            profile=read(f"{prefix}/profile.json"),
            corpus=read(f"{prefix}/corpus.json"),
            test_client=read(f"{prefix}/test_client.json"),
        )

    voices: dict[str, dict[str, Any]] = {}
    voices_dir = root / "voices"
    for path in sorted(voices_dir.glob("*.json")) if voices_dir.is_dir() else []:
        voices[path.stem] = read(f"voices/{path.name}")

    # A file with a mistyped name would otherwise be left out without a word.
    strays = sorted(
        str(path.relative_to(root))
        for path in root.rglob("*")
        if path.is_file()
        and path not in known
        and path.relative_to(root).parts[0] != "schema"
        and path.name != "README.md"
        and not path.name.startswith(".")
    )
    return Knowledge(root, manifest, common, industries, voices, defaults, models, strays)
