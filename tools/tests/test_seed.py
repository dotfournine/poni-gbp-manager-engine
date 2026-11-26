"""Seeding against a real PostgreSQL: the kb_* tables are replaced as a whole, or not touched at all."""

from __future__ import annotations

import datetime

import pytest

from poni_tools import cli, knowledge, migrate, seed
from poni_tools.settings import REPOSITORY

FOOD = "industries/food_beverage"
T1 = datetime.datetime(2026, 11, 14, 12, 0, tzinfo=datetime.UTC)
T2 = datetime.datetime(2026, 11, 15, 12, 0, tzinfo=datetime.UTC)


@pytest.fixture
def migrated(db):
    migrate.apply_migrations(db, migrate.load_migrations(REPOSITORY / "engine/migrations"))
    return db


def one(conn, sql: str):
    return conn.execute(sql).fetchone()[0]


def test_a_migrated_database_has_no_knowledge_until_it_is_seeded(migrated):
    for table in seed.TABLES:
        assert one(migrated, f"SELECT count(*) FROM {table}") == 0


def test_seed_fills_every_table(migrated, kb):
    files = knowledge.load(kb.root)
    result = seed.seed(migrated, files, T1)
    assert result.changed and result.previous is None and result.version == files.version()

    assert migrated.execute("SELECT version, format, seeded_at, content_hash FROM kb_meta").fetchall() == [
        (files.version(), 2, T1, files.content_hash())
    ]
    assert [r[0] for r in migrated.execute("SELECT key FROM kb_industries ORDER BY position")] == [
        "food_beverage",
        "wellness",
    ]
    assert one(migrated, "SELECT count(*) FROM kb_corpus_cases") == 71
    assert one(migrated, "SELECT count(*) FROM kb_voice_templates") == 3
    assert one(migrated, "SELECT count(*) FROM kb_model_prices") == 4
    assert one(migrated, "SELECT jsonb_array_length(severity_flags) FROM kb_common") == 4
    # The defaults are typed columns, one row per bundle (D-107); read back as JSON they are the file's objects.
    assert one(migrated, "SELECT to_jsonb(d) - 'id' FROM kb_content_defaults d") == files.defaults["content"]
    assert one(migrated, "SELECT to_jsonb(d) - 'id' FROM kb_reply_defaults d") == files.defaults["reply"]
    assert (
        one(migrated, "SELECT words_to_avoid FROM kb_content_defaults") == files.defaults["content"]["words_to_avoid"]
    )
    template = migrated.execute("SELECT content, reply FROM kb_voice_templates WHERE key = 'family_owned'").fetchone()
    assert template == (files.voices["family_owned"]["content"], files.voices["family_owned"]["reply"])
    assert one(migrated, "SELECT analysis ->> 'judge_model' FROM kb_defaults") == "claude-opus-5-5"
    assert one(migrated, "SELECT analysis ->> 'summary_model' FROM kb_defaults") == "qwen3.7-plus"
    assert one(migrated, "SELECT analysis ->> 'posts_model' FROM kb_defaults") == "qwen3.7-plus"
    # The baseline starts the posts task from the writer's provider and model, and the summary task too.
    assert one(
        migrated, "SELECT posts_model = generator_model AND posts_provider = generator_provider FROM ai_settings"
    )
    assert one(
        migrated, "SELECT summary_model = generator_model AND summary_provider = generator_provider FROM ai_settings"
    )
    row = migrated.execute(
        "SELECT label, topics, test_client ->> 'name' FROM kb_industries WHERE key = 'food_beverage'"
    ).fetchone()
    assert row[0] == "Food and beverage"
    assert row[1] == files.industries["food_beverage"].profile["topics"]
    assert row[2] == "Golden Harbor Buffet"
    # The corpus keeps the order of the file, which is the order a test runs it in.
    ids = [
        r[0]
        for r in migrated.execute(
            "SELECT case_id FROM kb_corpus_cases WHERE industry_key = 'food_beverage' ORDER BY position"
        )
    ]
    assert ids == [c["id"] for c in files.industries["food_beverage"].corpus["cases"]]
    assert one(
        migrated, "SELECT input_per_mtok::float8 FROM kb_model_prices WHERE model = 'qwen3.7-plus'"
    ) == pytest.approx(0.4)


def test_seeding_the_same_version_again_does_nothing(migrated, kb):
    seed.seed(migrated, knowledge.load(kb.root), T1)
    again = seed.seed(migrated, knowledge.load(kb.root), T2)
    assert not again.changed and again.previous == again.version
    assert one(migrated, "SELECT seeded_at FROM kb_meta") == T1


