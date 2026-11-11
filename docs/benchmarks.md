# Benchmarks

How nind compares with other indexers when it is used as an index plus BM25 top-k search:
**Lucene** (through Anserini), **tantivy** and **bm25s**, on the same tokens, the same BM25
parameters (k1 = 1.2, b = 0.75) and the same machine, from 3,600 documents to **138 million** passages.
nind 3.2.0 (the same code as 4.0.1), measured on a 48-thread server from 2026-11-05 to 2026-11-10. The laptop results this page
showed before are kept at the end.

```{admonition} In short
:class: tip

- **Search, 138 million passages** (MS MARCO v2). One query's top 10 takes **20.5 ms** on one
  thread (median), the same as Lucene (20.6 ms; tantivy 142 ms). An index of 8
  segments searched with 8 threads per query takes **2.86 ms**. 99th percentile: 365 ms on
  one thread (Lucene 237 ms), 43.2 ms with 8 segments and 8 threads.
- **Search, 8.8 million passages** (MS MARCO). **2.66 ms** on one thread (Lucene 2.93 ms,
  tantivy 10.0 ms), **0.510 ms** with 8 segments and 8 threads.
- **Stopwords kept** is nind's weak case: very long postings lists. At 138M, 138 ms on one thread
  against 36.1 ms for Lucene; 17.2 ms with 8 segments and 8 threads.
- **Many callers at once**, every system: at 8.8M nind serves the most at every load (2,567
  queries/s with 12 callers, Lucene 2,065; 4,847 with 48, Lucene's threads
  2,574). At 138M nind leads with few callers and Lucene with many (786 queries/s
  with 48 callers, nind 519).
- **Index larger than memory.** With 1 GB of page cache for a 14 GB index, nind's top 10 stays at
  19.8 ms (it reads about 1 MB per query); Lucene's goes from 20.1 to 103 ms.
  But nind holds 6.98 GB of its own (document names and lengths, read whole at open), Lucene
  1.23 GB.
- **Indexing.** 138M passages in **507 s** in 32 segments (Lucene 883 s,
  tantivy 1,042 s, 8 threads each), **230 s** with 48 threads; one index:
  1,426 s and 102 GB of memory. 8.8M passages: 31.0 s in 32 segments,
  104 s for one index (Lucene 67.8 s).
- **Updates.** Adding 1% more documents writes a new segment: 4.33 s at 8.8M (Lucene
  3.53 s, tantivy 1.25 s), 59.6 s at 138M (Lucene
  11.6 s, tantivy 16.6 s).
- **Quality.** The same BM25: nDCG@10 0.222 for nind and 0.222 for Lucene at 8.8M,
  0.0790 and 0.0808 at 138M; every nind layout returns the same top 10.
```

## Search latency as the corpus grows

```{image} _static/bench/scaling-light.svg
:class: only-light
:alt: Median top-10 latency against corpus size, 1M to 138M passages, for nind, nind in 8 segments with 8 threads, Lucene and tantivy; the same data is in the table below.
```

```{image} _static/bench/scaling-dark.svg
:class: only-dark
:alt: Median top-10 latency against corpus size, 1M to 138M passages, for nind, nind in 8 segments with 8 threads, Lucene and tantivy; the same data is in the table below.
```

Median time per query to get the 10 best documents, one query at a time: prefixes of MS MARCO v2
(stopwords removed), its 3,903 dev queries. nind uses block-max MaxScore: only the blocks of postings
that may reach the top 10 are read. Up to 64M passages nind is faster than Lucene on one thread; at
138M they are even. With 8 segments and 8 threads per query, each thread searches a segment, with the
same results.

