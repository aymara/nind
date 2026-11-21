#!/usr/bin/env python3
"""verify_index on every segment of an index, then: time, peak memory, what it reports.
    verify.py INDEX_DIR [PREFIX]"""
import json, resource, sys, time
from nind import _native as native
from nind._index_store import IndexStore
index_dir = sys.argv[1]
prefix = sys.argv[2] if len(sys.argv) > 2 else "corpus"
base = IndexStore(index_dir, prefix).published_base()
segments = [s.base for s in native.SegmentedIndex(base).segments()]
for segment in segments:
    start = time.perf_counter()
    report = native.verify_index(segment)
    fields = {n: getattr(report, n) for n in dir(report) if not n.startswith("_") and not callable(getattr(report, n))}
    print(json.dumps({"segment": segment, "seconds": round(time.perf_counter() - start, 1),
                      "peak_gb": round(resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 2**20, 2),
                      **{k: (v if isinstance(v, (int, float, bool, str)) else str(v)[:300]) for k, v in fields.items()}}), flush=True)
