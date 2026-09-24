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

Retrieval effectiveness (nDCG/MAP vs. qrels) is out of scope: `NindEngine`
would first need stable external document ids and real inverted-list-driven
candidate generation instead of its current O(N·|Q|) full scan.

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
swaps around each Ctrl-C-safe critical section). A faster bulk load would
need a C++-side bulk mode (e.g. buffered indirection and sequential writes).
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

## Notes

- `gbuild.sh` installs binaries with an empty RPATH (see the top-level
  `CMakeLists.txt`), so `run_indexing_benchmark.sh` adds `<bin-dir>/../lib`
  to `LD_LIBRARY_PATH` itself — no manual setup needed.
- `bench/results/` is git-ignored; it's regenerated by these scripts. Both
  indexing benchmarks reuse the same `bench/results/msmarco_<size>/docs.txt`
  when present, so running benchmark 2 after benchmark 1 (or vice versa)
  doesn't reconvert the corpus.
