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
  rebuild (84 s). Each new document touches a few hundred bytes of each
  postings list it enters (at the end with `NindIndexer`'s increasing ids,
  anywhere with an application's own numbering), yet the whole list is
  rewritten. Lucene does
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

## Principles from the original design

The original author's documentation (Jean-Yves Sage, L'Atejcon:
*nind, structure des fichiers*, LAT2017.JYS.470; *diagnostic d'un système de
fichiers déconnant*, LAT2017.JYS.475; *indexation du corpus sample du LVIC*,
LAT2016.JYS.463) states principles this proposal keeps:

1. **"Function first, then architecture, then algorithms, and the byte
   last."** Each integer field is sized for its function, to keep files small
   *without compression* (`Entier1`..`Entier5`, varints `EntierULat`/`SLat`).
   So v2 widens a field only where a function needs it, field by field (see
   1 below), not wholesale.
2. **The EBNF grammar is the specification**, so that programs independent
   of the C++ can check the files it writes ("the tests are simpler and above
   all more complete"). That grammar has drifted: the code writes 4-byte word
   ids in the lexicon and retrolexicon (`putInt4`) where the grammar still
   says `Entier3`. v2 starts by reconciling grammar and code, then writes the
   v2 grammar **before** the code.
3. **The lexicon and the local index are the source of truth; the term index
   and the retrolexicon can be rebuilt from them** (LAT2017.JYS.475, §11). So
   migrating to v2, or recovering a damaged term index, can be a rebuild of
   the term index from the local index. An index built with
   `local_index=False` gives that up: its term index becomes the only copy.
4. **Incremental, in-place, idempotent indexing.** The LVIC sample corpus
   indexed as three *overlapping* parts gives the same files as indexed in
   one go (484,945 terms, 2,548,693 occurrences; LAT2016.JYS.463, §4.7): a
   re-indexed document replaces itself. Definitions are written with slack
   ("extensions", 2–3% of the data) so they can grow in place, and freed space
   ("vides") is reused. Blocks have to keep all three properties.
5. **Documents keep the application's numbering.** The term index stores
   *external* document ids, which can be sparse (CLEF: ids up to ~1.4×10⁸ for
   ~300k documents); only the local index maps them to dense internal
   numbers, "so that nind accepts any numbering of the documents". Postings
   deltas, block descriptors and `Bm25Scorer` (dense table or binary search)
   have to handle sparse ids as well as dense ones.
6. **The real vocabulary is compound.** In Amose's indexes, compound terms
   and named entities dominate: 97% of the CLEF French lexicon (6.86M of
   7.1M words), 80% of the LVIC sample's terms. The lexicon limits matter far
   more for Amose than for single-word corpora like MS MARCO.
7. **"A robust system isn't one without errors, but one that accommodates
   them":** detect and trace first, then recover, then fix. That means
   refusing bad input at write time (still open from LAT2017.JYS.475: make
   `NindLexiconIndex::addWord` refuse non-UTF-8 words, or have every tool stop
   assuming UTF-8), never writing a corrupt file (commit `2c0bc0a`), and tools
   to verify and rebuild (3).

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

Each line answers a function (principle 1): a file past 1 TiB (a local index
of ~10G documents), a definition past 16 MiB (a directory, see 2), a local
index past 16.7M documents, a lexicon past 16.7M buckets (Amose's compound
vocabulary, principle 6). Nothing else changes width. Word ids are already
4 bytes in the code; only the grammar says 3 (principle 2).

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
- **Blocks are written once and rewritten only when touched, in place when
  they can be** (principle 4). The last block of a list is written with slack
  ("extensions"), so adding documents with higher ids fills that slack
  without moving anything: the directory entry is updated in place, and a
  new block is appended once the last one is full. A removal or a
  replacement rewrites only the blocks holding the documents. A freed block
  becomes a hole (`<vide>`) and is reused, as definitions are today.
  Re-indexing a document still replaces it, so indexing overlapping parts
  stays idempotent. With *external* ids (principle 5), a new document can
  fall anywhere in the list, not only at the end: it then goes into the block
  its id belongs to, which is split when full, so updates stay local.
- **Block encoding**: ULat deltas, as in v1, to keep the change small. A later,
  independent step can switch the block body to bit-packing / PFor (SIMD
  decoding, as in Lucene and tantivy): the descriptor already gives the
  block's document count and length.

### 3. Readers read what they need

`NindTermIndex` gets, next to `getTermDef` (unchanged API, which assembles the
blocks), a block-level reader: read the directory, then `pread` the blocks
asked for. No whole-definition buffer, no stdio buffer discard, one syscall
per block (see the I/O follow-up in CLAUDE.md, and {doc}`thread-safe-readers`, whose positioned reads this builds on). `Bm25Scorer` then does
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

1. **Readers next.** v2 readers accept v1 files (entry size 8, flag 17) and v2
   files (entry size 10, flag 23), in C++ and in `nind._native`. Amose gets a
   reader that opens both before any v2 file exists.
0. **Reconcile the v1 grammar with the code** (principle 2), then write the
   v2 grammar, before any code.
2. **Then writers.** `NindIndexer`, `IndexUpdate`, `IndexBuilder` and
   `NindIndexMerge` write v2 only. Conversion: rebuild the v2 term index from
   the v1 lexicon and local index, which are the source of truth
   (principle 3); for indexes without a local index, `NindIndexMerge`
   reading v1 and writing v2 (`nind._native.merge_indexes([old], new)`).
   The same rebuild is the recovery tool for a damaged term index.
3. **Python diagnostics** (`dumpeFichier`, `afficheTerme`, ...): the low-level
   API stays a thin wrapper over `nind._native`, but **one verifier stays
   independent of the C++**, following the v2 grammar. It's the offline tool
   the original design relied on to tell, without any C++, whether a fault
   comes from indexing or from search (LAT2016.JYS.463, §8.3).

## Testing and measuring

- Unit tests per field width and per path: inline vs. blocked categories, a
  two-level directory, appending to the last block, removing from a middle
  block, holes reused.
- Robustness: extend the `*RobustnessTest.cpp` corruption sweeps to v2 files.
  Descriptor offsets, lengths and counts are read from the file, so each must
  be bounded against the file size and the buffer, as v1 fields are.
- A term over 16 MiB (`NindTermIndexTest.DefinitionOver16MiBIsRefused` today)
  must round-trip in v2.
- The independent grammar-driven verifier accepts every file the C++ writes
  and rejects every corrupted one from the sweeps.
- **Idempotent incremental indexing** (LAT2016.JYS.463, §4.7, as a test):
  indexing a corpus as overlapping parts, in any order, gives the same index
  as indexing it at once. Sparse external ids too (CLEF-like numbering).
- **Rebuild**: a term index rebuilt from the lexicon and local index equals
  the one written incrementally.
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
5. **The author's other documents**: LAT2014.JYS.440 (*nind, indexation
   post-S2*, the main design), LAT2015.JYS.448 (the Amose adaptation),
   LAT2017.JYS.468 (*synchronisation des lecteurs par les fichiers*),
   LAT2016.JYS.465 (measurements) and LAT2016.JYS.466 (hashing of the
   sample's words) are referenced but not available here. 448 and 466
   especially could change the block statistics and the lexicon sizing.
6. **Build memory** (not part of the format): `NindIndexer` keeps ~17 bytes
   per token in memory (8.8 GB for MS MARCO). Sharded builds plus a
   block-copying merge would bound it per shard.
