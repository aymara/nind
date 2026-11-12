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
  - **Merge memory.** Postings vectors grew by doubling (18.3 GB there, ~12
    possible with a reserve from the shards' directories). Done: a term of a
    million postings or more in a shard gets its categories reserved from every
    shard's directory (checked: 1.5M documents in 3 shards, the same index as a
    direct build, `verify_index` clean); to measure on the worst-case term. The
    merge is still one thread, and needs the shards and the merged index on disk
    together.
  - **Local index readers.** Opening an index no longer reads a header per
    document (0.4 s at 8.8M, was 7.3 s), but the first local index lookup does,
    once (~4 min at 300M, extrapolated): store the id map in the file?
  - [x] **Names and lengths.** They were read whole at open: at 138M, 6.6 GB read
    (names 4.96 GB, lengths 1.66 GB) and 7.5 GB of the search process's memory,
    against 1.3 GB for Lucene. Names are mapped now (`NindMappedFile`: page
    cache, shared, checked lookup by lookup, whole by `check()` in
    `verify_index`); lengths are read mapped, ids left out when consecutive;
    MaxScore searches no longer take an 8-byte-per-document scores array (1 GB
    per concurrent search at 138M). 138M: open 6.4 -> 1.0 s, the process's own
    memory 5.15 -> 0.52 GiB opened, 6.50 -> 0.85 GiB after queries; same
    search speed. The names check of a sharded build: 7.2 -> 2.6 GiB.
  - **Exhaustive searches' scratch.** Still 8 bytes per document per concurrent
    exhaustive search, kept in a pool of up to one per hardware thread: 1 GB at
    138M each, but only for a top k over documents / 4000 (34,500 at 138M) or
    several categories (Amose). Accumulate in the touched documents only (sorted
    postings: the same additions in the same order) when they are few?
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
    - [x] **Term-range passes.** `NindIndexBuilder::setPostingsBudget`
      (`IndexBuilder.postings_budget`): the postings of a range of terms at a
      time, the tokens scanned once per range; the same file. 16M direct:
      4.85 -> 3.19 GiB in 4 passes, +2 s a pass; from ~4 passes the tokenizing
      phase is the peak. `memory_gb` uses up to 4 (`MAX_BUILD_PASSES`), the
      fewest that fit, before more shards; the samples count the postings
      (each document's distinct terms). Model refit on shards built alone
      (7M, 14M: within 2%): 4.5 bytes per token, 5 per posting / passes, 35
      per document, 100 per term, 300 MB per process (refit below).
    - [x] **The plan's margin.** Built in a pool, a worker's later shards start
      from what its earlier ones left and equal shards peak together: 23 shards
      planned for 14.2 GiB peaked at 14.0 to 17.8 GiB from run to run (largest
      worker 2.55 GiB, model 1.7). Planned in 75% of the budget
      (`MEMORY_GB_MARGIN`); a direct build counts its tokenizing workers, which
      parse 64 MB pieces (1/32 of the file each was 5 GiB at 138M). 138M, 8
      workers: 16 GiB -> 29 segments, 11.5-12.2 GiB, 268 s; 8 GiB -> 83
      segments, 5.8-6.3 GiB; 44 GiB -> 2 segments, 31.5 GiB, 992 s (a direct
      build needs ~44: 31 GiB and its workers, in 75%).
    - [x] **Why later shards cost more**: glibc's mmap threshold, raised to the
      size of each mmapped block freed (up to 32 MiB), so that after a first
      build a worker's buffers came from its heap, fragmented. The same 4.77M
      passages again and again in one worker: 1.45 GiB, then ~2.0 each time;
      with the threshold fixed at its default (`mallopt` in the pool workers'
      initializer, `_fix_mmap_threshold`), 1.27-1.30 each, as fast. Model refit
      on shards of 1M to 9.5M built in a worker (0 to 6% over, 1M: 14%): 4.0
      bytes per token, 250 MiB per process; margin back to 90%. 138M, 8
      workers: 16 GiB -> 20 segments (29), 12.7-13.0 GiB, 273 s; 8 GiB -> 51
      (83), 6.4-6.5 GiB, 246-254 s; largest worker 1.8 GiB (2.55).
      A direct build fixes it in the caller's process too, for good
      (`NindIndexer.FIX_MMAP_THRESHOLD`, default True): 4.77M passages 2.13 ->
      1.51 GiB, as fast (model 1.79).
    Left:
    - **Compressed tokens.** Term ids in variable-length bytes: ~2 of the 4
      bytes per token (~9 GiB of a direct 138M build), now the largest part of
      a build in passes. But the ids are the lexicon's (by bytes, not by
      frequency): short codes for frequent words need a renumbering mapped back
      when writing, a decoding in every scan and documents by byte offsets.
      Segments already bound the memory: not worth it for now.
    - **Sorted runs on disk and a merge** (Lucene's way): memory bounded
      whatever the corpus, for I/O and a merge; segments already give it.
    - [x] **What Python held.** The vocabulary was ~1 KB per distinct term in a
      build: each chunk's word list, the union, `term_to_id`, and mostly the
      lexicon writer (`addWords`: every bucket and its retro words, ~380 bytes
      a word). Tokenizer batches are packed now, the builder numbers their words
      and writes the lexicon bucket by bucket (`addSimpleWords`, 4.6x faster);
      the same files. Shards of 1M to 14M passages: -8 to -13%; refit, the model
      is ~9 bytes per token, 110 per document, 100 per term, 300 MB per process
      (the old "per term" cost was mostly that fixed part): 138M in 16 GiB now
      35 segments (56) in 267 s (388), in 8 GiB 90 segments. A direct
      `index_jsonl` kept the record ids (77 bytes per document, 10.6 GiB at
      138M), as it returned them: now packed by the workers (`_PackedNames`,
      ~24 bytes), written first (`NindDocNames.write_packed`, streamed, 12
      bytes a document besides them; the string `write` no longer copies the
      names) and returned read from the file (`_RecordNames`, as sharded
      builds; numeric ids still a list). 16M direct: 6.15 -> 4.85 GiB, 100 ->
      77 s; 138M direct: 41.6 GiB, 775 s (57 GiB and 1,127 s at 4.1.0).
    - [x] **The names check after a sharded build** merged the shards' names to
      a file then removed (5 GB written, 20 bytes per document: 2.6 GiB at 138M,
      which no number of shards divided: `memory_gb` couldn't go below ~3.9 GiB).
      `NindDocNames::checkUnique`: each shard's `check()`, their name orders
      merged through a heap; 138M: 0.01 GiB, 13.9 s instead of 19-27, nothing
      written.
    - **The model is fitted on MS MARCO v2** (shards of 1M to 14M passages). Check it on another
      kind of corpus (long documents, code: `index_files(memory_gb=...)` now
      plans too, from runs of consecutive files scaled by their sizes);
      `merge=True`'s memory isn't in the budget.
    - **The benchmarks page** shows the builds and the search memory before
      these changes: rerun the build phases (direct and segments, 8.8M and
      138M) and stage 5 (memory budgets: nind's 7 GB of its own is 0.9 now).
  - [x] **Updates at 138M**: +1% took 60–84 s (a new segment) against 12 s for
    Lucene and 17 s for tantivy; 12.4 s now (16M: 7.8 -> 1.6 s, Lucene 3.5 s).
    Where it went:
    - the new segment's vocabulary in a tree of every token: words are ids of
      the update's own vocabulary now, hashed once as added (`NindSegmentedUpdate`);
      the merge policy reads segment counts from the side files' headers instead
      of opening the index;
    - per token in Python (the harness's JSON parsing and `add_tokens`, 20 s at
      138M; lists of strings to C++, 4 s): `IndexUpdate.add_jsonl` parses and
      tokenizes in worker processes and hands batches to C++
      (`SegmentedUpdate.add_documents`); the harness uses it (Lucene's update gets
      the raw JSON lines too); `add_tokens` skips encoding ASCII tokens;
    - writing the new segment's term index (12 of the last 17 s): its document
      lengths, for the blocks, were a table by id only for ids starting near 0;
      an update's (and a shard's) start far from it, so a binary search per
      posting, twice. A table from the first id: also 8M in 8 shards 25 -> 18 s.
    The new segment is still written by one thread.
  - **Amose.** Adapt it to `NindSegmentedIndex`/`NindSegmentedUpdate` (compound
    words on updates, term specifics: see the proposal); `NindAmoseMerge` needs
    its CLI (below). This doesn't constrain nind's design any more.
- [x] **Concurrency benchmark** on format v2, up to 48 threads and 138M
  (section 7): 20.6× on 24 cores.
- [x] **Several callers, all systems** (`docs/benchmarks.md`, Threads; stage 6 in
  `bench/retrieval/results/2026-11-11-server`). `concurrency.py` drives any system:
  threads sharing one searcher (pyjnius and tantivy-py release the GIL too) or
  processes each opening the index. 8.8M: Lucene up to 19% more with few
  callers, a tie with 12, nind ahead past them (4,303 queries/s with 48, Lucene
  2,574). 138M: **Lucene serves more at every load**, 1.6× with 48 callers.
  - Optional: queries arriving at a fixed rate (50% and 80% of each system's
    maximum) for tail latency, closer to a service.
- [x] **Throughput at 138M.** The medians tied with Lucene's, but nind's slow
  queries were slower (99th percentile 353 ms against 247 with one caller):
  queries of many terms (8+: 231 ms against 136), most of their time in the
  window scan, a million windows a query, each sorting its terms from scratch
  (a quarter of the time), each scoring a few documents. From 5 terms, MaxScore
  now scans windows of at least 4096 documents, a term's bound there the largest
  of its blocks', the essential terms' postings added term at a time into a dense
  array, the terms' order kept from one window to the next (`scanRangeByWindow`;
  fewer terms stay document at a time: 2 terms 3.5 -> 8.3 ms the other way). 138M
  one thread: mean 37.7 -> 29.8 ms (Lucene 35.9), median 17.0 -> 16.6 (18.6), p99
  271 -> 192 (188); 8.8M mean -17%. Identical results. Under load (stage 6
  again for nind, `stage6b.sh`): 8.8M, nind first at every load (48 threads
  4,303 -> 4,847 queries/s, Lucene 2,574); 138M, nind first up to 4 threads
  (24 processes), Lucene from 8 (48 threads: nind 477 -> 519, 8 segments 673,
  Lucene 786).
- **Many threads on a large index.** At 138M nind's threads scale less than
  Lucene's past 8: with 48, repeated runs give nind 634–642 queries/s, Lucene
  753–770 (`scale-repeat.txt`; stage 6b's 519 was a low run). perf at 12 and 48
  threads: the same profile, no lock; per-core efficiency with hardware threads,
  memory traffic (each search decodes its blocks into its own buffers)?
- **Term directories at 138M.** The scorer's directory cache (256 MB, emptied
  when full) holds a fifth of what 2,000 queries need: their 3,737 terms have
  15M blocks of 88 bytes (1.3 GB; "you": 286k blocks, 25 MB). With 2 GB, the
  138M mean is 6% lower (34.8 -> 32.8 ms), 8.8M unchanged; and each query also
  rebuilds its terms' block bounds from their directories (4–5%). Tried: the
  least recently used evicted instead of everything, at 256 MB: no gain at 138M,
  -2–4% at 8.8M (the working set doesn't fit, whatever goes). Done: the blocks'
  bounds without the idf cached with their directory for the parameters last
  asked (a bound: multiplicity * (idf * tf), the same value), the blocks read in
  the directory instead of copied each query, parsing reserving its blocks:
  138M mean -3.5%, 8.8M -2–4%, medians -2–4%, identical results. Left: a
  smaller cached block (its impacts dropped once its bounds are known: exact
  only while the parameters stay), or a larger cache for large indexes.
- [x] **A search process's own memory**, 138M: nind 1.06 GB, Lucene 0.83 (its JVM),
  tantivy 0.04 (both map their index). `NindEngine(map_lengths=True)` reads the
  documents' lengths in their mapped files (none of the process's memory: 0.33
  GiB after queries instead of 0.85, opened in 0.6 s instead of 1.0) for 3-6%
  slower searches (4 KB pages, not a copy's huge pages: lengths on fewer bytes
  wouldn't help, see "Search, what's left"); copied by default. Update lookups
  map them. Left: the 256 MB directory cache (above), the term index's and
  lexicon's indirection maps.
- [x] **Top 100** in the comparison benchmark.
- **Stopwords kept at scale.** MS MARCO v2 138M, top 10 on one thread: 138 ms vs
  36 ms for Lucene (16M: 15.5 vs 6.1 ms) with 3.2.0. With the windowed scan
  (5 terms or more) nind is level with Lucene from 5 terms; short queries were
  far behind (4 terms 58.6 ms against 16.4): a stopword's hundreds of thousands
  of blocks each ended a window of the document-at-a-time scan. Windows now also
  when the terms average 8 blocks or more per 4096 documents (102 of 153 short
  queries with stopwords, 5 of 637 without): 3 terms 19.9 -> 9.8 ms (Lucene 9.4),
  4 terms 58.6 -> 23.7 (16.4); median 42.4 -> 32.2 ms (28.8), mean 66.6 -> 59.8
  (46.7, 500 queries). Left: queries of 10 terms or more (282 ms against 169).
- **Latency varies between processes.** The same search, one process per run:
  2.12 or 2.67 ms (8.8M, one thread), seen again while measuring (runs of a
  setting 10-20% apart on every kind of query, the code path unchanged). Pinned
  to one CPU, ten processes agree within 2% (node 1's memory or node 0's), with
  the same huge pages (14-16 MB): not THP. The benchmarks pin to 12 CPUs, a
  process free to move among them: try one CPU against twelve when the machine
  is idle; meanwhile compare settings in alternated runs, several each.
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
  document goes to instructions spread over windows and probes. Done since:
  windows of documents scored term at a time into an array and a bitset, as
  Lucene's MaxScoreBulkScorer, for queries of 5 terms or more (see "Throughput
  at 138M"; the re-sorting above pays there: a million windows a query). Left:
  the same for fewer terms without the cost it has there (2 terms: 2.4× slower
  at 138M; fixed costs per window), compare documents scored per query with
  Lucene's, SIMD decoding. Several categories (Amose) aren't pruned yet.
- **Large k on small corpora.** The v2 exhaustive path is ~25% slower than v1
  at top 1000 on 1–2M documents (block decoding); not a target size.
- **Original author's to-do.** `NindLexiconIndex::addWord` doesn't check that
  words are UTF-8.
- **`NindLexiconAmose::getWord`.** It reads past the end of the component list
  for the lexicon word `§` alone (undefined behaviour).
- **CLI for Amose/LIMA multi-process indexing.** A small C++ program running
  `NindAmoseMerge` over shard prefixes.