def test_a_reseed_replaces_everything(migrated, kb):
    first = seed.seed(migrated, knowledge.load(kb.root), T1)

    (kb.root / "voices/family_owned.json").unlink()
    kb.edit("models.json", lambda m: m["prices"].update({"new-model": {"input_per_mtok": 1, "output_per_mtok": 2}}))
    kb.edit(f"{FOOD}/profile.json", lambda p: p["topics"][0].update({"label": "Food quality, taste and temperature"}))
    kb.set_version("20270101")
    second = seed.seed(migrated, knowledge.load(kb.root), T2)

    assert second.changed and second.previous == first.version and second.version != first.version
    assert migrated.execute("SELECT version, seeded_at FROM kb_meta").fetchall() == [(second.version, T2)]
    assert [r[0] for r in migrated.execute("SELECT key FROM kb_voice_templates ORDER BY position")] == [
        "polished_hospitality",
        "warm_casual",
    ]
    assert one(migrated, "SELECT count(*) FROM kb_model_prices") == 5
    assert one(migrated, "SELECT topics -> 0 ->> 'label' FROM kb_industries WHERE key = 'food_beverage'") == (
        "Food quality, taste and temperature"
    )
    assert one(migrated, "SELECT count(*) FROM kb_corpus_cases") == 71, "the cases are replaced, not added again"


def test_an_industry_that_left_the_files_leaves_the_tables(migrated, kb):
    seed.seed(migrated, knowledge.load(kb.root), T1)
    for file in (kb.root / "industries/wellness").iterdir():
        file.unlink()
    (kb.root / "industries/wellness").rmdir()
    kb.set_version("20270101")
    seed.seed(migrated, knowledge.load(kb.root), T2)
    assert [r[0] for r in migrated.execute("SELECT key FROM kb_industries")] == ["food_beverage"]
    assert one(migrated, "SELECT count(*) FROM kb_corpus_cases WHERE industry_key = 'wellness'") == 0


def test_an_incomplete_pack_is_refused_and_nothing_changes(migrated, kb):
    first = seed.seed(migrated, knowledge.load(kb.root), T1)

    def thin(corpus):
        corpus["cases"] = [c for c in corpus["cases"] if c["expect"].get("severity_any") != ["hygiene"]]

    kb.edit(f"{FOOD}/corpus.json", thin)
    kb.edit("models.json", lambda m: m["prices"].clear())
    with pytest.raises(seed.SeedError) as refused:
        seed.seed(migrated, knowledge.load(kb.root), T2)
    message = str(refused.value)
    assert (
        "industries/food_beverage/corpus.json: the pack is not complete: the severity flag 'hygiene' has 0 case(s)"
        in message
    )
    assert "Nothing was seeded" in message
    assert migrated.execute("SELECT version, seeded_at FROM kb_meta").fetchall() == [(first.version, T1)]
    assert one(migrated, "SELECT count(*) FROM kb_model_prices") == 4


def test_a_database_that_was_not_migrated_says_so(db, kb):
    with pytest.raises(seed.SeedError, match="run `poni-tools migrate` first"):
        seed.seed(db, knowledge.load(kb.root), T1)


def test_a_fresh_database_is_brought_up_by_the_commands(db_url, db, kb, tmp_path, monkeypatch, capsys):
    monkeypatch.setenv("PONI_DATABASE_URL", db_url)
    monkeypatch.setenv("PONI_KNOWLEDGE_DIR", str(kb.root))
    monkeypatch.delenv("PONI_MIGRATIONS_DIR", raising=False)
    monkeypatch.setenv("HOME", str(tmp_path))  # no local engine.env

    assert cli.main(["migrate"]) == 0
    assert cli.main(["check"]) == 0
    assert cli.main(["seed"]) == 0
    version = knowledge.load(kb.root).version()
    assert f"Seeded the knowledge base, version {version}." in capsys.readouterr().out
    assert cli.main(["seed"]) == 0
    assert f"already at version {version}" in capsys.readouterr().out

    kb.edit("models.json", lambda m: m["prices"].update({"new-model": {"input_per_mtok": 1, "output_per_mtok": 2}}))
    assert cli.main(["seed"]) == 1
    assert f"its version is still {version}" in capsys.readouterr().err
    kb.set_version("20270101")
    assert cli.main(["seed"]) == 0
    assert f"version 20270101 (was {version})" in capsys.readouterr().out

    kb.set_version("20270102")
    kb.drop_ordinary_cases()
    assert cli.main(["seed"]) == 1
    assert "0 ordinary (control) case(s), needs 3" in capsys.readouterr().err
    assert one(db, "SELECT count(*) FROM kb_model_prices") == 5


