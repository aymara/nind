#!/usr/bin/env python3
"""The worst term for the writers: one term in every document, through shards and their merge.

Usage: worst_case_term.py OUT_DIR [--documents 350000000] [--shards 4]

Synthetic (no text): every document holds the one term "x", once. Each phase runs in a child
process, one at a time, and prints its time and peak RSS. Writers used to reserve 14 bytes per
posting for a term's definition, capped at 4 GiB: no term could be in more than ~306M documents.
"""
import argparse
import os
import shutil
import sys
import time
from array import array

from nind import _native as native


def build_shard(base, first, count):
    lexicon = native.NindLexiconIndex(base, is_writer=True, indirection_bloc_size=1)
    lexicon.begin_bulk()
    term_id = lexicon.add_simple_words(["x"])[0]
    lexicon.end_bulk()
    identification = lexicon.get_identification()
    builder = native.IndexBuilder()
    builder.add_documents(first, array('I', range(1, count + 1)), array('I', [0]) * count, array('I', [term_id]))
    writer = native.NindTermIndex(base, is_writer=True, lexicon_identification=identification,
                                  specifics_number=0, indirection_bloc_size=2)
    writer.begin_bulk()
    builder.write_term_index(writer, identification)
    writer.end_bulk()


def merge(shard_bases, target, documents):
    stats = native.merge_indexes(shard_bases, target, doc_ids=array('Q', range(documents)),
                                 doc_lengths=array('I', [1]) * documents)
    print("  merged: %d terms, %d words" % (stats.terms_nb, stats.words_nb))


def verify(target):
    report = native.verify_index(target)
    print("  %r" % report)
    if not report.ok:
        raise SystemExit("verification failed: %s" % report.problems[:3])


def search(target, documents):
    lexicon = native.NindLexiconIndex(target, is_writer=False)
    term_index = native.NindTermIndex(target, is_writer=False, lexicon_identification=lexicon.get_identification(),
                                      specifics_number=0)
    term_id = lexicon.get_word_id(["x"])
    scorer = native.Bm25Scorer(array('Q', range(documents)), array('I', [1]) * documents, 1.0)
    start = time.perf_counter()
    results = scorer.search(term_index, [term_id], 1.2, 0.75, 10, True, 1)
    print("  pruned top 10 in %.3f s: first document %d" % (time.perf_counter() - start, results[0][0]))
    if [doc_id for doc_id, _ in results] != list(range(10)):
        raise SystemExit("unexpected results: %r" % results)


def phase(name, function, *args):
    # in a child, for its own peak RSS
    sys.stdout.flush()
    start = time.perf_counter()
    pid = os.fork()
    if pid == 0:
        try:
            function(*args)
            sys.stdout.flush()
            os._exit(0)
        except BaseException as error:
            print("  %s failed: %r" % (name, error), flush=True)
            os._exit(1)
    _, status, usage = os.wait4(pid, 0)
    print("%s: %.1f s, peak %.2f GB" % (name, time.perf_counter() - start, usage.ru_maxrss / 2**20), flush=True)
    if status != 0:
        raise SystemExit("%s failed" % name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("out")
    parser.add_argument("--documents", type=int, default=350000000)
    parser.add_argument("--shards", type=int, default=4)
    args = parser.parse_args()
    os.makedirs(args.out)
    bounds = [args.documents * i // args.shards for i in range(args.shards + 1)]
    shard_bases = [os.path.join(args.out, "shard%d" % i) for i in range(args.shards)]
    for i, base in enumerate(shard_bases):
        phase("shard %d" % i, build_shard, base, bounds[i], bounds[i + 1] - bounds[i])
    target = os.path.join(args.out, "merged")
    phase("merge", merge, shard_bases, target, args.documents)
    print("term index: %.3f GB" % (os.path.getsize(target + ".nindtermindex") / 2**30), flush=True)
    phase("verify", verify, target)
    phase("search", search, target, args.documents)
    shutil.rmtree(args.out)


if __name__ == "__main__":
    main()
