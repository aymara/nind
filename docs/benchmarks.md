# Benchmarks

How nind compares with other indexers when it is used as an index plus BM25 top-k search:
**Lucene** (through Anserini), **tantivy** and **bm25s**, on the same tokens, the same BM25
parameters (k1 = 1.2, b = 0.75) and the same machine. nind 3.2.0 measured on 2026-11-05, the other
systems on 2026-11-04 (the same data and machine; they don't depend on nind's version).

```{admonition} In short
:class: tip

- **Search.** On MS MARCO (8.8 million passages), one query's top 10 takes **1.50 ms** on one
  thread (median), against **1.62 ms** for Lucene and **5.71 ms** for tantivy. With
  4 threads per query (`NindEngine(..., search_threads=4)`), **0.886 ms**. The slowest 1% of
  queries take up to 26.6 ms (11.1 ms with 4 threads; Lucene 13.1 ms).
- **Indexing.** A full build takes **86.8 s** (Lucene 36.8 s, tantivy
  49.7 s, 8 threads each); the index is **825 MB** (Lucene 647 MB).
- **Updates.** Adding 1% more documents takes **108 s** (Lucene 2.16 s,
  tantivy 0.656 s, which write a new segment). This is a first update after a build,
  which rewrites the terms it touches; later updates append to them in place (see below).
- **Quality.** The same BM25: nDCG@10 0.228 for nind, 0.227 for Lucene (differences
  come from score ties and Lucene's 1-byte document lengths).
```

## Search latency as the corpus grows

```{image} _static/bench/scaling-light.svg
:class: only-light
:alt: Median top-10 latency against corpus size for nind, nind with 4 threads, Lucene and tantivy; the same data is in the table below.
```

```{image} _static/bench/scaling-dark.svg
:class: only-dark
:alt: Median top-10 latency against corpus size for nind, nind with 4 threads, Lucene and tantivy; the same data is in the table below.
```

Median time per query to get the 10 best documents, one query at a time (500 MS MARCO dev
queries per size). nind uses block-max MaxScore: only the blocks of postings that may reach the
top 10 are read. With 4 threads per query, the documents are split into 4 ranges searched at
once, with the same results.

| system | 1M | 2M | 4M | 8.84M |
|---|---:|---:|---:|---:|
| nind | 0.373 ms | 0.540 ms | 0.900 ms | 1.50 ms |
| nind, 4 threads per query | 0.197 ms | 0.333 ms | 0.517 ms | 0.886 ms |
| Lucene (Anserini) | 0.804 ms | 0.978 ms | 1.19 ms | 1.62 ms |
| tantivy | 0.761 ms | 1.50 ms | 2.68 ms | 5.71 ms |

## MS MARCO, 8.8 million passages

```{image} _static/bench/msmarco-light.svg
:class: only-light
:alt: MS MARCO 8.8M: search latency, build time, index size and update time per system; the same data is in the table below.
```

```{image} _static/bench/msmarco-dark.svg
:class: only-dark
:alt: MS MARCO 8.8M: search latency, build time, index size and update time per system; the same data is in the table below.
```

| system | top 10, median | top 10, 99th pct | top 1000, median | build | build memory | index | update +1% | nDCG@10 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| nind | 1.50 ms | 26.6 ms | 6.36 ms | 86.8 s | 7,797 MB | 825 MB | 108 s | 0.228 |
| nind, 4 threads per query | 0.886 ms | 11.1 ms | 3.79 ms | – | – | – | – | 0.228 |
| nind + local index | 1.44 ms | 31.0 ms | 6.16 ms | 234 s | 8,206 MB | 2,985 MB | 101 s | 0.228 |
| Lucene (Anserini) | 1.62 ms | 13.1 ms | 16.0 ms | 36.8 s | 3,439 MB | 647 MB | 2.16 s | 0.227 |
| Lucene + positions | 1.54 ms | 11.5 ms | 15.4 ms | 75.0 s | 5,638 MB | 2,525 MB | 3.38 s | 0.227 |
| tantivy | 5.71 ms | 29.6 ms | 9.39 ms | 49.7 s | 1,447 MB | 658 MB | 0.656 s | 0.227 |
| bm25s | 394 ms | 508 ms | 349 ms | 247 s | 14,003 MB | 2,237 MB | 252 s | 0.228 |

