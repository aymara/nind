#!/usr/bin/env python3
"""Run the benchmark matrix (prepared datasets x systems x phases) and report.

    driver.py --work WORK --data DATA_DIR [DATA_DIR...] [--systems nind,lucene,...]
    driver.py --work WORK --report-only

Each phase runs in its own process (worker.py). Its peak memory is the peak,
sampled every 50 ms, of the PSS summed over its whole process tree (nind's
indexing worker processes, Anserini's JVM...); "largest process" is wait4's
ru_maxrss, the peak RSS of the largest single process. Results are appended
to WORK/results.jsonl (the last result of a dataset/system/phase wins), the
report is written to WORK/report.md and WORK/report.csv.

The quality columns (nDCG@10, R@100, RR@10) are a parity check, not a quality
claim: every system gets the same tokens (prepare.py) and the same BM25, so
they should agree, up to Lucene's and tantivy's lossy document lengths and
ties. "top10 = nind" is the mean overlap of a system's top 10 with nind's;
"update = build" the same between the index built from base + update and the
one built at once.
"""
import argparse
import csv
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from systems import SYSTEMS  # noqa: E402

DEFAULT_SYSTEMS = "nind,nind+local,lucene,lucene+pos,bm25s,tantivy"
MEASURES = ["nDCG@10", "R@100", "RR@10"]


def run_phase(python, phase, system, data_dir, work_dir, args):
    command = [python, os.path.join(HERE, "worker.py"), phase, system, data_dir, work_dir,
               "--threads", str(args.threads), "--k1", str(args.k1), "--b", str(args.b)]
    if args.max_queries:
        command += ["--max-queries", str(args.max_queries)]
    command += ["--warmup", str(args.warmup)]
    os.makedirs(work_dir, exist_ok=True)
    log_path = os.path.join(work_dir, phase + ".log")
    out_path = os.path.join(work_dir, phase + ".out")
    start = time.perf_counter()
    peak = 0
    with open(log_path, "w") as log, open(out_path, "w") as out:
        process = subprocess.Popen(command, stdout=out, stderr=log)
        while True:
            pid, status, usage = os.wait4(process.pid, os.WNOHANG)
            if pid:
                break
            peak = max(peak, tree_memory(process.pid))
            time.sleep(0.05)
    wall = time.perf_counter() - start
    result = None
    with open(out_path) as f:
        for line in f:
            if line.startswith("{"):
                result = json.loads(line)
    if os.waitstatus_to_exitcode(status) != 0 or result is None:
        result = {"phase": phase, "system": system, "error": "failed, see %s" % log_path}
    result["peak_mb"] = peak / 2 ** 20
    result["largest_process_mb"] = usage.ru_maxrss / 1024          # KiB on Linux
    result["wall_s"] = wall
    return result


def tree_memory(pid):
    """Bytes of memory of process pid and its descendants (PSS: shared pages split between them)."""
    import psutil
    try:
        root = psutil.Process(pid)
        processes = [root] + root.children(recursive=True)
    except psutil.Error:
        return 0
    total = 0
    for process in processes:
        try:
            info = process.memory_full_info()
            total += getattr(info, "pss", info.rss)
        except psutil.Error:
            pass
    return total


def latest_results(path):
    results = {}
    if os.path.exists(path):
        with open(path) as f:
            for line in f:
                r = json.loads(line)
                results[(r["dataset"], r["system"], r["phase"])] = r
    return results


def read_run(path, depth=None):
    run = {}
    if os.path.exists(path):
        with open(path) as f:
            for line in f:
                qid, _, doc_id, rank, _, _ = line.split()
                if depth is None or int(rank) <= depth:
                    run.setdefault(qid, []).append(doc_id)
    return run


def overlap(run, reference, depth=10):
    """Mean |top-depth(run) & top-depth(reference)| / depth over the reference's queries (None: no run)."""
    if not run or not reference:
        return None
    total = 0.0
    for qid, ref in reference.items():
        ref = set(ref[:depth])
        got = set(run.get(qid, [])[:depth])
        total += len(ref & got) / max(len(ref), len(got), 1)
    return total / len(reference)


def asked_queries(data_dir, max_queries):
    with open(os.path.join(data_dir, "queries.tsv"), encoding="utf-8") as f:
        qids = [line.split("\t", 1)[0] for line in f if line.strip()]
    return set(qids[:max_queries] if max_queries else qids)


