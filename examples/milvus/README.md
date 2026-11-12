# nind as the BM25 index of a Milvus collection (proof of concept)

Milvus keeps the rows (primary key, text, vectors, scalar fields) and answers vector
searches and filters; nind keeps a BM25 index of one text field of the same rows. The
nind document ids **are** the collection's INT64 primary keys (nind's external ids are
64-bit), so a BM25 hit is a Milvus row, with no mapping table.

| | |
|---|---|
| `nind_milvus.py` | `NindBM25Collection`: writes to both, searches either or both |
| `demo.py` | a 16-row collection, BM25 / vector / hybrid searches, filter, upsert, delete, re-sync, JSONL bulk load, checked against nind alone |

## Run it

```
uv pip install "pymilvus==2.4.15" "setuptools<81"      # the client (pymilvus 2.4 imports pkg_resources)
uv run python examples/milvus/demo.py                   # embedded Milvus 2.3.5 server (pip package "milvus")
uv run python examples/milvus/demo.py --uri ./demo.db   # Milvus Lite (installed with pymilvus on Linux/macOS)
uv run python examples/milvus/demo.py --uri http://localhost:19530   # any Milvus server
```

pymilvus 2.4 is the oldest client with wheels for Python 3.13 (2.3 pins grpcio 1.60),
and works with the 2.3 server. The demo's vectors come from a toy embedding (hashed
character trigrams), so nothing is downloaded: swap `embed` for a real model.

## Using it

```python
from pymilvus import MilvusClient
from nind_milvus import NindBM25Collection

client = MilvusClient(uri="http://localhost:19530")
# an existing collection: INT64 primary key (not auto_id), a VARCHAR text field, a vector field
docs = NindBM25Collection(client, "docs", "nind-index", text_field="text", vector_field="vector",
                          tokenizer={"lowercase": True})

with docs.batch():                       # one nind update (one publish) for many writes
    docs.insert(rows)                    # rows: dicts with id, text, vector, other fields
docs.upsert([row]); docs.delete([42])

docs.bm25_search("binary search tree", 10, filter='lang == "en"', output_fields=["text"])
docs.hybrid_search("binary search tree", query_vector, 10, output_fields=["text"])
docs.sync_from_milvus()                  # (re)index an existing collection's rows in nind
docs.load_jsonl("corpus.jsonl", embed, row_fields=lambda record: {"lang": "en"})
```

- **`bm25_search`**: nind's BM25 ranking (`NindEngine.search`, block-max MaxScore);
  with a Milvus `filter`, the ranking is read further down (4x at a time) until enough
  rows pass `pk in [...] and (filter)`; `output_fields` are read from Milvus. Scores are
  BM25 over the whole collection (a filter selects rows, it doesn't recompute statistics).
- **`hybrid_search`**: nind's BM25 top `candidates` and Milvus's vector top `candidates`
  (default 5 x `limit`), fused by reciprocal rank fusion (as Milvus's `RRFRanker`); each
  result tells its rank in both lists.
- **Writes**: Milvus first, nind when the write (or the `batch()`) ends: the nind update
  is published atomically, searches switch to it by themselves. An update costs ~0.25 s
  to publish whatever its size, so batch small writes.
- **`load_jsonl`**: the bulk path. nind parses and tokenizes the file in worker processes
  (`IndexUpdate.add_jsonl`) and gives each record an id (a record named like an existing
  document replaces it), which becomes the Milvus primary key; the record's `id` is kept
  in Milvus as the dynamic field `name`.

## Why not Milvus's own BM25?

The installed server is Milvus 2.3.5: it has neither sparse vectors (2.4) nor the BM25
function (2.5). This adapter works with any version. With Milvus >= 2.4 another
integration would be possible: nind as the *encoder*, giving each document a sparse
vector of BM25 term weights (its lexicon's term ids as dimensions) for Milvus to search;
but then Milvus does the search, the statistics (idf, average length) are frozen in the
stored vectors and every update of them means rewriting vectors. Keeping nind as the
index keeps exact, always current BM25 and nind's search speed (block-max MaxScore,
segments, see `docs/benchmarks.md`).

## Limits of the proof of concept

- **Not transactional.** If the nind side of a write fails, Milvus has rows nind lacks;
  `sync_from_milvus` re-reads every row (it doesn't remove documents nind has that
  Milvus lost). A production version would log writes, or rebuild nind from Milvus
  periodically into a new prefix.
- **One writer.** nind serializes its updates (`<prefix>.lock`); concurrent writers
  through several processes wait for each other. Searches run in parallel.
- **Filters by id list.** A filtered BM25 search sends the candidates' ids to Milvus
  (`pk in [...]`): fine for selective rankings, slow when the filter rejects most top
  BM25 rows. Indexing the filter's field in nind (one index per value, or a document
  mask in the scorer) would avoid it.
- **Partitions, multiple text fields, databases**: one nind index per (collection, text
  field); a partition key could map to a nind prefix.
- **Milvus Lite 2.4** crashes on a row missing a schema field (pymilvus then retries
  forever): give every field (`row_fields` in `load_jsonl`).
