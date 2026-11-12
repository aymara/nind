"""Proof of concept: nind as the BM25 index of a Milvus collection.

    uv run python examples/milvus/demo.py                      # embedded Milvus 2.3 server (pip package "milvus")
    uv run python examples/milvus/demo.py --uri ./demo.db      # Milvus Lite
    uv run python examples/milvus/demo.py --uri http://localhost:19530   # a running Milvus server

Builds a small collection (text, category, vector), indexes its text with nind, then
compares BM25, vector and hybrid (RRF) searches, filters, an upsert and a delete, and
checks the results against nind alone. The vectors come from a toy embedding (hashed
character trigrams): replace ``embed`` with a real model for real use.
"""

import argparse
import hashlib
import os
import shutil
import sys
import tempfile
import time

import numpy as np
from pymilvus import DataType, MilvusClient, MilvusException

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nind_milvus import NindBM25Collection  # noqa: E402

DIM = 256
COLLECTION = "nind_bm25_demo"

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
    ("ops", "The garbage collector removes old generations nobody reads anymore"),
]


def embed(texts):
    """Toy embedding: hashed character trigrams of the lowercased words, L2-normalized."""
    vectors = np.zeros((len(texts), DIM), dtype=np.float32)
    for row, text in enumerate(texts):
        for word in text.lower().split():
            word = "^%s$" % word.strip(".,:;()")
            for i in range(len(word) - 2):
                vectors[row, int.from_bytes(hashlib.blake2b(word[i:i + 3].encode(), digest_size=4).digest(), "little") % DIM] += 1
    norms = np.linalg.norm(vectors, axis=1, keepdims=True)
    return vectors / np.where(norms == 0, 1, norms)


def connect(uri, attempts=30):
    # the embedded server answers its health check before its gRPC proxy accepts connections
    for attempt in range(attempts):
        try:
            return MilvusClient(uri=uri, timeout=10)
        except MilvusException:
            if attempt == attempts - 1:
                raise
            time.sleep(2)


def create_collection(client):
    if client.has_collection(COLLECTION):
        client.drop_collection(COLLECTION)
    schema = client.create_schema(auto_id=False, enable_dynamic_field=True)
    schema.add_field("id", DataType.INT64, is_primary=True)
    schema.add_field("text", DataType.VARCHAR, max_length=8192)
    schema.add_field("category", DataType.VARCHAR, max_length=64)
    schema.add_field("vector", DataType.FLOAT_VECTOR, dim=DIM)
    index_params = client.prepare_index_params()
    index_params.add_index(field_name="vector", index_type="FLAT", metric_type="COSINE")
    client.create_collection(COLLECTION, schema=schema, index_params=index_params, consistency_level="Strong")


