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
identical. Lucene and tantivy store document lengths in one byte (lossy) and
leave out BM25's constant (k1 + 1) factor: their rankings agree with each
other and overlap nind's top 10 at 0.96–0.99, quality within ±0.004.

### Results (2026-11-03)

One laptop: Intel i7-1370P (20 threads), 62 GB, NVMe; 8 threads to build,
1 to query; k1=1.2, b=0.75; nind 2.6.0, Anserini 2.3.0 (Lucene 10.4), bm25s
0.3.16, tantivy-py 0.27.0, JDK 25.

BEIR SciFact, NFCorpus, FiQA (sizes in MB, latencies in ms; full columns in
`WORK/report.md`):

| dataset | system | build s | build peak MB | index MB | k10 p50 ms | k10 p99 ms | k1000 p50 ms | nDCG@10 | top10 = nind | update s | incremental |
|---|---|---|---|---|---|---|---|---|---|---|---|
| fiqa | nind | 1.22 | 215 | 15.1 | 12.013 | 25.623 | 13.035 | 0.2365 |  | 3.070 | yes |
| fiqa | nind+local | 2.04 | 217 | 51.5 | 11.915 | 25.340 | 12.974 | 0.2365 | 1.000 | 3.176 | yes |
| fiqa | lucene | 2.53 | 446 | 9.3 | 0.891 | 2.501 | 11.404 | 0.2375 | 0.959 | 1.332 | yes |
| fiqa | lucene+pos | 3.84 | 1053 | 46.9 | 0.935 | 2.567 | 11.065 | 0.2375 | 0.959 | 1.528 | yes |
| fiqa | bm25s | 3.39 | 312 | 39.6 | 0.325 | 1.928 | 0.594 | 0.2365 | 1.000 | 3.081 | no (rebuild) |
| fiqa | tantivy | 1.21 | 131 | 9.0 | 0.669 | 1.511 | 1.833 | 0.2375 | 0.959 | 0.765 | yes |
| nfcorpus | nind | 0.37 | 81 | 2.3 | 0.086 | 0.872 | 0.162 | 0.3067 |  | 0.300 | yes |
| nfcorpus | nind+local | 0.49 | 81 | 6.0 | 0.075 | 0.983 | 0.167 | 0.3067 | 1.000 | 0.319 | yes |
| nfcorpus | lucene | 2.12 | 216 | 1.4 | 0.295 | 1.210 | 3.231 | 0.3080 | 0.988 | 1.102 | yes |
| nfcorpus | lucene+pos | 2.56 | 538 | 5.8 | 0.304 | 1.266 | 3.182 | 0.3080 | 0.988 | 1.183 | yes |
| nfcorpus | bm25s | 0.48 | 85 | 4.4 | 0.062 | 0.141 | 0.262 | 0.3067 | 1.000 | 0.310 | no (rebuild) |
| nfcorpus | tantivy | 0.60 | 80 | 1.1 | 0.023 | 0.082 | 0.361 | 0.3078 | 0.988 | 0.572 | yes |
| scifact | nind | 0.49 | 89 | 3.1 | 1.142 | 3.964 | 1.460 | 0.6615 |  | 0.326 | yes |
| scifact | nind+local | 0.53 | 90 | 8.1 | 1.086 | 2.389 | 1.534 | 0.6615 | 1.000 | 0.350 | yes |
| scifact | lucene | 2.12 | 223 | 1.9 | 0.790 | 2.580 | 8.609 | 0.6611 | 0.981 | 1.075 | yes |
| scifact | lucene+pos | 2.21 | 526 | 7.9 | 0.802 | 2.772 | 8.944 | 0.6611 | 0.981 | 1.362 | yes |
| scifact | bm25s | 0.58 | 94 | 5.8 | 0.073 | 0.163 | 0.304 | 0.6615 | 1.000 | 0.368 | no (rebuild) |
| scifact | tantivy | 1.22 | 85 | 1.5 | 0.106 | 0.192 | 1.045 | 0.6611 | 0.981 | 0.975 | yes |

### Findings

- **Parity holds** on every dataset: N and avgdl are the same for every
  system, nind's rankings equal bm25s's, its top 10 overlaps Lucene's and
  tantivy's at 0.96–0.99, and every updated index answers as one built at once.
- **Building**: nind is among the fastest builders and has the smallest
  memory footprint after tantivy; its index is about 1.6x Lucene's without
  the local index, about the size of Lucene with positions and document
  vectors with it.
- **Updates**: nind, Lucene and tantivy update in place, bm25s rebuilds.
  nind's update cost is dominated by reopening its writers, which scan their
  whole files (see the I/O follow-up in CLAUDE.md), so it grows with the
  index, not the update: batch documents per update.
- **Querying is nind's weak point, and it isn't for lack of pruning**:
  `Bm25Scorer` costs a constant **~85–100 ns per posting** (82 on SciFact,
  88 on FiQA, 100 on NFCorpus), when bm25s, also exhaustive, costs ~2 ns.
  Each posting goes through a `std::list` node (decoding by
  `NindTermIndex::getTermDef`), a `seen` hash set, a hash-map lookup of the
  document and a hash-map score update. Queries with frequent terms (FiQA:
  ~146k postings per query, no stopword removal) are then ~12 ms, against
  ~0.9 ms for Lucene and ~0.7 ms for tantivy, which also skip postings that
  can't reach the top k (block-max WAND/MaxScore). Flat arrays indexed by
  document (scores, lengths) and decoding postings into a vector would
  remove most of it; dynamic pruning would be the next step.
- Lucene's k=1000 latency is mostly Anserini's `SimpleSearcher` loading each
  hit's stored `docid` and building one Java object per hit for pyjnius (the
  `search` call alone, without reading the hits from Python, is as slow): k=10
  is the comparable figure.

## Notes

- `gbuild.sh` installs binaries with an empty RPATH (see the top-level
  `CMakeLists.txt`), so `run_indexing_benchmark.sh` adds `<bin-dir>/../lib`
  to `LD_LIBRARY_PATH` itself — no manual setup needed.
- `bench/results/` is git-ignored; it's regenerated by these scripts. Both
  indexing benchmarks reuse the same `bench/results/msmarco_<size>/docs.txt`
  when present, so running benchmark 2 after benchmark 1 (or vice versa)
  doesn't reconvert the corpus.