Stopwords kept (Lucene's 33 English stopwords are otherwise removed): frequent terms have very long
postings lists.

| system | top 10, median | top 10, 99th pct | top 1000, median | build | build memory | index | update +1% | nDCG@10 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| nind | 4.45 ms | 45.2 ms | 13.6 ms | 131 s | 10,507 MB | 915 MB | 121 s | 0.226 |
| nind, 4 threads per query | 2.43 ms | 21.8 ms | 9.03 ms | – | – | – | – | 0.226 |
| nind + local index | 4.59 ms | 52.4 ms | 14.4 ms | 357 s | 10,900 MB | 3,741 MB | 111 s | 0.226 |
| Lucene (Anserini) | 2.40 ms | 16.9 ms | 20.3 ms | 52.8 s | 3,438 MB | 709 MB | 3.47 s | 0.228 |
| Lucene + positions | 2.41 ms | 19.9 ms | 22.1 ms | 106 s | 5,459 MB | 3,058 MB | 4.33 s | 0.228 |
| tantivy | 41.1 ms | 108 ms | 46.2 ms | 55.1 s | 1,481 MB | 739 MB | 0.850 s | 0.227 |
| bm25s | 49.7 ms | 429 ms | 47.5 ms | 413 s | 18,153 MB | 2,853 MB | 341 s | 0.226 |

## Smaller corpora (BEIR)

Every query of each dataset; stopwords removed.

| dataset | documents | nind | Lucene (Anserini) | tantivy | bm25s |
|---|---:|---:|---:|---:|---:|
| SciFact | 5,183 | 0.066 ms · 0.665 | 0.646 ms · 0.664 | 0.053 ms · 0.664 | 0.081 ms · 0.665 |
| NFCorpus | 3,633 | 0.023 ms · 0.308 | 0.561 ms · 0.308 | 0.018 ms · 0.307 | 0.092 ms · 0.308 |
| FiQA | 57,600 | 0.493 ms · 0.238 | 0.803 ms · 0.241 | 0.308 ms · 0.241 | 0.248 ms · 0.238 |

Each cell: median top-10 latency · nDCG@10.

## How to read these numbers

- **Latency is measured per query, one at a time.** A server answering many queries at once
  gets its throughput from running them in parallel: nind's search releases Python's GIL and is
  thread-safe, so `search` can be called from many threads on one `NindEngine`. Splitting one query
  across threads (`search_threads`, `threads=`) lowers its latency on idle cores, but the cores do
  more work in total (4 threads: about 1.5–2× faster, not 4×). Keep the default (1) for
  throughput.
- **"nind, 4 threads per query" has no counterpart for the others here.** Lucene can also split a
  query (an `IndexSearcher` with an executor, over its segments); Anserini's searcher, used
  here, doesn't.
- **Lucene is called from Python** (pyjnius): its top-1000 latency mostly measures building one Java
  object per hit, so only the top 10 compares fairly.
- **nind + local index** also stores each document's terms and positions (as Amose reads them):
  larger and slower to build, the same search.
- **bm25s** precomputes every score: fast on small corpora, not built for millions of documents.
- **Updates.** nind updates its index files in place, term by term; Lucene and tantivy write a new
  segment, merged later. A build leaves no room in nind's postings lists, so the first update
  after a build rewrites every term it touches (the time above); a term rewritten by an update gets
  room, and later updates that only add documents rewrite only its last block (MS MARCO 4M: 20,000
  documents in about 5 s instead of 23 s).
- Single machine: a 14-core laptop-class processor (8 fast and 6 efficient cores), 62 GB of memory.
  Builds use 8 threads. Absolute times vary by a few percent between runs; the ratios hold.

## Reproduce

The scripts are in `bench/retrieval/` (see `bench/README.md`, section 4): `prepare.py` writes
the tokenized corpora, `driver.py` builds, searches and updates every system and writes
`report.csv`, and `site.py` writes this page from it.
