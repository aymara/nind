#!/usr/bin/env python3
"""Latency of one query at a time against the threads it uses (NindEngine.search(threads=t)).
    search_threads.py INDEX_DIR QUERIES_TSV T [--k 10]
One JSON line: after a warm-up pass over every query, one measured pass."""
import argparse, json, statistics, time
from nind.nind_engine import NindEngine
p = argparse.ArgumentParser()
p.add_argument("index_dir"); p.add_argument("queries"); p.add_argument("threads", type=int)
p.add_argument("--k", type=int, default=10)
a = p.parse_args()
with open(a.queries, encoding="utf-8") as f:
    queries = [line.rstrip("\n").split("\t", 1)[1] for line in f if line.strip()]
engine = NindEngine(a.index_dir)
def run():
    times = []
    for q in queries:
        s = time.perf_counter(); engine.search(q, top_k=a.k, k1=1.2, b=0.75, threads=a.threads)
        times.append(time.perf_counter() - s)
    return sorted(times)
run()
t = run()
print(json.dumps({"index": a.index_dir, "threads": a.threads, "k": a.k, "queries": len(t),
                  "p50_ms": 1000 * statistics.median(t), "p99_ms": 1000 * t[int(0.99 * (len(t) - 1))],
                  "mean_ms": 1000 * statistics.fmean(t)}), flush=True)
