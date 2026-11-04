# nind performance benchmarks

nind is a flat-file indexer, not a full search engine, so it's benchmarked
here like a storage/indexing engine rather than with an anserini/pyserini-style
retrieval-effectiveness harness (nDCG/MAP against relevance judgments) — see
the design discussion this came out of for why. Three things are measured:

1. **Indexing throughput & on-disk index size**, across a corpus-size sweep —
   using nind's efficient *bulk-build* write pattern.
2. **The cost of incremental, per-document indexing** into a growing index —
   a different, also-real usage pattern, with a real and important caveat
   (see below).
3. **Term/document lookup latency**, through the same `nind._native`-backed
   Python reader classes real callers use.

A fourth benchmark (section 4) compares nind with other indexers (Lucene,
tantivy, bm25s) on the same tokens, including rankings as a parity check.

## Corpus: MS MARCO Passage Ranking

Freely downloadable, no license/registration required, and its passage ids
are already a dense integer sequence from 0 — a good match for nind's
document-identifier model. Get `collection.tsv` (one passage per line,
`pid<TAB>text`, ~8.84M rows) from the official MS MARCO Passage Ranking
release — see <https://github.com/microsoft/MSMARCO-Passage-Ranking> for the
current download instructions/links (verify the link is still current;
dataset hosting has moved before).

## Prerequisites

```bash
# Python side: builds nind._native and installs the reader/writer classes.
# This alone is enough for the bulk-build benchmark (1) and lookup latency (3).
uv sync

# C++ side, only needed for the incremental-indexing benchmark (2):
NIND_DIST=/path/to/dist ./gbuild.sh -m Release   # Release, not Debug - Debug timings are misleading
# if cmake/ninja aren't installed system-wide, `uv tool install cmake ninja` works too
```

## 1. Indexing throughput & size (bulk build)

```bash
./bench/run_bulk_indexing_benchmark.sh /path/to/collection.tsv 10000,100000,1000000
```

For each size in the comma-separated list, this converts the first N
passages to nind's corpus text format (`bench/msmarco/convert_to_nind.py`,
offsetting `pid` by +1 since nind treats document id 0 as a sentinel
elsewhere), then runs `bench/index_bulk.py`, which writes each term's
definition and each document's local definition to disk **exactly once**,
via `nind._native`'s `NindLexiconIndex`/`NindTermIndex`/`NindLocalIndex`
writers (the same write-once pattern `nind.nind_engine.NindIndexer` uses),
reading from the compact bulk corpus file instead of one file per document.

The corpus is split into line-aligned byte ranges handled by a pool of worker
processes (`--workers`, default: all cores); the main process only drives
the nind writers, which are single-file and single-threaded:

1. *pass 1* (workers): vocabulary and highest document id per chunk; the
   main process then writes the lexicon, terms added in sorted order;
2. *pass 2* (workers): parsing and term→id mapping. Each chunk's documents
   come back as flat `array('I')` buffers, and the main process writes them
   in document order with one `set_local_defs_arrays` call per chunk. Each
   worker also spills its chunk's postings to a temporary file next to the
   index, sorted by term id and split into term-id ranges (SPIMI-style), so
   no process holds the whole inverted index;
3. *pass 3* (workers): one term-id range each, merging that range's slice
   of every spill file; the main process writes the terms in id order with
   one `set_term_defs_arrays` call per range.

The output is byte-identical to a serial accumulate-in-memory build, apart
from the lexicon creation timestamp in each file's identification trailer.
Wall-clock time (`time.perf_counter`), peak RSS of the main process and of
the largest worker (`resource.getrusage`), and the index file sizes go into
one row of `bench/results/indexing_bulk.csv`; per-phase timings are printed
to stderr.

Measured on the actual MS MARCO data (48-core machine; the `workers` column
of the CSV records the pool size of each run):

| docs | workers | wall | docs/s | main peak RSS | largest worker RSS | index size |
|---:|---:|---:|---:|---:|---:|---:|
| 10K | 24 | 1.0 s | 10,400 | 33 MB | 23 MB | 5.5 MB |
| 100K | 24 | 5.0 s | 20,100 | 87 MB | 42 MB | 46 MB |
| 1M | 24 | 42 s | 23,800 | 675 MB | 174 MB | 455 MB |
| 8.84M (full) | 48 | 255 s | 34,700 | 3.1 GB | 573 MB | 4.1 GB |

