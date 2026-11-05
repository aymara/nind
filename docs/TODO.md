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
- [ ] Updates, what's left proportional to the index: the generation copy
  (Python's immutable generations; Amose writes in place) and the names and
  lengths side files rewritten whole; and a first update after a build
  rewrites its touched terms whole (builds leave no room: an option to leave
  some?).
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
- **Build memory.** ~19 bytes per token (7.8 GB for MS MARCO 8.8M); sharded
  builds would bound it per shard.
- **Concurrency benchmark.** Rerun `bench/retrieval/concurrency.py` on format
  v2.
- **Top 100.** Measure top 100 in the comparison benchmark (only top 10 and
  top 1000 are timed).
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
