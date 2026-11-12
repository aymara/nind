"""Milvus's built-in BM25 vs nind's, on a BEIR-style corpus: effectiveness, agreement, speed.

    uv run python examples/milvus/compare.py DATA_DIR [--uri http://127.0.0.1:19530] [--limit N]

DATA_DIR holds ``corpus.jsonl`` (``{"id", "contents"}`` per line), ``queries.tsv``
(``id<TAB>text``) and optionally ``qrels.txt`` (``query 0 doc relevance``), as
``bench/retrieval/prepare.py`` writes them. The corpus goes into two collections
of the same full text search schema, one with Milvus's BM25 (``MilvusClient``), one
with nind's (``NindMilvusClient``); every query is searched in both (top 10, one at
a time), then: nDCG@10 (with qrels), how often both return the same top 10,
latencies, and the insertion times.
"""

import argparse
import json
import math
import os
import shutil
import statistics
import sys
import tempfile
import time

from pymilvus import DataType, Function, FunctionType, MilvusClient

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nind_milvus import NindMilvusClient  # noqa: E402


def read_corpus(path, limit):
    texts, names = [], []
    with open(path, encoding="utf-8") as lines:
        for line in lines:
            if limit and len(texts) == limit:
                break
            record = json.loads(line)
            names.append(str(record["id"]))
            texts.append(record["contents"])
    return texts, names


def read_queries(path, limit):
    queries = []
    with open(path, encoding="utf-8") as lines:
        for line in lines:
            query_id, _, text = line.rstrip("\n").partition("\t")
            queries.append((query_id, text))
    return queries[:limit] if limit else queries


def read_qrels(path):
    qrels = {}
    if os.path.exists(path):
        with open(path) as lines:
            for line in lines:
                query_id, _, doc, relevance = line.split()
                qrels.setdefault(query_id, {})[doc] = int(relevance)
    return qrels


def ndcg10(ranked_names, judged):
    dcg = sum(judged.get(name, 0) / math.log2(rank + 2) for rank, name in enumerate(ranked_names[:10]))
    ideal = sorted(judged.values(), reverse=True)[:10]
    idcg = sum(relevance / math.log2(rank + 2) for rank, relevance in enumerate(ideal))
    return dcg / idcg if idcg > 0 else 0.0


def create(client, name):
    if client.has_collection(name):
        client.drop_collection(name)
    schema = client.create_schema(auto_id=False)
    schema.add_field("id", DataType.INT64, is_primary=True)
    schema.add_field("text", DataType.VARCHAR, max_length=65535, enable_analyzer=True)
    schema.add_field("sparse", DataType.SPARSE_FLOAT_VECTOR)
    schema.add_function(Function(name="text_bm25", function_type=FunctionType.BM25,
                                 input_field_names=["text"], output_field_names=["sparse"]))
    index_params = client.prepare_index_params()
    index_params.add_index(field_name="sparse", index_type="SPARSE_INVERTED_INDEX", metric_type="BM25",
                           params={"inverted_index_algo": "DAAT_MAXSCORE", "bm25_k1": 1.2, "bm25_b": 0.75})
    client.create_collection(name, schema=schema, index_params=index_params)


def load(client, name, texts, batch, nind_batch_docs):
    """Insert the corpus; the seconds until every row is inserted, then until flushed."""
    start = time.perf_counter()
    is_nind = isinstance(client, NindMilvusClient)
    for first in range(0, len(texts), nind_batch_docs):
        # (nind: one update, one published segment, per nind_batch_docs rows)
        with client.bm25_batch() if is_nind else _nothing():
            for begin in range(first, min(first + nind_batch_docs, len(texts)), batch):
                rows = [{"id": i, "text": texts[i]} for i in range(begin, min(begin + batch, len(texts)))]
                client.insert(name, rows)
    inserted = time.perf_counter() - start
    client.flush(name)
    # ready: Milvus's index of the sparse field built over every row (nind's: a placeholder of nulls)
    while True:
        index = client.describe_index(name, "sparse")
        if index.get("pending_index_rows", 0) == 0 and index.get("indexed_rows", 0) >= len(texts):
            break
        time.sleep(0.5)
    return inserted, time.perf_counter() - start


class _nothing:
    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


def run_queries(client, name, queries, limit=10, output_fields=None):
    results, latencies = [], []
    for _, text in queries[:20]:        # warm up
        client.search(name, data=[text], anns_field="sparse", limit=limit, output_fields=output_fields)
    for _, text in queries:
        start = time.perf_counter()
        hits = client.search(name, data=[text], anns_field="sparse", limit=limit, output_fields=output_fields)[0]
        latencies.append(time.perf_counter() - start)
        results.append([(hit.id, hit.distance) for hit in hits])
    return results, latencies