For comparison, the earlier serial version of this script (whole corpus
accumulated in RAM as Python objects) did ~2,700-3,000 docs/s and needed
7.9 GB at 1M docs; its rows are kept in
`bench/results/indexing_bulk_serial_v1.csv`.

The main process is the bottleneck: at 1M docs, 16 and 48 workers take the
same time. Its time goes almost entirely into the C++ writers, which make
every entry durable on its own (~13 system calls per definition written:
seeks, reads of the indirection entry, `fwrite`+`fflush`, and signal-handler
swaps around each Ctrl-C-safe critical section). These measurements predate
the writers' bulk mode (`begin_bulk`/`end_bulk`, see `NindIndex::beginBulk`),
which `NindIndexer` now uses and this script doesn't yet.
The main process's memory is mostly worker results waiting to be written;
at most `2 × workers` chunks (~8 MB of corpus each) are in flight.

Don't run two builds of the same index at once: they'd delete and
overwrite each other's files, so `index_bulk.py` takes a lock on
`<index-base>.lock` and refuses to start if it's held.

Start with a small size (e.g. 10000) as a smoke test before scaling up.

## 2. Incremental per-document indexing cost

```bash
NIND_DIST=/path/to/dist ./bench/run_indexing_benchmark.sh \
    /path/to/collection.tsv 10000,100000
```

This drives `Nind_indexeCorpus` instead, which indexes one document at a
time against the on-disk files as it reads them — a different, also-real
usage pattern (e.g. appending new documents to an already-large index over
time, rather than building it once from scratch). **Its scaling is much
worse: on the actual MS MARCO data, going from 10K to 100K documents made it
~89x slower, not ~10x.**

The reason is structural, not a bug in this particular tool:
`NindTermIndex::setTermDef` (`src/cpp/NindIndex/NindTermIndex.h:80-88`) only
supports writing a term's *entire* postings list, no incremental append.
`Nind_indexeCorpus` calls `getTermDef`+`setTermDef` for every term in every
document (`tst/cpp/NindAmose/Nind_indexeCorpus.cpp:142-150`), so a term
appearing in *df* documents has its (growing) postings list read and
rewritten *df* times — total cost is dominated by common terms and scales
roughly O(N²) in corpus size. **Don't run this one past ~100K documents** —
it won't finish in reasonable time and the quadratic trend is already clear
by then. It exists to document a real limitation (streaming/incremental
indexing into a large existing base gets expensive), not as nind's headline
throughput number — use benchmark 1 for that.

Same output shape as before: one row per size in
`bench/results/indexing.csv`.

## 3. Lookup latency

Run against an index built by either benchmark above (prefer benchmark 1's
`docs_bulk` index — it's the one built the way nind is actually meant to be
built at scale):

```bash
uv run python3 bench/lookup_latency_bench.py \
    bench/results/msmarco_10000/docs.txt \
    bench/results/msmarco_10000/docs_bulk \
    10000
```

Samples random terms and document ids from the corpus text file, times
repeated calls to `NindLexiconindex.donneIdentifiant` (term→id),
`NindTermindex.donneListeTermesCG` (id→postings), and
`NindLocalindex.donneListeTermes` (doc→local terms), and appends
mean/median/p95/p99 latency and throughput to `bench/results/lookup.csv`.
Run it once per size point to see how lookup latency scales with index size.

By default each operation's sample set is run once untimed first
(`--warmup 1`), so the numbers are steady-state lookup cost. With
`--warmup 0` you mostly measure first-touch page-cache misses on a freshly
opened file instead: on MS MARCO that made term→id look like it grew from
~3µs (10K docs) to ~175µs median (1M docs). Don't run it concurrently with an
indexing benchmark: I/O contention skews the cold numbers further.

Warm, on the bulk-built MS MARCO indexes (median / p99, µs):

| docs | term→id | id→postings | doc→local terms |
|---:|---:|---:|---:|
| 10K | 2.2 / 3.9 | 2.7 / 9.6 | 15.4 / 28.7 |
| 100K | 2.7 / 5.4 | 3.2 / 22 | 16.7 / 31.3 |
| 1M | 3.0 / 5.6 | 3.8 / 107 | 19.5 / 34.0 |
| 8.84M | 3.1 / 5.1 | 3.7 / 284 | 19.9 / 36.1 |

