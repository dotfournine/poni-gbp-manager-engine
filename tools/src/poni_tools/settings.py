"""Where the scripts get their settings: the same PONI_* names the engine reads, from the same places.

The environment first; then a KEY=VALUE file named by --config or PONI_CONFIG;
without either, ~/.config/poni-gbp/engine.env if it exists (local development,
where secrets live outside the repository). Values are never printed.
"""

from __future__ import annotations

import os
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[3]
DEFAULT_DATABASE_URL = "postgresql://poni:poni@127.0.0.1:55432/poni"


class SettingsError(Exception):
    pass


def parse_env_file(text: str) -> dict[str, str]:
    """KEY=VALUE lines; '#' comments; one pair of quotes around a value is dropped. As the engine reads it."""
    values: dict[str, str] = {}
    for number, raw in enumerate(text.split("\n"), start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        key, separator, value = line.partition("=")
        if not separator or not key.strip():
            raise SettingsError(f"line {number}: expected KEY=VALUE")
        value = value.strip()
        if len(value) >= 2 and value[0] in "\"'" and value[-1] == value[0]:
            value = value[1:-1]
        values[key.strip()] = value
    return values


@dataclass
class Settings:
    database_url: str
    migrations_dir: Path
    knowledge_dir: Path
    # Where tests make their throwaway schemas and databases; never the address of real data.
    test_database_url: str
    # Where published knowledge archives are kept, and the one this installation runs on
    # ("knowledge-20261115.tar.gz"). Both empty: seed from knowledge_dir.
    knowledge_bucket: str
    knowledge_archive: str


def load(config: Path | None = None, env: Mapping[str, str] | None = None) -> Settings:
    env = os.environ if env is None else env
    # The file is found as the engine finds it (core::default_config_file): keep the two in step.
    if config is None and env.get("PONI_CONFIG"):
        config = Path(env["PONI_CONFIG"])
    if config is None and env.get("HOME"):
        local = Path(env["HOME"]) / ".config/poni-gbp/engine.env"
        if local.is_file():
            config = local
    from_file: dict[str, str] = {}
    if config is not None:
        try:
            from_file = parse_env_file(config.read_text(encoding="utf-8"))
        except OSError:
            raise SettingsError(f"cannot read config file {config}") from None
        except SettingsError as error:
            raise SettingsError(f"{config}: {error}") from None

    def get(key: str) -> str | None:
        # Empty values count as unset, as in the engine.
        return env.get(key) or from_file.get(key) or None

    return Settings(
        database_url=get("PONI_DATABASE_URL") or DEFAULT_DATABASE_URL,
        migrations_dir=Path(get("PONI_MIGRATIONS_DIR") or REPOSITORY / "engine/migrations"),
        knowledge_dir=Path(get("PONI_KNOWLEDGE_DIR") or REPOSITORY / "knowledge"),
        # From the environment only, as the engine's tests read it: not a setting of the engine.
        test_database_url=env.get("PONI_TEST_DATABASE_URL") or DEFAULT_DATABASE_URL,
        knowledge_bucket=get("PONI_KNOWLEDGE_BUCKET") or "",
        knowledge_archive=get("PONI_KNOWLEDGE_ARCHIVE") or "",
    )
