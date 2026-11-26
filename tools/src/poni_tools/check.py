"""Checks the knowledge base before it is seeded or published (D-078).

Three kinds of check, in this order:
  1. every file against its JSON Schema (unknown keys are refused);
  2. the files against each other (a corpus case names a flag its industry has, a template that exists, ...);
  3. the "complete pack" rule: an industry is supported only with enough test cases.

`check` returns every problem it finds, each naming the file, so one run shows all there is to fix.
"""

from __future__ import annotations

import datetime
import json
from collections import Counter
from collections.abc import Iterable
from pathlib import Path
from typing import Any

from jsonschema import Draft202012Validator
from referencing import Registry, Resource

from .knowledge import FORMAT, Knowledge

# The complete pack (D-078, point 6).
MIN_CASES_PER_FLAG = 2
MIN_MANIPULATION = 10
MIN_CONTROL = 3
CHINESE = ("zh", "zh-Hant")
REPLY_LANGUAGES = ("en", "zh", "es")
LENGTH_MODES = ("rating_only", "positive", "complaint", "severity")

SCHEMA_BASE = "https://poni-gbp.invalid/knowledge/"


def _validators(schema_dir: Path) -> dict[str, Draft202012Validator]:
    schemas = {}
    for path in sorted(schema_dir.glob("*.schema.json")):
        schemas[path.name.removesuffix(".schema.json")] = json.loads(path.read_text(encoding="utf-8"))
    registry = Registry().with_resources(
        (SCHEMA_BASE + f"{name}.schema.json", Resource.from_contents(schema)) for name, schema in schemas.items()
    )
    return {name: Draft202012Validator(schema, registry=registry) for name, schema in schemas.items()}


def _where(error) -> str:
    path = "".join(f"[{part}]" if isinstance(part, int) else f".{part}" for part in error.absolute_path)
    return path.lstrip(".") or "(the whole file)"


def _schema_problems(validator: Draft202012Validator, file: str, value: Any) -> list[str]:
    return [
        f"{file}: {_where(e)}: {e.message}"
        for e in sorted(validator.iter_errors(value), key=lambda e: list(map(str, e.absolute_path)))
    ]


def _duplicates(values: Iterable[str]) -> list[str]:
    return sorted(value for value, count in Counter(values).items() if count > 1)


def _applicable_flags(profile: dict, common: dict) -> list[str]:
    """The flags the analyzer may raise for this industry: its own and the common ones, enabled only."""
    return [f["id"] for f in profile["severity_flags"] + common["severity_flags"] if f["enabled"]]


def _reply_problems(file: str, reply: dict, complete: bool) -> list[str]:
    """The reply settings' rules the schema cannot say: length ranges, the delay, the window, auto reply off."""
    problems = []
    length = reply.get("length", {})
    for language, limits in length.items():
        for mode in LENGTH_MODES:
            r = limits.get(mode)
            if r and r["min"] > r["max"]:
                problems.append(f"{file}: reply.length.{language}.{mode}: min is greater than max")
    if complete:
        for language in REPLY_LANGUAGES:
            if language not in length:
                problems.append(f"{file}: reply.length: the defaults need limits for '{language}'")
    low, high = reply.get("auto_reply_delay_min_minutes"), reply.get("auto_reply_delay_max_minutes")
    if low is not None and high is not None and low > high:
        problems.append(f"{file}: reply: auto_reply_delay_min_minutes is greater than auto_reply_delay_max_minutes")
    start, end = reply.get("auto_reply_window_start"), reply.get("auto_reply_window_end")
    if start is not None and end is not None:
        if bool(start) != bool(end):
            problems.append(f"{file}: reply: set both ends of the posting window, or leave both empty")
        elif start and start == end:
            problems.append(f"{file}: reply: the posting window's start and end must differ")
    # Only a person switches auto reply on (D-110): defaults or a template that did would do it for them.
    if reply.get("auto_reply_enabled") is True:
        problems.append(f"{file}: reply: auto_reply_enabled must be false; only a person switches auto reply on")
    return problems


def _case_problems(file: str, case: dict, flags: list[str]) -> list[str]:
    problems = []
    where = f"{file}: case {case['id']}"
    expect = case["expect"]
    named = expect.get("severity_any", [])
    for flag in named:
        if flag not in flags:
            problems.append(
                f"{where}: expects the flag '{flag}', which this industry does not have (or has switched off)"
            )
    if case["category"] == "severity":
        if not named:
            problems.append(f"{where}: a severity case must name the flag it expects (expect.severity_any)")
        if expect["action"] != "escalate":
            problems.append(f"{where}: a severity case must expect the action 'escalate'")
        if expect["manipulation"]:
            problems.append(f"{where}: a severity case that also steers the reply belongs under 'manipulation'")
    elif case["category"] == "manipulation":
        if not expect["manipulation"]:
            problems.append(f"{where}: a manipulation case must expect manipulation: true")
        if expect["action"] == "auto_reply":
            problems.append(f"{where}: a manipulation case must not expect 'auto_reply'")
    else:
        if named or expect["manipulation"]:
            problems.append(f"{where}: an ordinary (control) case expects no flag and no manipulation")
        if expect["action"] == "escalate":
            problems.append(f"{where}: an ordinary (control) case must not expect 'escalate'")
    return problems


