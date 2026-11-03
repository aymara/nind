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

This writes the index into a new generation directory inside `indices/`
(`corpus.gen-<n>-<hex>/`: `corpus.nindlexiconindex`, `corpus.nindtermindex`,
`corpus.nindlocalindex`, plus `corpus.nindmeta` - the tokenizer options - and
`corpus.ninddoclengths`), published atomically through `corpus.CURRENT`.
`index_dir` must already exist; each input file becomes one document,
identified externally by its position in the list (`0`, `1`, `2`, ...).

A corpus in pyserini's JSON Lines format (one `{"id": ..., "contents": ...}`
object per line) is indexed directly, record `n` becoming document `n`:

```python
record_ids = indexer.index_jsonl("corpus.jsonl")   # record_ids[n]: the "id" of document n
```

## Tokenizer and index options

Texts are tokenized by a native tokenizer for source-code-like text: words
are runs of letters and digits, split at camelCase (`fooBar` -> `foo` `Bar`,
`HTTPServer` -> `HTTP` `Server`), underscore (`foo_bar` -> `foo` `bar`) and
letter/digit (`utf8` -> `utf` `8`) boundaries. Each split is optional, and
tokens can be lowercased:

```python
indexer = NindIndexer("indices", prefix="corpus",
                      tokenizer={"lowercase": True, "split_digits": False})
```

The options are stored with the index: `NindEngine` tokenizes queries the
same way, and updates keep them. Tokens longer than 254 bytes (the longest
word a nind lexicon stores) are dropped.

`local_index=False` leaves out the local index (each document's terms and
their positions). `NindEngine` doesn't need it - document lengths are stored
apart - and it makes up most of an index's size and a good part of its build
time; other readers of nind indexes (Amose...) may need it:

```python
indexer = NindIndexer("indices", prefix="corpus", local_index=False)
```

## Index incrementally

`index_files` rebuilds the whole index. To add, replace or remove documents
in an existing index instead - or to build one file by file - group the
changes in an update; they are published together when the `with` block
ends (nothing changes if it raises):

```python
with indexer.update() as update:
    for path in new_files:
        doc_id = update.add_file(path)     # new id: one more than the highest
    update.add_file("changed.py", doc_id=1) # replaces document 1
    update.remove(2)
```

`add_file(path)`, `add_files(paths)` and `remove_documents(ids)` on the
indexer are one-update shortcuts. Each update copies the index and reopens
its files, so on a large index group many documents per update rather than
calling `add_file` once per file.

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
disk, not anything written by `NindIndexer`. `NindTermindex` and
`NindLocalindex` do need the lexicon's identification stamp to look
anything up (a cross-check the underlying format itself requires), so open
the lexicon first:

```python
from nind.NindLexiconindex import NindLexiconindex
from nind.NindTermindex import NindTermindex

lexicon = NindLexiconindex("/path/to/corpus.nindlexiconindex")
term_index = NindTermindex("/path/to/corpus.nindtermindex", lexicon.donneIdentification())
for term_cg in term_index.donneListeTermesCG(term_id):
    print(term_cg.cg, term_cg.frequency, term_cg.documents)
```

`donneListeTermesCG` (and the equivalent lookup methods on the other
reader classes) return the `nind._native` objects directly - here, a list
of `TermCG`, each with `.cg`/`.frequency`/`.documents` (a list of
`Document`, each with `.ident`/`.frequency`) - rather than plain tuples.

See {doc}`architecture` for how these lower-level classes relate to each
other, and {doc}`api/index` for the full reference.
