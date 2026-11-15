# Yamaha corpus (private, issue #27)

The Genos acceptance styles used by the `*_corpus` tests are user-owned Yamaha content marked "no redistribution".
They are not in this repository. Keep them in a private folder outside the repo and configure the build with
`-DAIARR_GENOS_CORPUS_DIR=/path/to/that/folder`; without it the `*_corpus` tests report Skipped.
See `docs/governance/IMPORTER_GOLDEN_CORPUS_POLICY.md` and `docs/legal/ISSUE_27_HISTORY_REMEDIATION_PLAN.md`.

Public tests use the CC0 styles in `fixtures/synthetic/`. `TEMPLATE.meta.json` is an empty metadata template.
