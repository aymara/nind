---
orphan: true
---

# Segments: an index of immutable parts, searched as one

Status: **implemented** (2026-11-05), branch `sharding-scale`. Reverses the earlier decision to
merge shards into one index and never search across them: performance and hundreds of millions of
documents come first, and Amose adapts (see the end).

## Why

One index has three costs that grow with the corpus and can't be engineered away:

- **An update copies the generation** (Python's immutable generations): 237 s to add 1% at 8.8M
  documents, hundreds of GB per update at 300M.
- **A merge takes as long as a build's writes**, single-threaded, after the shards were built in
  parallel.
- **A search is one sequence of postings**, however many cores there are, unless cut by document ranges.

## Principles kept from the original design

- **A published file never changes; damage is repaired by a rebuild, never by patching files under
  live readers.** A segment is that, taken further: its deletions go next to it.
- **Function first, the byte last.** A segment is a standard nind index, in the existing format:
  every tool that reads an index reads a segment, and no format changes for it.
- **Readers synchronize through the files.** Publishing stays one atomic pointer (`CURRENT`).

## Design

**Layout.** A generation holds either the index files (one index, as before) or a manifest,
`<prefix>.nindsegments` (`nindsegments 1`, then one directory per line), with one `seg-<n>-<hex>/`
per segment. A new generation **hard-links** the segments of the previous one: each generation stays
complete by itself, deleting one leaves the others' files (the inode lives on), and
`_index_store`'s publishing and collection don't change. Names are given once, so a name identifies
the files: `NindSegmentedIndex(base, previous)` shares the opened segments of an earlier generation.

**Search (`NindSegmentedIndex`).** One `NindBm25Scorer` per segment, immutable. What depends on the
rest of the index is given per search (`SearchContext`): N, the average length, each term's df, the
bitset of deleted documents, and theta, shared by the segments, raised by each. Segments are taken
largest first by `threads` threads; with more threads than segments, the rest go within them.

**The invariant: the results of one index built from the live documents**, to rounding: N, the
average length and every df are the live documents', ties go by increasing document id, and a
deleted document never reaches theta (checked before the candidates). `NindSegmentedIndexTest` and
`NindSegmentedUpdateTest` hold every search to it, for every k, thread count, and with or without
pruning.

**Deletions (`.ninddeletes`).** Document ids, and per term how many of its postings they hold: df is
exact without reading a list. The counts come from the local index, at the document's expected
internal id (`getLocalDefAt`, no id map loaded), or without one from every term's directory and only
the blocks that may hold the documents. A term of a segment may thus have deleted postings; block
bounds stay upper bounds, and MaxScore only gets less sharp until a merge.

**Updates (`NindSegmentedUpdate`).** New and replaced documents go to a new segment, the replaced
and removed ones are masked. A new id costs no lookup at all (only the last id of each lengths file
is read). Python passes tokens and names, and holds no whole-corpus structure.

**Merges.** Log-structured, at the end of a commit: segments in tiers of live documents (factor
10); a tier of 10 is merged, or those of its smallest that fit under `maxMergedDocs` (5M); a
segment of at least half of that is *full* and stays as it is; a segment over half deleted is
rewritten alone. However many updates, that leaves at most about 2 N / maxMergedDocs full segments
(120 at 300M) and fewer than 10 per tier below. `NindIndexMerge` and `NindDocNames::merge` drop
deleted documents and accept interleaved ids (a replaced document keeps its id in a newer
segment). `force_merge` makes one segment of everything. Without the limit, a first update merged
all the shards of a build (63 s at 8.8M, hours at 300M); without "full", full segments piled up in
a tier that could never merge, so that steady updates made the count grow without bound.

A build's shards are segments like any others: those under 2.5M documents may be merged by a
later update. A large build (64 shards of 4.7M at 300M) is full, so left alone.

**Builds.** `shards=N` keeps its shards as segments (`merge=True` for one index). Names are checked
unique across shards, by merging them and dropping the result.

## Measures

MS MARCO 8.84M passages, stopwords removed, no local index, 6980 queries, top 10; every layout
gave the single index's documents in the same order.

| segments | build | p50, 1 thread | p50, 4 | p50, 8 | p99, 8 |
|---|---|---|---|---|---|
| 1 | 45.1 s | 1.57 ms | 0.81 ms | 0.77 ms | 6.99 ms |
| 8 | 26.2 s | 1.82 ms | 0.72 ms | 0.74 ms | 6.88 ms |
| 32 | 24.3 s | 2.65 ms | 1.03 ms | 0.90 ms | 6.90 ms |

Updates of the index built in 8 shards (base + 1%; after each update of 88k documents, top 10
identical to a direct build: 2000/2000):

| update | time | before |
|---|---|---|
| +88,418 documents | 2.57 s | 237 s (v1); 5.3–22.9 s per 20k at 4M |
| the same, built in 16 shards | 17.55 s: one merge of 9 of them (550k each), once | |
| +1 | 0.27 s | |
| +1000 | 0.24 s | |
| -1, -100 (no local index) | 0.80 s, 0.81 s | |
| reader switching to it | under 10 ms | |
| `force_merge` | 24.3 s (35.0 s from 16 shards) | |

## Limits, and what's left

- Above 8.8M documents, nothing has run on a real corpus yet.
- A single thread pays for segments: choose `MERGE_FACTOR`, or `force_merge`, for it.
- Names are read whole when a segment opens (~30 bytes per document); `mmap` would help. The
  uniqueness check of a sharded build needs them all in memory at once.
- A commit's merges are synchronous, single-threaded.
- An update needing a lookup (a removal, a replacement, a name) opens the whole index in the writer:
  every segment's lengths and names, ~40 bytes per document (~12 GB at 300M). Only new documents
  without names avoid it. Without a local index, a removal also reads the directories of the touched
  segments' terms: 0.8 s at 8.8M, growing with them.
- The scorer's directory cache is per segment (256 MB at most each): several GB at most with many
  segments.
- `NindEngine.lexicon`, `term_index`, `local_index`, `get_term_id` raise `ValueError` on several
  segments or deletions: they assume one index (`force_merge` gives them back).

## Amose

Its C++ readers and in-place writers (`NindIndexUpdate`, `NindAmoseMerge`) keep working on one
index. To get these gains it would use `NindSegmentedIndex` and `NindSegmentedUpdate`, after they
learn compound words on updates and term specifics (its 8 counters would be summed at open; unique
terms aren't additive, so they'd come from a merge). Its deletion and `tf` issues in `docs/TODO.md`
would go with it.
