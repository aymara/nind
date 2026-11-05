"""The indexers compared, behind one interface.

Every system indexes the pre-tokenized corpus of prepare.py as is (its
tokens split on spaces) and scores with BM25 at the same k1 and b, so what is
compared is the index: build cost, size, query cost, update cost, and, as a
check that they agree, the rankings (not a quality claim: the analysis isn't
theirs).

    system = SYSTEMS[name](threads, k1, b)
    system.prepare()                         # untimed: e.g. start the JVM
    system.build(corpus_jsonl, index_dir)
    searcher = system.open(index_dir)        # .search(tokens, k) -> [(doc_id, score)], .stats()
    system.update(index_dir, add_jsonl, corpus_jsonl)   # sets system.update_mode

Known scoring differences, none changing the ranking much: Lucene and tantivy
leave out BM25's constant (k1 + 1) factor and store document lengths in one
byte (lossy); bm25s ("lucene" method) and nind use exact lengths. tantivy's
k1 and b are fixed (1.2, 0.75).
"""
import glob
import importlib.util
import json
import os
import shutil
import subprocess


def read_jsonl(path):
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.strip():
                yield json.loads(line)


def dir_size(path):
    if os.path.isfile(path):
        return os.path.getsize(path)
    return sum(os.path.getsize(p) for p in glob.glob(os.path.join(path, "**"), recursive=True)
               if os.path.isfile(p) and not os.path.islink(p))


class System:
    #: how the last update() went: "in place", or how it rebuilt the index
    update_mode = "in place"

    def __init__(self, threads, k1, b):
        self.threads, self.k1, self.b = threads, k1, b

    def prepare(self):
        """Load what open() and update() need, outside of their timing."""

    def index_path(self, index_dir):
        """What index size is measured on."""
        return index_dir


# -- nind ---------------------------------------------------------------------

class Nind(System):
    """nind through its Python frontend (NindIndexer / NindEngine)."""
    local_index = False
    prefix = "corpus"

    def build(self, corpus, index_dir):
        from nind.nind_engine import NindIndexer
        from prepare import TOKENIZER
        os.makedirs(index_dir, exist_ok=True)
        NindIndexer(index_dir, self.prefix, tokenizer=TOKENIZER, local_index=self.local_index) \
            .index_jsonl(corpus, workers=self.threads)

    def open(self, index_dir):
        from nind.nind_engine import NindEngine
        return _NindSearcher(NindEngine(index_dir, self.prefix), self.k1, self.b)

    def prepare(self):
        import nind.nind_engine  # noqa: F401

    def update(self, index_dir, add, corpus):
        from nind import nind_engine
        with nind_engine.NindIndexer(index_dir, self.prefix).update() as update:
            for doc in read_jsonl(add):
                update.add_tokens(doc["contents"].split(), name=doc["id"])
        # a tier of segments merged in this update: record it
        if update.stats is not None and update.stats.merges_nb:
            self.update_mode = "merged %d documents" % update.stats.merged_docs_nb

    def index_path(self, index_dir):
        with open(os.path.join(index_dir, self.prefix + ".CURRENT")) as f:
            return os.path.join(index_dir, f.read().strip())


class NindLocal(Nind):
    """nind with its local index (each document's terms and positions, as Amose reads them)."""
    local_index = True


class NindThreads(Nind):
    """nind searching each query with 4 threads (NindEngine(search_threads=4)): the same index and
    results as nind, a lower latency per query on idle cores. Not comparable with the others'
    single-thread latencies: Lucene can also split a query across threads, Anserini doesn't here."""
    search_threads = 4

    def open(self, index_dir):
        # nind's own index (WORK/dataset/nind/index), not a copy built for this system: the same
        # files, so that both are searched with the same page cache
        from nind.nind_engine import NindEngine
        shared = os.path.join(os.path.dirname(os.path.dirname(index_dir)), "nind", os.path.basename(index_dir))
        if os.path.exists(os.path.join(shared, self.prefix + ".CURRENT")):
            index_dir = shared
        return _NindSearcher(NindEngine(index_dir, self.prefix, search_threads=self.search_threads), self.k1, self.b)


class _NindSearcher:
    def __init__(self, engine, k1, b):
        self.engine, self.k1, self.b = engine, k1, b

    def search(self, tokens, k):
        return self.engine.search(" ".join(tokens), top_k=k, doc_names=True, k1=self.k1, b=self.b)

    def stats(self):
        return {"documents": self.engine.total_docs, "avgdl": self.engine.avg_doc_len}


# -- Lucene (Anserini) ----------------------------------------------------------

