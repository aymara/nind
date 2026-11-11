# Server results, 2026-11-05 to 2026-11-10

The runs of `bench/README.md` section 7 (the indexes and corpora aren't kept:
`prepare.py`, `scripts/prepare_v2.py` and `subset.py` regenerate the corpora).
`data_dir` paths are relative to the run's working directory.

- `v1-*`: stage 1, MS MARCO v1 (stopwords removed and kept), its 1M/2M/4M prefixes and
  BEIR: `results.jsonl` (`driver.py`'s raw results, `passes` per k), `report.md`,
  `report.csv`, `report-compact.md`. `v1-rep2-*`, `v1-rep3-*`: the builds and updates
  of MS MARCO v1 run twice more.
- `v2-*`: stage 2, MS MARCO v2 prefixes 1M–138M, stopwords removed (`v2-stop*`) and
  kept (`v2`, `v2-16M`).
- `verify.jsonl`: `verify_index` on every segment of the 138M one-index and 8-segment
  builds, stopwords removed and kept (one line per segment).
- `search-threads.jsonl`: stage 3, latency against threads per query
  (`scripts/search_threads.py`).
- `concurrency.jsonl`: stage 3, `concurrency.py`, with the index and the CPU pinning.
- `build-threads.jsonl`: stage 3, builds with 1–48 threads (`run`: `s3-build-<threads>`,
  `s3-build-138M-48` for the 138M builds).
- `memory-budget.jsonl`: stage 5, searches under a page-cache budget (`budget_gb`, null:
  no limit; `cgroup`: the cap, peak, peak anonymous memory and memory events).
  `memory-io.txt`: nind's disk reads per query (`scripts/search_io.py`).
- `environment.txt`: machine, versions, commit; `harness-at-stage1.diff`: the harness's
  changes over that commit when the runs started (`--repeats`, k=100, page cache,
  segment systems).
- `scripts/`: the stage scripts (`common.sh` sets the paths and the pinning). Stage 5:
  `stage5.sh` ran the uncapped round, `stage5b.sh` the capped ones, with caps from the
  uncapped round (the short calibration runs of `stage5.sh` underestimated nind's memory).
