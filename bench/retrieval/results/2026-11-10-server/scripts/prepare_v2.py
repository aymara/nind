#!/usr/bin/env python3
"""MS MARCO v2 passages through prepare.py's analysis, in parallel (prepare.py alone: one process).

    prepare_v2.py PASSAGES_DIR OUT_DIR --queries msmarco-passage-v2/dev1 [--stopwords] [--workers 24]

PASSAGES_DIR holds msmarco_passage_NN.gz. Same files as prepare.py: documents in the order of
the archive, the same tokens, the last 1% in add.jsonl.
"""
import argparse, glob, gzip, json, os, sys
from concurrent.futures import ProcessPoolExecutor
sys.path.insert(0, "/scratch/gdechalendar/Projets/nind/bench/retrieval")
from prepare import analyse, LUCENE_STOPWORDS, TOKENIZER


def part(job):
    path, out, stopwords = job
    n = dropped = total = 0
    with gzip.open(path, "rt", encoding="utf-8") as src, open(out, "w", encoding="utf-8") as dst:
        for line in src:
            doc = json.loads(line)
            tokens = analyse(doc["passage"], stopwords)
            if not tokens:
                dropped += 1
                continue
            dst.write(json.dumps({"id": doc["pid"], "contents": " ".join(tokens)}, ensure_ascii=False) + "\n")
            n += 1
            total += len(tokens)
    return n, dropped, total


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("passages")
    parser.add_argument("out")
    parser.add_argument("--queries", default="msmarco-passage-v2/dev1")
    parser.add_argument("--stopwords", action="store_true")
    parser.add_argument("--workers", type=int, default=24)
    parser.add_argument("--update-fraction", type=float, default=0.01)
    args = parser.parse_args()
    stopwords = LUCENE_STOPWORDS if args.stopwords else frozenset()
    os.makedirs(args.out, exist_ok=True)
    paths = sorted(glob.glob(os.path.join(args.passages, "msmarco_passage_*.gz")))
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

    import ir_datasets
    dataset = ir_datasets.load(args.queries)
    n_queries = empty = 0
    with open(os.path.join(args.out, "queries.tsv"), "w", encoding="utf-8") as queries:
        for query in dataset.queries_iter():
            tokens = list(dict.fromkeys(analyse(query.text, stopwords)))
            if not tokens:
                empty += 1
                continue
            queries.write("%s\t%s\n" % (query.query_id, " ".join(tokens)))
            n_queries += 1
    with open(os.path.join(args.out, "qrels.txt"), "w", encoding="utf-8") as qrels:
        for qrel in dataset.qrels_iter():
            qrels.write("%s 0 %s %d\n" % (qrel.query_id, qrel.doc_id, qrel.relevance))
    total = sum(c[2] for c in counts)
    stats = {"dataset": args.queries, "limit": None, "truncated": False, "documents": n_docs,
             "dropped_empty_documents": sum(c[1] for c in counts), "avgdl": total / n_docs, "tokens": total,
             "update_documents": n_add, "queries": n_queries, "dropped_empty_queries": empty,
             "tokenizer": TOKENIZER, "stopwords": "lucene" if args.stopwords else None, "stopwords_nb": len(stopwords)}
    with open(os.path.join(args.out, "stats.json"), "w") as f:
        json.dump(stats, f, indent=2)
    print(json.dumps(stats))


if __name__ == "__main__":
    main()
