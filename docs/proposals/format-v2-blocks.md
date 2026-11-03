---
orphan: true
---

# Proposal: nind format v2, block-structured postings

Status: **proposal, not implemented** (2026-11-04). Written after the
comparison benchmark (`bench/retrieval/`, `bench/README.md` section 4), whose
MS MARCO runs reached the limits of the current format.

## Why

### Hard limits of the current format

Measured or read off the grammar (`src/cpp/NindIndex/*.cpp` headers):

| limit | field | what reaches it |
|---|---|---|
| **16 MiB per definition** | indirection `<longueurDejfinition>` and term `<longueurDonnejes>`: `Entier3` | a term in more than ~8M documents (dense postings take ~2 bytes). MS MARCO passage, 8.8M documents, stopwords kept: `the` is at 15.4 MB (7.7M postings); the limit falls at **~10M documents**. Without stopwords, ordinary content words reach it somewhere around 100M documents. |
| **16.7M documents** with a local index | local index `<identifiantDoc>` (internal number): `Entier3` | any corpus over 16.7M documents with `local_index=True` (what Amose reads) |
| **16.7M lexicon hash buckets** | lexicon `<identifiantHash>`: `Entier3` | ~8.4M distinct words when the lexicon is rebuilt at 2 buckets per word (`IndexUpdate`'s `LEXICON_MAX_LOAD` path) |
| 1 TiB per file | `<offsetDejfinition>`: `Entier5` | a local index of ~10G documents with positions |
| 4.29G documents | document ids: 32 bits (`Entier4`, `unsigned int`) | 10G documents in one index |

Until commit `2c0bc0a`, overflowing a 3-byte field wrote the value truncated: a
corrupt index, detected only when read. `NindFile::putInt3`/`putInt5` now throw
`OutOfBoundException` instead, so these limits fail loudly. They are still
limits.

### Costs that don't depend on a limit

- **An update rewrites every touched term definition whole.** Adding 88k
  documents (1%) to the MS MARCO index took 438 s and 16.8 GB, 5x a full
  rebuild (84 s). New documents have the highest ids, so they only ever extend
  the end of each postings list, yet the whole list is rewritten. Lucene does
  the same update in 3.5 s (a new segment).
- **A read loads a whole definition.** `getDefinition` reads the full
  definition into one buffer (15 MB for `the`, and it grows with the corpus),
  even when a top-k query could skip most of it.
- **No skipping.** With flat-array scoring (commit `453caeb`), nind costs
  ~9–12 ns per posting and reads every posting of every query term. Lucene and
  tantivy skip blocks that can't reach the top k (block-max WAND / MaxScore):
  on FiQA, nind takes 1.3 ms per query vs. 0.9 ms (Lucene) and 0.7 ms
  (tantivy), and the gap widens with the corpus, because frequent terms'
  postings grow linearly while the top k doesn't.
- **A merge re-encodes every posting.** `NindIndexMerge` decodes and re-encodes
  every posting of every shard, so a merge costs about as much as a build.
  That's the main obstacle to building on hundreds of cores (N shards, then a
  merge).

## Proposal

Keep the pad-file envelope (header, chained indirection blocks, bulk data,
specifics, identification), the single writer per file, the generations, and
the term → categories → documents semantics Amose relies on. Change three
things.

### 1. Widen the fixed fields (file-level, all files)

| field | v1 | v2 |
|---|---|---|
| indirection `<offsetDejfinition>` | `Entier5` (1 TiB) | `Entier6` (256 TiB) |
| indirection `<longueurDejfinition>` | `Entier3` (16 MiB) | `Entier4` (4 GiB) |
| local index `<identifiantDoc>` | `Entier3` | `Entier4` |
| lexicon `<identifiantHash>` | `Entier3` | `Entier4` |
| term `<identifiantTerme>`, `<longueurDonnejes>` | `Entier4`, `Entier3` | `Entier4`, `Entier4` |

An indirection entry grows from 8 to 10 bytes. The pad-file header already
stores the entry size (`<tailleEntreje>`), and today's readers require it to
match (`NindPadFile.cpp:100`). So **a v1 reader, Amose's included, refuses a
v2 file with an exception instead of misreading it**: the version gate exists
already. v2 readers accept both sizes, which keeps v1 files readable.

Document ids: see open question 1.

### 2. Term definitions: a directory of blocks

