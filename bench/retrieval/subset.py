#!/usr/bin/env python3
"""Cut a dataset prepared by prepare.py to its first N documents (no new tokenization).

    subset.py SRC_DIR DST_DIR N [--update-fraction F]

For scaling curves: prefixes of one corpus (e.g. MS MARCO at 1M, 2M, 4M
documents next to the whole 8.8M) differ only by their size. The queries are
kept; the qrels aren't (judgments of a corpus subset mean nothing).
"""
import argparse
import json
import os
import shutil


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("src")
    parser.add_argument("dst")
    parser.add_argument("documents", type=int)
    parser.add_argument("--update-fraction", type=float, default=0.01)
    args = parser.parse_args()

    os.makedirs(args.dst, exist_ok=True)
    n_docs = total_len = 0
    with open(os.path.join(args.src, "corpus.jsonl"), encoding="utf-8") as src, \
            open(os.path.join(args.dst, "corpus.jsonl"), "w", encoding="utf-8") as dst:
        for line in src:
            if n_docs == args.documents:
                break
            if line.strip():
                dst.write(line)
                n_docs += 1
                total_len += len(json.loads(line)["contents"].split())
    n_add = max(1, int(n_docs * args.update_fraction))
    with open(os.path.join(args.dst, "corpus.jsonl"), encoding="utf-8") as corpus, \
            open(os.path.join(args.dst, "base.jsonl"), "w", encoding="utf-8") as base, \
            open(os.path.join(args.dst, "add.jsonl"), "w", encoding="utf-8") as add:
        for i, line in enumerate(corpus):
            (base if i < n_docs - n_add else add).write(line)
    shutil.copy(os.path.join(args.src, "queries.tsv"), os.path.join(args.dst, "queries.tsv"))
    qrels = os.path.join(args.dst, "qrels.txt")
    if os.path.exists(qrels):
        os.remove(qrels)
    with open(os.path.join(args.src, "stats.json")) as f:
        stats = json.load(f)
    stats.update(subset_of=os.path.abspath(args.src), limit=args.documents, truncated=True,
                 documents=n_docs, dropped_empty_documents=None, tokens=total_len,
                 avgdl=total_len / n_docs if n_docs else 0.0, update_documents=n_add)
    with open(os.path.join(args.dst, "stats.json"), "w") as f:
        json.dump(stats, f, indent=2)
    print(json.dumps(stats))


if __name__ == "__main__":
    main()
