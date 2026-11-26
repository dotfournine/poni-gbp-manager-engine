# Knowledge base

Everything the system knows that is the same for every client and that nobody changes while it runs (FR-13, D-078). These files are the source; a script checks them and seeds them into the `kb_*` database tables, which nobody edits.

| Path | What |
| ---- | ---- |
| `manifest.json` | The format number of these files |
| `common/profile.json` | The severity flags of every industry |
| `industries/<key>/profile.json` | An industry's topics, item types and severity flags |
| `industries/<key>/corpus.json` | The industry's test corpus: severity, manipulation and ordinary cases |
| `industries/<key>/test_client.json` | A made-up business the test suites run as |
| `voices/<key>.json` | Voice templates: each sets some members of the content settings and of the reply settings (`content`, `reply`) |
| `defaults.json` | What a newly discovered location starts from, its complete content settings and reply settings (`content`, `reply`, D-107), and the default of every analysis setting (`analysis`) |
| `models.json` | Model prices in USD per million tokens |
| `schema/` | A JSON Schema per kind of file. Unknown keys are refused |

Prompts, the judge's rubric and the reply checks are code, not knowledge.

## Commands

From `tools/` (needs [uv](https://docs.astral.sh/uv/)):

```bash
uv run poni-tools check                              # schemas, cross-checks, complete packs
uv run poni-tools seed                               # check, then replace the kb_* tables of the database
```

## Rules

- Every review in a corpus is synthetic. No reviewer's words, no real business name, phone numbers from the reserved 555-01xx range, made-up domains.
- An attack seen in a real review is rewritten before it becomes a case.
- An industry is supported when its pack is complete: a profile, at least 2 cases for every severity flag that applies to it (its own and the common ones), at least 10 manipulation cases with one in Chinese and one in mixed language, and at least 3 ordinary cases.
- Nothing in a corpus says who read it (D-082). A new or changed severity or manipulation case is read by a person in the pull request that brings it.
- Every change gets a new `version` in `manifest.json`: today's date, `YYYYMMDD`; a second one on the same day ends in `-2`. The seed refuses changed content under a version that is already seeded, and `publish` never replaces an archive.
- A file that is not part of the layout (a mistyped name) and a key no schema knows are refused: nothing is left out without a word.
- A change to what the analyzer is told about an industry (its label, its topic ids, its item types, a severity flag's id, label, description or switch, and the common flags) changes the fingerprint of every location of that industry, so each needs a new go-live test before it auto replies again. `poni-tools seed` prints those locations for the person who seeds; the engine notifies no one. A topic's label, new corpus cases, templates, defaults and prices change no fingerprint.
- An item type with `"person": true` is a staff member a reviewer named: a location's report lists such items under employee recognition, not among the items (D-086). The analyzer is not told the flag, so setting it changes no fingerprint; adding the item type does.

## Onboarding a new industry

1. Write `industries/<key>/profile.json`.
2. Collect or generate `corpus.json`. Generated cases are a draft.
3. Write `test_client.json`: a made-up business of that industry.
4. `uv run poni-tools check` passes.
5. A person reads every severity and manipulation case in the pull request.
6. Run the corpus against the real models with the made-up business (`engine/scripts/linux.sh ai-live`); no case may reach auto reply.
7. Publish and seed in production (`deploy/README.md`).
8. Give a location the industry in the dashboard, and run its go-live test before anything is posted by auto reply.
