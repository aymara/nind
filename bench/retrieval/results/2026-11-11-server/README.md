# Server results, 2026-11-11

Stage 6 of `bench/README.md` section 7 (same machine, indexes and data as `2026-11-10-server`),
with nind 4.1.0's development version: document names mapped, the scores array only for
exhaustive searches (see `docs/TODO.md`).

- `concurrency-callers.jsonl`: several callers at once, every system (`concurrency.py`;
  `scripts/stage6.sh`): nind, nind-8s, Lucene, tantivy; threads sharing one searcher or processes
  each opening the index; 1-48 callers (`pin`: their CPUs); MS MARCO v1 8.8M (4,000 queries
  after 1,000) and v2 138M (2,000 after 300), stopwords removed, top 10.
- `concurrency-callers-nind2.jsonl`: nind and nind-8s again (`scripts/stage6b.sh`), after
  MaxScore scanned queries of 5 terms or more in windows, term at a time (`docs/TODO.md`,
  "Throughput at 138M"); the page shows these for nind.
- `scale-repeat.txt`: nind and Lucene with 24 and 48 threads at 138M, twice each, alternated.
- `anonymous-138M.jsonl`: each system's memory per process at 138M (`anonymous_gb`: not a file's;
  one and four processes, 400 queries after 100).
- `updates.txt`: updates of 1% (MS MARCO v2 16M and 138M) through `IndexUpdate.add_jsonl`.
