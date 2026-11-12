"""nind in place of Milvus's built-in BM25: the same full text search code, run with
Milvus's BM25 (``MilvusClient``) and with nind's (``NindMilvusClient``), results compared.

    examples/milvus/milvus-standalone.sh start            # Milvus 3.0 in Docker
    uv run python examples/milvus/demo.py                 # http://127.0.0.1:19530
    uv run python examples/milvus/demo.py --uri ./demo.db # Milvus Lite

The dense vectors of the hybrid search come from a toy embedding (hashed character
trigrams): nothing to download.
"""

import argparse
import hashlib
import os
import shutil
import sys
import tempfile

import numpy as np
from pymilvus import AnnSearchRequest, DataType, Function, FunctionType, MilvusClient, RRFRanker, WeightedRanker

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nind_milvus import NindMilvusClient  # noqa: E402

DIM = 64
CORPUS = [
    ("errors", "ERR_4012: the lexicon file is locked by another writer process"),
    ("errors", "ERR_5003 raised when a posting list block descriptor is corrupt"),
    ("errors", "Timeout while waiting for the index writer lock to be released"),
    ("errors", "The search server returned HTTP 503 because the shard was offline"),
    ("indexing", "Indexing millions of documents in shards, then merging the shards offline"),
    ("indexing", "An indexer tokenizes each document and writes the inverted file"),
    ("indexing", "Incremental updates add, replace or remove documents in a new segment"),
    ("indexing", "Segments are merged by a log-structured policy to bound their number"),
    ("search", "BM25 ranks documents by term frequency, inverse document frequency and length"),
    ("search", "Block-max MaxScore skips posting blocks that cannot reach the top k"),
    ("search", "Dense retrieval embeds queries and passages into vectors compared by cosine"),
    ("search", "Hybrid retrieval fuses lexical and semantic rankings with reciprocal rank fusion"),
    ("ops", "Restart the vector database after upgrading its configuration file"),
    ("ops", "Back up the collection, then restore it on another cluster"),
    ("ops", "Monitor memory usage of the query nodes during peak traffic"),
    ("ops", "The garbage collector removes old generations of the index nobody reads anymore"),
]
QUERIES = ["err_4012 locked lexicon", "documents index", "reciprocal rank fusion", "the index writer"]


def embed(texts):
    """Toy embedding: hashed character trigrams of the lowercased words, L2-normalized."""
    vectors = np.zeros((len(texts), DIM), dtype=np.float32)
    for row, text in enumerate(texts):
        for word in text.lower().split():
            word = "^%s$" % word.strip(".,:;()")
            for i in range(len(word) - 2):
                digest = hashlib.blake2b(word[i:i + 3].encode(), digest_size=4).digest()
                vectors[row, int.from_bytes(digest, "little") % DIM] += 1
    return vectors / np.maximum(np.linalg.norm(vectors, axis=1, keepdims=True), 1e-9)


# -- Milvus full text search, as its documentation writes it: nothing nind-specific below --

def create(client, name, pk_type=DataType.INT64, auto_id=False):
    if client.has_collection(name):
        client.drop_collection(name)
    schema = client.create_schema(auto_id=auto_id)
    if pk_type == DataType.VARCHAR:
        schema.add_field("id", DataType.VARCHAR, is_primary=True, max_length=64)
    else:
        schema.add_field("id", DataType.INT64, is_primary=True)
    schema.add_field("text", DataType.VARCHAR, max_length=8192, enable_analyzer=True)
    schema.add_field("category", DataType.VARCHAR, max_length=64)
    schema.add_field("sparse", DataType.SPARSE_FLOAT_VECTOR)
    schema.add_field("dense", DataType.FLOAT_VECTOR, dim=DIM)
    schema.add_function(Function(name="text_bm25", function_type=FunctionType.BM25,
                                 input_field_names=["text"], output_field_names=["sparse"]))
    index_params = client.prepare_index_params()
    index_params.add_index(field_name="sparse", index_type="SPARSE_INVERTED_INDEX", metric_type="BM25",
                           params={"inverted_index_algo": "DAAT_MAXSCORE", "bm25_k1": 1.2, "bm25_b": 0.75})
    index_params.add_index(field_name="dense", index_type="FLAT", metric_type="COSINE")
    client.create_collection(name, schema=schema, index_params=index_params, consistency_level="Strong")


def rows(start=0):
    vectors = embed([text for _, text in CORPUS])
    return [{"id": start + i, "text": text, "category": category, "dense": vectors[i].tolist()}
            for i, (category, text) in enumerate(CORPUS)]


def bm25(client, name, query, limit=3, **kwargs):
    return client.search(name, data=[query], anns_field="sparse", limit=limit,
                         output_fields=["text", "category"], **kwargs)[0]


def hybrid(client, name, query, ranker, limit=3):
    reqs = [AnnSearchRequest(data=[query], anns_field="sparse", param={}, limit=10),
            AnnSearchRequest(data=[embed([query])[0].tolist()], anns_field="dense", param={}, limit=10)]
    return client.hybrid_search(name, reqs, ranker=ranker, limit=limit, output_fields=["text"])[0]


