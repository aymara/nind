#!/usr/bin/env python3
"""Fit each metric's growth with the corpus size and extrapolate it.

    extrapolate.py WORK DATASET [DATASET...] [--to 1e8 1e9 1e10]

DATASET: prefixes of one corpus (subset.py) and the corpus itself, e.g.
msmarco-stop-1M msmarco-stop-2M msmarco-stop-4M msmarco-stop. For every system
and metric, fits y = a N^p on the measured points (least squares in log-log)
and prints the exponent p and the values extrapolated to --to documents.
Extrapolations far past the measured range are orders of magnitude, not
predictions: they assume the corpus keeps the same statistics, ignore format
limits (see docs/proposals/format-v2-blocks.md), memory hierarchy changes and
I/O, and per-query latency also depends on the queries.
"""
import argparse
import json
import math
import os

METRICS = [("build_s", "build s", lambda b, s, u: b.get("build_s")),
           ("build_mem", "build peak GB", lambda b, s, u: b.get("peak_mb", 0) / 1024 or None),
           ("index", "index GB", lambda b, s, u: b.get("index_bytes", 0) / 2 ** 30 or None),
           ("k10", "k10 p50 ms", lambda b, s, u: s.get("k10", {}).get("p50_ms")),
           ("search_mem", "search peak GB", lambda b, s, u: s.get("peak_mb", 0) / 1024 or None),
           ("update_s", "update s (1%)", lambda b, s, u: u.get("update_s"))]


def fit(points):
    """(a, p) of y = a x^p, least squares on logs."""
    xs = [math.log(x) for x, _ in points]
    ys = [math.log(y) for _, y in points]
    mx, my = sum(xs) / len(xs), sum(ys) / len(ys)
    sxx = sum((x - mx) ** 2 for x in xs)
    p = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx
    return math.exp(my - p * mx), p


def human(value):
    if value is None:
        return ""
    for limit, fmt in ((1e5, "%.3g"), (1e8, "%.3g")):
        if value < limit:
            return fmt % value
    return "%.2e" % value


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("work")
    parser.add_argument("datasets", nargs="+")
    parser.add_argument("--to", nargs="*", type=float, default=[1e8, 1e9, 1e10])
    args = parser.parse_args()

    results = {}
    with open(os.path.join(args.work, "results.jsonl")) as f:
        for line in f:
            r = json.loads(line)
            results[(r["dataset"], r["system"], r["phase"])] = r
    sizes = {}
    for dataset in args.datasets:
        r = next((r for (d, _, _), r in results.items() if d == dataset), None)
        with open(os.path.join(r["data_dir"], "stats.json")) as f:
            sizes[dataset] = json.load(f)["documents"]
    systems = sorted({s for (d, s, _) in results if d in sizes},
                     key=lambda s: ["nind", "nind+local", "lucene", "lucene+pos", "bm25s", "tantivy"].index(s))

    targets = ["%gM" % (n / 1e6) if n < 1e9 else "%gG" % (n / 1e9) for n in args.to]
    print("| system | metric | " + " | ".join("%.1fM" % (sizes[d] / 1e6) for d in args.datasets)
          + " | exponent p | " + " | ".join(targets) + " |")
    print("|---" * (3 + len(args.datasets) + len(targets)) + "|")
    for system in systems:
        for key, label, get in METRICS:
            values = []
            for dataset in args.datasets:
                b, s, u = (results.get((dataset, system, phase), {}) for phase in ("build", "search", "update"))
                values.append(get(b, s, u))
            points = [(sizes[d], v) for d, v in zip(args.datasets, values) if v]
            if len(points) < 3:
                continue
            a, p = fit(points)
            print("| %s | %s | %s | %.2f | %s |" % (system, label, " | ".join(human(v) for v in values), p,
                                                    " | ".join(human(a * n ** p) for n in args.to)))


if __name__ == "__main__":
    main()
