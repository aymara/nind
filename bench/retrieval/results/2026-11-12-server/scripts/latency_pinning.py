"""One process's single-thread top-10 latency: latency_pinning.py INDEX QUERIES LABEL

200 warm-up queries, then 3 passes over every query; prints one JSON line: each pass's median and
mean, the process's CPU migrations (/proc/self/sched) and the CPUs it may run on."""
import json, os, statistics, sys, time
from nind.nind_engine import NindEngine


def migrations():
    try:
        for line in open("/proc/self/sched"):
            if line.startswith("se.nr_migrations"):
                return int(line.split()[-1])
    except OSError:
        pass
    return None


index, queries_path, label = sys.argv[1:4]
queries = [l.rstrip("\n").split("\t", 1)[1] for l in open(queries_path) if l.strip()]
start = time.perf_counter()
engine = NindEngine(index)
open_s = time.perf_counter() - start
for query in queries[:200]:
    engine.search(query, top_k=10, k1=1.2, b=0.75)
before = migrations()
passes = []
for _ in range(3):
    times = []
    for query in queries:
        t = time.perf_counter()
        engine.search(query, top_k=10, k1=1.2, b=0.75)
        times.append(time.perf_counter() - t)
    passes.append({"p50_ms": 1000 * statistics.median(times), "mean_ms": 1000 * statistics.fmean(times)})
after = migrations()
print(json.dumps({"label": label, "cpus": sorted(os.sched_getaffinity(0)), "open_s": open_s,
                  "queries": len(queries), "passes": passes,
                  "migrations": None if before is None else after - before}), flush=True)
