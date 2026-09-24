#!/usr/bin/env python3
"""Term/document lookup latency benchmark for a nind index, driven through the
same Python reader classes real callers use (NindLexiconindex/NindTermindex/
NindLocalindex), whose hot-path lookups delegate to nind._native.

Terms and document ids to probe are sampled from the corpus text file that
was fed to Nind_indexeCorpus (produced by msmarco/convert_to_nind.py), rather
than by introspecting the lexicon file itself, since that keeps this script
independent of the diagnostic-only (hand-rolled) lexicon methods.

usage: lookup_latency_bench.py <docs.txt> <index-base> <size-label> [--samples N] [--warmup N] [--csv PATH]
  <index-base> is the .nind* file path without extension, e.g. .../docs
"""
import argparse
import random
import statistics
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src" / "py" / "nind"))

from NindLexiconindex import NindLexiconindex
from NindTermindex import NindTermindex
from NindLocalindex import NindLocalindex


def parse_docs_file(docs_path):
    doc_ids = []
    terms = set()
    with open(docs_path, "r", encoding="utf-8") as f:
        for line in f:
            fields = line.split()
            if not fields:
                continue
            doc_ids.append(int(fields[0]))
            # fields alternate word, "pos,len", word, "pos,len", ...
            for tok in fields[1::2]:
                terms.add(tok)
    return doc_ids, list(terms)


def percentile(sorted_values, pct):
    if not sorted_values:
        return 0.0
    k = (len(sorted_values) - 1) * pct
    f = int(k)
    c = min(f + 1, len(sorted_values) - 1)
    if f == c:
        return sorted_values[f]
    return sorted_values[f] + (sorted_values[c] - sorted_values[f]) * (k - f)


def time_calls(fn, args_list, warmup=0):
    # untimed passes first, so the numbers reflect steady-state lookup cost
    # rather than first-touch page-cache misses on a freshly opened index
    for _ in range(warmup):
        for args in args_list:
            fn(*args)
    latencies = []
    for args in args_list:
        start = time.perf_counter()
        fn(*args)
        latencies.append(time.perf_counter() - start)
    return latencies


def summarize(latencies):
    if not latencies:
        return dict(mean_us=0.0, median_us=0.0, p95_us=0.0, p99_us=0.0, throughput_per_sec=0.0)
    sorted_lat = sorted(latencies)
    total = sum(latencies)
    return dict(
        mean_us=statistics.mean(latencies) * 1e6,
        median_us=statistics.median(latencies) * 1e6,
        p95_us=percentile(sorted_lat, 0.95) * 1e6,
        p99_us=percentile(sorted_lat, 0.99) * 1e6,
        throughput_per_sec=(len(latencies) / total) if total > 0 else 0.0,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("docs_file")
    parser.add_argument("index_base")
    parser.add_argument("size_label")
    parser.add_argument("--samples", type=int, default=2000)
    parser.add_argument("--csv", default=None)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--warmup", type=int, default=1,
                        help="untimed passes over the samples before timing (0 = cold-cache numbers)")
    args = parser.parse_args()

    csv_path = Path(args.csv) if args.csv else Path(__file__).resolve().parent / "results" / "lookup.csv"
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    new_file = not csv_path.exists()

    random.seed(args.seed)
    doc_ids, terms = parse_docs_file(args.docs_file)
    sample_doc_ids = random.sample(doc_ids, min(args.samples, len(doc_ids)))
    sample_terms = random.sample(terms, min(args.samples, len(terms)))

    lexicon = NindLexiconindex(args.index_base + ".nindlexiconindex")
    identification = lexicon.donneIdentification()
    term_index = NindTermindex(args.index_base + ".nindtermindex", identification)
    local_index = NindLocalindex(args.index_base + ".nindlocalindex", identification)

    # term -> id
    term_lookup_latencies = time_calls(lexicon.donneIdentifiant, [([t],) for t in sample_terms], args.warmup)
    term_ids = [lexicon.donneIdentifiant([t]) for t in sample_terms]
    known_term_ids = [tid for tid in term_ids if tid != 0]

    # id -> postings
    postings_latencies = time_calls(term_index.donneListeTermesCG, [(tid,) for tid in known_term_ids], args.warmup)

    # doc -> local terms
    local_latencies = time_calls(local_index.donneListeTermes, [(d,) for d in sample_doc_ids], args.warmup)

    rows = [
        ("term_to_id", len(term_lookup_latencies), term_lookup_latencies),
        ("id_to_postings", len(postings_latencies), postings_latencies),
        ("doc_to_local_terms", len(local_latencies), local_latencies),
    ]

    with open(csv_path, "a", encoding="utf-8") as f:
        if new_file:
            f.write("size,operation,warmup,samples,mean_us,median_us,p95_us,p99_us,throughput_per_sec\n")
        for operation, samples, latencies in rows:
            stats = summarize(latencies)
            f.write(f"{args.size_label},{operation},{args.warmup},{samples},"
                    f"{stats['mean_us']:.3f},{stats['median_us']:.3f},"
                    f"{stats['p95_us']:.3f},{stats['p99_us']:.3f},"
                    f"{stats['throughput_per_sec']:.1f}\n")
            print(f"{operation}: n={samples} mean={stats['mean_us']:.1f}us "
                  f"p95={stats['p95_us']:.1f}us throughput={stats['throughput_per_sec']:.0f}/s",
                  file=sys.stderr)

    print(f"Results appended to {csv_path}", file=sys.stderr)


if __name__ == "__main__":
    main()
