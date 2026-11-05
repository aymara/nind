#!/usr/bin/env python3
"""Direct vs sharded index_jsonl on one corpus: time, memory, size, same results.

Usage: sharded_build.py CORPUS.jsonl OUT_DIR [--shards 1,4,8,16] [--workers 8] [--queries N]
                        [--no-local-index]

For each shard count (1 = direct build), builds OUT_DIR/s<N> and prints one CSV row. Memory is the
PSS summed over the process tree, less what the script held before the build, sampled every 0.2 s:
`peak` over the whole build, `merge` once the workers are gone (the parent alone: the merge). Every
index built after the first one is compared with it on queries made from the corpus's own lines:
their top 10 must be identical, and it must pass verify_index. Each index is removed after its row
but the first one.
"""
import argparse
import gc
import json
import os
import random
import shutil
import threading
import time

from nind import _native as native
from nind.nind_engine import NindEngine, NindIndexer


def memory_of(pid):
    # PSS, not RSS: forked workers map this process's pages, which RSS counts once per worker
    with open("/proc/%d/smaps_rollup" % pid) as f:
        for line in f:
            if line.startswith("Pss:"):
                return int(line.split()[1]) * 1024
    return 0


def children_of(pid):
    result = []
    try:
        for tid in os.listdir("/proc/%d/task" % pid):
            with open("/proc/%d/task/%s/children" % (pid, tid)) as f:
                result.extend(int(child) for child in f.read().split())
    except OSError:
        pass
    return result


class MemorySampler(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.peak = self.parent_alone = 0
        self.baseline = memory_of(os.getpid())      # what this script already holds
        self.stopping = threading.Event()

    def run(self):
        me = os.getpid()
        while not self.stopping.wait(0.2):
            total, pending, seen = 0, [me], 0
            while pending:
                pid = pending.pop()
                seen += 1
                try:
                    total += memory_of(pid)
                except OSError:
                    continue
                pending.extend(children_of(pid))
            self.peak = max(self.peak, total - self.baseline)
            if seen == 1:
                self.parent_alone = max(self.parent_alone, total - self.baseline)


def queries_from(path, count):
    # words of lines spread over the corpus: 2-4 of a line's words
    rng = random.Random(42)
    size = os.path.getsize(path)
    queries = []
    with open(path, "rb") as f:
        while len(queries) < count:
            f.seek(rng.randrange(size))
            f.readline()
            line = f.readline()
            if not line:
                continue
            words = json.loads(line)["contents"].split()
            if len(words) >= 4:
                queries.append(" ".join(rng.sample(words, rng.randint(2, 4))))
    return queries


def directory_size(path):
    return sum(os.path.getsize(os.path.join(root, name)) for root, _, names in os.walk(path) for name in names)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("corpus")
    parser.add_argument("out")
    parser.add_argument("--shards", default="1,4,8,16")
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument("--queries", type=int, default=2000)
    parser.add_argument("--no-local-index", action="store_true")
    args = parser.parse_args()
    queries = queries_from(args.corpus, args.queries)
    reference = None
    print("shards,workers,seconds,peak_gb,merge_gb,index_gb,documents,top10_identical", flush=True)
    for shards in map(int, args.shards.split(",")):
        index_dir = os.path.join(args.out, "s%d" % shards)
        os.makedirs(index_dir)
        sampler = MemorySampler()
        sampler.start()
        start = time.perf_counter()
        indexer = NindIndexer(index_dir, local_index=False if args.no_local_index else None)
        indexer.index_jsonl(args.corpus, workers=args.workers, shards=shards)
        seconds = time.perf_counter() - start
        sampler.stopping.set()
        sampler.join()
        engine = NindEngine(index_dir)
        results = [engine.search(query, top_k=10, doc_names=True) for query in queries]
        if reference is None:
            reference = results
            identical = "reference"
        else:
            # the same documents in the same order; scores to rounding
            identical = sum(1 for got, expected in zip(results, reference)
                            if [name for name, _ in got] == [name for name, _ in expected])
            identical = "%d/%d" % (identical, len(queries))
            report = native.verify_index(engine._snapshot.base, cross_check=not args.no_local_index)
            if not report.ok:
                identical += " VERIFY FAILED: %s" % report.problems[:3]
        print("%d,%d,%.1f,%.2f,%.2f,%.2f,%d,%s" % (
            shards, args.workers, seconds, sampler.peak / 2**30, sampler.parent_alone / 2**30,
            directory_size(index_dir) / 2**30, engine.total_docs, identical), flush=True)
        del engine
        gc.collect()
        if reference is not results:
            shutil.rmtree(index_dir)


if __name__ == "__main__":
    main()