Lookups stay essentially flat from 10K to 8.84M documents: the lexicon's
hash-bucket count is sized to the vocabulary, so buckets don't lengthen, and
the term and local indexes are addressed directly by id. The id→postings tail
is the few very frequent terms in the sample, whose postings lists are long.

## 4. Comparison with other indexers (`bench/retrieval/`)

nind is only the indexing layer of the search stack: text analysis is LIMA's
job, search Amose's. So the comparison is between indexes, not search
engines: every system gets **the same tokens** and scores with **the same
BM25** (k1, b), and what is measured is building, size, querying and
updating. Retrieval quality (nDCG@10, R@100, RR@10) is only reported as a
**parity check** — the systems must rank alike — not as a quality claim: the
analysis (a stand-in for LIMA) isn't any system's own.

| system | what | update |
|---|---|---|
| `nind` | `NindIndexer.index_jsonl` / `NindEngine.search`, no local index | `NindIndexer.update()`, in place |
| `nind+local` | the same, with the local index (terms and positions per document, read by Amose) | in place |
| `lucene` | Lucene 10.4 through Anserini (`IndexCollection -pretokenized`, `SimpleSearcher` with `WhitespaceAnalyzer`), frequencies only | `SimpleIndexer -append`, in place |
| `lucene+pos` | the same with `-storePositions -storeDocvectors` (what nind's local index holds) | in place |
| `bm25s` | bm25s, `lucene` BM25 variant: precomputed score matrix, in memory | none: full rebuild |
| `tantivy` | tantivy-py, `whitespace` tokenizer, frequencies only (its BM25 is fixed at k1=1.2, b=0.75) | `writer.add_document` + `commit` |

### Running

The benchmark has its own environment (not the package's dependencies).
pyserini's Python layer imports torch, so only its bundled Anserini fatjar is
used (installed `--no-deps`, driven through pyjnius); Anserini needs a Java
21+ runtime (a JRE is enough: `JAVA_HOME` is derived from `java` on the
`PATH`, or set it; `ANSERINI_JAR` overrides the jar).

```bash
uv venv bench-env && export VIRTUAL_ENV=$PWD/bench-env
uv pip install .                    # nind itself (CXX=g++ if clang++ can't find libstdc++)
uv pip install --no-deps pyserini   # only for its Anserini fatjar
uv pip install pyjnius numpy bm25s tantivy ir_datasets ir_measures

# 1. tokens for every system, once (ir_datasets downloads into $IR_DATASETS_HOME)
bench-env/bin/python bench/retrieval/prepare.py beir/scifact/test data/scifact
bench-env/bin/python bench/retrieval/prepare.py beir/fiqa/test data/fiqa
bench-env/bin/python bench/retrieval/prepare.py msmarco-passage/dev/small data/msmarco
# 2. the matrix: every system x build, search, update; then the report
bench-env/bin/python bench/retrieval/driver.py --work work --data data/scifact data/fiqa \
    [--systems nind,lucene,...] [--threads 8] [--k1 1.2 --b 0.75] [--max-queries N]
bench-env/bin/python bench/retrieval/driver.py --work work --report-only

# scaling: prefixes of one prepared corpus, the matrix on each, then the fits
bench-env/bin/python bench/retrieval/prepare.py msmarco-passage/dev/small data/msmarco-stop --stopwords
bench-env/bin/python bench/retrieval/subset.py data/msmarco-stop data/msmarco-stop-1M 1000000   # also 2M, 4M
bench-env/bin/python bench/retrieval/driver.py --work work --max-queries 500 \
    --data data/msmarco-stop-1M data/msmarco-stop-2M data/msmarco-stop-4M data/msmarco-stop
bench-env/bin/python bench/retrieval/extrapolate.py work \
    msmarco-stop-1M msmarco-stop-2M msmarco-stop-4M msmarco-stop [--to 1e8 1e9 1e10]
```

`prepare.py` writes the corpus as JSON Lines whose `contents` are the tokens
joined by spaces — nind's own tokenizer's words (no identifier splitting),
lowercased, a fixed point of that tokenizer, so nind and the whitespace
tokenizers of the others see exactly the same tokens. Documents without any
token are dropped for everyone (nind doesn't store them: N and avgdl would
differ), query tokens are deduplicated (systems don't all count a repeated
term twice). The last 1% of the documents (`--update-fraction`) form the
update. With `--limit` (a corpus subset) no qrels are written: judgments of a
subset mean nothing. `prepare.py` is the place to plug LIMA's analysis in.

`driver.py` runs each phase of each system in its own process (`worker.py`)
and takes its peak RSS from `wait4` (children included: Anserini's JVM). Times
are measured around the operation (not interpreter or JVM start; the Lucene
build does include its JVM start, about 0.5 s). Per dataset and system:

- **build**: index the whole corpus with `--threads` threads (Anserini
  parallelizes over input files: the corpus is split into one part per
  thread; bm25s is single-threaded); index size on disk.
- **search**: open time, N and avgdl as the index sees them (they must be
  equal for all), single-thread latency per query (p50, p99, QPS) at k=10 and
  k=1000 after a warm-up pass, and a run file (k=1000) scored with
  ir_measures; `top10 = nind` is the mean overlap of the system's top 10 with
  nind's.
- **update**: build the corpus minus the update, add the update (in place, or
  rebuild for bm25s), then query: `update = build` is the overlap of the
  updated index's top 10 with the index built at once (1.000 expected).

Results go to `WORK/results.jsonl` (the last run of a dataset/system/phase
wins), `WORK/report.md` and `WORK/report.csv`.

### What to expect from the scores

nind and bm25s compute BM25 exactly: their quality numbers and rankings are
identical (up to bm25s's float32 near-ties on large corpora). Lucene and tantivy store document lengths in one byte (lossy) and
leave out BM25's constant (k1 + 1) factor: their rankings agree with each
other and overlap nind's top 10 at 0.96–0.99, quality within ±0.004.

### Results (2026-11-04)

One laptop: Intel i7-1370P (20 threads), 62 GB, NVMe; 8 threads to build,
1 to query; k1=1.2, b=0.75; nind 2.6.0 with the flat-array `Bm25Scorer`
(commit `453caeb`), Anserini 2.3.0 (Lucene 10.4), bm25s 0.3.16, tantivy-py
0.27.0, JDK 25. `-stop`: Lucene's 33 English stopwords removed
(`prepare.py --stopwords`), for every system. MS MARCO: 500 dev queries
(`--max-queries 500`); prefixes (`-1M`...) have no qrels; `nind+local`
searches with the same scorer as `nind`, so only its build and update are run
there. Sizes in MB, latencies in ms; every column in `WORK/report.md`.

BEIR SciFact, NFCorpus, FiQA, with and without stopwords:

| dataset | system | build s | build peak MB | index MB | k10 p50 ms | k10 p99 ms | k1000 p50 ms | nDCG@10 | top10 = nind | update s | update |
|---|---|---|---|---|---|---|---|---|---|---|---|
| fiqa | nind | 1.31 | 209 | 15.1 | 1.642 | 3.296 | 2.220 | 0.2365 |  | 3.125 | in place |
| fiqa | nind+local | 2.07 | 211 | 51.5 | 1.636 | 3.392 | 2.153 | 0.2365 | 1.000 | 3.193 | in place |
| fiqa | lucene | 2.94 | 561 | 9.3 | 1.033 | 2.916 | 11.312 | 0.2375 | 0.959 | 1.063 | in place |
| fiqa | lucene+pos | 3.93 | 1254 | 47.0 | 0.971 | 2.543 | 11.484 | 0.2375 | 0.959 | 1.070 | in place |
| fiqa | bm25s | 3.16 | 288 | 39.6 | 0.330 | 1.955 | 0.670 | 0.2365 | 1.000 | 2.923 | full rebuild |
| fiqa | tantivy | 0.52 | 103 | 10.5 | 0.736 | 1.263 | 2.140 | 0.2375 | 0.959 | 0.606 | in place |
| fiqa-stop | nind | 1.05 | 181 | 13.3 | 0.443 | 1.383 | 0.947 | 0.2379 |  | 2.550 | in place |
| fiqa-stop | nind+local | 1.66 | 185 | 40.6 | 0.440 | 1.305 | 1.016 | 0.2379 | 1.000 | 2.777 | in place |
| fiqa-stop | lucene | 2.80 | 503 | 8.5 | 0.803 | 1.925 | 10.542 | 0.2406 | 0.964 | 0.995 | in place |
| fiqa-stop | lucene+pos | 3.69 | 1130 | 38.4 | 0.854 | 2.193 | 10.631 | 0.2406 | 0.964 | 1.156 | in place |
| fiqa-stop | bm25s | 2.50 | 222 | 32.7 | 0.248 | 2.709 | 0.535 | 0.2379 | 1.000 | 2.256 | full rebuild |
| fiqa-stop | tantivy | 0.64 | 90 | 9.2 | 0.308 | 0.677 | 1.514 | 0.2406 | 0.965 | 1.224 | in place |
| nfcorpus | nind | 0.37 | 74 | 2.3 | 0.021 | 0.150 | 0.104 | 0.3067 |  | 0.290 | in place |
| nfcorpus | nind+local | 0.48 | 74 | 6.0 | 0.022 | 0.248 | 0.105 | 0.3067 | 1.000 | 0.316 | in place |
| nfcorpus | lucene | 2.08 | 308 | 1.4 | 0.526 | 2.869 | 3.470 | 0.3080 | 0.988 | 0.789 | in place |
| nfcorpus | lucene+pos | 2.54 | 622 | 5.8 | 0.487 | 2.572 | 3.494 | 0.3080 | 0.988 | 0.903 | in place |
| nfcorpus | bm25s | 0.32 | 74 | 4.4 | 0.068 | 0.138 | 0.268 | 0.3067 | 1.000 | 0.335 | full rebuild |
| nfcorpus | tantivy | 0.91 | 71 | 1.1 | 0.023 | 0.088 | 0.381 | 0.3078 | 0.988 | 0.788 | in place |
| nfcorpus-stop | nind | 0.41 | 71 | 2.2 | 0.018 | 0.071 | 0.068 | 0.3079 |  | 0.265 | in place |
| nfcorpus-stop | nind+local | 0.49 | 71 | 5.1 | 0.018 | 0.071 | 0.053 | 0.3079 | 1.000 | 0.295 | in place |
| nfcorpus-stop | lucene | 2.32 | 304 | 1.4 | 0.561 | 2.593 | 1.645 | 0.3080 | 0.989 | 0.842 | in place |
| nfcorpus-stop | lucene+pos | 2.57 | 599 | 5.0 | 0.474 | 1.906 | 1.609 | 0.3080 | 0.989 | 0.873 | in place |
| nfcorpus-stop | bm25s | 0.25 | 67 | 3.9 | 0.092 | 0.170 | 0.262 | 0.3079 | 1.000 | 0.247 | full rebuild |
| nfcorpus-stop | tantivy | 0.94 | 65 | 1.0 | 0.018 | 0.045 | 0.144 | 0.3075 | 0.990 | 0.853 | in place |
| scifact | nind | 0.43 | 79 | 3.1 | 0.191 | 0.440 | 0.537 | 0.6615 |  | 0.338 | in place |
| scifact | nind+local | 0.52 | 83 | 8.1 | 0.180 | 0.419 | 0.544 | 0.6615 | 1.000 | 0.361 | in place |
| scifact | lucene | 2.10 | 317 | 1.9 | 0.830 | 2.617 | 8.857 | 0.6611 | 0.981 | 0.778 | in place |
| scifact | lucene+pos | 2.63 | 632 | 7.9 | 0.813 | 3.117 | 8.671 | 0.6611 | 0.981 | 0.884 | in place |
| scifact | bm25s | 0.43 | 82 | 5.8 | 0.073 | 0.179 | 0.313 | 0.6615 | 1.000 | 0.427 | full rebuild |
| scifact | tantivy | 0.84 | 74 | 1.5 | 0.098 | 0.195 | 1.051 | 0.6611 | 0.981 | 0.803 | in place |
| scifact-stop | nind | 0.44 | 78 | 2.9 | 0.067 | 0.181 | 0.369 | 0.6645 |  | 0.304 | in place |
| scifact-stop | nind+local | 0.49 | 86 | 6.8 | 0.068 | 0.197 | 0.374 | 0.6645 | 1.000 | 0.329 | in place |
| scifact-stop | lucene | 2.21 | 327 | 1.8 | 0.646 | 1.935 | 8.860 | 0.6639 | 0.982 | 0.756 | in place |
| scifact-stop | lucene+pos | 2.65 | 646 | 6.8 | 0.683 | 1.716 | 8.679 | 0.6639 | 0.982 | 0.863 | in place |
| scifact-stop | bm25s | 0.35 | 77 | 5.2 | 0.081 | 0.299 | 0.316 | 0.6645 | 1.000 | 0.337 | full rebuild |
| scifact-stop | tantivy | 0.96 | 73 | 1.4 | 0.053 | 0.118 | 1.011 | 0.6639 | 0.982 | 0.870 | in place |

MS MARCO passage (8.8M passages) with and without stopwords, and prefixes of
the stopword-free corpus:

| dataset | system | build s | build peak MB | index MB | k10 p50 ms | k10 p99 ms | k1000 p50 ms | nDCG@10 | top10 = nind | update s | update |
|---|---|---|---|---|---|---|---|---|---|---|---|
| msmarco | nind | 84.35 | 8765 | 1053.6 | 132.917 | 388.855 | 140.297 | 0.2263 |  | 437.697 | in place |
| msmarco | nind+local | 291.54 | 8971 | 3812.4 |  |  |  |  |  | 340.490 | in place |
| msmarco | lucene | 52.83 | 3438 | 709.1 | 2.398 | 16.883 | 20.312 | 0.2276 | 0.974 | 3.466 | in place |
| msmarco | lucene+pos | 106.31 | 5459 | 3058.3 | 2.407 | 19.941 | 22.112 | 0.2276 | 0.974 | 4.327 | in place |
| msmarco | bm25s | 413.24 | 18153 | 2852.8 | 49.721 | 429.079 | 47.450 | 0.2263 | 0.998 | 340.787 | full rebuild |
| msmarco | tantivy | 55.09 | 1481 | 739.3 | 41.108 | 108.156 | 46.218 | 0.2270 | 0.973 | 0.850 | in place |
| msmarco-stop | nind | 58.24 | 6535 | 899.6 | 14.531 | 108.985 | 15.855 | 0.2280 |  | 237.125 | in place |
| msmarco-stop | nind+local | 146.68 | 6763 | 2992.3 |  |  |  |  |  | 240.543 | in place |
| msmarco-stop | lucene | 36.82 | 3439 | 646.9 | 1.621 | 13.052 | 15.953 | 0.2270 | 0.987 | 2.156 | in place |
| msmarco-stop | lucene+pos | 75.01 | 5638 | 2525.4 | 1.541 | 11.462 | 15.397 | 0.2270 | 0.987 | 3.377 | in place |
| msmarco-stop | bm25s | 246.53 | 14003 | 2236.8 | 393.569 | 507.768 | 349.030 | 0.2280 | 0.997 | 251.560 | full rebuild |
| msmarco-stop | tantivy | 49.67 | 1447 | 658.0 | 5.709 | 29.599 | 9.391 | 0.2273 | 0.986 | 0.656 | in place |
| msmarco-stop-1M | nind | 6.33 | 942 | 109.7 | 1.195 | 7.299 | 1.979 |  |  | 19.916 | in place |
| msmarco-stop-1M | nind+local | 12.66 | 973 | 334.0 |  |  |  |  |  | 21.331 | in place |
| msmarco-stop-1M | lucene | 6.39 | 1077 | 76.5 | 0.804 | 3.528 | 11.744 |  | 0.984 | 3.121 | in place |
| msmarco-stop-1M | lucene+pos | 11.97 | 4127 | 286.9 | 0.802 | 3.425 | 11.292 |  | 0.984 | 1.338 | in place |
| msmarco-stop-1M | bm25s | 23.67 | 1643 | 254.7 | 27.273 | 39.485 | 27.085 |  | 0.989 | 23.228 | full rebuild |
| msmarco-stop-1M | tantivy | 6.89 | 381 | 75.5 | 0.761 | 3.848 | 4.121 |  | 0.974 | 0.917 | in place |
| msmarco-stop-2M | nind | 12.05 | 1682 | 212.3 | 2.893 | 17.355 | 3.710 |  |  | 43.069 | in place |
| msmarco-stop-2M | nind+local | 29.06 | 1714 | 669.3 |  |  |  |  |  | 48.713 | in place |
| msmarco-stop-2M | lucene | 10.55 | 1290 | 149.7 | 0.978 | 5.091 | 13.133 |  | 0.983 | 1.559 | in place |
| msmarco-stop-2M | lucene+pos | 21.17 | 4414 | 572.3 | 0.938 | 5.010 | 12.269 |  | 0.983 | 1.794 | in place |
| msmarco-stop-2M | bm25s | 53.50 | 3196 | 506.3 | 60.253 | 86.745 | 59.834 |  | 0.994 | 49.128 | full rebuild |
| msmarco-stop-2M | tantivy | 13.61 | 646 | 148.2 | 1.501 | 7.376 | 5.492 |  | 0.985 | 0.822 | in place |
| msmarco-stop-4M | nind | 28.00 | 3120 | 415.8 | 7.232 | 44.214 | 7.529 |  |  | 98.486 | in place |
| msmarco-stop-4M | nind+local | 60.58 | 3213 | 1346.1 |  |  |  |  |  | 100.259 | in place |
| msmarco-stop-4M | lucene | 19.55 | 2174 | 299.8 | 1.190 | 6.587 | 13.912 |  | 0.983 | 2.155 | in place |
| msmarco-stop-4M | lucene+pos | 38.39 | 4598 | 1151.6 | 1.181 | 6.665 | 14.372 |  | 0.983 | 2.959 | in place |
| msmarco-stop-4M | bm25s | 112.34 | 6367 | 1012.7 | 147.101 | 202.162 | 147.447 |  | 0.998 | 104.742 | full rebuild |
| msmarco-stop-4M | tantivy | 26.51 | 1166 | 295.9 | 2.680 | 13.543 | 6.101 |  | 0.984 | 0.569 | in place |

Growth with the corpus size, fitted as y = a N^p on the four stopword-free
MS MARCO sizes (`extrapolate.py`; bm25s and the `+local`/`+pos` variants in
its full output). The extrapolations are orders of magnitude, assuming the
corpus keeps MS MARCO's statistics, and **ignore format limits** (see below):

| system | metric | 1.0M | 2.0M | 4.0M | 8.8M | exponent p | 100M | 1G | 10G |
|---|---|---|---|---|---|---|---|---|---|
| nind | build s | 6.33 | 12.1 | 28 | 58.2 | 1.04 | 736 | 8e+03 | 8.69e+04 |
| nind | build peak GB | 0.92 | 1.64 | 3.05 | 6.38 | 0.89 | 54.3 | 421 | 3.27e+03 |
| nind | index GB | 0.107 | 0.207 | 0.406 | 0.878 | 0.97 | 9.12 | 84.3 | 779 |
| nind | k10 p50 ms | 1.2 | 2.89 | 7.23 | 14.5 | 1.16 | 265 | 3.82e+03 | 5.51e+04 |
| nind | search peak GB | 0.215 | 0.342 | 0.588 | 1.16 | 0.78 | 7.42 | 44.5 | 266 |
| nind | update s (1%) | 19.9 | 43.1 | 98.5 | 237 | 1.14 | 3.81e+03 | 5.28e+04 | 7.31e+05 |
| lucene | build s | 6.39 | 10.5 | 19.5 | 36.8 | 0.81 | 263 | 1.7e+03 | 1.1e+04 |
| lucene | build peak GB | 1.05 | 1.26 | 2.12 | 3.36 | 0.55 | 12.5 | 44.9 | 161 |
| lucene | index GB | 0.0747 | 0.146 | 0.293 | 0.632 | 0.98 | 6.85 | 65.6 | 629 |
| lucene | k10 p50 ms | 0.804 | 0.978 | 1.19 | 1.62 | 0.32 | 3.44 | 7.16 | 14.9 |
| lucene | search peak GB | 1.1 | 0.983 | 1.1 | 1.33 | 0.10 | 1.58 | 1.97 | 2.46 |
| lucene | update s (1%) | 3.12 | 1.56 | 2.15 | 2.16 | -0.10 | 1.51 | 1.18 | 0.931 |
| tantivy | build s | 6.89 | 13.6 | 26.5 | 49.7 | 0.91 | 471 | 3.83e+03 | 3.11e+04 |
| tantivy | build peak GB | 0.372 | 0.631 | 1.14 | 1.41 | 0.63 | 7.32 | 31.3 | 134 |
| tantivy | index GB | 0.0738 | 0.145 | 0.289 | 0.643 | 0.99 | 7.12 | 70.2 | 692 |
| tantivy | k10 p50 ms | 0.761 | 1.5 | 2.68 | 5.71 | 0.92 | 52.5 | 433 | 3.57e+03 |
| tantivy | search peak GB | 0.147 | 0.182 | 0.237 | 0.411 | 0.47 | 1.18 | 3.44 | 10.1 |
| tantivy | update s (1%) | 0.917 | 0.822 | 0.569 | 0.656 | -0.19 | 0.377 | 0.246 | 0.16 |

### Findings

- **Parity holds everywhere.** N and avgdl are equal for every system; nind
  ranks exactly like bm25s on BEIR (top-10 overlap 1.000; 0.997–0.998 on MS
  MARCO, where bm25s's float32 scores order near-ties differently); its top
  10 overlaps Lucene's and tantivy's at 0.96–0.99 (their one-byte document
  lengths). nind's and Lucene's updated indexes answer exactly as indexes
  built at once (1.000 everywhere); tantivy's at 0.987–0.998 on MS MARCO.
- **Size: linear for every system** (p ≈ 1). nind's index is ~1.4x Lucene's
  without the local index (MS MARCO without stopwords: 0.90 vs 0.65 GB),
  ~1.2x Lucene with positions and document vectors with it (2.99 vs 2.53 GB).
- **Build time: nind is linear (p 1.04)**, comparable to the others at MS
  MARCO's size (58 s vs 37 s for Lucene and 50 s for tantivy, stopwords
  removed). **Its build memory grows with the corpus** (p 0.89, ~19 bytes per
  token: 6.4 GB for 8.8M passages), where Lucene's and tantivy's stay bounded
  (they flush segments: p 0.55 and 0.63).
- **Search: nind reads every posting, Lucene doesn't.** The flat-array
  scorer made nind 10–15x faster (~9–12 ns per posting, results unchanged),
  and on small corpora nind is among the fastest (NFCorpus: 0.018 ms, Lucene
  through pyjnius ~0.5 ms). But its latency grows as **N^1.16** (1.2 ms at
  1M passages, 14.5 ms at 8.8M) while Lucene's grows as **N^0.32** (0.8 to
  1.6 ms): Lucene skips the blocks of postings that can't reach the top k
  (block-max WAND). With stopwords kept, MS MARCO costs nind 133 ms per query
  vs 2.4 ms for Lucene. tantivy grows as N^0.92 here. bm25s, exhaustive over
  a precomputed matrix, is slowest at scale (and unexpectedly slower on MS
  MARCO without stopwords, 394 ms, than with them, 50 ms: not investigated).
- **Updates: nind rewrites every postings list a new document enters.**
  Adding 1% of the documents costs **4–5x a full rebuild** (237 s vs 58 s at
  8.8M passages, without stopwords; 438 s vs 84 s with them) and grows as
  N^1.14; Lucene and tantivy write a new segment: 2 s and 0.7 s, about
  constant (their later segment merges aren't measured).
- **Stopwords** (33 words) are 30% of MS MARCO's tokens. Removing them cuts
  nind's index by 15%, its build by 31%, its query time 9x and its update
  time by 46%, and pushes the 16 MiB term limit (below) from ~10M to at most
  ~38M passages.
- **Format limits come before the extrapolated costs.** A term definition is
  at most 16 MiB (~8M postings): MS MARCO-like corpora reach it at ~10M
  passages with stopwords (`the`, 15.4 MB at 8.8M) and at ≤38M without them
  (`you`, in 22% of the passages); a local index holds at most 16.7M
  documents. These used to be written truncated, silently corrupting the
  index; they now throw (commit `2c0bc0a`). So nind can't index 100M
  documents today, whatever the machine; at 1G, its build would also need
  ~420 GB of memory and a query ~4 s; 10G exceeds its 32-bit document ids
  (4.29G) and the 1 TiB per file of a local index (~3.4 TB). Lucene caps an
  index at ~2.1G documents (several indexes beyond). The block-structured
  format proposed in `docs/proposals/format-v2-blocks.md` addresses the
  limits, the update cost and the search growth together, and
  `docs/proposals/thread-safe-readers.md` the concurrent search the many-core
  target needs.
- Lucene's k=1000 latency is mostly Anserini's `SimpleSearcher` loading each
  hit's stored `docid` and building one Java object per hit for pyjnius: k=10
  is the comparable figure.

## Notes

- `gbuild.sh` installs binaries with an empty RPATH (see the top-level
  `CMakeLists.txt`), so `run_indexing_benchmark.sh` adds `<bin-dir>/../lib`
  to `LD_LIBRARY_PATH` itself — no manual setup needed.
- `bench/results/` is git-ignored; it's regenerated by these scripts. Both
  indexing benchmarks reuse the same `bench/results/msmarco_<size>/docs.txt`
  when present, so running benchmark 2 after benchmark 1 (or vice versa)
  doesn't reconvert the corpus.
