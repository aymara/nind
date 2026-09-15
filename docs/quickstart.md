# Quickstart

The fastest way to use nind from Python is the high-level API in
{mod}`nind.nind_engine`: {class}`~nind.nind_engine.NindIndexer` builds a
binary index from a list of files, and {class}`~nind.nind_engine.NindEngine`
opens that index and answers BM25-ranked search queries.

## Index a corpus

```python
from nind.nind_engine import NindIndexer

indexer = NindIndexer(index_dir="indices", prefix="corpus")
indexer.index_files([
    "src/py/nind/NindFile.py",
    "src/py/nind/NindPadFile.py",
    "src/py/nind/nind_engine.py",
])
```

This writes three files into `indices/`:
`corpus.nindlexiconindex`, `corpus.nindtermindex` and
`corpus.nindlocalindex`. `index_dir` must already exist; each input file
becomes one document, identified externally by its position in the list
(`0`, `1`, `2`, ...).

## Search it

```python
from nind.nind_engine import NindEngine

engine = NindEngine("indices")
for doc_id, score in engine.search("ejcrit nombre", top_k=5):
    print(f"{doc_id}: {score:.3f}")
```

`NindEngine` auto-discovers the index by scanning `index_dir` for a
`*.nindlexiconindex` file, so it only needs the directory - not the prefix.

## Inspecting a document or term directly

Beyond `search`, `NindEngine` exposes the building blocks it uses
internally, which are handy for debugging relevance or inspecting a
specific document:

```python
engine.get_term_id("nombre")      # -> internal lexicon identifier, or 0
engine.get_df("nombre")           # -> how many documents contain it
engine.get_tf("nombre", doc_id=1) # -> occurrences in that one document
engine.get_doc_len(doc_id=1)      # -> document length in tokens
```

## Reading files written by the C++ implementation

Every class `NindEngine` builds on
({class}`~nind.NindLexiconindex.NindLexiconindex`,
{class}`~nind.NindTermindex.NindTermindex`,
{class}`~nind.NindLocalindex.NindLocalindex`) can also open index files
produced by the C++ stack directly - they only need the binary files on
disk, not anything written by `NindIndexer`:

```python
from nind.NindTermindex import NindTermindex

term_index = NindTermindex("/path/to/corpus.nindtermindex")
for categorie, frequence, docs in term_index.donneListeTermesCG(term_id):
    print(categorie, frequence, docs)
```

See {doc}`architecture` for how these lower-level classes relate to each
other, and {doc}`api/index` for the full reference.
