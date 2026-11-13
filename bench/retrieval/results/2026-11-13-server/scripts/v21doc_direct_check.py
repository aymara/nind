"""A direct build of a prefix of MS MARCO v2.1 documents (over 16.7M terms), then verify_index and
a few searches: v21doc_direct_check.py CORPUS OUT_DIR"""
import json, os, sys, time
from nind import _native as native
from nind.nind_engine import NindIndexer, NindEngine
TOKENIZER = dict(split_camel_case=False, split_underscore=False, split_digits=False,   # prepare.py's
                 lowercase=False, min_length=1, max_length=254)

corpus, out = sys.argv[1:3]
os.makedirs(out)
start = time.time()
NindIndexer(out, tokenizer=TOKENIZER, local_index=False).index_jsonl(corpus, workers=8)
print("build %.0f s" % (time.time() - start), flush=True)
current = open(os.path.join(out, "corpus.CURRENT")).read().strip()
base = os.path.join(out, current, "corpus")
lexicon = native.NindLexiconIndex(base, is_writer=False)
print("words", lexicon.get_identification().lexicon_words_nb, flush=True)
start = time.time()
report = native.verify_index(base)
print("verify %.0f s: %r" % (time.time() - start, report), flush=True)
engine = NindEngine(out)
for query in ("androgen receptor define", "what is produced by muscle", "who recorded be my baby"):
    print(query, "->", engine.search(query, top_k=3, doc_names=True), flush=True)
