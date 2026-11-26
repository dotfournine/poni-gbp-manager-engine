from __future__ import annotations

import shutil

import pytest

from poni_tools import cli, knowledge
from poni_tools.check import check

FOOD = "industries/food_beverage"


def problems(kb) -> list[str]:
    return check(knowledge.load(kb.root))


def only(found: list[str], *parts: str) -> str:
    assert len(found) == 1, found
    for part in parts:
        assert part in found[0], found[0]
    return found[0]


def test_the_repositorys_knowledge_base_passes_once_it_is_signed(kb):
    assert problems(kb) == []


def test_the_version_is_the_one_the_files_name_and_the_hash_follows_the_content(kb):
    kb.set_version("20270203-2")
    before = knowledge.load(kb.root)
    assert before.version() == "20270203-2"
    path = kb.root / "models.json"
    path.write_text(path.read_text() + "\n\n")
    assert knowledge.load(kb.root).content_hash() == before.content_hash(), "whitespace is not content"
    kb.edit("models.json", lambda m: m["prices"].update({"new-model": {"input_per_mtok": 1, "output_per_mtok": 2}}))
    assert knowledge.load(kb.root).content_hash() != before.content_hash()


@pytest.mark.parametrize("version", ["be626a02df1b", "2026-11-15", "20261115-0", "v1", ""])
def test_a_version_that_is_not_a_day_is_refused(kb, version):
    kb.set_version(version)
    only(problems(kb), "manifest.json", "version")


def test_a_version_whose_day_does_not_exist_is_refused(kb):
    kb.set_version("20261345")
    only(problems(kb), "the version '20261345' does not start with a day")


def test_an_unknown_key_is_refused(kb):
    kb.edit(f"{FOOD}/profile.json", lambda p: p["topics"][0].update({"slow refills)": None}))
    only(problems(kb), f"{FOOD}/profile.json", "topics[0]", "slow refills)")


def test_a_stray_file_is_reported(kb):
    kb.write("voices/warm_casual.jsn", {})
    only(problems(kb), "voices/warm_casual.jsn", "not part of")


def test_a_new_case_is_accepted(kb):
    def add(corpus):
        corpus["cases"].append(
            {
                "id": "C-99",
                "category": "control",
                "stars": 5,
                "lang": "en",
                "text": "Lovely.",
                "expect": {"manipulation": False, "action": "auto_reply"},
            }
        )

    kb.edit(f"{FOOD}/corpus.json", add)
    assert problems(kb) == []


def test_a_flag_with_one_case_is_named(kb):
    def drop(corpus):
        hygiene = [c for c in corpus["cases"] if c["expect"].get("severity_any") == ["hygiene"]]
        corpus["cases"].remove(hygiene[0])

    kb.edit(f"{FOOD}/corpus.json", drop)
    only(problems(kb), "not complete", "'hygiene' has 1 case(s) and needs 2")


def test_a_new_flag_in_the_common_profile_makes_every_pack_incomplete(kb):
    kb.edit(
        "common/profile.json",
        lambda p: p["severity_flags"].append(
            {"id": "privacy_breach", "label": "Privacy", "description": "x", "enabled": True}
        ),
    )
    found = problems(kb)
    assert len(found) == 2, found
    assert all("'privacy_breach' has 0 case(s) and needs 2" in p for p in found)


def test_a_flag_that_is_switched_off_needs_no_cases_and_may_not_be_expected(kb):
    def off(profile):
        next(f for f in profile["severity_flags"] if f["id"] == "hygiene")["enabled"] = False

    kb.edit(f"{FOOD}/profile.json", off)
    found = problems(kb)
    assert found and all("expects the flag 'hygiene'" in p for p in found), found


def test_too_few_manipulation_cases_and_no_chinese_or_mixed_one(kb):
    def thin(corpus):
        keep = [c for c in corpus["cases"] if c["category"] == "manipulation" and c["lang"] == "en"][:4]
        corpus["cases"] = [c for c in corpus["cases"] if c["category"] != "manipulation"] + keep

    kb.edit(f"{FOOD}/corpus.json", thin)
    found = problems(kb)
    assert len(found) == 3, found
    assert "4 manipulation case(s), needs 10" in found[0]
    assert "no manipulation case in Chinese" in found[1]
    assert "no manipulation case in mixed language" in found[2]


def test_too_few_ordinary_cases(kb):
    def thin(corpus):
        control = [c for c in corpus["cases"] if c["category"] == "control"]
        for case in control[2:]:
            corpus["cases"].remove(case)

    kb.edit(f"{FOOD}/corpus.json", thin)
    only(problems(kb), "2 ordinary (control) case(s), needs 3")


def test_a_new_industry_without_a_corpus_cannot_be_read(kb, capsys):
    (kb.root / "industries/dental").mkdir()
    kb.write("industries/dental/profile.json", kb.read(f"{FOOD}/profile.json") | {"key": "dental"})
    assert cli.main(["check", "--knowledge", str(kb.root)]) == 1
    assert "industries/dental/corpus.json: the file is missing" in capsys.readouterr().err


