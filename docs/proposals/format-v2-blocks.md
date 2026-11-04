---
orphan: true
---

# Proposal: nind format v2, block-structured postings

Status: **accepted, being implemented** (2026-11-04; decisions below). Written after the
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
2. **The EBNF grammar is the specification.** The original design also had
   Python programs check the files from the grammar alone, independently of
   the C++. That isn't kept: Amose reads the files through the same C++
   libraries, so no second implementation of the format exists to conform
   to, and a parallel decoder would have to follow every format change. The
   grammar is enforced instead by golden files (byte-exact fixtures) and a
   native `verify` tool (see Testing). The grammar has drifted: the code
   writes 4-byte word ids in the lexicon and retrolexicon (`putInt4`) where
   the grammar still says `Entier3`. v2 starts by reconciling grammar and
   code, then writes the v2 grammar **before** the code.
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
   to verify (a native `verify`, see Testing) and rebuild (3).

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
| a merge that copies block bodies instead of re-encoding them (documents keep their ids, and a body is relative to its block's first document: only the small header, with the merged term id, is rewritten) | any sharded build, i.e. the way to use hundreds of cores |
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

Decided (2026-11-04): **writers write v2 by default**; readers read v1 and v2.
Amose follows as needed (or keeps an older nind for v1).

1. **Reconcile the v1 grammar with the code** (principle 2), then write the
   v2 grammar, before any code.
2. **Readers read both**: v1 files (entry size 8, flag 17) and v2 files
   (entry size 10, flag 23), in C++ and in `nind._native`.
3. **Writers write v2**: `NindIndexer`, `IndexUpdate`, `IndexBuilder`,
   `NindIndexMerge` and the C++ writers Amose uses. An existing v1 file
   reopened by a writer is refused (it can't grow v2 definitions): rebuild
   it. Conversion: rebuild the v2 term index from the v1 lexicon and local
   index, which are the source of truth (principle 3); for indexes without a
   local index, `NindIndexMerge` reading v1 and writing v2
   (`nind._native.merge_indexes([old], new)`). The same rebuild is the
   recovery tool for a damaged term index.
4. **Diagnostics** (`dumpeFichier`, `afficheTerme`, ...) move to
   `nind._native` like the rest of the low-level API (see "Planned follow-up
   work" in CLAUDE.md), with a native **`verify`**: it walks every structure
   of an index and checks its invariants (flags, bounds, counts matching
   their lists, increasing document ids in a list, holes accounted for,
   files paired by their identification). That's the offline tool the
   original design used to tell whether a fault comes from indexing or from
   search (LAT2016.JYS.463, §8.3), without a second decoder.
5. **Golden files**: the v1 set (`tst/cpp/unit/NindIndex/golden/v1`) stays,
   read-only (v2 readers must decode it); a v2 set is written and checked
   both ways (writers reproduce it, readers decode it).

## Decisions (2026-11-04)

1. **External document ids are 64 bits** (the application's numbering, in
   postings, the local index's `<identifiantExterne>`, `.ninddocnames`,
   `.ninddoclengths`). **The internal numbering stays 32 bits**: the local
   index's `<identifiantDoc>`, `NindIndex`'s identifiers (its indirection
   numbering), `Bm25Scorer`'s positions. So **one index holds at most
   4.29G documents**; beyond that, several indexes (see "Possible
   evolutions").
2. **Blocks of 128 postings** by default (a build-time parameter stored in
   the file's specifics; 256 to be measured).
3. **Block statistics: max term frequency and min document length.** More
   can be added later under a new flag.
4. **Writers write v2 by default** (see Migration).

## Possible evolutions

- **64-bit internal numbering**, for one index over 4.29G documents: the
  local index's `<identifiantDoc>` (`Entier8`), `NindIndex`'s identifiers
  (`getEntryPos`, the indirection numbering: 64-bit identifiers, and the
  indirection blocks' `<nombreIndex>` counts summing past 2^32), and
  `Bm25Scorer`'s positions (`uint64_t`, its per-document arrays then
  beyond 4.29G entries, so a bounded accumulator rather than N-sized arrays,
  see the inventory). The lexicon and term index keep 32-bit word
  identifiers. Not needed while one index stays under 4.29G documents.

## Implementation inventory

What the decisions reach beyond the term index's grammar:

- **64-bit document ids** go through public C++ structures that Amose
  compiles against: `NindTermIndex::Document::ident`, the local index's
  external id (`setLocalDef`/`getLocalDef`/`getDocIdents`), `NindIndexMerge`'s
  document lists, `NindAmose`'s `NindTermAmose`. One type for them
  (`NindDocId`, `uint64_t`), so the change is visible in one place.
- **Varints**: `EntierULat` decodes at most 32 bits. A 64-bit form,
  `<EntierULat64>`, encodes values below 2^32 exactly as `EntierULat` does,
  and larger ones as the byte `0xF8` followed by 8 bytes, big-endian: a
  first byte of the form `11111xxx` is rejected by today's decoder, so it is
  free. Postings deltas and block descriptors use it.
- **Auxiliary files**: `.ninddoclengths` (`NINDDL01`: 32-bit ids and lengths)
  and `.ninddocnames` (`NindDocNames`: `uint32_t` ids) need 64-bit versions
  (`NINDDL02`, a new `NindDocNames` version), their readers accepting both.
- **Python**: `array('I')` for document ids in `nind_engine` (`IndexBuilder`
  chunks, document lengths, names), `Bm25Scorer`'s `UIntView` of document
  ids. `array('Q')` for ids.
- **Python parses v1 bytes at open time**: `NindPadFile.py` (the header and
  the map of indirection blocks), `NindLexiconindex.__init__`
  (`donneMaxIdentifiant`), `NindLocalindex.__init__` (its own external ->
  internal id map, from every document's header: O(N) Python reads at each
  open). With 10-byte entries `NindEngine` would fail at open, so moving what
  is parsed at open into `nind._native` comes first. **The diagnostic
  methods** (`dumpeFichier`, `afficheTerme`, ...) then refuse v2 files with a
  clear error until they move to `nind._native` too (the planned follow-up
  in CLAUDE.md); the native `verify` of the Testing section covers v2.
- **`Bm25Scorer`'s memory**: one N-sized scratch per concurrent search
  (~12 bytes per document: ~1.4 GB for 16 threads on MS MARCO, terabytes at
  10G documents). Block skipping reduces the work, not the array; at very
  large N, scoring block by block into a bounded accumulator (document-at-a-
  time over blocks) replaces it.

## The v2 grammar

As precise as the v1 grammar in the source comments, written before the code
(principle 2). Only what changes from v1 is shown; the retrolexicon and the
identification block (`<maxIdentifiant>` and `<dateHeure>`, both `Entier4`)
are unchanged.

```
// integers
<EntierULat64>          ::= <EntierULat>                        // values < 2^32: as in v1
                          | <flagULat64=0xF8> <Entier8>          // values >= 2^32
<Entier6>               ::= <Octet>{6}                           // big-endian, as Entier5
<Entier8>               ::= <Octet>{8}                           // big-endian

// NindPadFile / NindIndex envelope (lexicon, term and local indexes)
<fichier>               ::= <tailleEntreje=10> <tailleSpejcifiques> { <blocIndexej> <blocEnVrac> }
                            <blocSpejcifique> <blocIdentification>
<blocIndexej>           ::= <flagIndexej=47> <addrBlocSuivant> <nombreIndex> { <indirection> }
<addrBlocSuivant>       ::= <Entier6>
<indirection>           ::= <offsetDejfinition> <longueurDejfinition>
<offsetDejfinition>     ::= <Entier6>
<longueurDejfinition>   ::= <Entier4>

// .nindlexiconindex
<dejfinition>           ::= <flagDejfinition=13> <identifiantHash> <longueurDonnejes> <donnejesHash>
<identifiantHash>       ::= <Entier4>
<longueurDonnejes>      ::= <Entier4>

// .nindlocalindex
<dejfinition>           ::= <flagDejfinition=19> <identifiantDoc> <identifiantExterne>
                            <longueurDonnejes> <donnejesDoc>
<identifiantDoc>        ::= <Entier4>
<identifiantExterne>    ::= <Entier8>
<longueurDonnejes>      ::= <Entier4>

// .nindtermindex
<dejfinition>           ::= <flagDejfinition=23> <identifiantTerme> <longueurDonnejes> { <catejgorieV2> }
                            { <bloc> }                       // after the directory, within the definition
<identifiantTerme>      ::= <Entier4>
<longueurDonnejes>      ::= <Entier4>                        // of the directory (the categories), not the blocks
<catejgorieV2>          ::= <flagCg=67> <catejgorie> <frejquenceTerme> <nbreDocs> <nbreBlocs>
                            ( <postingsEnLigne>              // nbreBlocs = 0
                            | { <descripteurBloc> } )        // nbreBlocs > 0
<catejgorie>            ::= <Entier1>
<frejquenceTerme>       ::= <EntierULat64>
<nbreDocs>              ::= <EntierULat64>
<nbreBlocs>             ::= <EntierULat>
<postingsEnLigne>       ::= { <identDocRelatif> <frejquenceDoc> }      // nbreDocs times
<descripteurBloc>       ::= <premierDocRelatif> <dernierDocRelatif> <nbreDocsBloc>
                            <frejquenceMax> <longueurMin> <offsetBloc> <longueurBloc>
<premierDocRelatif>     ::= <EntierULat64>      // from the previous block's last document (0 first)
<dernierDocRelatif>     ::= <EntierULat64>      // from this block's first document
<nbreDocsBloc>          ::= <EntierULat>
<frejquenceMax>         ::= <EntierULat>        // largest frequency in the block
<longueurMin>           ::= <EntierULat>        // shortest document length in the block, 0 = unknown
<offsetBloc>            ::= <Entier4>          // from the definition's first byte (its flag)
<longueurBloc>          ::= <Entier4>
<bloc>                  ::= <flagBloc=71> <identifiantTerme> <catejgorie> <nbreDocsBloc>
                            { <identDocRelatif> <frejquenceDoc> }      // the first one relative to premierDoc
<identDocRelatif>       ::= <EntierULat64>
<frejquenceDoc>         ::= <EntierULat>
```

- **Blocks are written inside their term's definition, after its directory**
  (implementation decision, 2026-11-04), at offsets relative to the
  definition's first byte: the indirection's length covers directory and
  blocks, so the free-space management, the hole statistics and the merge
  work unchanged, and a definition moved by a rewrite keeps valid offsets.
  A search still reads a term's directory, then only the blocks it needs.
  An update rewrites the whole definition, as in v1: rewriting only the last
  block, in place, needs slack in the definition (the original design's
  "extensions"), a writer change that needs no format change. A block
  repeats its term and category, and its count, first and last documents and
  max frequency are checked against its descriptor.
- `<offsetBloc>` and `<longueurBloc>` are fixed-width so that the writer can
  write the directory first and patch them once the blocks are written.
- The block size (128 by default) is the writer's: readers only follow the
  descriptors, so blocks of different sizes can coexist (a block split by an
  update holds fewer).
- `<longueurMin>` is 0 when the writer doesn't know the documents' lengths
  (Amose's writers today): a top-k search then bounds the block by its max
  frequency alone.
- Every count, offset and length above is read from the file: bounded
  against the definition or the file before use, never used to `reserve`.
- A two-level directory isn't needed at first: a category of 7.7M documents
  (MS MARCO's `the`) has ~60k descriptors, ~1 MB, well below the 4-byte
  length.

## Implementation order

Each step keeps every test passing (ASan, TSan, golden v1 files) and is
measured against the previous one (`bench/retrieval/`):

1. **Open-time parsing in `nind._native`** (no format change): the Python
   wrappers stop parsing the envelope; the diagnostics keep working on v1.
2. **Reconcile the v1 grammar comments with the code**, then write the v2
   grammar (this document's section 2 made precise), with `EntierULat64`.
3. **64-bit external document ids** (the v1 formats refuse ids above 2^32):
   `NindDocId`, the C++ structures, Python arrays, `NINDDL02`, the new
   `NindDocNames` version.
4. **The v2 envelope** (entry size 10; 4-byte local document and lexicon
   bucket numbers; 4-byte lengths): readers read v1 and v2, writers write v2.
   The v1 golden files stay as a reader test; golden v2 files.
5. **Blocks in the term index** (flag 23): writers, readers, `getTermPostings`
   over blocks, in-place updates of the last block, `NindIndexMerge`
   (copying block bodies, rewriting their headers: a block repeats its term
   id, renumbered by the merge), `NindAmose`.
   A writer reopening a file must know the blocks' areas: the free-space
   management (`findNewArea`/`vacateOldArea`) and the hole statistics
   (`chercheVides`) only know the areas indirections point at.
6. **Block-max top-k in `Bm25Scorer`**, results identical to today's.
7. **Migration** (rebuild of v1 indexes), native `verify`, the diagnostics.

### Step 6 as implemented: block-max MaxScore, document at a time

A first version kept the term-at-a-time scorer and filtered blocks once, against
a θ taken up front from the rarest terms' partial scores. It was exact but never
faster: it still read 60% of the frequent terms' postings on MS MARCO and scored
the rarest terms twice. On MS MARCO 1M, its top 10 took 2.6 ms against the
exhaustive search's 2.3 ms, and on 4M 12.5 ms against 11.7. It was replaced by
dynamic **block-max MaxScore**, document at a time
(`Bm25Scorer::searchMaxScore`, `search(..., prune=True)`):

- The terms are sorted by increasing upper bound. Every prefix whose bounds sum
  to less than θ (the k-th best score so far) is *non-essential*: a document in
  none of the other terms can't reach the top k.
- The essential lists are read in document order, 16 blocks at a time.
- A document is completed by the non-essential terms, taken by decreasing
  bound. For each, the document is first checked against that term's block
  bound, and only that block is read.
- **Exactness.** The scores are accumulated in another order than
  `NindEngine._bm25_score`'s query order. So every comparison with θ has a
  relative margin of 1e-9, far above the rounding error of a few dozen
  additions. The candidates left at the end are then scored again, exactly, in
  query order. The results are identical (scores and ties) to the exhaustive
  search on FiQA, MS MARCO 1M and 4M, at k = 10 and k = 1000.
- **Fallback to the exhaustive search**: a term in several categories, an
  idf ≤ 0, k ≤ 0, k1 < 0, b outside [0, 1], or a long list without blocks.
  The last one can only come from a format v1 file: v2 writers keep at most
  128 postings inline. Such a list would otherwise be read whole as a single
  block.
- **Trusted descriptors.** A block that is read is checked against its
  descriptor (`decodeBlock`: first and last documents, count, exact largest
  frequency), so a mismatch throws. A block that MaxScore skips is never
  read. If such a block's descriptor understates it (a damaged file, a buggy
  writer), the search can't notice and may miss documents, where the
  exhaustive search would throw. Block-max skipping can't avoid this:
  `nind._native.verify_index` (`NindIndexVerify`) checks every descriptor
  against its block.

The native scorer's search times (µs per query, single thread, MS MARCO without
stopwords). Each row's four numbers come from the same run. Absolute times vary
by about 20% between runs on this machine (the first version's figures above
come from another run):

| corpus | top 10, exhaustive | top 10, MaxScore | top 1000, exhaustive | top 1000, MaxScore |
|---|---|---|---|---|
| FiQA (57.6k) | 818 | 715 | 1087 | 1670 |
| MS MARCO 1M | 2702 | 1309 | 3108 | 3103 |
| MS MARCO 4M | 14378 | 4387 | 12824 | 9706 |

The gain grows with the corpus, as the lists the top k doesn't need get longer.
For a k large relative to the corpus, the per-document overhead of a
document-at-a-time traversal outweighs the skipping. `NindEngine` therefore
uses MaxScore only when `top_k * PRUNING_DOCS_PER_RESULT <= documents`
(4000, measured). The exhaustive search keeps a cost that grows faster than
the corpus (2.7 ms → 14.4 ms for 4 times the documents): its score array
(8 bytes per document) no longer fits in the cache.

Still open:

- **Tighter bounds.** The writers don't fill `<longueurMin>` yet (0: the
  bound uses the corpus's shortest document); `IndexBuilder` knows the
  lengths.
- **Faster large k.** Use the exhaustive search below a document count, or
  keep a score array instead of the candidates once θ stops rising.
- **Several categories** (Amose) aren't pruned.

## Remaining questions

1. **Build memory** (not part of the format): `NindIndexer` keeps ~17 bytes
   per token in memory (8.8 GB for MS MARCO). Sharded builds plus a
   merge copying block bodies would bound it per shard.
2. **The author's other documents**: LAT2014.JYS.440 (*nind, indexation
   post-S2*, the main design), LAT2015.JYS.448 (the Amose adaptation),
   LAT2017.JYS.468 (*synchronisation des lecteurs par les fichiers*),
   LAT2016.JYS.465 (measurements) and LAT2016.JYS.466 (hashing of the
   sample's words) are referenced but not available here. 448 and 466
   especially could change the block statistics and the lexicon sizing.
3. **The local index**: v2 only widens its fields; its per-document
   definitions are small, so no blocks are needed there.
