# Comparison results, 2026-11-04

Summary outputs of the runs reported in `bench/README.md` section 4 (the
prepared corpora, indexes and run files aren't kept: regenerate them with
`prepare.py`, `subset.py` and `driver.py`). `data_dir` paths are relative to
the run's working directory.

- `results.jsonl`: every phase of every system on every dataset
  (`driver.py`'s raw results), with the flat-array `Bm25Scorer` (commit
  `453caeb`).
- `results-old-scorer.jsonl`: the same for the first BEIR and MS MARCO runs,
  with the hash-map scorer before `453caeb` (the "before" numbers).
- `report.md`, `report.csv`: every column, per dataset (`driver.py --report-only`).
- `report-compact.md`: the columns of the README tables.
- `extrapolation.md`: `extrapolate.py` on the stopword-free MS MARCO sizes,
  every system.

The overflow guards of commit `2c0bc0a` came after these runs; a full MS MARCO
build with them (stopwords kept, with and without the local index) gives
indexes of exactly the same sizes (1,104,803,549 and 3,997,597,893 bytes).
