#!/usr/bin/env python3
"""Turn an ir_datasets dataset into the pre-tokenized files every system indexes.

nind is only the indexing layer of the search stack (text analysis is LIMA's
job, search Amose's), so the systems compared are all given the same tokens:
this script stands in for the analysis, once, and writes

    <out>/corpus.jsonl   {"id": ..., "contents": "tok tok ..."} per document
    <out>/base.jsonl     the corpus minus its last --update-fraction documents
    <out>/add.jsonl      those last documents (the update benchmark adds them to base)
    <out>/queries.tsv    qid <TAB> tok tok ...   (query tokens, deduplicated)
    <out>/qrels.txt      TREC qrels (absent when --limit truncates the corpus:
                         relevance judgments of a corpus subset mean nothing)
    <out>/stats.json     documents, dropped empty documents, avgdl, queries...

Tokens are nind's own tokenizer's words (runs of letters and digits; no
identifier splitting), lowercased: tokenizing "contents" again, with every
split off, gives the same tokens back, and so does splitting it on spaces
(what Lucene's WhitespaceAnalyzer, tantivy's "whitespace" tokenizer and
bm25s are given). Documents without any token are dropped (nind doesn't
store them, so they would otherwise change N and avgdl for the others only).

Usage: prepare.py DATASET OUT_DIR [--limit N] [--update-fraction F]
  e.g. prepare.py beir/scifact/test data/scifact
       prepare.py msmarco-passage/dev/small data/msmarco-1M --limit 1000000
ir_datasets downloads into $IR_DATASETS_HOME (default ~/.ir_datasets).
"""
import argparse
import json
import os
import sys

import ir_datasets
from nind import _native

#: options of the tokenizer the files are made with, and that nind indexes them with
TOKENIZER = dict(split_camel_case=False, split_underscore=False, split_digits=False,
                 lowercase=False, min_length=1, max_length=254)
_tokenizer = _native.Tokenizer(_native.TokenizerOptions(**TOKENIZER))


def analyse(text):
    """The tokens of ``text``: nind's words, lowercased (a fixed point of the tokenizer)."""
    return _tokenizer.tokenize(" ".join(token.lower() for token in _tokenizer.tokenize(text)))


def doc_text(doc):
    title = getattr(doc, "title", "") or ""
    text = getattr(doc, "text", "") or ""
    return title + "\n" + text if title else text


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("dataset", help="ir_datasets id, e.g. beir/scifact/test")
    parser.add_argument("out", help="output directory")
    parser.add_argument("--limit", type=int, default=None, help="only the first N documents")
    parser.add_argument("--update-fraction", type=float, default=0.01,
                        help="fraction of the documents in add.jsonl (default 0.01, at least 1)")
    args = parser.parse_args()

    dataset = ir_datasets.load(args.dataset)
    os.makedirs(args.out, exist_ok=True)
    corpus_path = os.path.join(args.out, "corpus.jsonl")
    n_docs = dropped = total_len = 0
    with open(corpus_path, "w", encoding="utf-8") as corpus:
        for doc in dataset.docs_iter():
            if args.limit is not None and n_docs + dropped >= args.limit:
                break
            tokens = analyse(doc_text(doc))
            if not tokens:
                dropped += 1
                continue
            corpus.write(json.dumps({"id": doc.doc_id, "contents": " ".join(tokens)}, ensure_ascii=False) + "\n")
            n_docs += 1
            total_len += len(tokens)
    n_add = max(1, int(n_docs * args.update_fraction))
    with open(corpus_path, encoding="utf-8") as corpus, \
            open(os.path.join(args.out, "base.jsonl"), "w", encoding="utf-8") as base, \
            open(os.path.join(args.out, "add.jsonl"), "w", encoding="utf-8") as add:
        for i, line in enumerate(corpus):
            (base if i < n_docs - n_add else add).write(line)

    n_queries = empty_queries = 0
    with open(os.path.join(args.out, "queries.tsv"), "w", encoding="utf-8") as queries:
        for query in dataset.queries_iter():
            # deduplicated: systems don't all count a repeated query term twice
            tokens = list(dict.fromkeys(analyse(query.text)))
            if not tokens:
                empty_queries += 1
                continue
            queries.write("%s\t%s\n" % (query.query_id, " ".join(tokens)))
            n_queries += 1

    truncated = args.limit is not None and n_docs + dropped >= args.limit
    qrels_path = os.path.join(args.out, "qrels.txt")
    if dataset.has_qrels() and not truncated:
        with open(qrels_path, "w", encoding="utf-8") as qrels:
            for qrel in dataset.qrels_iter():
                qrels.write("%s 0 %s %d\n" % (qrel.query_id, qrel.doc_id, qrel.relevance))
    elif os.path.exists(qrels_path):
        os.remove(qrels_path)

    stats = {"dataset": args.dataset, "limit": args.limit, "truncated": truncated,
             "documents": n_docs, "dropped_empty_documents": dropped,
             "avgdl": total_len / n_docs if n_docs else 0.0, "tokens": total_len,
             "update_documents": n_add, "queries": n_queries, "dropped_empty_queries": empty_queries,
             "tokenizer": TOKENIZER}
    with open(os.path.join(args.out, "stats.json"), "w") as f:
        json.dump(stats, f, indent=2)
    json.dump(stats, sys.stdout, indent=2)
    print()


if __name__ == "__main__":
    main()
