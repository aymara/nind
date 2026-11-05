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
`corpus.nindlocalindex`, plus `corpus.nindmeta` - the tokenizer options -,
`corpus.ninddoclengths` and, when documents have names, `corpus.ninddocnames`),
published atomically through `corpus.CURRENT`.
`index_dir` must already exist; each input file becomes one document,
identified externally by its position in the list (`0`, `1`, `2`, ...).

A corpus in pyserini's JSON Lines format (one `{"id": ..., "contents": ...}`
object per line) is indexed directly, record `n` becoming document `n`:

```python
record_ids = indexer.index_jsonl("corpus.jsonl")   # record_ids[n]: the "id" of document n
```

The records' ids are stored with the index as the documents' *names*
(`index_files` takes names as `doc_names=[...]`), so search results can be
reported with them, like pyserini's hits' `docid` - see
"Document names" below.

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
    update.add_text("...", name="d49309")   # replaces the document named d49309, if any
```

A document's name is unique in the index: adding a document under a name
that is already used replaces that document (giving another document's id
with it raises `ValueError`); a replaced document keeps its name unless a new
one is given. `update.doc_id(name)` finds a document by name, changes of the
update included.

`add_file(path)`, `add_files(paths)` and `remove_documents(ids)` on the
indexer are one-update shortcuts. An update costs what it changes, whatever
the size of the index (about 0.25 s to publish, with its `fsync`s, plus the
new documents): it writes a new segment and masks the replaced and removed
documents in theirs. Segments are merged now and then; the results are always
those of one index of the live documents. `indexer.force_merge()` makes it one
index, which a search on a single thread answers fastest, and which
`engine.get_term_id`, `engine.lexicon`, `engine.term_index` and
`engine.local_index` need (they raise `ValueError` on several segments or
deleted documents).

A corpus too large to build at once: `indexer.index_jsonl(path, shards=64)`
builds that many segments in worker processes, in parallel, with memory bounded
by `workers` shards; `merge=True` merges them into one index at the end.

## Search it

```python
from nind.nind_engine import NindEngine

engine = NindEngine("indices")
for doc_id, score in engine.search("ejcrit nombre", top_k=5):
    print(f"{doc_id}: {score:.3f}")
```

`NindEngine` auto-discovers the index by scanning `index_dir` for a
`*.nindlexiconindex` file, so it only needs the directory - not the prefix.

## Search speed and threads

A search reads only the postings it needs: for a top k small for the corpus, the blocks of
postings that can't reach the top k are skipped (block-max MaxScore). On MS MARCO (8.8 million
passages) one query's top 10 takes about 1.5 ms on one thread (median).

**Many queries at once** (a server): call `search` from several threads on one `NindEngine`. It
is thread-safe and releases Python's GIL, so the queries run in parallel on the cores.

**One query faster** (idle cores, latency matters): split each query across threads, for the
whole engine or per call:

```python
engine = NindEngine("indices", search_threads=4)   # every search uses 4 threads
engine.search("binary search tree", top_k=10)      # MS MARCO: about 0.9 ms instead of 1.5
engine.search("binary search tree", threads=1)     # this one on a single thread
```

The results are the same. The default is 1: splitting a query costs more work in total (4
threads make it about 1.5–2× faster, not 4×), which a busy server would rather spend on other
queries. Splitting applies to block-max searches (a `top_k` small for the corpus, the usual
case); see the {doc}`benchmarks`.

## Document names

When the index stores document names (`index_jsonl`, `index_files(...,
doc_names=...)`, or `name=` in updates), the engine maps them to and from
document ids, and `search` can return them instead of ids:

```python
engine.search("binary search tree", doc_names=True)  # [("d49309", 12.7), ...]
engine.doc_name(0)         # "d49309"; None if the index has no names
engine.doc_id("d49309")    # 0; None if there is no such document
```

## Inspecting a document or term directly

Beyond `search`, `NindEngine` exposes the building blocks it uses
internally, which are handy for debugging relevance or inspecting a
specific document:

```python
engine.get_term_id("nombre")      # -> internal lexicon identifier, or 0 (one index only)
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