| system | 1M | 4M | 8M | 16M | 32M | 64M | 138M |
|---|---:|---:|---:|---:|---:|---:|---:|
| nind | 0.468 ms | 1.23 ms | 2.09 ms | 3.53 ms | 5.94 ms | 10.2 ms | 20.5 ms |
| nind, 8 threads per query | 0.182 ms | 0.609 ms | 0.537 ms | 0.906 ms | 2.20 ms | 5.27 ms | 10.4 ms |
| nind, 8 segments, 8 threads per query | 0.350 ms | 0.434 ms | 0.517 ms | 0.689 ms | 1.01 ms | 1.71 ms | 2.86 ms |
| nind, 32 segments, 8 threads per query | 0.558 ms | 0.770 ms | 0.762 ms | 0.970 ms | 1.26 ms | 1.89 ms | 3.10 ms |
| Lucene (Anserini) | 1.37 ms | 2.13 ms | 2.98 ms | 4.54 ms | 7.67 ms | 13.2 ms | 20.6 ms |
| tantivy | 1.07 ms | 3.76 ms | 7.53 ms | 16.1 ms | 33.0 ms | 63.2 ms | 142 ms |

## MS MARCO v2, 138 million passages

```{image} _static/bench/msmarco-v2-light.svg
:class: only-light
:alt: MS MARCO v2 138M: search latency, build time, index size and update time per system; the same data is in the table below.
```

```{image} _static/bench/msmarco-v2-dark.svg
:class: only-dark
:alt: MS MARCO v2 138M: search latency, build time, index size and update time per system; the same data is in the table below.
```

| system | top 10, median | top 10, 99th pct | top 100, median | top 1000, median | build | build memory | index | update +1% | nDCG@10 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| nind | 20.5 ms | 365 ms | 32.2 ms | 55.2 ms | 1,426 s | 104,680 MB | 14,177 MB | 59.6 s | 0.079 |
| nind, 4 threads per query | 9.95 ms | 145 ms | 12.1 ms | 21.2 ms | – | – | – | – | 0.079 |
| nind, 8 threads per query | 10.4 ms | 111 ms | 12.3 ms | 20.3 ms | – | – | – | – | 0.079 |
| nind, 8 segments | 20.6 ms | 365 ms | 34.7 ms | 66.6 ms | 630 s | 111,005 MB | 14,640 MB | 76.9 s | 0.079 |
| nind, 8 segments, 8 threads per query | 2.86 ms | 43.2 ms | 5.06 ms | 10.9 ms | – | – | – | – | 0.079 |
| nind, 32 segments | 23.7 ms | 407 ms | 41.6 ms | 85.8 ms | 507 s | 30,314 MB | 15,164 MB | 83.5 s | 0.079 |
| nind, 32 segments, 8 threads per query | 3.10 ms | 43.7 ms | 5.36 ms | 11.8 ms | – | – | – | – | 0.079 |
| Lucene (Anserini) | 20.6 ms | 237 ms | 40.3 ms | 75.5 ms | 883 s | 26,503 MB | 12,972 MB | 11.6 s | 0.081 |
| tantivy | 142 ms | 728 ms | 142 ms | 157 ms | 1,042 s | 4,682 MB | 11,187 MB | 16.6 s | 0.079 |

