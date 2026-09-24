#!/usr/bin/env python3
"""Bulk-build indexing throughput benchmark, using nind's efficient write
pattern: each term's definition and each document's local definition is
written to disk exactly once (as nind.nind_engine.NindIndexer does) - as
opposed to bench/run_indexing_benchmark.sh, which drives Nind_indexeCorpus
and does a read-modify-write of each term's growing postings list on every
document (see bench/README.md for why that shows ~quadratic scaling).

The corpus is split into line-aligned byte ranges processed by a pool of
worker processes; the main process only drives the (single-file,
single-threaded) nind writers, handing them a whole chunk's documents or a
whole term-id range's terms per call, as flat buffers (nind._native's
set_local_defs_arrays/set_term_defs_arrays, which write with the GIL released,
so the main process keeps receiving worker results meanwhile):

- pass 1 (workers): per-chunk vocabulary and highest document id;
- main: builds the lexicon, terms added in sorted order;
- pass 2 (workers): parse and map terms to ids; each document's occurrences
  come back as flat arrays and the main process writes its local definition
  right away, in document order. Each worker also spills its chunk's
  postings to a temporary file, sorted by term id and split into term-id
  ranges (SPIMI-style), so no process ever holds the whole inverted index;
- pass 3 (workers): one term-id range each, merging that range's slice of
  every chunk file into per-term postings; the main process writes them in
  term id order.

The output is byte-identical to a serial accumulate-then-write build (apart
from the lexicon creation timestamp in each file's identification trailer).

Reads the same "one line per document" corpus format produced by
msmarco/convert_to_nind.py (so it works on the compact bulk file, not
one-file-per-document like NindIndexer.index_files expects).

usage: index_bulk.py <docs.txt> <index-base> <size-label> [--workers N] [--csv PATH]
  <index-base> is the .nind* file path prefix to write, e.g. .../docs
"""
import argparse
from array import array
import bisect
import fcntl
import gc
import multiprocessing
import os
import resource
import sys
import tempfile
import time
from collections import defaultdict, deque
from itertools import islice
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src" / "py" / "nind"))
from nind import _native as native

# state inherited by forked workers (set in the main process before the pool
# that needs it is created, so it's shared copy-on-write, not pickled)
_term_to_id = None
_spill_dir = None
_range_bounds = None     # term id lower bounds of each pass-3 range, ascending

CHUNK_BYTES = 8 * 1024 * 1024    # target corpus bytes per chunk


def bounded_imap(pool, fn, tasks, window):
    """Like pool.imap (results in task order), but with at most `window`
    tasks in flight: the main process, which consumes the results, is the
    bottleneck, and pool.imap would otherwise queue every finished result
    in its memory."""
    tasks = iter(tasks)
    pending = deque(pool.apply_async(fn, (task,)) for task in islice(tasks, window))
    while pending:
        result = pending.popleft().get()
        for task in islice(tasks, 1):
            pending.append(pool.apply_async(fn, (task,)))
        yield result


def parse_lines(text):
    """Yields (doc_id, [(term, position, length), ...]) per non-empty line."""
    for line in text.splitlines():
        fields = line.split()
        if not fields:
            continue
        occurrences = []
        for term, loc in zip(fields[1::2], fields[2::2]):
            pos_str, len_str = loc.split(",")
            occurrences.append((term, int(pos_str), int(len_str)))
        yield int(fields[0]), occurrences


def read_chunk(docs_path, start, end):
    with open(docs_path, "rb") as f:
        f.seek(start)
        return f.read(end - start).decode("utf-8")