def percentile(values, p):
    values = sorted(values)
    return values[min(len(values) - 1, int(p / 100 * len(values)))]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("data_dir")
    parser.add_argument("--uri", default="http://127.0.0.1:19530")
    parser.add_argument("--limit", type=int, help="first N documents only")
    parser.add_argument("--queries", type=int, help="first N queries only")
    parser.add_argument("--batch", type=int, default=2000, help="rows per insert call")
    parser.add_argument("--nind-batch-docs", type=int, default=200000, help="rows per nind update")
    parser.add_argument("--keep", action="store_true", help="keep the collections")
    args = parser.parse_args()

    texts, names = read_corpus(os.path.join(args.data_dir, "corpus.jsonl"), args.limit)
    queries = read_queries(os.path.join(args.data_dir, "queries.tsv"), args.queries)
    qrels = read_qrels(os.path.join(args.data_dir, "qrels.txt")) if not args.limit else {}
    print("%s: %d documents, %d queries%s" % (args.data_dir, len(texts), len(queries),
                                              ", qrels" if qrels else ""))
    nind_dir = tempfile.mkdtemp(prefix="nind-milvus-compare-")
    milvus = MilvusClient(uri=args.uri)
    nind = NindMilvusClient(uri=args.uri, nind_dir=nind_dir)
    report = {}
    try:
        for label, client, name in (("Milvus BM25", milvus, "cmp_builtin"), ("nind", nind, "cmp_nind")):
            create(client, name)
            inserted, flushed = load(client, name, texts, args.batch, args.nind_batch_docs)
            client.load_collection(name)
            time.sleep(5)       # the default (Bounded) consistency: rows visible within a few seconds
            results, latencies = run_queries(client, name, queries)
            _, with_text = run_queries(client, name, queries, output_fields=["text"])
            report[label] = {"inserted": inserted, "ready": flushed, "results": results, "latencies": latencies,
                             "with_text": with_text}
            print("  %-12s inserted in %.1f s, indexed (ready) after %.1f s, %d queries searched"
                  % (label, inserted, flushed, len(queries)))
        # nind alone: its engine, no Milvus round trip
        field = nind._fulltext("cmp_nind").fields["sparse"]
        direct = []
        for _, text in queries:
            start = time.perf_counter()
            field.engine.search(field.prepare(text), top_k=10, k1=field.k1, b=field.b)
            direct.append(time.perf_counter() - start)

        builtin, replaced = report["Milvus BM25"]["results"], report["nind"]["results"]
        same_ranking = sum([pk for pk, _ in a] == [pk for pk, _ in b] for a, b in zip(builtin, replaced))
        same_set = sum({pk for pk, _ in a} == {pk for pk, _ in b} for a, b in zip(builtin, replaced))
        overlap = statistics.mean(len({pk for pk, _ in a} & {pk for pk, _ in b}) / len(a) if a else float(not b)
                                  for a, b in zip(builtin, replaced))
        top_score = [abs(a[0][1] - b[0][1]) / a[0][1] for a, b in zip(builtin, replaced) if a and b]
        print("\nagreement (top 10): same ranking %.1f%%, same set %.1f%%, mean overlap %.3f; "
              "top score relative difference: median %.2g, max %.2g"
              % (100 * same_ranking / len(queries), 100 * same_set / len(queries), overlap,
                 statistics.median(top_score), max(top_score)))
        print("\n%-24s %10s %10s %10s %10s %10s" % ("", "nDCG@10", "p50 ms", "p95 ms", "mean ms", "ready s"))
        rows = [(label, report[label]["results"], report[label]["latencies"], report[label]["ready"])
                for label in report]
        rows += [(label + " +text", None, report[label]["with_text"], None) for label in report]
        rows += [("nind engine (no Milvus)", None, direct, None)]
        for label, results, latencies, inserted in rows:
            ndcg = "-"
            if qrels and results is not None:
                judged = [(query_id, ranked) for (query_id, _), ranked in zip(queries, results) if query_id in qrels]
                ndcg = "%.4f" % statistics.mean(ndcg10([names[pk] for pk, _ in ranked], qrels[query_id])
                                                for query_id, ranked in judged)
            print("%-24s %10s %10.2f %10.2f %10.2f %10s" % (
                label, ndcg, 1000 * percentile(latencies, 50), 1000 * percentile(latencies, 95),
                1000 * statistics.mean(latencies), "-" if inserted is None else "%.1f" % inserted))
    finally:
        if not args.keep:
            for client, name in ((milvus, "cmp_builtin"), (nind, "cmp_nind")):
                if client.has_collection(name):
                    client.drop_collection(name)
        shutil.rmtree(nind_dir, ignore_errors=True)


if __name__ == "__main__":
    main()