def test_a_new_industry_with_a_thin_corpus_is_refused_with_what_is_missing(kb, capsys):
    shutil.copytree(kb.root / FOOD, kb.root / "industries/dental")
    kb.edit("industries/dental/profile.json", lambda p: p.update({"key": "dental"}))

    def thin(corpus):
        corpus["cases"] = [c | {"id": "D-" + c["id"]} for c in corpus["cases"] if c["category"] == "control"]

    kb.edit("industries/dental/corpus.json", thin)
    assert cli.main(["check", "--knowledge", str(kb.root)]) == 1
    err = capsys.readouterr().err
    assert "industries/dental/corpus.json: the pack is not complete: the severity flag 'hygiene' has 0 case(s)" in err
    assert "0 manipulation case(s), needs 10" in err
    assert "Nothing can be seeded or published" in err


def test_a_case_id_used_in_two_packs(kb):
    def rename(corpus):
        next(c for c in corpus["cases"] if c["category"] == "control")["id"] = "C-01"

    kb.edit("industries/wellness/corpus.json", rename)
    only(problems(kb), "'C-01' is used in 2 packs")


def test_a_case_that_contradicts_its_category(kb):
    def wrong(corpus):
        next(c for c in corpus["cases"] if c["category"] == "manipulation")["expect"]["action"] = "auto_reply"

    kb.edit(f"{FOOD}/corpus.json", wrong)
    only(problems(kb), "a manipulation case must not expect 'auto_reply'")


def test_the_folder_and_the_key_must_agree(kb):
    kb.edit(f"{FOOD}/profile.json", lambda p: p.update({"key": "food"}))
    only(problems(kb), "key is 'food' but the folder is 'food_beverage'")


def test_a_test_client_names_a_template_that_exists(kb):
    kb.edit(f"{FOOD}/test_client.json", lambda c: c.update({"template": "brisk"}))
    only(problems(kb), "the voice template 'brisk' does not exist")


def test_the_defaults_are_complete(kb):
    kb.edit("defaults.json", lambda d: d["reply"].pop("signature"))
    only(problems(kb), "defaults.json", "signature")
    kb.edit("defaults.json", lambda d: d["reply"].update({"signature": ""}) or d["reply"]["length"].pop("es"))
    only(problems(kb), "reply.length: the defaults need limits for 'es'")


def test_a_layer_is_held_to_the_bundles_limits(kb):
    kb.edit(f"{FOOD}/test_client.json", lambda c: c["reply"].update({"signature": "Thanks,\nthe team"}))
    only(problems(kb), f"{FOOD}/test_client.json", "reply.signature")
    kb.edit(f"{FOOD}/test_client.json", lambda c: c["reply"].update({"signature": ""}))
    kb.edit(f"{FOOD}/test_client.json", lambda c: c["content"].update({"chinese_variant": "simplified"}))
    only(problems(kb), f"{FOOD}/test_client.json", "content.chinese_variant")
    kb.edit(f"{FOOD}/test_client.json", lambda c: c["content"].update({"chinese_variant": None}))
    kb.edit(f"{FOOD}/test_client.json", lambda c: c["reply"].update({"auto_reply_window_start": "09:00"}))
    kb.edit(f"{FOOD}/test_client.json", lambda c: c["reply"].update({"auto_reply_window_end": "09:00"}))
    only(problems(kb), "reply: the posting window's start and end must differ")


def test_a_default_model_needs_a_price(kb):
    kb.edit("defaults.json", lambda d: d["analysis"].update({"judge_model": "unpriced"}))
    only(problems(kb), "analysis.judge_model 'unpriced' has no price")


def test_the_posts_task_has_a_default_provider_and_a_priced_model(kb):
    kb.edit("defaults.json", lambda d: d["analysis"].update({"posts_model": "unpriced"}))
    only(problems(kb), "analysis.posts_model 'unpriced' has no price")
    kb.edit("defaults.json", lambda d: d["analysis"].pop("posts_provider"))
    assert any("posts_provider" in problem for problem in problems(kb))


def test_the_report_summary_has_a_default_provider_and_a_priced_model(kb):
    kb.edit("defaults.json", lambda d: d["analysis"].update({"summary_model": "unpriced"}))
    only(problems(kb), "analysis.summary_model 'unpriced' has no price")
    kb.edit("defaults.json", lambda d: d["analysis"].pop("summary_provider"))
    assert any("summary_provider" in problem for problem in problems(kb))


def test_a_format_these_scripts_do_not_read(kb):
    kb.edit("manifest.json", lambda m: m.update(format=1))
    only(problems(kb), "format 1 is not the format these scripts read (2)")


def test_check_says_what_it_found(kb, capsys):
    assert cli.main(["check", "--knowledge", str(kb.root)]) == 0
    out = capsys.readouterr().out
    assert "2 industries (food_beverage, wellness)" in out
    assert "71 corpus cases" in out
