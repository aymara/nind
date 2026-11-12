# nind in place of Milvus's built-in BM25 (proof of concept)

`NindMilvusClient` (`nind_milvus.py`) is a `pymilvus.MilvusClient` whose **full text
search is done by nind** instead of Milvus's BM25 function. Code written for
[Milvus full text search](https://milvus.io/docs/full-text-search.md) runs unchanged;
only the client's class changes:

```python
from pymilvus import DataType, Function, FunctionType
from nind_milvus import NindMilvusClient

client = NindMilvusClient(uri="http://localhost:19530", nind_dir="nind-indexes")   # was MilvusClient(...)

schema = client.create_schema()
schema.add_field("id", DataType.INT64, is_primary=True)
schema.add_field("text", DataType.VARCHAR, max_length=65535, enable_analyzer=True)
schema.add_field("sparse", DataType.SPARSE_FLOAT_VECTOR)
schema.add_function(Function(name="bm25", function_type=FunctionType.BM25,
                             input_field_names=["text"], output_field_names=["sparse"]))
index_params = client.prepare_index_params()
index_params.add_index(field_name="sparse", index_type="SPARSE_INVERTED_INDEX", metric_type="BM25",
                       params={"bm25_k1": 1.2, "bm25_b": 0.75})
client.create_collection("docs", schema=schema, index_params=index_params)

client.insert("docs", rows)                          # upsert, delete (ids or filter) alike
client.search("docs", data=["a query"], anns_field="sparse", limit=10,
              filter='lang == "en"', output_fields=["text"])
client.hybrid_search("docs", [AnnSearchRequest(data=["a query"], anns_field="sparse", ...),
                              AnnSearchRequest(data=[vector], anns_field="dense", ...)],
                     ranker=RRFRanker(60), limit=10)
```

| | |
|---|---|
| `nind_milvus.py` | `NindMilvusClient` |
| `demo.py` | one full text search scenario run with `MilvusClient` and with `NindMilvusClient`, results compared, plus checks of the replacement |
| `compare.py` | built-in vs nind BM25 on a BEIR-style corpus: nDCG@10, agreement, latency, insertion time |
| `milvus-standalone.sh` | a Milvus standalone server in Docker (embedded etcd, local storage) |

## Run it

```
uv pip install "pymilvus[milvus-lite]==3.0.3"     # client 3.0, Milvus Lite (local file) included
examples/milvus/milvus-standalone.sh start         # Milvus 3.0.6 in Docker, http://127.0.0.1:19530
uv run python examples/milvus/demo.py              # or --uri ./demo.db for Milvus Lite
uv run python examples/milvus/compare.py ../nind-bench/data/fiqa-stop
examples/milvus/milvus-standalone.sh stop          # (purge: also removes its data volume)
```

## How it works

- **`create_collection`** takes the BM25 functions out of the schema. Each function's
  output field stays (same name), as a nullable sparse vector nothing fills, with an IP
  index: Milvus needs a vector field to load a collection, and the code that searches
  `anns_field="sparse"` keeps working. nind indexes the function's input field, one
  index per function under `nind_dir/<db>/<collection>/`, with the `bm25_k1`/`bm25_b`
  of the BM25 index. `create_index`, `drop_collection` and `rename_collection` follow.
- **Writes** go to Milvus, then to nind: one nind update per call (its documents
  tokenized natively in one batch, `IndexUpdate.add_texts`), published atomically; nind
  searches switch to it by themselves. An update costs ~0.25 s to publish, so
  `with client.bm25_batch(): ...` groups many calls into one. nind's document ids are
  the primary keys (INT64, negative ones too), or nind document names (VARCHAR keys);
  with `auto_id`, the ids Milvus returns. `delete(filter=...)` reads the selected keys,
  deletes them by id in both. A partial upsert without the text leaves it as it is.
- **`search`** on a BM25 output field is answered by nind (block-max MaxScore): the hits
  are pymilvus `Hit`s, `{pk: id, "distance": score, "entity": {...}}`. A `filter` (or
  `partition_names`) is applied by Milvus to nind's ranking (`pk in [...] and (filter)`,
  read further down, 4x at a time, until enough rows pass); `output_fields` are read
  from Milvus in one query. `offset` and per-search `bm25_k1`/`bm25_b` are supported.
- **`hybrid_search`** with a BM25 request: that request goes to nind, the others to
  Milvus, and the lists are fused client-side as Milvus's `RRFRanker` and
  `WeightedRanker` do (with its score normalizations: arctan for BM25 and IP, (1+x)/2
  for COSINE).