def anserini_jar():
    """The Anserini fatjar: $ANSERINI_JAR, else the one bundled with pyserini (not imported: it imports torch)."""
    if os.environ.get("ANSERINI_JAR"):
        return os.environ["ANSERINI_JAR"]
    spec = importlib.util.find_spec("pyserini")
    if spec is None:
        raise RuntimeError("set ANSERINI_JAR or install pyserini (pip install --no-deps pyserini)")
    jars = sorted(glob.glob(os.path.join(os.path.dirname(spec.origin), "resources", "jars", "anserini-*-fatjar.jar")))
    if not jars:
        raise RuntimeError("no Anserini fatjar in pyserini; set ANSERINI_JAR")
    return jars[-1]


def java_home():
    if os.environ.get("JAVA_HOME"):
        return os.environ["JAVA_HOME"]
    java = shutil.which("java")
    if java is None:
        raise RuntimeError("java not found (Anserini needs Java 21+)")
    return os.path.dirname(os.path.dirname(os.path.realpath(java)))


JVM_OPTIONS = ["--add-modules=jdk.incubator.vector"]
_jnius = None


def jnius():
    global _jnius
    if _jnius is None:
        os.environ["JAVA_HOME"] = java_home()
        import jnius_config
        jnius_config.add_options(*JVM_OPTIONS)
        jnius_config.set_classpath(anserini_jar())
        import jnius as module
        _jnius = module
    return _jnius


