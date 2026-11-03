#!/usr/bin/env python3
"""Converts a prefix of the MS MARCO Passage Ranking collection.tsv into nind's
"one line per document" corpus format, as read by NindAmose_litTexteAnalysej:

    <docId> <term> <pos>,<len> <term> <pos>,<len> ...

collection.tsv has one passage per line: "<pid>\t<passage text>", with pid
already a dense sequence starting at 0. Since nind treats document id 0 as a
sentinel elsewhere in the codebase, documents are emitted as pid + 1.

Tokenization is a simple [A-Za-z0-9]+ regex (deliberately excluding '_', which
the nind format reserves as the compound-word separator), lowercased. pos/len
are character offsets into the original passage text.

usage: convert_to_nind.py <collection.tsv> <output.txt> [--limit N]
"""
import argparse
import re
import sys

TOKEN_RE = re.compile(r"[A-Za-z0-9]+")


def convert(input_path, output_path, limit):
    docs = 0
    tokens_total = 0
    vocab = set()
    with open(input_path, "r", encoding="utf-8") as fin, \
         open(output_path, "w", encoding="utf-8") as fout:
        for line in fin:
            if limit is not None and docs >= limit:
                break
            pid_str, _, passage = line.partition("\t")
            if not pid_str:
                continue
            doc_id = int(pid_str) + 1
            parts = [str(doc_id)]
            for match in TOKEN_RE.finditer(passage):
                term = match.group().lower()
                parts.append(f"{term} {match.start()},{len(term)}")
                tokens_total += 1
                vocab.add(term)
            fout.write(" ".join(parts) + "\n")
            docs += 1
    return docs, tokens_total, len(vocab)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("collection_tsv")
    parser.add_argument("output_txt")
    parser.add_argument("--limit", type=int, default=None,
                         help="only convert the first N passages (default: all)")
    args = parser.parse_args()

    docs, tokens_total, vocab_size = convert(args.collection_tsv, args.output_txt, args.limit)
    print(f"docs={docs} tokens={tokens_total} vocab={vocab_size}", file=sys.stderr)


if __name__ == "__main__":
    main()