def split_chunks(docs_path, chunks_nb):
    """Line-aligned byte ranges covering the whole file, in file order."""
    size = os.path.getsize(docs_path)
    bounds = [0]
    with open(docs_path, "rb") as f:
        for i in range(1, chunks_nb):
            f.seek(max(bounds[-1], size * i // chunks_nb))
            f.readline()
            pos = f.tell()
            if pos >= size:
                break
            if pos > bounds[-1]:
                bounds.append(pos)
    bounds.append(size)
    return list(zip(bounds[:-1], bounds[1:]))


def pass1_worker(task):
    docs_path, start, end = task
    vocab = set()
    max_doc_id = 0
    for doc_id, occurrences in parse_lines(read_chunk(docs_path, start, end)):
        max_doc_id = max(max_doc_id, doc_id)
        vocab.update(term for term, _pos, _len in occurrences)
    return vocab, max_doc_id


def pass2_worker(task):
    """Returns the chunk's documents as flat arrays for set_local_def_arrays
    and spills its postings, as (term_id, doc_id, freq) triples sorted by
    term id, to <spill_dir>/<chunk_no>; triple offsets of each pass-3 range
    are returned so pass 3 can read just its slice."""
    chunk_no, docs_path, start, end = task
    doc_ids = array("I")
    doc_ends = array("I")                # end offset of each doc in the three arrays below
    term_ids, positions, lengths = array("I"), array("I"), array("I")
    postings = []                        # (term_id, doc_id, freq)
    tokens = 0
    for doc_id, occurrences in parse_lines(read_chunk(docs_path, start, end)):
        counts = defaultdict(int)
        for term, pos, length in occurrences:
            tid = _term_to_id[term]
            counts[tid] += 1
            term_ids.append(tid)
            positions.append(pos)
            lengths.append(length)
        tokens += len(occurrences)
        doc_ids.append(doc_id)
        doc_ends.append(len(term_ids))
        postings.extend((tid, doc_id, freq) for tid, freq in counts.items())
    # documents are in ascending id order within the file, so a stable sort
    # on term id keeps each term's documents ascending
    postings.sort(key=lambda p: p[0])
    triples = array("I")
    for p in postings:
        triples.extend(p)
    with open(os.path.join(_spill_dir, str(chunk_no)), "wb") as f:
        triples.tofile(f)
    sorted_tids = [p[0] for p in postings]
    range_offsets = [bisect.bisect_left(sorted_tids, bound) for bound in _range_bounds]
    range_offsets.append(len(postings))
    return doc_ids, doc_ends, term_ids, positions, lengths, tokens, range_offsets


def pass3_worker(task):
    """Merges one term-id range across all chunk spill files, in chunk
    (= document) order; returns the range's terms (ascending) as flat arrays
    for set_term_defs_arrays: term ids, end offset of each term's postings,
    and the concatenated postings' doc ids and frequencies."""
    range_no, chunk_slices = task
    by_term = {}
    for chunk_no, first, last in chunk_slices:
        if last == first:
            continue
        triples = array("I")
        with open(os.path.join(_spill_dir, str(chunk_no)), "rb") as f:
            f.seek(first * 3 * triples.itemsize)
            triples.fromfile(f, (last - first) * 3)
        for i in range(0, len(triples), 3):
            tid = triples[i]
            entry = by_term.get(tid)
            if entry is None:
                entry = by_term[tid] = (array("I"), array("I"))
            entry[0].append(triples[i + 1])
            entry[1].append(triples[i + 2])
    term_ids, term_ends, doc_ids, freqs = array("I"), array("I"), array("I"), array("I")
    for tid in sorted(by_term):
        docs, term_freqs = by_term.pop(tid)
        term_ids.append(tid)
        doc_ids.extend(docs)
        freqs.extend(term_freqs)
        term_ends.append(len(doc_ids))
    return term_ids, term_ends, doc_ids, freqs


def build_index(docs_path, index_base, workers):
    global _term_to_id, _spill_dir, _range_bounds
    ctx = multiprocessing.get_context("fork")
    phase_start = time.perf_counter()

    def phase_done(name):
        nonlocal phase_start
        now = time.perf_counter()
        print(f"  {name}: {now - phase_start:.2f}s", file=sys.stderr)
        phase_start = now
    chunks = split_chunks(docs_path, max(workers * 4, os.path.getsize(docs_path) // CHUNK_BYTES))
    window = workers * 2

    # pass 1: vocabulary (NindLexiconIndex needs the entry count up front) and
    # the highest document id (so NindLocalIndex can be opened before pass 2)
    vocab = set()
    max_doc_id = 0
    with ctx.Pool(workers) as pool:
        for chunk_vocab, chunk_max in pool.imap_unordered(
                pass1_worker, [(docs_path, s, e) for s, e in chunks]):
            vocab |= chunk_vocab
            max_doc_id = max(max_doc_id, chunk_max)
    terms_sorted = sorted(vocab)
    del vocab
    phase_done("pass 1 (vocabulary)")

    lexicon_writer = native.NindLexiconIndex(index_base, is_writer=True,
                                              indirection_bloc_size=max(1, len(terms_sorted)))
    _term_to_id = {term: lexicon_writer.add_word([term]) for term in terms_sorted}
    del terms_sorted
    vocab_size = len(_term_to_id)
    lexicon_identification = lexicon_writer.get_identification()
    del lexicon_writer
    phase_done("lexicon write")

    max_term_id = max(_term_to_id.values(), default=0)
    ranges_nb = workers * 4
    _range_bounds = [1 + (max_term_id * r) // ranges_nb for r in range(ranges_nb)]

    docs = 0
    tokens_total = 0
    local_writer = native.NindLocalIndex(index_base, is_writer=True,
                                          lexicon_identification=lexicon_identification,
                                          indirection_bloc_size=max_doc_id + 1)
    term_writer = native.NindTermIndex(index_base, is_writer=True,
                                        lexicon_identification=lexicon_identification,
                                        specifics_number=0,
                                        indirection_bloc_size=max_term_id + 1)
    spill_parent = os.path.dirname(os.path.abspath(index_base))
    with tempfile.TemporaryDirectory(prefix="nind_spill_", dir=spill_parent) as spill_dir:
        _spill_dir = spill_dir
        # keep the inherited lexicon out of the (forked) workers' GC scans, so
        # it isn't copied page by page into each of them
        gc.freeze()
        with ctx.Pool(workers) as pool:
            # pass 2: chunks come back in file order, so local definitions are
            # written in document order
            all_range_offsets = []
            for doc_ids, doc_ends, term_ids, positions, lengths, tokens, range_offsets in bounded_imap(
                    pool, pass2_worker, [(i, docs_path, s, e) for i, (s, e) in enumerate(chunks)], window):
                local_writer.set_local_defs_arrays(doc_ids, doc_ends, term_ids, positions, lengths,
                                                   lexicon_identification)
                docs += len(doc_ids)
                tokens_total += tokens
                all_range_offsets.append(range_offsets)
            del local_writer
            phase_done("pass 2 (local index write + postings spill)")

            # pass 3: ranges come back in term id order
            tasks = [(r, [(c, offsets[r], offsets[r + 1]) for c, offsets in enumerate(all_range_offsets)])
                     for r in range(ranges_nb)]
            for term_ids, term_ends, doc_ids, freqs in bounded_imap(pool, pass3_worker, tasks, window):
                term_writer.set_term_defs_arrays(term_ids, term_ends, doc_ids, freqs, lexicon_identification)
            del term_writer
            phase_done("pass 3 (postings merge + term index write)")
        gc.unfreeze()

    return docs, tokens_total, vocab_size


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("docs_file")
    parser.add_argument("index_base")
    parser.add_argument("size_label")
    parser.add_argument("--workers", type=int, default=os.cpu_count() or 1)
    parser.add_argument("--csv", default=None)
    args = parser.parse_args()

    csv_path = Path(args.csv) if args.csv else Path(__file__).resolve().parent / "results" / "indexing_bulk.csv"
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    new_file = not csv_path.exists()

    # two runs writing the same index would delete/overwrite each other's
    # files and fail (or worse) in the middle; refuse to start instead
    lock_file = open(args.index_base + ".lock", "w")
    try:
        fcntl.flock(lock_file, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        sys.exit(f"index_bulk.py: another run is already writing {args.index_base}.* (lock held on "
                 f"{args.index_base}.lock)")

    docs_file_size = Path(args.docs_file).stat().st_size
    start = time.perf_counter()
    docs, tokens_total, vocab = build_index(args.docs_file, args.index_base, max(1, args.workers))
    wall_seconds = time.perf_counter() - start
    # process-lifetime peak RSS of the main process, and of the largest
    # worker; accurate for this benchmark since it's run as a fresh
    # subprocess per corpus size, doing nothing else beforehand
    peak_rss_kb = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    peak_worker_rss_kb = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss

    index_bytes = 0
    for ext in (".nindlexiconindex", ".nindtermindex", ".nindlocalindex"):
        p = Path(args.index_base + ext)
        if p.exists():
            index_bytes += p.stat().st_size

    docs_per_sec = docs / wall_seconds if wall_seconds > 0 else docs
    mb_per_sec = (docs_file_size / 1048576) / wall_seconds if wall_seconds > 0 else docs_file_size / 1048576

    with open(csv_path, "a", encoding="utf-8") as f:
        if new_file:
            f.write("size,workers,docs,tokens,vocab,input_bytes,wall_seconds,docs_per_sec,mb_per_sec,"
                     "peak_rss_kb,peak_worker_rss_kb,index_bytes\n")
        f.write(f"{args.size_label},{args.workers},{docs},{tokens_total},{vocab},{docs_file_size},"
                f"{wall_seconds:.3f},{docs_per_sec:.3f},{mb_per_sec:.5f},"
                f"{peak_rss_kb},{peak_worker_rss_kb},{index_bytes}\n")

    print(f"docs={docs} workers={args.workers} wall={wall_seconds:.2f}s docs/sec={docs_per_sec:.1f} "
          f"MB/sec={mb_per_sec:.3f} peak_rss={peak_rss_kb}KB peak_worker_rss={peak_worker_rss_kb}KB "
          f"index_bytes={index_bytes}", file=sys.stderr)
    print(f"Results appended to {csv_path}", file=sys.stderr)


if __name__ == "__main__":
    main()