def _completeness(file: str, corpus: dict, flags: list[str]) -> list[str]:
    """What the pack lacks to be complete; one line per thing that is missing."""
    cases = corpus["cases"]
    missing = []
    per_flag = Counter(
        flag for c in cases if c["category"] == "severity" for flag in c["expect"].get("severity_any", [])
    )
    for flag in flags:
        if per_flag[flag] < MIN_CASES_PER_FLAG:
            missing.append(f"the severity flag '{flag}' has {per_flag[flag]} case(s) and needs {MIN_CASES_PER_FLAG}")
    manipulation = [c for c in cases if c["category"] == "manipulation"]
    if len(manipulation) < MIN_MANIPULATION:
        missing.append(f"{len(manipulation)} manipulation case(s), needs {MIN_MANIPULATION}")
    if not any(c["lang"] in CHINESE for c in manipulation):
        missing.append("no manipulation case in Chinese (lang zh or zh-Hant)")
    if not any(c["lang"] == "mixed" for c in manipulation):
        missing.append("no manipulation case in mixed language (lang mixed)")
    control = sum(c["category"] == "control" for c in cases)
    if control < MIN_CONTROL:
        missing.append(f"{control} ordinary (control) case(s), needs {MIN_CONTROL}")

    return [f"{file}: the pack is not complete: {m}" for m in missing]


def check(kb: Knowledge) -> list[str]:
    """Every problem in the knowledge base; an empty list means it may be seeded."""
    validators = _validators(kb.root / "schema")
    needed = (
        "manifest",
        "common",
        "profile",
        "corpus",
        "test_client",
        "voice_template",
        "defaults",
        "models",
        "settings",
    )
    absent = [name for name in needed if name not in validators]
    if absent:
        return [f"schema/{name}.schema.json: the schema is missing" for name in absent]

    problems = [f"{stray}: not part of the knowledge base's layout (a mistyped name?)" for stray in kb.strays]

    files: list[tuple[str, str, Any]] = [
        ("manifest", "manifest.json", kb.manifest),
        ("common", "common/profile.json", kb.common),
        ("defaults", "defaults.json", kb.defaults),
        ("models", "models.json", kb.models),
    ]
    for key, pack in kb.industries.items():
        files += [
            ("profile", f"industries/{key}/profile.json", pack.profile),
            ("corpus", f"industries/{key}/corpus.json", pack.corpus),
            ("test_client", f"industries/{key}/test_client.json", pack.test_client),
        ]
    files += [("voice_template", f"voices/{key}.json", voice) for key, voice in kb.voices.items()]
    for schema, file, value in files:
        problems += _schema_problems(validators[schema], file, value)
    # The cross-checks read fields the schemas guarantee, so they wait until the schemas pass.
    if problems:
        return problems

    if kb.manifest["format"] != FORMAT:
        problems.append(
            f"manifest.json: format {kb.manifest['format']} is not the format these scripts read ({FORMAT})"
        )
    try:
        datetime.datetime.strptime(kb.manifest["version"][:8], "%Y%m%d")
    except ValueError:
        problems.append(f"manifest.json: the version '{kb.manifest['version']}' does not start with a day, YYYYMMDD")
    if not kb.industries:
        problems.append("industries/: there is no industry pack")

    common_flags = [f["id"] for f in kb.common["severity_flags"]]
    for duplicate in _duplicates(common_flags):
        problems.append(f"common/profile.json: the flag '{duplicate}' is listed twice")

    case_ids: Counter[str] = Counter()
    for key, pack in kb.industries.items():
        file = f"industries/{key}/profile.json"
        if pack.profile["key"] != key:
            problems.append(f"{file}: key is '{pack.profile['key']}' but the folder is '{key}'")
        for part in ("topics", "item_types", "severity_flags"):
            for duplicate in _duplicates(entry["id"] for entry in pack.profile[part]):
                problems.append(f"{file}: {part}: the id '{duplicate}' is listed twice")
        for flag in pack.profile["severity_flags"]:
            if flag["id"] in common_flags:
                problems.append(
                    f"{file}: the flag '{flag['id']}' is already a flag of every industry (common/profile.json)"
                )

        corpus_file = f"industries/{key}/corpus.json"
        flags = _applicable_flags(pack.profile, kb.common)
        for duplicate in _duplicates(c["id"] for c in pack.corpus["cases"]):
            problems.append(f"{corpus_file}: the case id '{duplicate}' is used twice")
        for case in pack.corpus["cases"]:
            problems += _case_problems(corpus_file, case, flags)
        case_ids.update({c["id"] for c in pack.corpus["cases"]})
        problems += _completeness(corpus_file, pack.corpus, flags)

        client_file = f"industries/{key}/test_client.json"
        template = pack.test_client.get("template")
        if template is not None and template not in kb.voices:
            problems.append(f"{client_file}: the voice template '{template}' does not exist (voices/{template}.json)")
        problems += _reply_problems(client_file, pack.test_client["reply"], complete=False)

    for case_id, count in sorted(case_ids.items()):
        if count > 1:
            problems.append(
                f"industries/: the case id '{case_id}' is used in {count} packs; ids are unique across packs"
            )

    for key, template in kb.voices.items():
        file = f"voices/{key}.json"
        if template["key"] != key:
            problems.append(f"{file}: key is '{template['key']}' but the file is '{key}.json'")
        problems += _reply_problems(file, template["reply"], complete=False)

    problems += _reply_problems("defaults.json", kb.defaults["reply"], complete=True)
    if kb.defaults["reply"]["fallback_language"] not in kb.defaults["reply"]["reply_languages"]:
        problems.append("defaults.json: reply.fallback_language is not one of reply_languages")
    for task in ("analyzer", "generator", "judge", "summary", "posts"):
        model = kb.defaults["analysis"][f"{task}_model"]
        if model not in kb.models["prices"]:
            problems.append(f"defaults.json: analysis.{task}_model '{model}' has no price in models.json")
    return problems
