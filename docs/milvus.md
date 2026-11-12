# Milvus full text search with nind

[Milvus](https://milvus.io) answers full text search with a BM25 *function*: a
`VARCHAR` field's analyzer feeds a sparse vector field, which Milvus indexes and
searches. `NindMilvusClient` replaces that function with nind: it is a
`pymilvus.MilvusClient` in which **nind does all the BM25 work**, so code written for
[Milvus full text search](https://milvus.io/docs/full-text-search.md) runs unchanged.
Only the client's class changes. Compared with Milvus's own BM25, nind gives the same
results, answers searches returning only ids 2 to 10 times faster, and keeps its
statistics exact after updates and deletions (see the results below).

```{admonition} An example module, not part of the wheel
:class: note

`NindMilvusClient` lives in the repository's
[`examples/milvus/`](https://github.com/aymara/nind/tree/master/examples/milvus)
directory (`nind_milvus.py`, one file), not in the `nind` package: copy it into your
project. It needs nind 4.2.0 or later (for {meth}`~nind.nind_engine.IndexUpdate.add_texts`)
and pymilvus 3.0, and targets a Milvus 3.0 server (tested with 3.0.6). It is a proof
of concept: read its limits below before using it in production.
```

## Set up

```bash
pip install "nind>=4.2" "pymilvus[milvus-lite]==3.0.3"
```

A Milvus server: the example directory has a script that runs Milvus 3.0.6 standalone
in Docker (embedded etcd, local storage, its data in a Docker volume):

```bash
examples/milvus/milvus-standalone.sh start     # http://127.0.0.1:19530 (MILVUS_PORT to change it)
examples/milvus/milvus-standalone.sh stop      # purge: also removes its data
```

Milvus Lite (a local file as `uri`, installed with pymilvus) works too, but its
built-in BM25 isn't the server's (see "Differences" below).

## Use it

The code of Milvus's full text search documentation, with `NindMilvusClient` in place
of `MilvusClient` and a directory for nind's indexes:

```python
from pymilvus import AnnSearchRequest, DataType, Function, FunctionType, RRFRanker
from nind_milvus import NindMilvusClient

client = NindMilvusClient(uri="http://localhost:19530", nind_dir="nind-indexes")

schema = client.create_schema()
schema.add_field("id", DataType.INT64, is_primary=True)
schema.add_field("text", DataType.VARCHAR, max_length=65535, enable_analyzer=True)
schema.add_field("sparse", DataType.SPARSE_FLOAT_VECTOR)
schema.add_field("dense", DataType.FLOAT_VECTOR, dim=768)
schema.add_function(Function(name="bm25", function_type=FunctionType.BM25,
                             input_field_names=["text"], output_field_names=["sparse"]))
index_params = client.prepare_index_params()
index_params.add_index(field_name="sparse", index_type="SPARSE_INVERTED_INDEX", metric_type="BM25",
                       params={"bm25_k1": 1.2, "bm25_b": 0.75})
index_params.add_index(field_name="dense", index_type="AUTOINDEX", metric_type="COSINE")
client.create_collection("docs", schema=schema, index_params=index_params)

client.insert("docs", rows)                      # upsert and delete (ids or filter) alike
hits = client.search("docs", data=["binary search tree"], anns_field="sparse", limit=10,
                     filter='lang == "en"', output_fields=["text"])
for hit in hits[0]:
    print(hit.id, hit.distance, hit["text"])

client.hybrid_search("docs", [AnnSearchRequest(data=["binary search tree"], anns_field="sparse",
                                               param={}, limit=50),
                              AnnSearchRequest(data=[query_vector], anns_field="dense",
                                               param={}, limit=50)],
                     ranker=RRFRanker(60), limit=10, output_fields=["text"])
```

Clients of one Milvus must share `nind_dir` (a shared filesystem when they run on
several machines). Many small writes are cheaper grouped: each write call is one nind
update, which costs about 0.25 s to publish, whatever its size:

```python
with client.bm25_batch():                        # one nind update for all of them
    for batch in batches:
        client.insert("docs", batch)
```

## How it works

- **`create_collection`** takes the BM25 functions out of the schema. Each function's
  output field stays, with the same name, as a nullable sparse vector nothing fills
  (Milvus loads no collection without a vector field), so code searching
  `anns_field="sparse"` keeps working. nind indexes the function's input field, one
  index per function under `nind_dir/<database>/<collection>/`, with the `bm25_k1` and
  `bm25_b` of the field's BM25 index (Milvus's defaults: 1.2 and 0.75).
  `create_index`, `drop_collection` and `rename_collection` follow on nind's side.
- **Writes** go to Milvus, then to nind, in one {class}`~nind.nind_engine.IndexUpdate`
  per call: the rows are tokenized natively in one batch
  ({meth}`~nind.nind_engine.IndexUpdate.add_texts`), and the update is published
  atomically, searches switching to it by themselves. nind's document ids are the
  primary keys (INT64, negative ones included), or nind document names for VARCHAR
  keys. With `auto_id`, they are the ids Milvus returns. `delete(filter=...)` reads
  the keys the filter selects, then deletes them by id in both. A partial upsert
  without the text leaves it as it is.
- **`search`** on a BM25 output field is answered by nind (block-max MaxScore, see
  {doc}`benchmarks`). The hits are pymilvus `Hit`s (`hit.id`, `hit.distance`,
  `hit["field"]`). A `filter` (or `partition_names`) is applied by Milvus to nind's
  ranking, as `pk in [...] and (filter)`, read further down until enough rows pass.
  `output_fields` are read from Milvus, in one query. `offset` and per-search
  `bm25_k1`/`bm25_b` (in `search_params["params"]`) are supported.
- **`hybrid_search`**: the BM25 requests go to nind, the others to Milvus, and the
  lists are fused client-side as Milvus's `RRFRanker` and `WeightedRanker` do
  (`WeightedRanker` normalizes BM25 and IP scores with arctan, COSINE with (1+x)/2).
- **The analyzer.** Milvus's default analyzer (`standard`) is reproduced by nind's
  tokenizer: runs of letters and digits (`err_4012` gives `err` and `4012`, `don't`
  gives `don` and `t`, `utf8` stays whole), lowercased (Unicode, by the client, since
  nind's tokenizer lowercases ASCII only). This was checked with Milvus's `run_analyzer`.
  The `whitespace` tokenizer and the `lowercase` filter are supported too. Other
  analyzers (stemming, stop words, languages) raise `NotImplementedError`; pass
  `NindMilvusClient(tokenizer=...)` with nind tokenizer options instead (see
  {doc}`quickstart`).
- **Repair.** Writes are not transactional (Milvus first, then nind). If a write fails
  in between, `client.rebuild_bm25("docs")` rebuilds the collection's nind indexes
  from its rows. It also attaches nind to rows written by another client.

## Results

`examples/milvus/compare.py` loads a corpus into two collections with the same schema,
one with Milvus's BM25 and one with nind's, then searches each query in both (top 10,
one query at a time, from Python). These runs used Milvus 3.0.6 standalone on 48 cores,
with the corpora of `bench/retrieval` (lowercased, Lucene stop words removed, except
the full MS MARCO).

| corpus | documents | nDCG@10, Milvus / nind | same top 10 ranking |
|---|---:|---|---:|
| nfcorpus | 3,633 | 0.3079 / 0.3079 | 99.7% |
| scifact | 5,183 | 0.6645 / 0.6645 | 100% |
| fiqa | 57,600 | 0.2380 / 0.2380 | 99.5% |
| MS MARCO, first 1M passages | 1,000,000 | (no judgments) | 99.1% |
| MS MARCO (dev, 2,000 queries) | 8,841,823 | 0.2317 / 0.2317 | 99.8% (same set: 100%) |

The remaining differences are ties (equal scores, in another order) and float rounding.
Latency, median / 95th percentile in ms:

| | fiqa | MS MARCO 1M | MS MARCO 8.8M |
|---|---|---|---|
| Milvus BM25, ids | 7.19 / 9.01 | 7.43 / 9.77 | 11.08 / 20.65 |
| nind, ids | **0.46 / 1.06** | **0.72 / 2.75** | **4.95 / 19.84** |
| Milvus BM25, with the text | 7.60 / 9.49 | 7.94 / 11.72 | 11.70 / 20.95 |
| nind, with the text | 8.60 / 10.99 | 8.81 / 17.13 | 11.94 / 28.55 |

Insertion until searchable took 7.5 s with Milvus's BM25 and 7.7 s with nind on fiqa,
56.7 s and 57.6 s on MS MARCO 1M, and 572 s and 735 s on MS MARCO 8.8M (nind merging
its segments as it goes, with one update per 500,000 rows).

These figures show two things:

- **Ids only:** the BM25 search itself is 2 to 10 times faster in nind. On this server
  any Milvus request costs at least 5 to 7 ms, even a query of 10 rows by id.
- **With fields:** returning fields costs nind one Milvus query, about one Milvus round
  trip, so a search with `output_fields` is on par with Milvus's own, or slightly
  slower.

The gain is for searches needing ids only (or fields kept elsewhere), and for hybrid
pipelines that fuse ids first and read the fields once.

To reproduce them (`queries.tsv`, optional `qrels.txt` and `corpus.jsonl` as
`bench/retrieval/prepare.py` writes them):

```bash
python examples/milvus/compare.py DATA_DIR --uri http://127.0.0.1:19530 --queries 2000
python examples/milvus/demo.py                 # one scenario with both clients, results compared
```

## Differences with Milvus's BM25

- **Statistics of live documents.** After an upsert or a delete, Milvus's BM25 still
  counts the replaced and deleted rows in the number of documents, the average length
  and the document frequencies (in our tests, even after a manual flush and
  compaction). nind counts the live rows only: its scores are those of an index rebuilt
  from them. Before any replacement or deletion, the scores are the same, to float
  precision.
- **Not supported** with a BM25 field: `ranker` and `function_chains` in `search`,
  highlighters, `group_by_field`, search by `ids`, rerankers other than RRF and
  weighted in `hybrid_search`, sparse vectors given as queries, and analyzers other
  than standard, whitespace and lowercase. `text_match` is not BM25: Milvus does it,
  with `enable_match` on the field.
- **Consistency.** A nind search sees every published nind update at once. The rows
  read for `output_fields` and `filter` follow the collection's consistency level, or
  the search's `consistency_level`.
- **Milvus Lite**'s built-in BM25 differs from the server's: it returns negated scores
  and keeps `err_4012` whole. nind follows the server's. On Lite, the client fills the
  placeholder sparse field with a dummy vector, because Lite refuses a missing one.

## Limits

- **Not transactional:** Milvus is written first. If a write fails before nind is
  updated, run `rebuild_bm25`.
- **One writer per collection at a time:** nind serializes its updates with a lock
  file. Searches run in parallel.
- **Filters and fields cost a Milvus query:** a filtered search, or one returning
  fields, makes one more Milvus query (`pk in [...]`). Storing the fields in nind, or
  running nind inside Milvus's query nodes, would avoid it.
- **Databases:** the client uses the `db_name` it was created with. `using_database`
  isn't followed.