def scenario(client, name):
    """Every step's results, for both clients."""
    out = {}
    create(client, name)
    client.insert(name, rows(100))
    for query in QUERIES:
        out["search " + query] = bm25(client, name, query, 5)
    out["filter"] = bm25(client, name, "documents index", 3, filter='category == "search"')
    out["offset"] = client.search(name, data=["documents index"], anns_field="sparse", limit=2,
                                  search_params={"offset": 1})[0]
    out["hybrid rrf"] = hybrid(client, name, "rank fusion", RRFRanker(60))
    out["hybrid weighted"] = hybrid(client, name, "rank fusion", WeightedRanker(0.6, 0.4))
    new_text = "ERR_7777: quota exceeded while writing the vector index"
    client.upsert(name, [{"id": 100, "text": new_text, "category": "errors", "dense": embed([new_text])[0].tolist()}])
    out["after upsert, old text"] = bm25(client, name, "4012 lexicon locked")
    out["after upsert, new text"] = bm25(client, name, "quota exceeded")
    client.delete(name, ids=[111])
    client.delete(name, filter='category == "ops"')
    out["after deletes"] = bm25(client, name, "reciprocal rank fusion vector database", 5)
    return out


def compare(builtin, replaced, tolerance=1e-3):
    """"same", "same rows" (other scores) or "different": rows at each rank, scores to ``tolerance``
    (relative); rows of equal scores may come in any order."""
    if [hit.id for hit in builtin] == [hit.id for hit in replaced]:
        same_scores = all(abs(a.distance - b.distance) <= tolerance * abs(a.distance)
                          for a, b in zip(builtin, replaced))
        return "same" if same_scores else "same rows"
    if len(builtin) == len(replaced) and all(abs(a.distance - b.distance) <= tolerance * abs(a.distance)
                                             for a, b in zip(builtin, replaced)):
        return "same"       # other rows at equal scores: ties
    return "different"


def show(title, hits):
    print("  %s" % title)
    for hit in hits:
        print("    %5s  %.4f  %s" % (hit.id, hit.distance, (hit.get("text") or "")[:60]))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--uri", default="http://127.0.0.1:19530", help="Milvus URI, or a Milvus Lite file")
    args = parser.parse_args()
    nind_dir = tempfile.mkdtemp(prefix="nind-milvus-")
    try:
        milvus = MilvusClient(uri=args.uri)
        nind = NindMilvusClient(uri=args.uri, nind_dir=nind_dir)
        builtin, replaced = scenario(milvus, "fts_builtin"), scenario(nind, "fts_nind")
        verdicts = {step: compare(builtin[step], replaced[step]) for step in builtin}
        labels = {"same": "same rows and scores", "same rows": "same rows, other scores",
                  "different": "DIFFERENT rows"}
        for step in builtin:
            print("\n== %s: %s" % (step, labels[verdicts[step]]))
            show("Milvus BM25", builtin[step])
            show("nind", replaced[step])
        print("\nsame rows and scores: %d/%d steps; same rows: %d/%d"
              % (sum(v == "same" for v in verdicts.values()), len(verdicts),
                 sum(v != "different" for v in verdicts.values()), len(verdicts)))
        print("(after an upsert or a delete, Milvus's BM25 statistics still count the replaced and deleted\n"
              " rows; nind's count the live rows only, as if the index were rebuilt)")

        # checks of the replacement itself
        assert [hit.id for hit in replaced["after upsert, new text"]] == [100]
        assert all(hit.id != 100 for hit in replaced["after upsert, old text"])
        assert all(hit.id != 111 and hit["category"] != "ops" for hit in replaced["after deletes"])
        assert all(hit["category"] == "search" for hit in replaced["filter"])
        assert nind.describe_collection("fts_nind")["functions"] == []     # Milvus has no BM25 function
        if args.uri.startswith(("http:", "https:", "tcp:", "grpc:")):
            assert all(verdict != "different" for verdict in verdicts.values()), verdicts
            assert all(verdicts[step] == "same" for step in builtin if not step.startswith("after")), verdicts
        else:
            print("(Milvus Lite's BM25 isn't the server's: negated scores, err_4012 kept whole... nind follows "
                  "the server's)")

        # repair: the nind index rebuilt from the collection's rows gives the same results
        before = {query: [(hit.id, hit.distance) for hit in bm25(nind, "fts_nind", query, 5)] for query in QUERIES}
        print("\nrebuild_bm25: %d rows" % nind.rebuild_bm25("fts_nind"))
        for query in QUERIES:
            after = [(hit.id, hit.distance) for hit in bm25(nind, "fts_nind", query, 5)]
            assert [pk for pk, _ in after] == [pk for pk, _ in before[query]]

        # VARCHAR primary keys, and auto_id
        create(nind, "fts_varchar", DataType.VARCHAR)
        nind.insert("fts_varchar", [dict(row, id="doc-%d" % row["id"]) for row in rows()])
        assert [hit.id for hit in bm25(nind, "fts_varchar", "garbage collector", 1)] == ["doc-15"]
        nind.delete("fts_varchar", ids=["doc-15"])
        assert all(hit.id != "doc-15" for hit in bm25(nind, "fts_varchar", "garbage collector"))
        create(nind, "fts_auto", auto_id=True)
        ids = nind.insert("fts_auto", [{k: v for k, v in row.items() if k != "id"} for row in rows()])["ids"]
        assert [hit.id for hit in bm25(nind, "fts_auto", "garbage collector", 1)] == [ids[15]]
        for name in ("fts_builtin", "fts_nind", "fts_varchar", "fts_auto"):
            (nind if name != "fts_builtin" else milvus).drop_collection(name)
        assert os.listdir(os.path.join(nind_dir, "default")) == []
        print("all checks passed")
    finally:
        shutil.rmtree(nind_dir, ignore_errors=True)


if __name__ == "__main__":
    main()