class Lucene(System):
    """Lucene, through Anserini: IndexCollection (-pretokenized: whitespace analyzer) to build,
    SimpleSearcher (bag of words, whitespace analyzer) to search, SimpleIndexer -append to update."""
    flags = []

    def build(self, corpus, index_dir):
        # Anserini parallelizes over input files: split the corpus into one file per thread
        parts_dir = index_dir + ".input"
        shutil.rmtree(parts_dir, ignore_errors=True)
        os.makedirs(parts_dir)
        with open(corpus, "rb") as f:
            total = sum(1 for _ in f)
        n = max(1, min(self.threads, total))
        parts = [open(os.path.join(parts_dir, "part-%03d.jsonl" % i), "wb") for i in range(n)]
        with open(corpus, "rb") as f:
            for i, line in enumerate(f):
                parts[i * n // total].write(line)      # contiguous parts, streamed
        for part in parts:
            part.close()
        try:
            subprocess.run([os.path.join(java_home(), "bin", "java"), *JVM_OPTIONS, "-cp", anserini_jar(),
                            "io.anserini.index.IndexCollection", "-collection", "JsonCollection",
                            "-input", parts_dir, "-index", index_dir,
                            "-generator", "DefaultLuceneDocumentGenerator",
                            "-threads", str(n), "-pretokenized", *self.flags],
                           check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        finally:
            shutil.rmtree(parts_dir, ignore_errors=True)

    def prepare(self):
        j = jnius()             # starts the JVM
        for name in ("io.anserini.search.SimpleSearcher", "io.anserini.index.SimpleIndexer",
                     "io.anserini.index.IndexReaderUtils", "org.apache.lucene.analysis.core.WhitespaceAnalyzer"):
            j.autoclass(name)

    def open(self, index_dir):
        j = jnius()
        searcher = j.autoclass("io.anserini.search.SimpleSearcher")(index_dir)
        searcher.set_analyzer(j.autoclass("org.apache.lucene.analysis.core.WhitespaceAnalyzer")())
        searcher.set_bm25(float(self.k1), float(self.b))
        reader = j.autoclass("io.anserini.index.IndexReaderUtils").getReader(index_dir)
        return _LuceneSearcher(searcher, reader)

    def update(self, index_dir, add, corpus):
        j = jnius()
        args = ["-index", index_dir, "-input", "", "-collection", "JsonCollection",
                "-threads", str(self.threads), "-pretokenized", "-append", *self.flags]
        indexer = j.autoclass("io.anserini.index.SimpleIndexer")(args)
        with open(add, encoding="utf-8") as f:
            indexer.addRawDocuments([line.rstrip("\n") for line in f if line.strip()])
        indexer.close()         # commits


class LucenePositions(Lucene):
    """Lucene also storing positions and document vectors (what nind's local index holds)."""
    flags = ["-storePositions", "-storeDocvectors"]


class _LuceneSearcher:
    def __init__(self, searcher, reader):
        self.searcher, self.reader = searcher, reader

    def search(self, tokens, k):
        return [(hit.docid, hit.score) for hit in self.searcher.search(" ".join(tokens), k)]

    def stats(self):
        documents = self.reader.numDocs()
        return {"documents": documents, "avgdl": self.reader.getSumTotalTermFreq("contents") / documents}


# -- bm25s ------------------------------------------------------------------------

class Bm25s(System):
    """bm25s ("lucene" BM25): a static, in-memory sparse matrix of precomputed scores;
    k1 and b are baked in at indexing; an update is a full rebuild."""
    update_mode = "full rebuild"

    def prepare(self):
        import bm25s  # noqa: F401

    def build(self, corpus, index_dir):
        import bm25s
        from bm25s.tokenization import Tokenized
        # token ids, not strings: each id is one shared int object (strings would take
        # tens of GB on MS MARCO)
        ids, token_ids, vocab, total = [], [], {}, 0
        for doc in read_jsonl(corpus):
            ids.append(doc["id"])
            tokens = doc["contents"].split()
            token_ids.append([vocab.setdefault(t, len(vocab)) for t in tokens])
            total += len(tokens)
        retriever = bm25s.BM25(method="lucene", k1=self.k1, b=self.b)
        retriever.index(Tokenized(ids=token_ids, vocab=vocab), show_progress=False)
        del token_ids
        retriever.save(index_dir)
        with open(os.path.join(index_dir, "bench_meta.json"), "w") as f:
            json.dump({"ids": ids, "avgdl": total / len(ids)}, f)

    def open(self, index_dir):
        import bm25s
        with open(os.path.join(index_dir, "bench_meta.json")) as f:
            meta = json.load(f)
        return _Bm25sSearcher(bm25s.BM25.load(index_dir), meta)

    def update(self, index_dir, add, corpus):
        shutil.rmtree(index_dir)
        self.build(corpus, index_dir)


class _Bm25sSearcher:
    def __init__(self, retriever, meta):
        self.retriever, self.ids, self.avgdl = retriever, meta["ids"], meta["avgdl"]
        self.vocab = retriever.vocab_dict

    def search(self, tokens, k):
        tokens = [t for t in tokens if t in self.vocab]       # bm25s refuses unknown tokens
        if not tokens:
            return []
        docs, scores = self.retriever.retrieve([tokens], k=min(k, len(self.ids)),
                                               show_progress=False, n_threads=0)
        return [(self.ids[d], float(s)) for d, s in zip(docs[0], scores[0]) if s > 0]

    def stats(self):
        return {"documents": len(self.ids), "avgdl": self.avgdl}


# -- tantivy ------------------------------------------------------------------------

class Tantivy(System):
    """tantivy (tantivy-py): "whitespace" tokenizer, term frequencies without positions."""
    HEAP = 1 << 30

    def __init__(self, threads, k1, b):
        if (k1, b) != (1.2, 0.75):
            raise ValueError("tantivy's BM25 is fixed at k1=1.2, b=0.75")
        super().__init__(threads, k1, b)

    def prepare(self):
        import tantivy  # noqa: F401

    @staticmethod
    def _schema():
        import tantivy
        builder = tantivy.SchemaBuilder()
        builder.add_text_field("id", stored=True, tokenizer_name="raw")
        builder.add_text_field("body", stored=False, tokenizer_name="whitespace", index_option="freq")
        return builder.build()

    def _add(self, index, path):
        import tantivy
        writer = index.writer(self.HEAP, self.threads)
        for doc in read_jsonl(path):
            writer.add_document(tantivy.Document(id=doc["id"], body=doc["contents"]))
        writer.commit()
        writer.wait_merging_threads()

    def build(self, corpus, index_dir):
        import tantivy
        os.makedirs(index_dir, exist_ok=True)
        self._add(tantivy.Index(self._schema(), path=index_dir), corpus)

    def open(self, index_dir):
        import tantivy
        index = tantivy.Index.open(index_dir)
        return _TantivySearcher(index, index.schema)

    def update(self, index_dir, add, corpus):
        import tantivy
        self._add(tantivy.Index.open(index_dir), add)


class _TantivySearcher:
    def __init__(self, index, schema):
        import tantivy
        self.tantivy, self.schema = tantivy, schema
        self.searcher = index.searcher()

    def search(self, tokens, k):
        t = self.tantivy
        query = t.Query.boolean_query([(t.Occur.Should, t.Query.term_query(self.schema, "body", token))
                                       for token in tokens])
        return [(self.searcher.doc(address)["id"][0], score)
                for score, address in self.searcher.search(query, k).hits]

    def stats(self):
        return {"documents": self.searcher.num_docs, "avgdl": None}


SYSTEMS = {"nind": Nind, "nind+local": NindLocal, "nind-4t": NindThreads, "lucene": Lucene, "lucene+pos": LucenePositions,
           "bm25s": Bm25s, "tantivy": Tantivy}
