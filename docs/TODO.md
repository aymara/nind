# nind to-do list

Kept up to date as items are done; the decisions behind them are in
`docs/proposals/` and `bench/README.md`. Started 2026-11-04, after the release
of 3.0.0 (format v2).

## In progress (branch `v2-perf`)

- [x] Writers fill each block's `<longueurMin>` (the shortest document length
  in the block: tighter MaxScore bounds); MaxScore's threshold
  (`PRUNING_DOCS_PER_RESULT`) measured again at the target sizes (≥ 4M
  documents; 1–2M isn't a target).
- [x] Cheap updates: an update rewrites only what changed (the touched terms'
  last blocks and directories), in the C++ API (`NindIndexUpdate`,
  `NindTermIndex::updateTermDef`).
- [x] Updates, what's left proportional to the index: gone for Python
  (segments, hard-linked generations: `docs/proposals/segments.md`). A C++
  client writing one index in place (`NindIndexUpdate`) still rewrites a touched
  term whole on a first update after a build (builds leave no room).
- [x] Bit-packed blocks (flag 73): 18% smaller index, MaxScore 19–26% faster
  (nind ≥ 3.1 needed to read them).
- [x] Everything in the C++ API (Amose and other clients use it directly):
  `NindBm25Scorer` (block-max MaxScore), `NindIndexBuilder`, and
  `IndexUpdate`'s change logic (done: `NindIndexUpdate`); Python keeps the
  generations and publishing.

## Later

- **v1-only diagnostics.** The per-format structural statistics
  (`analyseFichierLexiconindex`/`Termindex`/`Localindex`/`Retrolexicon`
  beyond the native base) and the lexicon's own tools (`dumpeFichier`,
  `debogueIndex`, `donneCollisions`, `donneMax`, `donneClef`, and the retro
  lexicon's `dumpeFichier`) still parse v1 only and raise
  `NotImplementedError` on v2. Move them to native (see CLAUDE.md, "Planned
  follow-up work").
- **Amose and LIMA readers.** They need adapting to format v2 (nind's own
  `NindAmose` classes already write v2); until then they need nind < 3.
  From `Amose/amose/docs/search-algorithms.md` (§10, §13), for that work,
  on the Amose side (the plugin `amose_nindindex`):
  - `CoreNindIndexInvertedfileReader.cpp:249` passes a `std::list<unsigned int>`
    to `NindTermAmose::getDocList` (64-bit `NindDocId` since 3.0.0).
  - `tf` is read from the local index once per posting
    (`DocListIteratorImpl.cpp:80`, unlocked): `NindTermAmose::getDocPostings`
    (3.1.1) returns the postings with their frequencies.
  - Deletion only empties the node's local definition: postings and statistics
    keep the node until a rebuild (`NindTermAmose::removeDocFromTerm` exists).
  - One `QMutex` per reader around nind's sequential API: the `const` paths
    (`getDocPostings(…, buffer)`, `getTermDef(…, buffer)`...) are thread-safe.
  - `reset()` removes files with the old extensions (`.lexiconindex`, …), not
    `.nindlexiconindex`.
  - List mode's MaxScore is compiled out (`SATURATION`): the block descriptors
    (`NindTermIndex::getTermDirectory`, `getTermBlocks`: max frequency, shortest
    length per 128 postings) could drive it.
  On the nind side, done in 3.1.1: `NindTermAmose::addDocsToTerm` appends in
  place (node-by-node indexing, no bulk mode: per call cost flat, 12.9 vs
  66.4 µs at 50k nodes).
- **Hundreds of millions of documents** (`docs/proposals/segments.md`). Done:
  `index_jsonl(shards=N)`, a merge without lists, Python dicts or global sort,
  search across segments, updates that cost what they change, tiered merges
  (+88k documents in 2.6 s at 8.8M, build 25 s vs 46 s, identical top 10). Left:
  - [x] **Run it for real.** MS MARCO v2, 138M passages (`bench/README.md`
    section 7): builds direct and in 8 or 32 segments, updates, search with
    threads, `verify_index` on every segment (4.0G and 5.2G postings), the same
    top 10 for every layout.
  - **Worst-case term.** A synthetic term in 350M documents went through shards
    and merge (18.3 GB, 71 MB); the run was stopped under system memory pressure
    during `verify_index` (it then read such a term twice as lists, possibly a
    contributor; it reads flat now), so reading it back is unverified:
    `bench/worst_case_term.py` to rerun (the server has the room).
  - **Merge memory.** Postings vectors grow by doubling (18.3 GB there, ~12
    possible with a reserve from the shards' directories); the merge is one
    thread, and needs the shards and the merged index on disk together.
  - **Local index readers.** Opening an index no longer reads a header per
    document (0.4 s at 8.8M, was 7.3 s), but the first local index lookup does,
    once (~4 min at 300M, extrapolated): store the id map in the file?
  - **Names and lengths.** Read whole at open (names validated: 0.10 s at 8.8M,
    ~30 bytes per document). At 138M that is 6.6 GB read at open (names 4.96 GB,
    lengths 1.66 GB) and 7.5 GB of the search process's memory, against 1.3 GB
    for Lucene: with 1 GB of page cache nind's search is unchanged, so this is
    what bounds the machine it needs (`bench/README.md` section 7). `mmap`, or
    names looked up in the file?
  - **Merges.** A commit's merges are synchronous and single-threaded (17.6 s for
    the first update of a 16-shard 8.8M build): in the background, published as
    their own generation?
  - **Writer memory.** An update needing a lookup (removal, replacement, name)
    opens the whole index, ~40 bytes per document (~12 GB at 300M): lookups in
    the files instead? The scorer's 256 MB directory cache is per segment.
  - [x] **Search at 100M+ with threads** (section 7): one index stops gaining at
    4 threads (2.6×), 8 segments reach 7× on 8–12. An exhaustive search takes 12
    bytes per document of scratch per concurrent search (per segment, so at most
    its own size at a time).
  - **Build memory.** A direct build at 138M took 102 GiB and 1,426 s (one
    writer), 4 times Lucene's 26 GiB for a similar index (13.8 vs 12.7 GiB): 23
    bytes per token, 7.4 times the index. Done:
    - [x] **The builder's buffers.** `NindIndexBuilder` writes the term index
      from 5 bytes per posting (32-bit document positions, 8-bit frequencies,
      255 and more apart) instead of 12, its buffers are reserved up front (they
      were copied at every batch), `_write_chunks` frees each tokenized chunk
      once added, the names are written without a dict. The same files; 8M:
      6.5 -> 4.3 GiB, adds 11.5 -> 3.5 s; 138M: 102 -> 57 GiB, 1,426 -> 1,127 s.
    - [x] **Segments, the documented route past ~10M documents** (README,
      quickstart, `index_jsonl`): faster builds, bounded memory, same results.
    - [x] **A memory budget**, `index_jsonl(memory_gb=X)`: the fewest shards
      (and, if needed, fewer workers) that fit, from samples of the file and a
      model of a build (`BUILD_BYTES_PER_*`: tokens, documents, and the
      vocabulary, ~1 KB per distinct term, extrapolated from the samples with
      Heaps' law); pool workers give freed memory back after each shard
      (`malloc_trim`: a worker's next shard started 0.4 GiB higher). 138M,
      8 workers: 16 GiB -> 56 segments, 14.3 GiB peak, 388 s; 12 GiB -> 86
      segments, 10.3 GiB, 400 s; 8M in 2 GiB: 13 segments, 2 workers, 1.3 GiB.
    Left:
    - **Term-range passes.** Build the postings of 1/K of the terms at a time,
      scanning the tokens K times: the postings array (5 bytes per posting, the
      largest buffer with the tokens' 4) divided by K, for one more sequential
      scan per pass (~18 GiB at 138M: seconds against minutes). For a direct
      build under a budget.
    - **Compressed tokens.** Term ids in variable-length bytes: ~2 of the 4
      bytes per token, for a decoding cost on the two passes over them.
    - **Sorted runs on disk and a merge** (Lucene's way): memory bounded
      whatever the corpus, for I/O and a merge; segments already give it.
    - **What Python holds.** A direct `index_jsonl` keeps the record ids, as it
      returns them (77 bytes per document: 10.6 GiB at 138M); the vocabulary
      costs ~950 bytes per distinct term in a build (each chunk's own word list,
      the union, `term_to_id`, the lexicon writer's input), 2.2 GiB for 2.5M
      terms. The union and the id maps in C++ (chunk vocabularies as one buffer)?
    - **The names check after a sharded build** reads every shard's names whole
      (`NindDocNames::merge` to a file then removed): 7.2 GiB at 138M, which no
      number of shards divides, so `memory_gb` can't go below ~8.5 GiB there.
      Hashes of the names, sorted, and only collisions compared? With the
      "Names and lengths" item below.
    - **The model is fitted on MS MARCO v2** (shards of 1M to 14M passages; up
      to 50% over for small shards, which costs segments). Check it on another
      kind of corpus (long documents, code); `merge=True`'s memory isn't in the
      budget; `index_files` has no `memory_gb` yet.
    - **The benchmarks page** shows the builds before these changes: rerun the
      build phases (direct and segments, 8.8M and 138M).
  - **Updates at 138M**: +1% in 60–84 s (a new segment) against 12 s for Lucene
    and 17 s for tantivy: where does it go?
  - **Amose.** Adapt it to `NindSegmentedIndex`/`NindSegmentedUpdate` (compound
    words on updates, term specifics: see the proposal); `NindAmoseMerge` needs
    its CLI (below). This doesn't constrain nind's design any more.
- [x] **Concurrency benchmark** on format v2, up to 48 threads and 138M
  (section 7): 20.6× on 24 cores.
- **Several callers, all systems.** The concurrency benchmark ran nind only: does
  its per-query lead hold under load against Lucene and tantivy (bm25s if cheap)?
  - Threads sharing one searcher (how nind and Lucene are served). First check
    whether pyjnius and tantivy-py release the GIL during a search; if not, run
    Lucene's threads in Java (one `IndexSearcher`).
  - N processes on the same index: independent of the GIL, and shows nind's
    names and lengths read again by each process (6.6 GB at 138M).
  - Systems nind, nind-8s, Lucene, tantivy; 1–48 callers, each sending its next
    query when the last returns, stage 3's pinning; queries/s, p50 and p99 per
    caller; 8.8M (in memory) and 138M. The indexes in `nind-bench` serve as is.
  - Optional: queries arriving at a fixed rate (50% and 80% of each system's
    maximum) for tail latency, closer to a service.
  - `concurrency.py` generalised to every system; about 6–8 h of runs; extends
    the Threads section of `docs/benchmarks.md`.
- [x] **Top 100** in the comparison benchmark.
- **Stopwords kept at scale.** MS MARCO v2 138M, top 10 on one thread: 138 ms vs
  36 ms for Lucene (16M: 15.5 vs 6.1 ms); the gap grows with the corpus.
- **Latency varies between processes.** The same search, one process per run:
  2.12 or 2.67 ms (8.8M, one thread). Not huge pages, the memory sampler or
  names; NUMA placement of the page cache or of the scorer's arrays?
- [x] **Index larger than memory** (section 7): with 1 GB of page cache for a
  14 GB index, nind's top 10 is unchanged; 8 segments with 8 threads lose
  their lead (3.8 -> 19 ms).
- **Search, what's left** (done in 3.1.2–3.2: windowed block-max MaxScore,
  intra-query threads with a persistent pool, impacts, a directory cache,
  length prefetching; MS MARCO 8.8M top 10: 2.4–2.5 ms on one thread, about
  1.5× Lucene's 1.6 ms). perf shows the window scan at ~70% of a search and
  charges it to the length of each scored document and the BM25 division;
  decoding ~17%. Tried after 3.2.0 and dropped, measured with alternated runs
  (identical results each time):
  - batch scoring (a decoded essential block's contributions computed at once,
    lengths gathered first): +1–2%, no gain;
  - re-sorting the window's terms from the previous order (nearly sorted):
    +4%;
  - lengths on one byte (Lucene's norms, 8.8 MB instead of 17.7): no gain
    (2.63 vs 2.58 ms), so scores weren't changed for it.
  So the cost isn't the lengths' memory: ~70 ns (~250 cycles) per scored
  document goes to instructions spread over windows and probes. What's left
  is deeper: score windows in batches as Lucene's MaxScoreBulkScorer does
  (fixed windows of documents, arrays and bitsets, fewer per-document
  branches), compare documents scored per query with Lucene's, SIMD
  decoding. Several categories (Amose) aren't pruned yet.
- **Large k on small corpora.** The v2 exhaustive path is ~25% slower than v1
  at top 1000 on 1–2M documents (block decoding); not a target size.
- **Original author's to-do.** `NindLexiconIndex::addWord` doesn't check that
  words are UTF-8.
- **`NindLexiconAmose::getWord`.** It reads past the end of the component list
  for the lexicon word `§` alone (undefined behaviour).
- **CLI for Amose/LIMA multi-process indexing.** A small C++ program running
  `NindAmoseMerge` over shard prefixes.
