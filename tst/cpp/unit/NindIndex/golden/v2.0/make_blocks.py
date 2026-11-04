# writes tst/cpp/unit/NindIndex/golden/v2.0/blocks.nindtermindex with nind 3.0.0 (blocks of flag 71):
# the definitions of NindFormatV2Test.cpp's blockedDefinitions(), terms 1..8
import sys
from nind import _native as n

def category(cg, docs_nb, start):
    docs, ident, total = [], start, 0
    for i in range(docs_nb):
        ident += 1 + (i * 7919) % 100000
        frequency = 1 + (i * 13) % 300
        docs.append(n.Document(ident, frequency))
        total += frequency
    term = n.TermCG(cg, total)
    term.documents = docs
    return term

sizes = [1, 127, 128, 129, 256, 300, 1000]
defs = [[category(0, size, 0x100000000 * i)] for i, size in enumerate(sizes)]
defs.append([category(1, 5, 3), category(2, 400, 0x7000000000), category(3, 2, 9), category(4, 129, 0)])
terms = n.NindTermIndex(sys.argv[1], is_writer=True, lexicon_identification=n.Identification(0, 0),
                        specifics_number=0, indirection_bloc_size=4)
for i, d in enumerate(defs):
    terms.set_term_def(i + 1, d, n.Identification(0, 0), [])
del terms
print("written")