def show(title, hits):
    print("\n== %s" % title)
    for hit in hits:
        extra = ""
        if "bm25_rank" in hit:
            extra = "  [bm25 #%s, vector #%s]" % (hit["bm25_rank"] or "-", hit["vector_rank"] or "-")
        print("  %4d  %.4f  %-9s %s%s" % (hit["id"], hit["score"], hit.get("category", ""), hit.get("text", "")[:70], extra))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--uri", help="Milvus URI (default: start the embedded Milvus server)")
    parser.add_argument("--work-dir", help="directory of the nind index and the embedded server's data "
                                           "(default: a temporary one, removed at the end)")
    parser.add_argument("--port", type=int, default=19530, help="embedded server's port")
    args = parser.parse_args()

    work_dir = args.work_dir or tempfile.mkdtemp(prefix="nind-milvus-")
    server = None
    try:
        if args.uri is None:
            from milvus import MilvusServer
            server = MilvusServer()
            server.set_base_dir(os.path.join(work_dir, "milvus"))
            server.listen_port = args.port
            print("starting the embedded Milvus server...")
            server.start()
            uri = "http://127.0.0.1:%d" % args.port
        else:
            uri = args.uri
        client = connect(uri)
        try:
            version = client.get_server_version()
        except Exception:           # Milvus Lite doesn't implement it
            version = "(Lite)"
        print("Milvus", version, "at", uri)
        create_collection(client)

        nind_dir = os.path.join(work_dir, "nind")
        shutil.rmtree(nind_dir, ignore_errors=True)
        collection = NindBM25Collection(client, COLLECTION, nind_dir, tokenizer={"lowercase": True})

        texts = [text for _, text in CORPUS]
        rows = [{"id": 100 + i, "text": text, "category": category, "vector": vector.tolist()}
                for i, ((category, text), vector) in enumerate(zip(CORPUS, embed(texts)))]
        with collection.batch():            # one nind update for the whole insert
            collection.insert(rows)
        print("inserted %d rows; nind: %d documents, average length %.1f"
              % (len(rows), collection.engine.total_docs, collection.engine.avg_doc_len))

        fields = ["text", "category"]
        queries = ["ERR_4012 locked lexicon", "indexer writes documents", "reciprocal rank fusion"]
        for query in queries:
            show("BM25 (nind): %r" % query, collection.bm25_search(query, 3, output_fields=fields))
            show("vector (Milvus): %r" % query, collection.vector_search(embed([query])[0], 3, output_fields=fields))
            show("hybrid RRF: %r" % query, collection.hybrid_search(query, embed([query])[0], 3, output_fields=fields))

        # BM25 ranking restricted by a Milvus filter
        hits = collection.bm25_search("documents index", 3, filter='category == "search"', output_fields=fields)
        show('BM25 "documents index", filter category == "search"', hits)
        assert hits and all(hit["category"] == "search" for hit in hits)

        # self-check: BM25 through the collection = nind alone
        engine = collection.engine
        for query in queries:
            assert [hit["id"] for hit in collection.bm25_search(query, 5)] == \
                   [doc_id for doc_id, _ in engine.search(query, top_k=5)]

        # upsert: row 100 gets a new text, in both
        new_text = "ERR_7777: quota exceeded while writing the vector index"
        collection.upsert([{"id": 100, "text": new_text, "category": "errors", "vector": embed([new_text])[0].tolist()}])
        # (the tokenizer splits ERR_4012 into err 4012: the other errors still match "err")
        assert collection.bm25_search("4012", 5) == []
        assert [hit["id"] for hit in collection.bm25_search("ERR_7777 quota", 1)] == [100]
        show('after upsert of 100: BM25 "quota exceeded"', collection.bm25_search("quota exceeded", 3, output_fields=fields))

        # delete: row 111 (reciprocal rank fusion) leaves both
        collection.delete([111])
        assert 111 not in [hit["id"] for hit in collection.bm25_search("reciprocal rank fusion", 5)]
        assert not client.query(COLLECTION, filter="id == 111", output_fields=["id"])
        show('after delete of 111: hybrid "reciprocal rank fusion"',
             collection.hybrid_search("reciprocal rank fusion", embed(["reciprocal rank fusion"])[0], 3, output_fields=fields))

        # attach nind to an existing collection: rebuild the index from Milvus's rows
        rebuilt = NindBM25Collection(client, COLLECTION, os.path.join(work_dir, "nind-rebuilt"), tokenizer={"lowercase": True})
        count = rebuilt.sync_from_milvus()
        assert count == len(CORPUS) - 1
        for query in queries + ["quota exceeded"]:
            got, expected = rebuilt.bm25_search(query, 5), collection.bm25_search(query, 5)
            assert [hit["id"] for hit in got] == [hit["id"] for hit in expected]
            assert all(abs(a["score"] - b["score"]) <= 1e-9 * abs(b["score"]) for a, b in zip(got, expected))
        print("\nsync_from_milvus: %d rows reindexed, same BM25 results" % count)

        # bulk load of a JSON Lines corpus: nind gives the ids, Milvus uses them as primary keys
        import json
        path = os.path.join(work_dir, "extra.jsonl")
        records = [{"id": "doc-a", "contents": "Kubernetes operator deploying the vector database"},
                   {"id": "doc-b", "contents": "Snapshot isolation for concurrent index readers"},
                   {"id": "doc-a", "contents": "Helm chart deploying the vector database on Kubernetes"}]
        with open(path, "w", encoding="utf-8") as out:
            out.writelines(json.dumps(record) + "\n" for record in records)
        doc_ids = collection.load_jsonl(path, embed, row_fields=lambda record: {"category": "loaded"})
        print("load_jsonl: ids", doc_ids)
        assert doc_ids[0] == doc_ids[2] and doc_ids[0] > 115    # doc-a twice: the second replaces the first
        hits = collection.bm25_search("helm kubernetes", 2, output_fields=["text", "name"])
        show('after load_jsonl: BM25 "helm kubernetes"', hits)
        assert hits[0]["id"] == doc_ids[0] and hits[0]["name"] == "doc-a" and hits[0]["text"].startswith("Helm")
        print("\nall checks passed")
    finally:
        if server is not None:
            server.stop()
        if args.work_dir is None:
            shutil.rmtree(work_dir, ignore_errors=True)


if __name__ == "__main__":
    main()
