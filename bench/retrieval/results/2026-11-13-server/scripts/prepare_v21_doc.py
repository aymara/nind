#!/usr/bin/env python3
"""MS MARCO v2.1 documents (TREC RAG 2024's corpus) through prepare.py's analysis, in parallel.

    prepare_v21_doc.py DOCS_DIR OUT_DIR --topics DEV_TOPICS --qrels DEV_QRELS
        [--trec-out DIR --trec-topics T1 T2... --trec-qrels Q1 Q2...] [--stopwords] [--workers 40]

DOCS_DIR holds msmarco_v2.1_doc_NN.json.gz. A document's contents are Anserini's
(MsMarcoV2DocCollection): url, title, headings and body, joined by spaces. Same files as
prepare_v2.py: documents in the order of the archive, the last 1% in add.jsonl, the dev topics'
queries and qrels; with --trec-out, a second dataset of the TREC topics that were judged (queries,
qrels), its corpus files linked to OUT_DIR's. stats.json adds the documents' length distribution
and the postings with a frequency over 255 (stored apart by the builder).
"""
import argparse, collections, glob, gzip, json, os, sys
from concurrent.futures import ProcessPoolExecutor
sys.path.insert(0, "/scratch/gdechalendar/Projets/nind/bench/retrieval")
from prepare import analyse, LUCENE_STOPWORDS, TOKENIZER

LENGTH_BUCKETS = [10, 100, 300, 1000, 3000, 10000, 30000, 100000]


def part(job):
    path, out, stopwords = job
    n = dropped = total = high_tf = longest = 0
    lengths = collections.Counter()
    with gzip.open(path, "rt", encoding="utf-8") as src, open(out, "w", encoding="utf-8") as dst:
        for line in src:
            doc = json.loads(line)
            text = " ".join(doc.get(field) or "" for field in ("url", "title", "headings", "body"))
            tokens = analyse(text, stopwords)
            if not tokens:
                dropped += 1
                continue
            dst.write(json.dumps({"id": doc["docid"], "contents": " ".join(tokens)}, ensure_ascii=False) + "\n")
            n += 1
            total += len(tokens)
            longest = max(longest, len(tokens))
            lengths[next((b for b in LENGTH_BUCKETS if len(tokens) <= b), "more")] += 1
            high_tf += sum(1 for count in collections.Counter(tokens).values() if count > 255)
    return n, dropped, total, high_tf, longest, lengths


def write_queries(topics, qrels, out, stopwords, judged_only):
    judged = set()
    with open(os.path.join(out, "qrels.txt"), "w", encoding="utf-8") as dst:
        for path in qrels:
            for line in open(path, encoding="utf-8"):
                if line.strip():
                    query_id, _, doc_id, relevance = line.split()
                    judged.add(query_id)
                    dst.write("%s 0 %s %s\n" % (query_id, doc_id, relevance))
    n_queries = empty = 0
    with open(os.path.join(out, "queries.tsv"), "w", encoding="utf-8") as dst:
        for path in topics:
            for line in open(path, encoding="utf-8"):
                if not line.strip():
                    continue
                query_id, text = line.rstrip("\n").split("\t", 1)
                if judged_only and query_id not in judged:
                    continue
                tokens = list(dict.fromkeys(analyse(text, stopwords)))
                if not tokens:
                    empty += 1
                    continue
                dst.write("%s\t%s\n" % (query_id, " ".join(tokens)))
                n_queries += 1
    return n_queries, empty


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("docs")
    parser.add_argument("out")
    parser.add_argument("--topics", nargs="+", required=True)
    parser.add_argument("--qrels", nargs="+", required=True)
    parser.add_argument("--trec-out")
    parser.add_argument("--trec-topics", nargs="*", default=[])
    parser.add_argument("--trec-qrels", nargs="*", default=[])
    parser.add_argument("--stopwords", action="store_true")
    parser.add_argument("--workers", type=int, default=40)
    parser.add_argument("--update-fraction", type=float, default=0.01)
    args = parser.parse_args()
    stopwords = LUCENE_STOPWORDS if args.stopwords else frozenset()
    os.makedirs(args.out, exist_ok=True)
    paths = sorted(glob.glob(os.path.join(args.docs, "msmarco_v2.1_doc_*.json.gz")))
    if not paths:
        raise SystemExit("no msmarco_v2.1_doc_*.json.gz in " + args.docs)
    parts = [os.path.join(args.out, "part-%02d.tmp" % i) for i in range(len(paths))]
    with ProcessPoolExecutor(args.workers) as pool:
        counts = list(pool.map(part, [(p, o, stopwords) for p, o in zip(paths, parts)]))
    n_docs = sum(c[0] for c in counts)
    n_add = max(1, int(n_docs * args.update_fraction))
    index = 0
    with open(os.path.join(args.out, "corpus.jsonl"), "wb") as corpus, \
            open(os.path.join(args.out, "base.jsonl"), "wb") as base, \
            open(os.path.join(args.out, "add.jsonl"), "wb") as add:
        for path in parts:
            with open(path, "rb") as f:
                for line in f:
                    corpus.write(line)
                    (base if index < n_docs - n_add else add).write(line)
                    index += 1
            os.remove(path)
    n_queries, empty = write_queries(args.topics, args.qrels, args.out, stopwords, judged_only=False)
    lengths = collections.Counter()
    for c in counts:
        lengths.update(c[5])
    total = sum(c[2] for c in counts)
    stats = {"dataset": "msmarco-v2.1-doc/dev", "limit": None, "truncated": False, "documents": n_docs,
             "dropped_empty_documents": sum(c[1] for c in counts), "avgdl": total / n_docs, "tokens": total,
             "longest_document": max(c[4] for c in counts),
             "documents_by_length": {("<=%d" % b if b != "more" else ">%d" % LENGTH_BUCKETS[-1]): lengths[b]
                                     for b in LENGTH_BUCKETS + ["more"]},
             "postings_over_255": sum(c[3] for c in counts),
             "update_documents": n_add, "queries": n_queries, "dropped_empty_queries": empty,
             "tokenizer": TOKENIZER, "stopwords": "lucene" if args.stopwords else None, "stopwords_nb": len(stopwords)}
    with open(os.path.join(args.out, "stats.json"), "w") as f:
        json.dump(stats, f, indent=2)
    print(json.dumps(stats))
    if args.trec_out:
        os.makedirs(args.trec_out, exist_ok=True)
        for name in ("corpus.jsonl", "base.jsonl", "add.jsonl"):
            target = os.path.join(args.trec_out, name)
            if not os.path.lexists(target):
                os.symlink(os.path.abspath(os.path.join(args.out, name)), target)
        n_queries, empty = write_queries(args.trec_topics, args.trec_qrels, args.trec_out, stopwords, judged_only=True)
        trec = dict(stats, dataset="msmarco-v2.1-doc/trec-dl-2021-2023 (judged)", queries=n_queries,
                    dropped_empty_queries=empty)
        with open(os.path.join(args.trec_out, "stats.json"), "w") as f:
            json.dump(trec, f, indent=2)
        print(json.dumps({"trec_queries": n_queries, "dropped": empty}))


if __name__ == "__main__":
    main()
