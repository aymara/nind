# nind to-do list

Kept up to date as items are done; the decisions behind them are in
`docs/proposals/` and `bench/README.md`. Started 2026-11-04, after the release
of 3.0.0 (format v2).

## In progress (branch `v2-perf`)

- [ ] Writers fill each block's `<longueurMin>` (the shortest document length
  in the block: tighter MaxScore bounds); MaxScore's threshold
  (`PRUNING_DOCS_PER_RESULT`) measured again at the target sizes (≥ 4M
  documents; 1–2M isn't a target).
- [ ] Cheap updates: an update rewrites only what changed (the touched terms'
  last blocks and directories), not every list a new document enters (332 s
  for 1% of MS MARCO, vs 2 s for Lucene).
- [ ] Bit-packed blocks (Lucene-like): faster decoding than varints.
- [ ] Everything in the C++ API (Amose and other clients use it directly):
  move `Bm25Scorer` (block-max MaxScore) and `IndexBuilder` out of
  `src/cpp/NindPy/bindings.cpp` into the `NindIndex` library, and
  `IndexUpdate`'s change logic (touched terms, merge of postings, migration,
  lexicon rebuild) out of `nind_engine.py`; Python keeps the generations and
  publishing.

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
- **Build memory.** ~19 bytes per token (7.8 GB for MS MARCO 8.8M); sharded
  builds would bound it per shard.
- **Concurrency benchmark.** Rerun `bench/retrieval/concurrency.py` on format
  v2.
- **Top 100.** Measure top 100 in the comparison benchmark (only top 10 and
  top 1000 are timed).
- **Search, beyond bit-packing.** Block-max WAND; parallel scoring of one
  query's blocks across cores; several categories (Amose) aren't pruned yet.
- **Large k on small corpora.** The v2 exhaustive path is ~25% slower than v1
  at top 1000 on 1–2M documents (block decoding); not a target size.
- **Original author's to-do.** `NindLexiconIndex::addWord` doesn't check that
  words are UTF-8.
- **`NindLexiconAmose::getWord`.** It reads past the end of the component list
  for the lexicon word `§` alone (undefined behaviour).
- **CLI for Amose/LIMA multi-process indexing.** A small C++ program running
  `NindAmoseMerge` over shard prefixes.
