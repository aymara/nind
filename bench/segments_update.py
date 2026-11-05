#!/usr/bin/env python3
"""What updates cost on an index of segments, and whether it still answers as one index.

Usage: segments_update.py DATA_DIR OUT_DIR [--shards 16] [--workers 8] [--queries 2000] [--local-index]

DATA_DIR holds base.jsonl, add.jsonl (the last documents of corpus.jsonl), corpus.jsonl and
queries.tsv (bench/retrieval/prepare.py). Builds base.jsonl in shards, adds add.jsonl in one
update, compares top 10s with a direct build of corpus.jsonl, then times small updates (each
publishes a generation, with its fsyncs), removals, and a full merge. Each step: seconds, the
process's peak memory so far (GB).
"""
import argparse
import json
import os
import resource
import time

from nind import _native as native
from nind.nind_engine import NindEngine, NindIndexer


def step(name, start, extra=""):
    print("%-34s %8.2f s  peak %6.2f GB  %s" % (name, time.perf_counter() - start,
                                                 resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 2**20, extra), flush=True)


def read_queries(data_dir, count):
    queries = []
    with open(os.path.join(data_dir, "queries.tsv"), encoding="utf-8") as f:
        for line in f:
            queries.append(line.rstrip("\n").partition("\t")[2])
    return queries[:count]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("data_dir")
    parser.add_argument("out")
    parser.add_argument("--shards", type=int, default=16)
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument("--queries", type=int, default=2000)
    parser.add_argument("--local-index", action="store_true")
    args = parser.parse_args()
    local = args.local_index
    index_dir, direct_dir = os.path.join(args.out, "updated"), os.path.join(args.out, "direct")
    os.makedirs(index_dir)
    os.makedirs(direct_dir)
    queries = read_queries(args.data_dir, args.queries)

    start = time.perf_counter()
    indexer = NindIndexer(index_dir, local_index=local)
    indexer.index_jsonl(os.path.join(args.data_dir, "base.jsonl"), workers=args.workers, shards=args.shards)
    step("build base, %d shards" % args.shards, start)
    start = time.perf_counter()
    NindIndexer(direct_dir, local_index=local).index_jsonl(os.path.join(args.data_dir, "corpus.jsonl"), workers=args.workers)
    step("direct build of the whole corpus", start)
    direct = NindEngine(direct_dir)
    expected = [[name for name, _ in direct.search(query, top_k=10, doc_names=True)] for query in queries]
    del direct

    with open(os.path.join(args.data_dir, "add.jsonl"), encoding="utf-8") as f:
        records = [json.loads(line) for line in f]
    start = time.perf_counter()
    with indexer.update() as update:
        for record in records:
            update.add_text(record["contents"], name=str(record["id"]))
    step("update: +%d documents" % len(records), start,
         "segments %d, merges %d" % (update.stats.segments_nb, update.stats.merges_nb))
    start = time.perf_counter()
    engine = NindEngine(index_dir)
    step("open the updated index", start)
    start = time.perf_counter()
    found = [[name for name, _ in engine.search(query, top_k=10, doc_names=True)] for query in queries]
    step("%d searches" % len(queries), start)
    print("top 10 identical to the direct build: %d/%d" % (sum(1 for a, b in zip(found, expected) if a == b), len(queries)), flush=True)

    for documents in (1, 1000):
        start = time.perf_counter()
        with indexer.update() as update:
            for i in range(documents):
                update.add_text("tiny update number %d" % i)
        step("update: +%d" % documents, start, "segments %d, merges %d" % (update.stats.segments_nb, update.stats.merges_nb))
    start = time.perf_counter()
    engine.search("tiny")
    step("reader switches to it", start)
    for documents in (1, 100):
        start = time.perf_counter()
        with indexer.update() as update:
            for doc_id in range(documents):
                update.remove(1000 + doc_id)
        step("update: -%d" % documents, start, "segments %d, removed %d" % (update.stats.segments_nb, update.stats.removed_docs_nb))
    start = time.perf_counter()
    stats = indexer.force_merge()
    step("force_merge", start, "%d documents rewritten" % stats.merged_docs_nb)
    print("verify:", native.verify_index(NindEngine(index_dir)._snapshot.base), flush=True)


if __name__ == "__main__":
    main()
