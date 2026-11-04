#!/usr/bin/env python3
"""Search throughput (queries per second) against the number of threads sharing one NindEngine.

    concurrency.py INDEX_DIR QUERIES_TSV [--threads 1 2 4 8 16] [--queries N] [--k 10]

INDEX_DIR: an index written by NindIndexer (e.g. a worker.py build's WORK/<dataset>/nind/index);
QUERIES_TSV: prepare.py's queries.tsv. Every thread count runs the same queries (each thread
takes the next one), after a warm-up pass; prints one JSON line per thread count.
"""
import argparse
import json
import statistics
import threading
import time
from concurrent.futures import ThreadPoolExecutor

from nind.nind_engine import NindEngine


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("index_dir")
    parser.add_argument("queries")
    parser.add_argument("--threads", nargs="*", type=int, default=[1, 2, 4, 8, 16])
    parser.add_argument("--queries", dest="max_queries", type=int, default=2000)
    parser.add_argument("--k", type=int, default=10)
    parser.add_argument("--k1", type=float, default=1.2)
    parser.add_argument("--b", type=float, default=0.75)
    args = parser.parse_args()

    with open(args.queries, encoding="utf-8") as f:
        queries = [line.rstrip("\n").split("\t", 1)[1] for line in f if line.strip()][:args.max_queries]
    engine = NindEngine(args.index_dir)

    def run(query):
        start = time.perf_counter()
        engine.search(query, top_k=args.k, k1=args.k1, b=args.b)
        return time.perf_counter() - start

    for query in queries[:200]:          # warm-up
        run(query)
    for threads in args.threads:
        with ThreadPoolExecutor(max_workers=threads) as pool:
            start = time.perf_counter()
            latencies = list(pool.map(run, queries))
            elapsed = time.perf_counter() - start
        latencies.sort()
        print(json.dumps({"threads": threads, "queries": len(queries), "qps": len(queries) / elapsed,
                          "p50_ms": 1000 * statistics.median(latencies),
                          "p99_ms": 1000 * latencies[int(0.99 * (len(latencies) - 1))]}), flush=True)


if __name__ == "__main__":
    main()