def test_changed_content_under_the_same_version_is_refused(migrated, kb):
    first = seed.seed(migrated, knowledge.load(kb.root), T1)
    kb.edit("models.json", lambda m: m["prices"].update({"new-model": {"input_per_mtok": 1, "output_per_mtok": 2}}))
    with pytest.raises(
        seed.SeedError, match=f'its version is still {first.version}.*set a new "version" in manifest.json'
    ):
        seed.seed(migrated, knowledge.load(kb.root), T2)
    assert one(migrated, "SELECT count(*) FROM kb_model_prices") == 4


# ---- whom a new version changes (D-084): the seed says it, the engine watches nothing ----


def location(conn, title, industry, enabled=True):
    conn.execute(
        "INSERT INTO locations (gbp_location_id, gbp_account_id, title, industry_key, enabled, time_zone, raw, "
        "first_seen_at, last_seen_at, updated_at) VALUES (%s, 'a1', %s, %s, %s, 'America/New_York', '{}', now(), now(), now())",
        (f"loc-{title}", title, industry, enabled),
    )


def test_a_version_that_changes_what_the_analyzer_is_told_names_the_locations_to_test_again(migrated, kb, capsys):
    seed.seed(migrated, knowledge.load(kb.root), T1)
    location(migrated, "Willow - Midtown", "wellness")
    location(migrated, "Willow - Chelsea", "wellness")
    location(migrated, "Switched Off", "wellness", enabled=False)
    location(migrated, "Harbor", "food_beverage")
    location(migrated, "No Industry Yet", None)

    # Prices, a template, new corpus cases and a topic's label: nothing the analyzer is told.
    kb.set_version("20270101")
    kb.edit("models.json", lambda m: m["prices"].update({"new-model": {"input_per_mtok": 1, "output_per_mtok": 2}}))
    kb.edit("industries/wellness/profile.json", lambda p: p["topics"][0].update(label="Reworded for the dashboard"))
    quiet = seed.seed(migrated, knowledge.load(kb.root), T2)
    assert quiet.changed and quiet.retest == [] and quiet.stranded == []

    # Whether an item type is a person is for the report; the analyzer is not told it.
    kb.set_version("20270101-2")
    kb.edit("industries/wellness/profile.json", lambda p: p["item_types"][0].update(person=True))
    flagged = seed.seed(migrated, knowledge.load(kb.root), T2)
    assert flagged.changed and flagged.retest == []

    # A reworded definition of one of the wellness industry's flags.
    kb.set_version("20270102")
    kb.edit("industries/wellness/profile.json", lambda p: p["severity_flags"][0].update(description="Reworded."))
    changed = seed.seed(migrated, knowledge.load(kb.root), T2)
    assert [(x.title, x.industry) for x in changed.retest] == [
        ("Willow - Chelsea", "wellness"),
        ("Willow - Midtown", "wellness"),
    ]
    assert changed.stranded == []

    # A flag every industry shares: every industry's locations.
    kb.set_version("20270103")
    kb.edit("common/profile.json", lambda p: p["severity_flags"][0].update(description="Reworded."))
    everyone = seed.seed(migrated, knowledge.load(kb.root), T2)
    assert [x.title for x in everyone.retest] == ["Harbor", "Willow - Chelsea", "Willow - Midtown"]


def test_the_command_prints_whom_a_version_changes(db_url, db, kb, tmp_path, monkeypatch, capsys):
    monkeypatch.setenv("PONI_DATABASE_URL", db_url)
    monkeypatch.setenv("PONI_KNOWLEDGE_DIR", str(kb.root))
    monkeypatch.delenv("PONI_MIGRATIONS_DIR", raising=False)
    monkeypatch.setenv("HOME", str(tmp_path))
    assert cli.main(["migrate"]) == 0
    assert cli.main(["seed"]) == 0
    location(db, "Willow - Midtown", "wellness")
    location(db, "Harbor", "food_beverage")
    capsys.readouterr()

    kb.set_version("20270101")
    kb.edit("industries/food_beverage/profile.json", lambda p: p.update(label="Restaurants"))
    for file in (kb.root / "industries/wellness").iterdir():
        file.unlink()
    (kb.root / "industries/wellness").rmdir()
    assert cli.main(["seed"]) == 0
    out = capsys.readouterr().out
    assert "none of them auto replies until a new go-live test has passed" in out
    assert "  - Harbor (food_beverage)" in out
    assert "no longer has the industry of these locations" in out
    assert "  - Willow - Midtown (wellness)" in out

    kb.set_version("20270102")
    kb.edit("models.json", lambda m: m["prices"].update({"new-model": {"input_per_mtok": 1, "output_per_mtok": 2}}))
    assert cli.main(["seed"]) == 0
    assert "No location needs a new go-live test because of it." in capsys.readouterr().out
