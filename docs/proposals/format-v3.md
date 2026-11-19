---
orphan: true
---

# Proposal: nind format v3, a smaller term index

Status: **implemented** (2026-11-19, the four decisions below). Measured, built directly as the
benchmarks build nind: MS MARCO v2.1 documents' term index 10,235 -> 8,855 MB (-13.5%, estimated
-13.9%), the 138M MS MARCO v2 passages' 7,908 -> 7,237 MB (-8.5%, estimated -8.7%); every dev
query's top 10 and top 100 identical (ids and scores); top 10 medians on one thread, the same
binary on both indexes, alternated: long documents 4.35 -> 4.12 ms, passages 12.1 -> 11.95 ms (the
impacts' rounded lengths, measured with the rest, cost nothing). Written after the long-document benchmark
(MS MARCO v2.1, `bench/README.md` section 8), where nind's index takes a third more space than
Lucene's: 11.2 GB against 8.45.

## Why

On MS MARCO v2.1 documents (10.96M web pages, 4.82G postings, stopwords removed), the term index
takes 10,235 MB, Lucene's postings (`.doc`) 7,668 MB. Measured part by part
(`bench/v3_estimate.py`: every term's v2 bytes computed from the grammar, the directories' total
checked against the file's, byte for byte; `nind-bench/scripts/term_index_parts.py`):

| part | v2 | what it is |
|---|---:|---|
| block bodies | 7,249 MB | 4.69G postings in 36.9M blocks, gaps and frequencies bit-packed: 1.54 bytes a posting, Lucene's level |
| inline headers | 545 MB | 38.9M rare terms (128 postings or fewer), ~14 bytes of header each |
| inline postings | 488 MB | their 129M postings: a gap and a frequency byte each |
| descriptor impacts | 440 MB | the blocks' competitive (frequency, length) pairs |
| block headers | 406 MB | ~11 bytes a block: flag, term id, category, count, minimum frequency, widths |
| indirections | 393 MB | 10 bytes a term |
| descriptor offset/length | 295 MB | 8 bytes a block, fixed-width |
| descriptor ids, count, max/min | 302 MB | |

The postings themselves are as compact as Lucene's. The difference is fixed costs: per block
(headers and descriptors repeating what is known, fixed-width fields), and per rare term (a
definition of ~29 bytes for 3.3 postings on average; 27.4M terms have one posting). Lucene keeps
short lists' metadata in its terms dictionary and folds a frequency of 1 into the document gap.

What v3 doesn't change: the block bodies, the lexicon (996 MB against Lucene's 727 MB of terms
dictionary: a later step), the local index, the side files (lengths, names, deletions), and the
term -> categories -> documents semantics Amose relies on.

## Principles kept

From `format-v2-blocks.md`: function first, then the byte (each field sized for its function);
the grammar written before the code; v1 and v2 files still read; writers of a new file write the
new format; an existing file reopened by a writer keeps its format (an update of a v2 index
writes v2 segments next to them, as a v2 writer refuses to extend v1); readers never trust a
count, length or offset read from a file.

## The changes

Each saves what `bench/v3_estimate.py` computed on the long documents (passages: the 138M MS
MARCO v2 index, 7,908 MB):

| change | long documents | passages |
|---|---:|---:|
| 1, 2. single-posting terms in their indirection, without definition; the other rare terms' definitions without the term id, the length and the term frequency (implied) | 510 MB | 131 MB |
| 3. a frequency of 1 folded into the gap (`2 * gap + (frequency == 1)`, the frequency only when above 1), and the single postings gone with 1 | 194 MB | 61 MB |
| 4. block headers without the term id, category and count (the descriptor has them) | 258 MB | 218 MB |
| 5. descriptors without the offset (the lengths, summed) and with a varint length | 226 MB | 189 MB |
| 6. descriptors without the largest frequency and shortest length (the impacts' first and last) | 106 MB | 62 MB |
| 7. the impacts' lengths on one byte, rounded down on a log scale | 132 MB | 0 |
| 8. the count as `BLOCK_SIZE - count` (1 byte for full blocks, 2 today) | 36 MB | 31 MB |
| indirections of 11 bytes (the kind of entry, see the gate) | -39 MB | -10 MB |
| **total** | **1,426 MB (13.9%)** | **684 MB (8.7%)** |

(The estimate counts 1 and 2 together, its "inline headers", and the postings of 1 and 3 together.)
The long documents' index would go from 11.2 to ~9.8 GB (Lucene 8.45).

- **Exactness.** None changes a posting. 7 changes the impacts' lengths for the bounds only: a
  length rounded down gives a bound above the exact one (BM25 decreases with the length), so
  MaxScore stays exact, its bounds a little looser (to measure: block-max skips fewer blocks).
  Scores are computed from the documents' exact lengths, as today.
- **Robustness.** 2 and 4 drop fields repeated for checking (a block named its term, a definition
  its id and length, the indirection has them). What stays checked: the definition's flag and
  directory length against its indirection's length, every block's flag, its decoded count, first
  and last documents and largest frequency against its descriptor, and the blocks' lengths summing
  to the definition's. A block read at a wrong offset still fails its checks, so the robustness
  tests' guarantee holds (every corrupt read succeeds or throws `FileException`).

## The v3 grammar (term index only)

Only what changes from v2:

```
// NindPadFile envelope of a .nindtermindex
<fichier>               ::= <tailleEntreje=11> ...                      // the version gate
<indirection>           ::= <offsetDejfinition> <longueurDejfinition> <genreEntreje>
<genreEntreje>          ::= <Entier1>
    // 0: no term (offset and length 0)
    // 1: a definition at <offsetDejfinition>, of <longueurDejfinition> bytes
    // 2: a term of one posting, category 0, without definition: <offsetDejfinition> (Entier6) is
    //    its document (below 2^48), <longueurDejfinition> (Entier4) its frequency

// .nindtermindex
<dejfinitionV3>         ::= <flagDejfinition=29> <longueurRejpertoire> { <catejgorieV3> } { <blocV3> }
<longueurRejpertoire>   ::= <EntierULat>            // from the flag to the first block
<catejgorieV3>          ::= <flagCg=79> <catejgorie> <nbreDocs> <postingsEnLigneV3>    // nbreDocs <= BLOCK_SIZE
                          | <flagCg=81> <catejgorie> <frejquenceTerme> <nbreDocs> <nbreBlocs>
                            { <descripteurBlocV3> }                                    // with impacts
                          | <flagCg=83> <catejgorie> <frejquenceTerme> <nbreDocs> <nbreBlocs>
                            { <descripteurBlocSansImpacts> }                           // lengths unknown (Amose)
<postingsEnLigneV3>     ::= { <ecartEtUn> [ <frejquenceDoc> ] }    // nbreDocs times; frejquenceTerme: their sum
<ecartEtUn>             ::= <EntierULat64>          // 2 * (the gap from the previous document, the first from 0)
                                                    //   + 1 if the frequency is 1 (then no <frejquenceDoc>)
<descripteurBlocV3>     ::= <premierDocRelatif> <dernierDocRelatif> <manque> <longueurBloc>
                            <nbreImpacts> { <frejquence> <longueurPlancher> }
<descripteurBlocSansImpacts> ::= <premierDocRelatif> <dernierDocRelatif> <manque> <longueurBloc>
                            <frejquenceMax>
<manque>                ::= <EntierULat>            // BLOCK_SIZE - nbreDocsBloc
<longueurBloc>          ::= <EntierULat>            // a block's offset: <longueurRejpertoire> + the lengths before it
<longueurPlancher>      ::= <Entier1>               // a lower bound of the length (decoded by a fixed table)
    // frejquenceMax: the first impact's frequency; longueurMin: the last impact's length
<blocV3>                ::= <flagBloc=85> <frejquenceMin> <bitsEcarts> <bitsFrejquences>
                            <ecartsPackejs> <frejquencesPackejes>           // as flag 73's
```

- **The gate.** v2 readers check the entry size (`NindPadFile`), so they refuse a v3 term index
  with an exception rather than misread it; `NindIndex::isFormatV2` (entry size 10) becomes "wide
  fields" (10 or 11) and `isFormatV3` (11) is added. Only the term index changes: an index's
  lexicon and local index stay v2, and `verify_index`'s check that the files share a format
  becomes "a v3 term index goes with a v2 lexicon".
- A term of one posting in another category than 0, or of a document id of 2^48 or more, keeps a
  definition.
- The length table of `<longueurPlancher>`: 0-127 exact, then 128 more values each covering
  ~1/16 of a doubling (lengths up to 2^32), the floor of the true length.

## What it touches

`NindTermIndex` (readers: `getTermDirectory`, `getTermDef`, `getTermPostings`, `decodeBlock`,
`readDefinitionBytes`/`decodeTermBlock`; writers: `writeTermDefV2` becomes v2 or v3,
`putBlock`, `putBlockDescriptor`, `setTermPostings`, `setTermDef`; in place: `updateTermDef`,
`appendInPlace`, with the directory's length a varint and the offsets derived), `NindPadFile`
and `NindIndex` (the 11-byte entry and its kind), `NindIndexBuilder` (through
`setTermPostings`), `NindIndexMerge` (reads any, writes v3), `NindSegmentedUpdate` (new segments
v3), `NindIndexUpdate` (keeps the file's format), `NindIndexVerify` (v3 checks), `NindBm25Scorer`
(its `CompactBlock` from a v3 directory: the offsets summed, the impacts' lengths decoded),
`NindAmose` (through `NindTermIndex`), the bindings (`is_format_v3`), golden files (a v3 set; v1
and v2 stay, read only), the robustness tests (corrupt v3 files).

## Implementation order

Each step keeps every test passing (ASan; TSan when available) and the v1 and v2 golden files
read:

1. The v3 grammar in the term index's source comments; `bench/v3_estimate.py` kept to check the
   sizes once written.
2. `NindPadFile`/`NindIndex`: 11-byte entries and their kind (the singleton's fields), readers of
   10 and 11.
3. `NindTermIndex` v3 readers, then writers for new files (a reopened file keeps its format), the
   in-place update paths; golden v3 files; robustness tests.
4. `NindIndexVerify`, `NindIndexMerge`, `NindSegmentedUpdate`, `NindBm25Scorer`.
5. Measure: sizes against the estimate, build and search speed (both corpora, alternated runs,
   identical results), block-max with rounded lengths (7) separately, so that it can be dropped
   if it costs speed.
6. The benchmarks (sizes on the page), CLAUDE.md, the release notes: indexes written by it need
   nind >= that version.

## Later, not in v3

- **Patched blocks** (Lucene's PFor: a block's few widest values apart): ~7% of the block bodies
  (~500 MB here; measured on the query terms' blocks: gaps 6.08 -> 5.80 bits, frequencies 4.76 ->
  4.24, the widest 10% apart), but a slower, branchier decoding: measured first.
- **The lexicon** (996 MB against 727 MB): its hash buckets' definitions.

## Decisions (2026-11-19)

1. Single-posting terms in their indirection (1), with 11-byte entries: yes.
2. The impacts' lengths rounded down on one byte (7): yes, measured on their own.
3. Dropping the repeated fields (2, 4): yes.
4. Patched blocks: later, measured on their own.
