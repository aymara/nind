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
  - **Names.** Read whole and validated at open (0.10 s at 8.8M, ~30 bytes per
    document): `mmap`?
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
  - **Direct builds at 138M**: 102 GB of memory and 1,426 s (one writer); 32
    segments: 30 GB, 507 s. Make segments the documented way past ~10M?
  - **Updates at 138M**: +1% in 60–84 s (a new segment) against 12 s for Lucene
    and 17 s for tantivy: where does it go?
  - **Amose.** Adapt it to `NindSegmentedIndex`/`NindSegmentedUpdate` (compound
    words on updates, term specifics: see the proposal); `NindAmoseMerge` needs
    its CLI (below). This doesn't constrain nind's design any more.
- [x] **Concurrency benchmark** on format v2, up to 48 threads and 138M
  (section 7): 20.6× on 24 cores.
- [x] **Top 100** in the comparison benchmark.
- **Stopwords kept at scale.** MS MARCO v2 138M, top 10 on one thread: 138 ms vs
  36 ms for Lucene (16M: 15.5 vs 6.1 ms); the gap grows with the corpus.
- **Latency varies between processes.** The same search, one process per run:
  2.12 or 2.67 ms (8.8M, one thread). Not huge pages, the memory sampler or
  names; NUMA placement of the page cache or of the scorer's arrays?
- **Index larger than memory** (the benchmarks' stage 5): not run yet.
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
