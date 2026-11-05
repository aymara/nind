#!/usr/bin/env python3
"""Search latency of one corpus as 1, S... segments, against the single index.

Usage: segments_search.py DATA_DIR OUT_DIR [--segments 1,8,32] [--threads 1,4,8] [--k 10] [--workers 8]

DATA_DIR holds corpus.jsonl and queries.tsv (bench/retrieval/prepare.py). For each segment count,
builds OUT_DIR/s<S> (kept if already there) and prints a CSV row per thread count: build time, peak of
the search process so far, median, mean and p99 latency after a warm-up, and how many queries give the
single index's documents in the same order.
"""
import argparse
import os
import resource
import statistics
import time

from nind import _native as native
from nind._index_store import IndexStore
from nind.nind_engine import NindIndexer, PRUNING_DOCS_PER_RESULT


def read_queries(data_dir):
    queries = []
    with open(os.path.join(data_dir, "queries.tsv"), encoding="utf-8") as f:
        for line in f:
            _, _, text = line.rstrip("\n").partition("\t")
            queries.append(text.split())
    return queries


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("data_dir")
    parser.add_argument("out")
    parser.add_argument("--segments", default="1,8,32")
    parser.add_argument("--threads", default="1,4,8")
    parser.add_argument("--k", type=int, default=10)
    parser.add_argument("--workers", type=int, default=8)
    args = parser.parse_args()
    queries = read_queries(args.data_dir)
    corpus = os.path.join(args.data_dir, "corpus.jsonl")
    reference = None
    print("segments,build_s,threads,p50_ms,mean_ms,p99_ms,peak_gb,identical", flush=True)
    for segments in map(int, args.segments.split(",")):
        index_dir = os.path.join(args.out, "s%d" % segments)
        build_seconds = 0.0
        if not os.path.isdir(index_dir):
            os.makedirs(index_dir)
            start = time.perf_counter()
            NindIndexer(index_dir, local_index=False).index_jsonl(corpus, workers=args.workers, shards=segments, merge=False)
            build_seconds = time.perf_counter() - start
        store = IndexStore(index_dir, IndexStore.find_prefix(index_dir))
        index = native.SegmentedIndex(store.published_base())
        prune = args.k * PRUNING_DOCS_PER_RESULT <= index.docs_nb
        for query in queries[:200]:
            index.search(query, 1.5, 0.75, args.k, prune, 1)            # warm-up
        for threads in map(int, args.threads.split(",")):
            latencies, results = [], []
            for query in queries:
                start = time.perf_counter()
                found = index.search(query, 1.5, 0.75, args.k, prune, threads)
                latencies.append(time.perf_counter() - start)
                results.append([doc_id for doc_id, _ in found])
            if reference is None:
                reference = results
            identical = sum(1 for a, b in zip(results, reference) if a == b)
            latencies.sort()
            print("%d,%.1f,%d,%.2f,%.2f,%.2f,%.2f,%d/%d" % (
                segments, build_seconds, threads, latencies[len(latencies) // 2] * 1000,
                statistics.mean(latencies) * 1000, latencies[len(latencies) * 99 // 100] * 1000,
                resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 2**20, identical, len(queries)), flush=True)
        del index


if __name__ == "__main__":
    main()