Stopwords kept (Lucene's 33 English stopwords are otherwise removed): frequent terms have very long
postings lists (a term in most of the 138M passages). Not updated.

| system | top 10, median | top 10, 99th pct | top 100, median | top 1000, median | build | build memory | index | update +1% | nDCG@10 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| nind | 138 ms | 1,835 ms | 152 ms | 191 ms | 1,520 s | 134,430 MB | 15,409 MB | – | 0.077 |
| nind, 8 threads per query | 109 ms | 798 ms | 106 ms | 119 ms | – | – | – | – | 0.077 |
| nind, 8 segments | 125 ms | 1,778 ms | 151 ms | 219 ms | 844 s | 140,928 MB | 15,872 MB | – | 0.077 |
| nind, 8 segments, 8 threads per query | 17.2 ms | 232 ms | 22.5 ms | 34.8 ms | – | – | – | – | 0.077 |
| Lucene (Anserini) | 36.1 ms | 328 ms | 64.2 ms | 134 ms | 870 s | 27,336 MB | 13,905 MB | – | 0.080 |
| tantivy | 1,096 ms | 2,491 ms | 1,098 ms | 1,110 ms | 1,113 s | 4,590 MB | 12,173 MB | – | 0.078 |

The relevance judgments of MS MARCO v2's dev queries are sparse (about one passage per query), hence the
low nDCG@10 for every system. The top 10 of Lucene and nind overlap at 0.993 (tantivy
0.980). `verify_index` read back every definition of the one-index and 8-segment builds without error
(4.0G postings; 5.2G with stopwords).

## MS MARCO, 8.8 million passages

```{image} _static/bench/msmarco-light.svg
:class: only-light
:alt: MS MARCO 8.8M: search latency, build time, index size and update time per system; the same data is in the table below.
```

```{image} _static/bench/msmarco-dark.svg
:class: only-dark
:alt: MS MARCO 8.8M: search latency, build time, index size and update time per system; the same data is in the table below.
```

| system | top 10, median | top 10, 99th pct | top 100, median | top 1000, median | build | build memory | index | update +1% | nDCG@10 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| nind | 2.66 ms | 44.9 ms | 4.77 ms | 10.0 ms | 104 s | 7,704 MB | 825 MB | 4.33 s | 0.222 |
| nind, 4 threads per query | 0.864 ms | 9.04 ms | 1.69 ms | 3.96 ms | – | – | – | – | 0.222 |
| nind, 8 threads per query | 0.625 ms | 5.51 ms | 1.24 ms | 5.37 ms | – | – | – | – | 0.222 |
| nind, 8 segments | 3.02 ms | 49.1 ms | 6.24 ms | 13.4 ms | 34.6 s | 9,566 MB | 928 MB | 4.47 s | 0.222 |
| nind, 8 segments, 8 threads per query | 0.510 ms | 4.87 ms | 1.12 ms | 2.98 ms | – | – | – | – | 0.222 |
| nind, 32 segments | 4.22 ms | 57.9 ms | 9.24 ms | 18.2 ms | 31.0 s | 3,520 MB | 1,071 MB | 96.3 s | 0.222 |
| nind, 32 segments, 8 threads per query | 0.806 ms | 6.32 ms | 1.46 ms | 3.78 ms | – | – | – | – | 0.222 |
| nind + local index | 2.65 ms | 46.8 ms | 4.79 ms | 10.0 ms | 118 s | 7,954 MB | 2,985 MB | 5.14 s | 0.222 |
| Lucene (Anserini) | 2.93 ms | 21.6 ms | 6.73 ms | 27.6 ms | 67.8 s | 3,220 MB | 650 MB | 3.53 s | 0.222 |
| Lucene + positions | 2.98 ms | 22.7 ms | 6.94 ms | 24.1 ms | 130 s | 8,988 MB | 2,525 MB | 3.52 s | 0.222 |
| tantivy | 10.0 ms | 65.4 ms | 10.7 ms | 17.2 ms | 76.2 s | 1,434 MB | 658 MB | 1.25 s | 0.222 |
| bm25s | 777 ms | 976 ms | 778 ms | 777 ms | 381 s | 13,681 MB | 2,237 MB | 379 s | 0.228 |

Stopwords kept:

| system | top 10, median | top 10, 99th pct | top 100, median | top 1000, median | build | build memory | index | update +1% | nDCG@10 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| nind | 10.2 ms | 141 ms | 14.4 ms | 25.7 ms | 103 s | 9,895 MB | 915 MB | 5.45 s | 0.222 |
| nind, 4 threads per query | 3.28 ms | 31.8 ms | 4.76 ms | 10.1 ms | – | – | – | – | 0.222 |
| nind, 8 threads per query | 4.52 ms | 34.4 ms | 3.49 ms | 8.47 ms | – | – | – | – | 0.222 |
| nind, 8 segments | 10.8 ms | 144 ms | 17.1 ms | 33.7 ms | 39.3 s | 11,595 MB | 1,018 MB | 5.37 s | 0.222 |
| nind, 8 segments, 8 threads per query | 1.49 ms | 15.9 ms | 2.62 ms | 6.63 ms | – | – | – | – | 0.222 |
| nind, 32 segments | 12.5 ms | 152 ms | 22.2 ms | 51.9 ms | 36.2 s | 4,086 MB | 1,161 MB | 109 s | 0.222 |
| nind, 32 segments, 8 threads per query | 1.73 ms | 18.1 ms | 3.11 ms | 7.82 ms | – | – | – | – | 0.222 |
| nind + local index | 10.3 ms | 144 ms | 14.8 ms | 26.3 ms | 187 s | 10,149 MB | 3,741 MB | 6.55 s | 0.222 |
| Lucene (Anserini) | 4.02 ms | 29.0 ms | 8.99 ms | 32.0 ms | 73.7 s | 3,954 MB | 716 MB | 3.85 s | 0.222 |
| Lucene + positions | 4.19 ms | 30.4 ms | 9.31 ms | 28.6 ms | 146 s | 8,131 MB | 3,080 MB | 4.24 s | 0.222 |
| tantivy | 72.9 ms | 201 ms | 74.1 ms | 83.0 ms | 80.2 s | 1,458 MB | 740 MB | 1.34 s | 0.221 |
| bm25s | 193 ms | 1,445 ms | 194 ms | 186 ms | 473 s | 17,876 MB | 2,853 MB | 474 s | 0.226 |

bm25s: the first 500 queries (it scores every document), hence its different nDCG@10.

## Threads

**Threads per query**, one query at a time (`NindEngine.search(..., threads=t)`): median ms, top 10.
Up to 12 threads on one socket's physical cores, 16 and 24 on both sockets', 48 on every hardware
thread (here and below). Each run is a process of its own (see "Separate processes vary" below:
2.10 ms here for the 2.66 ms of the table above).

| index | 1 | 2 | 4 | 8 | 12 | 16 | 24 |
|---|---:|---:|---:|---:|---:|---:|---:|
| 8.8M, one index | 2.10 | 1.46 | 0.835 | **0.566** | 0.892 | 1.44 | 1.40 |
| 8.8M, 8 segments | 2.38 | 1.32 | 0.782 | 0.509 | **0.507** | 0.811 | 1.04 |
| 8.8M, 32 segments | 4.18 | 2.47 | 1.51 | 0.762 | **0.700** | 0.965 | 1.08 |
| 8.8M stopwords kept, one index | 8.18 | 5.04 | **3.57** | 4.27 | 5.33 | 5.95 | 5.56 |
| 8.8M stopwords kept, 8 segments | 8.69 | 4.61 | 2.52 | **1.46** | 1.47 | 2.19 | 2.89 |
| 8.8M stopwords kept, 32 segments | 9.79 | 5.28 | 2.97 | 1.72 | **1.42** | 1.73 | 1.68 |
| 138M, one index | 20.8 | 11.5 | **8.08** | 10.2 | 10.6 | 11.1 | 9.82 |
| 138M, 8 segments | 20.6 | 9.55 | 4.78 | 2.88 | **2.84** | 4.11 | 4.97 |
| 138M, 32 segments | 23.5 | 10.4 | 5.24 | 3.07 | 2.65 | **2.49** | 3.66 |
| 138M stopwords kept, one index | 179 | 139 | 117 | 107 | 103 | 103 | **89.0** |
| 138M stopwords kept, 8 segments | 128 | 59.5 | 34.5 | 16.2 | **15.8** | 21.8 | 24.4 |

A single index splits its documents into ranges, one per thread; its gain stops at 8 threads at 8.8M
(4 with stopwords kept) and at 4 at 138M. Segments scale further: each thread searches its
own segments, sharing the top 10's threshold. Crossing to the second socket costs more than it gives.

**Many callers at once**, every system: N callers, each sending its next query when the last
one returns (`bench/retrieval/concurrency.py`), top 10, stopwords removed; queries per second, the best
of each row in bold. Threads share one searcher (nind, Lucene through pyjnius and tantivy-py release
Python's GIL while they search); processes each open the index, as separate servers would. Pinned as
above. Measured 2026-11-11 with nind 4.1.0's development version (document names mapped, MaxScore by windows for 5 terms or more: `docs/TODO.md`).

MS MARCO 8.8M, threads:

| callers | nind | nind, 8 segments | Lucene (Anserini) | tantivy |
|---:|---:|---:|---:|---:|
| 1 | **218** | 182 | 208 | 76 |
| 2 | **436** | 360 | 415 | 150 |
| 4 | **870** | 718 | 808 | 304 |
| 8 | **1,727** | 1,428 | 1,590 | 603 |
| 12 | **2,567** | 2,105 | 2,065 | 928 |
| 24 | **4,350** | 3,939 | 3,144 | 1,762 |
| 48 | **4,847** | 4,371 | 2,574 | 1,857 |

MS MARCO v2 138M, threads:

| callers | nind | nind, 8 segments | Lucene (Anserini) | tantivy |
|---:|---:|---:|---:|---:|
| 1 | **24** | 24 | 22 | 5 |
| 2 | **48** | 48 | 43 | 10 |
| 4 | 94 | **96** | 86 | 20 |
| 8 | 183 | 187 | **205** | 41 |
| 12 | 263 | 276 | **284** | 60 |
| 24 | 474 | 483 | **599** | 120 |
| 48 | 519 | 673 | **786** | 132 |

Processes, 8.8M:

| callers | nind | nind, 8 segments | Lucene (Anserini) | tantivy |
|---:|---:|---:|---:|---:|
| 1 | **217** | 181 | 212 | 75 |
| 2 | **423** | 356 | 414 | 151 |
| 4 | **815** | 678 | 781 | 304 |
| 8 | **1,621** | 1,336 | 1,457 | 600 |
| 12 | **2,433** | 1,935 | 1,887 | 895 |
| 24 | **4,588** | 3,467 | 3,068 | 1,602 |
| 48 | **5,666** | 4,371 | 4,906 | 1,819 |

Processes, 138M:

| callers | nind | nind, 8 segments | Lucene (Anserini) | tantivy |
|---:|---:|---:|---:|---:|
| 1 | 24 | 24 | **25** | 5 |
| 2 | **48** | 47 | 42 | 10 |
| 4 | **89** | 86 | 79 | 20 |
| 8 | 163 | **165** | 145 | 38 |
| 12 | 237 | **240** | 162 | 55 |
| 24 | 384 | **444** | 359 | 117 |
| 48 | 498 | 554 | **627** | 102 |

99th percentile at 138M, threads, ms:

| callers | nind | nind, 8 segments | Lucene (Anserini) | tantivy |
|---:|---:|---:|---:|---:|
| 1 | 263 | 259 | **247** | 725 |
| 2 | 268 | 261 | **242** | 744 |
| 4 | 271 | 258 | **242** | 745 |
| 8 | 274 | 267 | **223** | 741 |
| 12 | 273 | 266 | **226** | 754 |
| 24 | 314 | 301 | **218** | 749 |
| 48 | 580 | 407 | **316** | 1,500 |

- **8.8M: nind serves the most at every load**, threads or processes: 2,567 queries/s with 12
  callers (one socket's cores; Lucene 2,065), 4,847 with 48 threads (Lucene's lose past the
  cores: 2,574), 5,666 with 48 processes (Lucene 4,906, each with its own JVM).
- **138M: nind leads with few callers, Lucene with many**: 24 against 22 queries/s
  alone, 519 (8 segments: 673) against 786 with 48 threads. nind gains less from
  more threads on the large index (474 to 519 from 24 to 48 callers, Lucene
  599 to 786); `docs/TODO.md`. 99th percentile alone: 263 ms, Lucene's
  247 ms. (Before MaxScore scanned queries of 5 terms or more in windows, term at a time,
  nind served 477 with 48 threads at 138M and 4,303 at 8.8M.)
- **Segments** serve about as many queries as one index up to 24 callers (each query does the same
  work; they lower latency with threads per query, above), more with 48 threads at 138M
  (673 against 519).
- **Processes** each run their share of the queries after their own warm-up: with many of them, few
  queries each (Lucene's JVMs warm up apart), so those rows vary more.
- **Memory of its own per process** (anonymous memory, 138M): nind 1.06 GB; nind, 8 segments 1.05 GB; Lucene (Anserini) 0.83 GB; tantivy 0.04 GB. A system that maps its
  index (Lucene, tantivy) shares those pages between processes, as nind shares its mapped document
  names; nind's are mostly its documents' lengths and its directory cache.

**Build threads**, 8.8M passages (`--threads`): seconds.

| system | 1 | 2 | 4 | 8 | 12 | 24 | 48 |
|---|---:|---:|---:|---:|---:|---:|---:|
| nind | 131 s | 97.9 s | 80.0 s | **76.4 s** | 78.2 s | 93.6 s | 124 s |
| nind, 32 segments | 168 s | 95.9 s | 51.0 s | 29.1 s | 23.3 s | 16.1 s | **12.4 s** |
| Lucene (Anserini) | 137 s | 122 s | 73.3 s | **59.1 s** | 71.9 s | 102 s | 98.6 s |
| tantivy | 82.2 s | **59.2 s** | 102 s | 90.2 s | 88.3 s | 152 s | 108 s |

A build in 32 segments runs its shards in parallel: it scales to every hardware thread (peak memory
0.492 GB on 1 thread, 8.53 GB on 48). One index has a single writer, so threads only
tokenize. At 138M with 48 threads: nind in 32 segments 230 s (peak 106 GB), Lucene
1,241 s, tantivy 1,543 s (both slower than with 8 threads on one socket).

## Index larger than memory

MS MARCO v2, 138M passages, stopwords removed (indexes of 11 to 15 GB): each system searches in a
cgroup (`systemd-run --user -p MemoryMax=...`) holding its own memory (the peak of an uncapped run,
+10%) plus a budget for the page cache; the index is evicted from the cache first, so every page
it reads counts. The first 2,000 queries after a warm-up of 500; median ms.

| system | own memory | no limit | 8 GB | 4 GB | 2 GB | 1 GB |
|---|---:|---:|---:|---:|---:|---:|
| nind | 6.98 GB | 23.1 | 19.3 | 19.2 | 19.4 | 19.8 |
| nind, 8 segments, 8 threads per query | 7.81 GB | 3.82 | 10.6 | 11.9 | 16.4 | 19.0 |
| Lucene (Anserini) | 1.23 GB | 20.1 | 22.9 | 52.3 | 84.3 | 103 |
| tantivy | 0.795 GB | 151 | 122 | 127 | 143 | 152 |

Top 1000, median ms:

| system | own memory | no limit | 8 GB | 4 GB | 2 GB | 1 GB |
|---|---:|---:|---:|---:|---:|---:|
| nind | 6.98 GB | 57.9 | 45.2 | 58.1 | 46.7 | 48.2 |
| nind, 8 segments, 8 threads per query | 7.81 GB | 12.6 | 12.9 | 13.3 | 19.2 | 24.7 |
| Lucene (Anserini) | 1.23 GB | 88.9 | 78.9 | 595 | 1,383 | 2,022 |
| tantivy | 0.795 GB | 163 | 132 | 157 | 326 | 478 |

- **One thread on nind's index barely reads.** MaxScore reads only the blocks it needs, with
  positioned reads: about 1.3 MB per query from disk with a 1 GB budget (1.0 MB from a cold cache
  without one), which the SSDs serve without changing the median. (Its uncapped run is the slower
  one: separate processes vary, see "How to read these numbers".)
- **8 segments with 8 threads per query** read more per query (2.3 MB at 1 GB) in a few
  milliseconds: the disk shows, from 3.82 ms without a limit to 10.6 ms with 8 GB and 19.0 ms with
  1 GB, about the single thread's time.
- **Lucene and tantivy map their files** (`mmap`): with less cache, Lucene's top 10 gets
  5× slower and its top 1000 (which loads each hit's stored id) much more; tantivy's top
  1000 too. Readahead on mapped files is a likely cause, not investigated.
- **nind's own memory is the limit**: 6.98 GB before any cache. At open it reads the document
  names (4.96 GB) and lengths (1.66 GB) files whole; Lucene needs 1.23 GB and
  tantivy 0.795 GB. Reading them on demand (`mmap`) is in `docs/TODO.md`.

## Smaller corpora (BEIR)

Every query of each dataset; stopwords removed.

| dataset | documents | nind | Lucene (Anserini) | tantivy | bm25s |
|---|---:|---:|---:|---:|---:|
| SciFact | 5,183 | 0.114 ms · 0.665 | 1.10 ms · 0.664 | 0.078 ms · 0.664 | 0.117 ms · 0.665 |
| NFCorpus | 3,633 | 0.033 ms · 0.308 | 0.749 ms · 0.308 | 0.029 ms · 0.307 | 0.140 ms · 0.308 |
| FiQA | 57,600 | 0.610 ms · 0.238 | 1.53 ms · 0.241 | 0.393 ms · 0.241 | 0.393 ms · 0.238 |

Each cell: median top-10 latency · nDCG@10.

## How to read these numbers

- **Latency is measured per query, one at a time**: the median of every dev query (3,903 for
  MS MARCO v2, 6,980 for v1), the 99th percentile for the slowest. Runs are pinned to one socket's
  12 physical cores with its memory (`numactl`); the corpus is evicted from the page cache and the
  index read once before searching; a warm-up (200 queries at 8.8M, a whole pass on v2), then 3
  measured passes at 8.8M (the median shown; they differ by a few percent), one on v2's prefixes and
  with stopwords kept, 2 at 138M.
- **Threads per query have no counterpart for the others here.** Lucene can also split a query (an
  `IndexSearcher` with an executor, over its segments); Anserini's searcher, used here, doesn't.
- **Segments** are what `index_jsonl(shards=N)` and updates leave (`docs/proposals/segments.md`): a
  build of N shards in parallel, searched as one index with the same results. The first update of a
  32-segment build merges a tier of them at 8.8M (96.3 s); at 138M segments that large are
  left alone.
- **Lucene is called from Python** (pyjnius): its top-1000 latency mostly measures building one Java
  object per hit, so the top 10 and top 100 compare best.
- **nind + local index** also stores each document's terms and positions (as Amose reads them):
  larger and slower to build, the same search.
- **bm25s** precomputes every score: fast on small corpora, not built for millions of documents (left
  out at 138M).
- **Separate processes vary.** The same search in a new process sometimes runs about 20% faster
  (2.1 instead of 2.7 ms at 8.8M), for reasons not found yet; the passes of one process agree.
- Server: 2 × Intel Xeon Gold 5317 (12 cores, 24 hardware threads, one memory node each), 251 GB of
  memory, SATA SSDs, Linux 5.14, Java 21. Builds use 8 threads unless said otherwise.

## Earlier results: laptop

nind 3.2.0 on 2026-11-05, the other systems on 2026-11-04: a 14-core
laptop-class processor (8 fast and 6 efficient cores), 62 GB of memory, MS MARCO v1 only, 500 queries
per size (hence a different nDCG@10), unpinned. The ratios mostly match the server's; absolute times are lower there (faster
cores).

```{image} _static/bench/laptop-scaling-light.svg
:class: only-light
:alt: Laptop: median top-10 latency against corpus size for nind, nind with 4 threads, Lucene and tantivy; the same data is in the table below.
```

```{image} _static/bench/laptop-scaling-dark.svg
:class: only-dark
:alt: Laptop: median top-10 latency against corpus size for nind, nind with 4 threads, Lucene and tantivy; the same data is in the table below.
```

| system | 1M | 2M | 4M | 8.8M |
|---|---:|---:|---:|---:|
| nind | 0.373 ms | 0.540 ms | 0.900 ms | 1.50 ms |
| nind, 4 threads per query | 0.197 ms | 0.333 ms | 0.517 ms | 0.886 ms |
| Lucene (Anserini) | 0.804 ms | 0.978 ms | 1.19 ms | 1.62 ms |
| tantivy | 0.761 ms | 1.50 ms | 2.68 ms | 5.71 ms |

```{image} _static/bench/laptop-msmarco-light.svg
:class: only-light
:alt: Laptop, MS MARCO 8.8M: search latency, build time, index size and update time per system; the same data is in the table below.
```

```{image} _static/bench/laptop-msmarco-dark.svg
:class: only-dark
:alt: Laptop, MS MARCO 8.8M: search latency, build time, index size and update time per system; the same data is in the table below.
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

Stopwords kept:

| system | top 10, median | top 10, 99th pct | top 1000, median | build | build memory | index | update +1% | nDCG@10 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| nind | 4.45 ms | 45.2 ms | 13.6 ms | 131 s | 10,507 MB | 915 MB | 121 s | 0.226 |
| nind, 4 threads per query | 2.43 ms | 21.8 ms | 9.03 ms | – | – | – | – | 0.226 |
| nind + local index | 4.59 ms | 52.4 ms | 14.4 ms | 357 s | 10,900 MB | 3,741 MB | 111 s | 0.226 |
| Lucene (Anserini) | 2.40 ms | 16.9 ms | 20.3 ms | 52.8 s | 3,438 MB | 709 MB | 3.47 s | 0.228 |
| Lucene + positions | 2.41 ms | 19.9 ms | 22.1 ms | 106 s | 5,459 MB | 3,058 MB | 4.33 s | 0.228 |
| tantivy | 41.1 ms | 108 ms | 46.2 ms | 55.1 s | 1,481 MB | 739 MB | 0.850 s | 0.227 |
| bm25s | 49.7 ms | 429 ms | 47.5 ms | 413 s | 18,153 MB | 2,853 MB | 341 s | 0.226 |

BEIR:

| dataset | documents | nind | Lucene (Anserini) | tantivy | bm25s |
|---|---:|---:|---:|---:|---:|
| SciFact | 5,183 | 0.066 ms · 0.665 | 0.646 ms · 0.664 | 0.053 ms · 0.664 | 0.081 ms · 0.665 |
| NFCorpus | 3,633 | 0.023 ms · 0.308 | 0.561 ms · 0.308 | 0.018 ms · 0.307 | 0.092 ms · 0.308 |
| FiQA | 57,600 | 0.493 ms · 0.238 | 0.803 ms · 0.241 | 0.308 ms · 0.241 | 0.248 ms · 0.238 |

Each cell: median top-10 latency · nDCG@10.

nind's update numbers there are those of an index written in place (before segments): a first update
after a build rewrote every term it touched.

## Reproduce

The scripts are in `bench/retrieval/` (see `bench/README.md`, sections 4 and 7): `prepare.py` writes
the tokenized corpora, `driver.py` builds, searches and updates every system and writes
`report.csv`, `concurrency.py` measures throughput, and `site.py` writes this page from them. The
server runs' raw results and scripts are in `bench/retrieval/results/2026-11-10-server/`.