def evaluate(run_path, qrels_path, queries):
    if not (os.path.exists(run_path) and os.path.exists(qrels_path)):
        return {}
    import ir_measures
    run = list(ir_measures.read_trec_run(run_path))
    # over the queries asked (with --max-queries, not all of them), with or without hits
    qrels = [qrel for qrel in ir_measures.read_trec_qrels(qrels_path) if qrel.query_id in queries]
    return {str(m): v for m, v in ir_measures.calc_aggregate([ir_measures.parse_measure(m) for m in MEASURES],
                                                               qrels, run).items()}


def fmt(value, digits=2):
    if value is None or value == "":
        return ""
    if isinstance(value, float):
        return "%.*f" % (digits, value)
    return str(value)


COLUMNS = [  # (header, key, digits)
    ("system", "system", 0), ("build s", "build_s", 2), ("build peak MB", "build_mem", 0),
    ("build largest process MB", "build_rss", 0),
    ("index MB", "index_mb", 1), ("N", "documents", 0), ("avgdl", "avgdl", 2), ("open s", "open_s", 3),
    ("k10 p50 ms", "k10_p50", 3), ("k10 p99 ms", "k10_p99", 3), ("k10 QPS", "k10_qps", 0),
    ("k1000 p50 ms", "k1000_p50", 3), ("search peak MB", "search_mem", 0),
    ("nDCG@10", "nDCG@10", 4), ("R@100", "R@100", 4), ("RR@10", "RR@10", 4), ("top10 = nind", "vs_nind", 3),
    ("update s", "update_s", 3), ("added docs", "added_documents", 0), ("update", "update_mode", 0),
    ("update peak MB", "update_mem", 0), ("update = build", "update_vs_build", 3),
]

#: the columns of WORK/report-compact.md, one table for every dataset
COMPACT = ["system", "build_s", "build_mem", "index_mb", "k10_p50", "k10_p99", "k1000_p50",
           "nDCG@10", "vs_nind", "update_s", "update_mode"]


