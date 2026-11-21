# Server results, 2026-11-20

Stage 10 (`scripts/stage10.sh`): nind only, at `a3f44c6` (format v3: a smaller term index, after `3a37604`: the directory
cache, compact builds, merges limited by bytes, the window scan's touched documents filtered),
everything the benchmarks page shows of it, with stages 7's and 8's commands and settings; Lucene's,
tantivy's and bm25s's results are those of `2026-11-10-server` and `2026-11-13-server` (the driver's
last result wins).

- `v1-*`: MS MARCO v1 8.8M and BEIR; `v1-rep2-*`, `v1-rep3-*`: the 8.8M builds and updates repeated.
- `v2-*`: MS MARCO v2 passages, 1M to 138M, stopwords removed and kept.
- `v21doc-*`: MS MARCO v2.1 documents, 1M, 4M and all 10.96M, stopwords removed and kept, and the
  judged TREC DL 2021-2023 topics; `v21doc-anserini-*`: those topics at k1=0.9, b=0.4.
- `verify.jsonl`: `verify_index` on every segment of the one-index and 8-segment builds of v2 and
  v2.1 (stopwords removed and kept).
- `memory-budget.jsonl`: an index larger than memory: nind's rows from this stage, Lucene's and
  tantivy's from `2026-11-10-server`.
- `stage10.log`; `scripts/`.