Per term and per grammatical category, the postings are cut into **blocks of
B documents** (B = 128, as in Lucene; tunable at build time and stored in the
file's specifics). A term's definition becomes a small **directory**; the
blocks live in the bulk data, addressed by the directory and not by the
indirection:

```
<dejfinition>      ::= <flagDejfinition=23> <identifiantTerme> <longueurDonnejes> { <catejgorieV2> }
<catejgorieV2>     ::= <flagCg=67> <catejgorie> <frejquenceTerme> <nbreDocs>
                       ( <postingsEnLigne> | <nbreBlocs> { <descripteurBloc> } )
<postingsEnLigne>  ::= { <identDocRelatif> <frejquenceDoc> }      // nbreDocs <= B: as in v1
<descripteurBloc>  ::= <premierDoc> <dernierDocRelatif> <nbreDocsBloc>
                       <frejquenceMax> <longueurMin> <offsetBloc> <longueurBloc>
<bloc>             ::= <flagBloc=71> { <identDocRelatif> <frejquenceDoc> }   // deltas from premierDoc
```

- **Short lists stay inline.** By Zipf's law, almost every term has fewer than
  B documents. Those are written exactly as in v1 (one read, no overhead).
  Only long lists get blocks.
- **`<frejquenceMax>` and `<longueurMin>`** (the largest term frequency and
  the shortest document length in the block) are generic statistics, not
  scores. They give an upper bound of any scoring function that increases
  with tf and decreases with document length (BM25 for any k1 and b, and
  likely Amose's own), so a top-k search can skip a block it can't win with.
  The index stays scoring-agnostic, unlike bm25s's precomputed scores.
- **Very long lists**: past a threshold of blocks (e.g. 4,096 descriptors,
  ~100 KB), the directory gets one more level (descriptors of directory
  pages). One top page of 4,096 entries then covers 4,096 × 4,096 × 128 ≈ 2G
  postings per category.
- **Blocks are written once and rewritten only when touched.** An update that
  adds documents (highest ids) rewrites the **last block** of each term it
  touches, appending new blocks when that one is full, plus the term's
  directory. A removal or replacement rewrites only the blocks holding the
  documents. A freed block becomes a hole (`<vide>`), as definitions do today.
- **Block encoding**: ULat deltas, as in v1, to keep the change small. A later,
  independent step can switch the block body to bit-packing / PFor (SIMD
  decoding, as in Lucene and tantivy): the descriptor already gives the
  block's document count and length.

### 3. Readers read what they need

`NindTermIndex` gets, next to `getTermDef` (unchanged API, which assembles the
blocks), a block-level reader: read the directory, then `pread` the blocks
asked for. No whole-definition buffer, no stdio buffer discard, one syscall
per block (see the I/O follow-up in CLAUDE.md). `Bm25Scorer` then does
MaxScore / block-max WAND over the descriptors. With the same operations on
the documents it does score, the top k is unchanged: nind and bm25s keep
identical rankings, now with skipping.

## Only for corpora known to be too large, or for everything?

**For everything: one format, adaptive by construction.** Each gain below
applies well before the limits:

| gain | from what size |
|---|---|
| skipping blocks in top-k search | as soon as queries contain frequent terms: FiQA (57k documents) already scores ~146k postings per query, most of which block-max skipping would avoid |
| updates that rewrite the last block, not the whole list | any index that's updated: the 438 s / 16.8 GB on MS MARCO becomes roughly (terms touched) × (one block), orders of magnitude less I/O |
| bounded read buffers, one `pread` per block | any index with long lists (memory no longer grows with the corpus per read) |
| a merge that copies blocks instead of re-encoding them (shards cover disjoint, ordered document ranges: only a block's first id changes) | any sharded build, i.e. the way to use hundreds of cores |
| parallel decoding/scoring of one query's blocks across cores | large corpora on many-core machines |
| no 16 MiB / 16.7M-document / 16.7M-bucket ceilings | ~10M documents (stopwords kept) to ~100M (without) |

And the cost on small corpora is close to nothing:

- **Size**: short lists are inline as today. A long list pays one descriptor
  (~16–20 bytes) per 128 postings (~256 bytes), about 6–8% on long lists and
  ~3–5% overall. Blocks of 256 halve that, at a coarser skip granularity.
- **Speed**: one more indirection (directory, then block) for long lists only.
  It's offset by reading less.

A separate "large corpus" format would mean two writers, two readers and two
test matrices in C++, Python and Amose, plus a choice users must make before
knowing their corpus will grow. And Amose would have to read both anyway. With
inline short lists, small corpora pay nothing, so there's no reason to keep
two formats.

## Migration

1. **Readers first.** v2 readers accept v1 files (entry size 8, flag 17) and v2
   files (entry size 10, flag 23), in C++ and in `nind._native`. Amose gets a
   reader that opens both before any v2 file exists.
2. **Then writers.** `NindIndexer`, `IndexUpdate`, `IndexBuilder` and
   `NindIndexMerge` write v2 only. `NindIndexMerge` reading v1 and writing v2
   is the conversion tool (`nind._native.merge_indexes([old], new)`).
3. **Python diagnostics** (`dumpeFichier`, `afficheTerme`, ...) follow the
   grammar or move to `nind._native` (see "Planned follow-up work" in
   CLAUDE.md).

## Testing and measuring

- Unit tests per field width and per path: inline vs. blocked categories, a
  two-level directory, appending to the last block, removing from a middle
  block, holes reused.
- Robustness: extend the `*RobustnessTest.cpp` corruption sweeps to v2 files.
  Descriptor offsets, lengths and counts are read from the file, so each must
  be bounded against the file size and the buffer, as v1 fields are.
- A term over 16 MiB (`NindTermIndexTest.DefinitionOver16MiBIsRefused` today)
  must round-trip in v2.
- `bench/retrieval/`, before and after: parity (rankings identical to bm25s),
  index size, build time, query latency at k = 10 and 1000, and update cost on
  MS MARCO and its prefixes; then a corpus over 16.7M documents with a local
  index.

## Open questions

1. **Document ids: 32 or 64 bits?** 32 bits caps one index at 4.29G documents.
   In postings, ids are delta-encoded varints, so 64-bit ids cost nothing
   there. The fixed-width fields that hold them (local index external id,
   `.ninddocnames`, `.ninddoclengths`, `Bm25Scorer`) grow by 4 bytes per
   document. Either 64 bits now, or 32 bits plus search across indexes at
   Amose's level (which the sharding design rejected).
2. **Block size**: 128 (finer skipping) or 256 (less overhead)? To measure on
   MS MARCO.
3. **Block statistics**: are `frejquenceMax` and `longueurMin` enough for
   Amose's scoring, or does it need others (e.g. per-category ones)?
4. **The local index**: v2 only widens its fields. Its per-document
   definitions are small, so no blocks are needed there.
5. **Build memory** (not part of the format): `NindIndexer` keeps ~17 bytes
   per token in memory (8.8 GB for MS MARCO). Sharded builds plus a
   block-copying merge would bound it per shard.
