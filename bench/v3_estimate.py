"""A term index's bytes as format v2 lays them out, field by field (computed from the grammar and
checked against the file), and as each change considered for a v3 would: the plan's numbers.

    v3_estimate.py INDEX [--terms N]      (every term by default)
"""
import argparse, collections, json, os, sys
from nind import _native as native


def ulat(value):
    # EntierULat / EntierULat64 (v2 grammar): 1 to 5 bytes below 2^32, 9 above
    for bytes_nb, limit in ((1, 1 << 7), (2, 1 << 14), (3, 1 << 21), (4, 1 << 28), (5, 1 << 32)):
        if value < limit:
            return bytes_nb
    return 9


parser = argparse.ArgumentParser()
parser.add_argument("index")
parser.add_argument("--terms", type=int, default=None)
args = parser.parse_args()
base = os.path.join(args.index, open(os.path.join(args.index, "corpus.CURRENT")).read().strip(), "corpus")
lexicon = native.NindLexiconIndex(base, is_writer=False)
term_index = native.NindTermIndex(base, is_writer=False, lexicon_identification=lexicon.get_identification(),
                                  specifics_number=0)
words = lexicon.get_identification().lexicon_words_nb
size = os.path.getsize(base + ".nindtermindex")
v2 = collections.Counter()      # bytes by part, v2
v3 = collections.Counter()      # bytes by part, v3 (each change)
count = collections.Counter()
for ident in range(1, (args.terms or words) + 1):
    categories = term_index.get_term_directory(ident)
    if not categories:
        continue
    count["terms"] += 1
    v2["indirections"] += 10
    v3["indirections"] += 11                    # <tailleEntreje=11>: a kind byte (singleton, definition)
    if len(categories) != 1:
        count["several categories"] += 1
    c = categories[0]
    if c.blocks:
        count["blocked terms"] += 1
        count["blocks"] += len(c.blocks)
        count["blocked postings"] += c.docs_nb
        header = 1 + 4 + 4 + 1 + 1 + ulat(c.frequency) + ulat(c.docs_nb) + ulat(len(c.blocks))
        v2["blocked headers"] += header
        v3["blocked headers"] += 1 + ulat(c.blocks[0].offset) + 1 + 1 + ulat(c.frequency) + ulat(c.docs_nb) + ulat(len(c.blocks))
        previous_last, directory = 0, header
        for b in c.blocks:
            first_last = ulat(b.first - previous_last) + ulat(b.last - b.first)
            impacts = b.impacts
            v2["descriptor ids"] += first_last
            v2["descriptor count"] += ulat(b.docs_nb)
            v2["descriptor max/min"] += ulat(b.max_frequency) + ulat(b.min_length)
            v2["descriptor offset/length"] += 8
            v2["descriptor impacts"] += (1 + sum(ulat(f) + ulat(l) for f, l in impacts)) if impacts else 0
            v3["descriptor ids"] += first_last
            v3["descriptor count"] += ulat(128 - b.docs_nb) if b.docs_nb <= 128 else 1 + ulat(b.docs_nb)
            v3["descriptor max/min"] += 0 if impacts else ulat(b.max_frequency) + ulat(b.min_length)
            v3["descriptor offset/length"] += ulat(b.length)
            v3["descriptor impacts"] += (1 + sum(ulat(f) + 1 for f, _ in impacts)) if impacts else 0
            directory += first_last + ulat(b.docs_nb) + ulat(b.max_frequency) + ulat(b.min_length) + 8 + (
                (1 + sum(ulat(f) + ulat(l) for f, l in impacts)) if impacts else 0)
            # a block's header: <flagBloc=73> <identifiantTerme> <catejgorie> <nbreDocsBloc> <frejquenceMin>
            # <bitsEcarts> <bitsFrejquences>; v3 without the term, category and count (the descriptor's)
            v2["block headers"] += 1 + 4 + 1 + ulat(b.docs_nb) + 3      # (frejquenceMin: 1 byte, nearly always)
            v3["block headers"] += 1 + 3
            v2["block bodies"] += b.length - (1 + 4 + 1 + ulat(b.docs_nb) + 3)
            v3["block bodies"] += b.length - (1 + 4 + 1 + ulat(b.docs_nb) + 3)
            previous_last = b.last
        count["directory as computed"] += directory
        count["directory in the file"] += c.blocks[0].offset
    else:
        postings = c.inline_postings
        count["inline terms"] += 1
        count["inline postings"] += len(postings)
        v2["inline headers"] += 1 + 4 + 4 + 1 + 1 + ulat(c.frequency) + ulat(c.docs_nb) + 1
        previous, body2, body3 = 0, 0, 0
        for p in postings:
            gap = p.ident - previous
            body2 += ulat(gap) + ulat(p.frequency)
            body3 += ulat(2 * gap + (p.frequency == 1)) + (ulat(p.frequency) if p.frequency != 1 else 0)
            previous = p.ident
        v2["inline postings"] += body2
        if len(postings) == 1 and postings[0].ident < 1 << 48:
            count["singletons"] += 1                            # in the indirection: no definition
        else:
            v3["inline headers"] += 1 + 1 + ulat(c.docs_nb)
            v3["inline postings"] += body3
total2, total3 = sum(v2.values()), sum(v3.values())
print(json.dumps({"file": size, "terms": count["terms"], "counts": count}, default=int))
print("file %.0f MB; v2 as computed %.0f MB (the rest: specifics, room, holes)" % (size / 1e6, total2 / 1e6))
print("directories: computed %.0f MB, in the file %.0f MB" % (count["directory as computed"] / 1e6,
                                                            count["directory in the file"] / 1e6))
print("%-26s %10s %10s %10s" % ("part", "v2 MB", "v3 MB", "saved"))
for part in sorted(set(v2) | set(v3), key=lambda p: -v2[p]):
    print("%-26s %10.0f %10.0f %10.0f" % (part, v2[part] / 1e6, v3[part] / 1e6, (v2[part] - v3[part]) / 1e6))
print("%-26s %10.0f %10.0f %10.0f (%.1f%% of the file)" % ("total", total2 / 1e6, total3 / 1e6,
                                                         (total2 - total3) / 1e6, 100 * (total2 - total3) / size))
