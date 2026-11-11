#!/usr/bin/env python3
"""Search throughput (queries per second) and latency against the number of callers searching at
once: threads sharing one searcher, or processes each opening the index.

    concurrency.py INDEX_DIR QUERIES_TSV [--system nind] [--mode threads|processes]
                   [--callers 1 2 4 8 16] [--queries N] [--warmup N] [--k 10]

INDEX_DIR: a system's index as worker.py builds it (WORK/<dataset>/<system>/index); QUERIES_TSV:
prepare.py's queries.tsv. Each caller sends its next query when the last one returns. Every caller
count runs the same queries, after a warm-up (each process its own); prints one JSON line per count:
queries/s, the median and 99th percentile latency, and the peak memory of every process of the
run (PSS: pages shared between processes, e.g. a mapped file, counted once; private: their own;
anonymous: not a file's, what compares systems that map their index and those that read it).
Threads need the system to release the GIL while it searches (nind, Lucene through pyjnius and
tantivy-py do; measured: 4 threads, 4 times the queries/s).
"""
import argparse
import json
import multiprocessing
import os
import statistics
import threading
import time
from concurrent.futures import ThreadPoolExecutor

from systems import SYSTEMS


def read_queries(path, count):
    with open(path, encoding="utf-8") as f:
        return [line.rstrip("\n").split("\t", 1)[1].split() for line in f if line.strip()][:count]


def open_searcher(system_name, index_dir, k1, b):
    system = SYSTEMS[system_name](1, k1, b)
    system.prepare()
    return system.open(system.search_index(index_dir))


def timed(searcher, query, k):
    start = time.perf_counter()
    searcher.search(query, k)
    return time.perf_counter() - start


class MemorySampler(threading.Thread):
    """The peak total PSS, private and anonymous memory of this process and its descendants (a file
    a system maps, as Lucene and tantivy their index, counts in PSS, not in anonymous memory)."""

    def __init__(self):
        super().__init__(daemon=True)
        self.pss = self.private = self.anonymous = 0
        self.done = False

    @staticmethod
    def _tree(pid):
        pids = [pid]
        for p in pids:
            try:
                with open("/proc/%d/task/%d/children" % (p, p)) as f:
                    pids.extend(int(c) for c in f.read().split())
            except OSError:
                pass
        return pids

    @staticmethod
    def _rollup(pid):
        pss = private = anonymous = 0
        try:
            with open("/proc/%d/smaps_rollup" % pid) as f:
                for line in f:
                    if line.startswith("Pss:"):
                        pss = int(line.split()[1]) * 1024
                    elif line.startswith(("Private_Clean:", "Private_Dirty:")):
                        private += int(line.split()[1]) * 1024
                    elif line.startswith("Anonymous:"):
                        anonymous = int(line.split()[1]) * 1024
        except OSError:
            pass
        return pss, private, anonymous

    def run(self):
        me = os.getpid()
        while not self.done:
            sizes = [self._rollup(p) for p in self._tree(me)]
            self.pss = max(self.pss, sum(s[0] for s in sizes))
            self.private = max(self.private, sum(s[1] for s in sizes))
            self.anonymous = max(self.anonymous, sum(s[2] for s in sizes))
            time.sleep(0.2)


def process_caller(system_name, index_dir, k1, b, k, queries, warmup, ready, start, results):
    # Runs in its own process: opens the index, warms up, waits for the others, then searches.
    searcher = open_searcher(system_name, index_dir, k1, b)
    for query in warmup:
        searcher.search(query, k)
    ready.wait()
    start.wait()
    began = time.perf_counter()
    latencies = [timed(searcher, query, k) for query in queries]
    results.put((began, time.perf_counter(), latencies))


def run_threads(searcher, queries, callers, k):
    with ThreadPoolExecutor(max_workers=callers) as pool:
        start = time.perf_counter()
        latencies = list(pool.map(lambda query: timed(searcher, query, k), queries))
        return time.perf_counter() - start, latencies


def run_processes(args, queries, callers):
    context = multiprocessing.get_context("spawn")      # each opens the index, as a server would
    ready, start = context.Barrier(callers + 1), context.Barrier(callers + 1)
    results = context.Queue()
    processes = [context.Process(target=process_caller,
                                 args=(args.system, args.index_dir, args.k1, args.b, args.k, queries[c::callers],
                                       queries[:args.warmup], ready, start, results))
                 for c in range(callers)]
    for process in processes:
        process.start()
    ready.wait()
    start.wait()
    parts = [results.get() for _ in processes]
    for process in processes:
        process.join()
    elapsed = max(part[1] for part in parts) - min(part[0] for part in parts)
    return elapsed, [latency for part in parts for latency in part[2]]


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("index_dir")
    parser.add_argument("queries")
    parser.add_argument("--system", default="nind", choices=sorted(SYSTEMS))
    parser.add_argument("--mode", default="threads", choices=["threads", "processes"])
    parser.add_argument("--callers", "--threads", nargs="*", type=int, default=[1, 2, 4, 8, 16])
    parser.add_argument("--queries", dest="max_queries", type=int, default=2000)
    parser.add_argument("--warmup", type=int, default=200)
    parser.add_argument("--k", type=int, default=10)
    parser.add_argument("--k1", type=float, default=1.2)
    parser.add_argument("--b", type=float, default=0.75)
    args = parser.parse_args()

    queries = read_queries(args.queries, args.max_queries)
    searcher = None
    if args.mode == "threads":
        searcher = open_searcher(args.system, args.index_dir, args.k1, args.b)
        for query in queries[:args.warmup]:
            searcher.search(query, args.k)
    for callers in args.callers:
        sampler = MemorySampler()
        sampler.start()
        if args.mode == "threads":
            elapsed, latencies = run_threads(searcher, queries, callers, args.k)
        else:
            elapsed, latencies = run_processes(args, queries, callers)
        sampler.done = True
        sampler.join()
        latencies.sort()
        print(json.dumps({"system": args.system, "mode": args.mode, "callers": callers, "queries": len(latencies),
                          "k": args.k, "qps": len(latencies) / elapsed,
                          "p50_ms": 1000 * statistics.median(latencies),
                          "p99_ms": 1000 * latencies[int(0.99 * (len(latencies) - 1))],
                          "pss_gb": sampler.pss / 2 ** 30, "private_gb": sampler.private / 2 ** 30,
                          "anonymous_gb": sampler.anonymous / 2 ** 30,
                          "index": args.index_dir}), flush=True)


if __name__ == "__main__":
    main()
