# Server results, 2026-11-12

Stage 7 of `bench/README.md` section 7 (same machine and data as `2026-11-10-server`): stages 1,
2 and 5 again for nind only, the same commands and settings (`scripts/stage7.sh`), with nind at
`8ba3ff1` (4.1.0's development version; 4.2.0's search code: its builds take less memory since,
see `docs/TODO.md`). Lucene's, tantivy's and bm25s's results are `2026-11-10-server`'s (the
driver's last result wins).

- `v1-*`, `v2-*`: `driver.py`'s results and reports for MS MARCO v1 and BEIR (stage 1) and MS MARCO
  v2, 1M to 138M (stage 2); `v1-rep2-*`, `v1-rep3-*`: the 8.8M builds and updates repeated.
- `verify.jsonl`: `verify_index` on every segment of the one-index and 8-segment builds at 138M,
  stopwords removed and kept.
- `memory-budget.jsonl`: stage 5, an index larger than memory: nind's rows from this stage (each
  run's driver result, `budget_gb`, and its cgroup's `capped.sh` log), Lucene's and tantivy's from
  `2026-11-10-server`.
- `stage7.log`: the stage's output; `scripts/`: the stage and its helpers.
- `worst-case-term.log`: `bench/worst_case_term.py` (one term in 350M documents, 4 shards, merge,
  `verify_index`, a pruned top 10), after stage 7 on the idle machine.
- `latency-pinning.jsonl`: one CPU against twelve, 10 processes each, alternated (`scripts/latency_pinning.sh`;
  8.8M, stopwords removed, top 10 on one thread, 3 passes each).