def report(work):
    results = latest_results(os.path.join(work, "results.jsonl"))
    datasets = sorted({d for d, _, _ in results})
    rows = []
    md = ["# nind vs other indexers", ""]
    for dataset in datasets:
        meta = next((r for (d, _, _), r in results.items() if d == dataset), {})
        data_dir = meta.get("data_dir", "")
        stats = {}
        if os.path.exists(os.path.join(data_dir, "stats.json")):
            with open(os.path.join(data_dir, "stats.json")) as f:
                stats = json.load(f)
        systems = [s for s in SYSTEMS if any((dataset, s, p) in results for p in ("build", "search", "update"))]
        nind_run = read_run(os.path.join(work, dataset, "nind", "run.txt"), 10)
        md += ["## %s" % dataset, "",
               "%s documents (avgdl %.1f), %s queries, %s documents added by the update; k1=%s b=%s, %s threads"
               % (stats.get("documents", "?"), stats.get("avgdl", 0),
                  "%s of %s" % (meta["max_queries"], stats.get("queries")) if meta.get("max_queries")
                  and meta["max_queries"] < stats.get("queries", 0) else stats.get("queries", "?"),
                  stats.get("update_documents", "?"), meta.get("k1"), meta.get("b"), meta.get("threads")), ""]
        md += ["| " + " | ".join(h for h, _, _ in COLUMNS) + " |", "|" + "---|" * len(COLUMNS)]
        for system in systems:
            build = results.get((dataset, system, "build"), {})
            search = results.get((dataset, system, "search"), {})
            update = results.get((dataset, system, "update"), {})
            system_dir = os.path.join(work, dataset, system)
            row = {"dataset": dataset, "system": system,
                   "build_s": build.get("build_s"), "build_mem": build.get("peak_mb"),
                   "build_rss": build.get("largest_process_mb"),
                   "index_mb": build["index_bytes"] / 2 ** 20 if "index_bytes" in build else None,
                   "documents": search.get("documents"), "avgdl": search.get("avgdl"), "open_s": search.get("open_s"),
                   "k10_p50": search.get("k10", {}).get("p50_ms"), "k10_p99": search.get("k10", {}).get("p99_ms"),
                   "k10_qps": search.get("k10", {}).get("qps"), "k1000_p50": search.get("k1000", {}).get("p50_ms"),
                   "search_mem": search.get("peak_mb"),
                   "update_s": update.get("update_s"), "added_documents": update.get("added_documents"),
                   "update_mode": update.get("update_mode"), "update_mem": update.get("peak_mb"),
                   "errors": "; ".join(r["error"] for r in (build, search, update) if "error" in r)}
            run_path = os.path.join(system_dir, "run.txt")
            row.update(evaluate(run_path, os.path.join(data_dir, "qrels.txt"),
                                asked_queries(data_dir, search.get("max_queries"))))
            run10 = read_run(run_path, 10)
            row["vs_nind"] = overlap(run10, nind_run) if system != "nind" else None
            row["update_vs_build"] = overlap(read_run(os.path.join(system_dir, "run-update.txt"), 10), run10)
            if update and "documents" in update and search.get("documents") is not None \
                    and update["documents"] != search["documents"]:
                row["errors"] += " updated index has %s documents, not %s" % (update["documents"], search["documents"])
            rows.append(row)
            md.append("| " + " | ".join(fmt(row.get(key), digits) for _, key, digits in COLUMNS) + " |")
        errors = [(r["system"], r["errors"]) for r in rows if r["dataset"] == dataset and r["errors"]]
        md += [""] + ["- **%s**: %s" % e for e in errors] + ([""] if errors else [])
    with open(os.path.join(work, "report.md"), "w") as f:
        f.write("\n".join(md) + "\n")
    headers = {key: (header, digits) for header, key, digits in COLUMNS}
    compact = ["| dataset | " + " | ".join(headers[k][0] for k in COMPACT) + " |",
               "|---|" + "---|" * len(COMPACT)]
    for row in rows:
        compact.append("| %s | " % row["dataset"] + " | ".join(fmt(row.get(k), headers[k][1]) for k in COMPACT) + " |")
    with open(os.path.join(work, "report-compact.md"), "w") as f:
        f.write("\n".join(compact) + "\n")
    with open(os.path.join(work, "report.csv"), "w", newline="") as f:
        fields = ["dataset"] + [key for _, key, _ in COLUMNS] + ["errors"]
        writer = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)
    return "\n".join(md)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--work", required=True, help="indexes, runs, logs, results and report")
    parser.add_argument("--data", nargs="*", default=[], help="directories written by prepare.py")
    parser.add_argument("--systems", default=DEFAULT_SYSTEMS, help="comma-separated, among: " + ",".join(SYSTEMS))
    parser.add_argument("--phases", default="build,search,update")
    parser.add_argument("--threads", type=int, default=8)
    parser.add_argument("--k1", type=float, default=1.2, help="default 1.2 (tantivy's, and Lucene's default)")
    parser.add_argument("--b", type=float, default=0.75)
    parser.add_argument("--max-queries", type=int, default=None, help="only the first N queries")
    parser.add_argument("--warmup", type=int, default=200, help="queries of the warm-up pass")
    parser.add_argument("--python", default=sys.executable, help="interpreter running worker.py")
    parser.add_argument("--report-only", action="store_true")
    args = parser.parse_args()

    os.makedirs(args.work, exist_ok=True)
    if not args.report_only:
        systems = [s for s in args.systems.split(",") if s]
        for name in systems:
            if name not in SYSTEMS:
                parser.error("unknown system %r" % name)
        with open(os.path.join(args.work, "results.jsonl"), "a") as results:
            for data_dir in args.data:
                dataset = os.path.basename(os.path.normpath(data_dir))
                for system in systems:
                    for phase in args.phases.split(","):
                        print("%s / %s / %s ..." % (dataset, system, phase), end=" ", flush=True)
                        result = run_phase(args.python, phase, system, os.path.abspath(data_dir),
                                           os.path.join(args.work, dataset, system), args)
                        result.update(dataset=dataset, data_dir=os.path.abspath(data_dir),
                                      threads=args.threads, k1=args.k1, b=args.b,
                                      max_queries=args.max_queries)
                        results.write(json.dumps(result) + "\n")
                        results.flush()
                        print(result.get("error", "%.1f s" % result["wall_s"]))
    print(report(args.work))


if __name__ == "__main__":
    main()