- **The analyzer.** The default (`standard`) analyzer is reproduced by nind's tokenizer:
  runs of letters and digits (`err_4012` -> `err 4012`, `don't` -> `don t`, `utf8`
  whole), lowercased (Unicode, by the client). Checked with Milvus's `run_analyzer`.
  `whitespace` and `lowercase` are supported too; other analyzers (stemming, stop
  words, languages) raise `NotImplementedError` (give `NindMilvusClient(tokenizer=...)`).
- **`rebuild_bm25(collection)`** rebuilds the nind indexes from the collection's rows:
  the repair if a write failed between Milvus and nind (it is not transactional).

## Results (Milvus 3.0.6 standalone, 48 cores)

`compare.py` on the corpora of `bench/retrieval` (lowercased, Lucene stop words removed):
the same rows go into two collections of the same schema, one with Milvus's BM25, one
with nind's; each query is searched in both, top 10, one at a time, from Python.

| corpus | documents | nDCG@10 Milvus / nind | same top 10 | top score difference (max) |
|---|---:|---|---:|---:|
| nfcorpus | 3,633 | 0.3079 / 0.3079 | 99.7% | 1.5e-6 |
| scifact | 5,183 | 0.6645 / 0.6645 | 100% | 5.7e-4 |
| fiqa | 57,600 | 0.2380 / 0.2380 | 99.5% | 1.3e-4 |
| MS MARCO, 1M passages | 1,000,000 | (no qrels) | 99.1% | 9.8e-4 |
MSMARCO_ROW

The remaining differences are ties and float rounding.

Latency, p50 / p95 in ms (fiqa; MS MARCO 1M):

| | fiqa | MS MARCO 1M |
|---|---|---|
| Milvus BM25, ids | 7.19 / 9.01 | 7.43 / 9.77 |
| nind, ids | **0.46 / 1.06** | **0.72 / 2.75** |
| Milvus BM25, with `text` | 7.60 / 9.49 | 7.94 / 11.72 |
| nind, with `text` | 8.60 / 10.99 | 8.81 / 17.13 |
| nind engine alone (no client) | 0.44 / 1.13 | 0.68 / 2.75 |

Time to insert and be searchable (index built): fiqa 7.5 s (Milvus) / 7.7 s (nind);
MS MARCO 1M 56.7 s / 57.6 s. Most of it is the Milvus insert both pay (the nind
collection still stores the text).

What it says: the BM25 search itself is ~10x faster in nind (a Milvus search costs at
least ~5-7 ms on this server, whatever it does: a query of 10 rows by id costs 5-6.6 ms).
Returning fields costs a Milvus query, so a search with `output_fields` is about one
Milvus round trip slower than the bare nind search, and then slightly slower than
Milvus's own. The win is for searches that need ids only (or fields kept elsewhere),
hybrid pipelines that fuse ids first and fetch once, and scale (see MS MARCO).
(On nfcorpus and scifact, Milvus's ids-only searches took 18-24 ms, more than with
fields: not investigated.)

## Differences with Milvus's BM25

- **Statistics of live documents.** After an upsert or a delete, Milvus's BM25 still
  counts the replaced and deleted rows in N, the average length and the document
  frequencies (here even after a manual flush and compaction); nind counts the live
  rows only: its scores are those of an index rebuilt from them. Before any
  replacement or deletion, the scores are the same (to float precision).
- **Ties** (equal scores) may come in another order.
- **Not supported** with a BM25 field: `ranker`/`function_chains` in `search`,
  highlighters, `group_by_field`, search by `ids`, other rerankers in `hybrid_search`,
  `text_match` (it is not BM25: Milvus does it, with `enable_match`), analyzers other
  than standard/whitespace/lowercase, and sparse vectors given as queries.
- **Consistency.** A nind search sees every published nind update at once (no
  consistency level); the rows of `output_fields` and `filter` are read from Milvus at
  the collection's level (or the search's `consistency_level`).
- **Milvus Lite**'s built-in BM25 differs from the server's (negated scores, its own
  tokenizer); nind follows the server's. On Lite, the client fills the placeholder
  sparse field with a dummy vector (Lite refuses a missing one).

## Limits of the proof of concept

- **Not transactional**: Milvus is written first; if nind fails, `rebuild_bm25`.
- **One writer per collection**: nind serializes its updates (a lock file); clients of
  one Milvus must share `nind_dir` (a shared filesystem across machines).
- **Each search returning fields or filtered costs a Milvus query** (`pk in [...]`):
  about one Milvus round trip, as much as Milvus's own search (see the results).
  Storing the fields in nind, or running nind inside Milvus's query nodes, would avoid
  it.
- **Databases**: the client's `db_name` at creation; `using_database` isn't followed.
