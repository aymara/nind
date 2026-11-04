# writes format v1 NindIndexer indexes (nind before format v2) for tst/py/data/v1
import os, shutil, sys
from nind.nind_engine import NindIndexer, NindEngine
TEXTS = ["the quick brown fox", "jumps over the lazy dog", "the dog sleeps", "a fox and a dog",
         "quick quick quick", "lazy afternoon in the sun", "brown bread and brown sugar", "sun and moon"]
out = sys.argv[1]
docs = os.path.join(out, "_docs")
os.makedirs(docs)
paths = []
for i, text in enumerate(TEXTS):
    p = os.path.join(docs, "doc%d.txt" % i)
    open(p, "w").write(text)
    paths.append(p)
for name, kwargs in (("with-local", {}), ("without-local", {"local_index": False})):
    d = os.path.join(out, name)
    os.makedirs(d)
    NindIndexer(d, prefix="corpus", **kwargs).index_files(paths, doc_names=["n%d" % i for i in range(len(TEXTS))])
    e = NindEngine(d)
    print(name, e.search("the dog", top_k=None))
shutil.rmtree(docs)
for root, _, files in os.walk(out):
    for f in files:
        if f.endswith(".lock") and f != "generation.lock":
            os.remove(os.path.join(root, f))
