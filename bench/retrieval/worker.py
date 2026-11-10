#!/usr/bin/env python3
"""One benchmark phase of one system on one prepared dataset, in a fresh process
(run by driver.py, which measures its peak memory). Prints one JSON object.

    worker.py build  SYSTEM DATA_DIR WORK_DIR   build the index of corpus.jsonl
    worker.py search SYSTEM DATA_DIR WORK_DIR   query it (after build): latencies, run file
    worker.py update SYSTEM DATA_DIR WORK_DIR   build base.jsonl, add add.jsonl, query again

Times are measured around the operation itself (not interpreter or JVM start).
"""
import argparse
import json
import os
import shutil
import statistics
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from systems import SYSTEMS, dir_size  # noqa: E402


def read_queries(data_dir, max_queries):
    queries = []
    with open(os.path.join(data_dir, "queries.tsv"), encoding="utf-8") as f:
        for line in f:
            qid, text = line.rstrip("\n").split("\t", 1)
            queries.append((qid, text.split()))
    return queries[:max_queries] if max_queries else queries


def write_run(path, name, results):
    with open(path, "w", encoding="utf-8") as f:
        for qid, hits in results:
            for rank, (doc_id, score) in enumerate(hits, 1):
                f.write("%s Q0 %s %d %.6f %s\n" % (qid, doc_id, rank, score, name))


def latencies(searcher, queries, k):
    times, results = [], []
    for qid, tokens in queries:
        start = time.perf_counter()
        hits = searcher.search(tokens, k)
        times.append(time.perf_counter() - start)
        results.append((qid, hits))
    times.sort()
    stats = {"qps": len(times) / sum(times) if sum(times) else None,
             "p50_ms": 1000 * statistics.median(times),
             "p99_ms": 1000 * times[min(len(times) - 1, int(0.99 * len(times)))],
             "mean_ms": 1000 * statistics.fmean(times)}
    return stats, results


def repeated(searcher, queries, k, repeats):
    """The queries asked repeats times: the median over the passes of each statistic, every pass's
    too, and the results of the last pass."""
    passes = []
    for _ in range(repeats):
        stats, results = latencies(searcher, queries, k)
        passes.append(stats)
    stats = {key: statistics.median(p[key] for p in passes) for key in passes[0]}
    stats["passes"] = passes
    return stats, results


def set_page_cache(index, data_dir):
    """Every search starts alike, whatever ran before: the index in the page cache, the corpus out of
    it (left there, it fills this NUMA node and pushes the index's pages to the other one).
    Returns the seconds spent reading the index."""
    for name in ("corpus.jsonl", "base.jsonl", "add.jsonl"):
        path = os.path.join(data_dir, name)
        if os.path.exists(path):
            descriptor = os.open(path, os.O_RDONLY)
            os.posix_fadvise(descriptor, 0, 0, os.POSIX_FADV_DONTNEED)
            os.close(descriptor)
    start = time.perf_counter()
    for root, _, files in os.walk(index):
        for name in files:
            with open(os.path.join(root, name), "rb", buffering=0) as f:
                while f.read(1 << 24):
                    pass
    return time.perf_counter() - start


def timed(function, *args):
    start = time.perf_counter()
    value = function(*args)
    return time.perf_counter() - start, value


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("phase", choices=["build", "search", "update"])
    parser.add_argument("system", choices=sorted(SYSTEMS))
    parser.add_argument("data_dir")
    parser.add_argument("work_dir", help="where this system's indexes and run files go")
    parser.add_argument("--threads", type=int, default=8)
    parser.add_argument("--k1", type=float, default=1.2)
    parser.add_argument("--b", type=float, default=0.75)
    parser.add_argument("--depth", type=int, default=1000, help="hits per query in the run file")
    parser.add_argument("--max-queries", type=int, default=None, help="only the first N queries")
    parser.add_argument("--warmup", type=int, default=200, help="queries of the warm-up pass")
    parser.add_argument("--repeats", type=int, default=1, help="passes over the queries per k")
    args = parser.parse_args()

    system = SYSTEMS[args.system](args.threads, args.k1, args.b)
    os.makedirs(args.work_dir, exist_ok=True)
    corpus = os.path.join(args.data_dir, "corpus.jsonl")
    full_index = os.path.join(args.work_dir, "index")
    out = {"phase": args.phase, "system": args.system, "threads": args.threads, "k1": args.k1, "b": args.b}
    system.prepare()

    if args.phase == "build":
        shutil.rmtree(full_index, ignore_errors=True)
        out["build_s"], _ = timed(system.build, corpus, full_index)
        out["index_bytes"] = dir_size(system.index_path(full_index))

    elif args.phase == "search":
        out["index_read_s"] = set_page_cache(system.search_index(full_index), args.data_dir)
        out["open_s"], searcher = timed(system.open, full_index)
        out.update(searcher.stats())
        queries = read_queries(args.data_dir, args.max_queries)
        out["queries"] = len(queries)
        latencies(searcher, queries[:args.warmup], 10)       # warm-up (JIT, page cache)
        out["k10"], _ = repeated(searcher, queries, 10, args.repeats)
        out["k100"], _ = repeated(searcher, queries, 100, args.repeats)
        out["k%d" % args.depth], results = repeated(searcher, queries, args.depth, args.repeats)
        write_run(os.path.join(args.work_dir, "run.txt"), args.system, results)

    else:
        index = os.path.join(args.work_dir, "index-update")
        shutil.rmtree(index, ignore_errors=True)
        out["base_build_s"], _ = timed(system.build, os.path.join(args.data_dir, "base.jsonl"), index)
        add = os.path.join(args.data_dir, "add.jsonl")
        with open(add, encoding="utf-8") as f:
            out["added_documents"] = sum(1 for line in f if line.strip())
        out["update_s"], _ = timed(system.update, index, add, corpus)
        out["update_mode"] = system.update_mode
        out["update_stats"] = getattr(system, "update_stats", None)
        out["index_bytes"] = dir_size(system.index_path(index))
        searcher = system.open(index)
        out.update(searcher.stats())
        # the updated index must answer like the one built at once (see driver.py's report)
        out["k10_updated"], results = latencies(searcher, read_queries(args.data_dir, args.max_queries), 10)
        write_run(os.path.join(args.work_dir, "run-update.txt"), args.system, results)
        del searcher
        shutil.rmtree(index, ignore_errors=True)

    json.dump(out, sys.stdout)
    print()


if __name__ == "__main__":
    main()
