"""High-level nind frontend: index text files and search them with BM25.

This module is the modern entry point for the ``nind`` package: unlike
:mod:`~nind.NindFile`/:mod:`~nind.NindPadFile` and the ``Nind*index``
classes (which only *read* the nind binary formats, as ergonomic Python
wrappers over the ``nind._native`` bindings where available - see the
package's ``CLAUDE.md``), :class:`NindIndexer` here is the only Python code
that *writes* ``.nindlexiconindex``/``.nindtermindex``/``.nindlocalindex``
files, and :class:`NindEngine` provides a simple BM25 search API built on
top of the read-only classes.

Typical usage::

    from nind.nind_engine import NindIndexer, NindEngine

    NindIndexer("indices", prefix="corpus").index_files(["a.py", "b.py"])
    engine = NindEngine("indices")
    for doc_id, score in engine.search("def foo"):
        print(doc_id, score)
"""
import contextlib
import math
import os
import re
import shutil
import tempfile
import threading
import time
from array import array
from collections import Counter, defaultdict
from concurrent.futures import ProcessPoolExecutor, ThreadPoolExecutor
from .NindLexiconindex import NindLexiconindex
from .NindTermindex import NindTermindex
from .NindLocalindex import NindLocalindex
from ._index_store import IndexStore
try:
    from . import _native as native
except ImportError:
    # nind._native is a compiled extension: not available when introspecting
    # the pure-Python source without building it (e.g. Sphinx autodoc, see
    # docs/conf.py). NindIndexer.index_files still requires it - only module
    # import is tolerant.
    native = None

def _pool_worker_init():
    """Initializer of NindIndexer's worker processes: exit when the parent dies.

    Otherwise a killed indexer leaves its workers behind, idle forever.
    """
    parent = os.getppid()

    def watch():
        while os.getppid() == parent:
            time.sleep(0.5)
        os._exit(1)

    threading.Thread(target=watch, daemon=True).start()


class _Snapshot:
    """One opened generation of an index: every file from the same build."""

    def __init__(self, base):
        # every file is opened here, while the generation can't be deleted (see _index_store)
        self.lexicon = NindLexiconindex(base + ".nindlexiconindex")
        lexicon_identification = self.lexicon.donneIdentification()
        self.term_index = NindTermindex(base + ".nindtermindex", lexicon_identification)
        self.local_index = NindLocalindex(base + ".nindlocalindex", lexicon_identification)
        self.doc_ids = [ext_id for ext_id, _ in self.local_index.donneidentifiantsExternes()]
        self.total_docs = len(self.doc_ids)
        total_len = sum(self.doc_len(doc_id) for doc_id in self.doc_ids)
        self.avg_doc_len = total_len / self.total_docs if self.total_docs else 0

    def term_id(self, term):
        return self.lexicon.donneIdentifiant([term])

    def term_defs(self, term):
        term_id = self.term_id(term)
        return self.term_index.donneListeTermesCG(term_id) if term_id else []

    def tf(self, term, doc_id):
        for term_cg in self.term_defs(term):
            for document in term_cg.documents:
                if document.ident == doc_id:
                    return document.frequency
        return 0

    def df(self, term):
        return sum(len(term_cg.documents) for term_cg in self.term_defs(term))

    def doc_len(self, doc_id):
        return sum(len(term.localisation) for term in self.local_index.donneListeTermes(doc_id))


class NindEngine:
    """BM25 search engine on top of a nind index produced by :class:`NindIndexer`.

    Opens the three index files (lexicon, term, local) for a corpus and
    exposes term/document-frequency lookups plus a ready-to-use
    :meth:`search`. Read-only: to build the index files this reads, use
    :class:`NindIndexer`.

    Safe to use while the index is being rebuilt, and from several threads:
    each call answers from one complete build of the index, and switches to
    a newer build as soon as one is published - nothing to reopen or close.
    Calls from several threads are serialized.
    """

    def __init__(self, index_dir, prefix=None):
        """Open the index in ``index_dir``.

        :param index_dir: directory the index was written into by
            :class:`NindIndexer` (or containing the flat
            ``.nindlexiconindex``/``.nindtermindex``/``.nindlocalindex``
            files of an index written by an older version).
        :param prefix: the index's filename prefix; only needed when
            ``index_dir`` holds several indexes.
        :raises FileNotFoundError: if there is no index in ``index_dir``.
        :raises ValueError: if there are several and no ``prefix`` is given.
        """
        self.index_dir = index_dir
        self._store = IndexStore(index_dir, prefix or IndexStore.find_prefix(index_dir))
        self._lock = threading.RLock()
        self._depth = 0
        self._snapshot, self._version = self._store.open_current(_Snapshot)

    @contextlib.contextmanager
    def _session(self):
        # outermost public call: switch to a newly published build, then answer from one snapshot
        with self._lock:
            self._depth += 1
            try:
                if self._depth == 1 and self._store.current_version() != self._version:
                    self._snapshot, self._version = self._store.open_current(_Snapshot)
                yield self._snapshot
            finally:
                self._depth -= 1

    @property
    def lexicon(self):
        """The :class:`~nind.NindLexiconindex.NindLexiconindex` of the current build."""
        with self._session() as snapshot:
            return snapshot.lexicon

    @property
    def term_index(self):
        """The :class:`~nind.NindTermindex.NindTermindex` of the current build."""
        with self._session() as snapshot:
            return snapshot.term_index

    @property
    def local_index(self):
        """The :class:`~nind.NindLocalindex.NindLocalindex` of the current build."""
        with self._session() as snapshot:
            return snapshot.local_index

    @property
    def total_docs(self):
        """Number of documents in the index."""
        with self._session() as snapshot:
            return snapshot.total_docs

    @property
    def avg_doc_len(self):
        """Average document length (in tokens) across the corpus."""
        with self._session() as snapshot:
            return snapshot.avg_doc_len

    def tokenize(self, text):
        """Split source-code-like text into tokens, splitting camelCase/snake_case identifiers into subwords.

        Used both here (query tokenization for :meth:`search`) and by
        :class:`NindIndexer` (corpus tokenization at indexing time) - the
        two must stay in sync for search results to be meaningful.

        :param text: the text to tokenize.
        :return: a list of lowercase-preserving token strings.
        """
        tokens = re.split(r'[^a-zA-Z0-9_]', text)
        refined_tokens = []
        for t in tokens:
            if not t: continue
            parts = re.findall(r'[A-Z]?[a-z]+|[A-Z]+(?=[A-Z][a-z]|\d|\W|$)|\d+', t)
            if parts:
                refined_tokens.extend(parts)
            else:
                refined_tokens.append(t)
        return refined_tokens

    def get_term_id(self, term):
        """Look up a term's internal lexicon identifier.

        :param term: the (single, non-compound) term.
        :return: its identifier, or ``0`` if the term is not in the lexicon.
        """
        with self._session() as snapshot:
            return snapshot.term_id(term)

    def get_tf(self, term, doc_id):
        """Get the raw term frequency of ``term`` in a specific document.

        :param term: the term to look up.
        :param doc_id: the document's external identifier.
        :return: the number of occurrences (summed across grammatical
            categories), or ``0`` if the term is unknown or absent from
            that document.
        """
        with self._session() as snapshot:
            return snapshot.tf(term, doc_id)

    def get_df(self, term):
        """Get the document frequency of ``term`` (how many documents contain it).

        :param term: the term to look up.
        :return: the number of documents containing the term, or ``0`` if
            it is unknown.
        """
        with self._session() as snapshot:
            return snapshot.df(term)

    def get_doc_len(self, doc_id):
        """Get the length (total token occurrences) of a document.

        :param doc_id: the document's external identifier.
        :return: the document's length in tokens.
        """
        with self._session() as snapshot:
            return snapshot.doc_len(doc_id)

    def bm25_score(self, query, doc_id, k1=1.5, b=0.75):
        """Compute the Okapi BM25 relevance score of a document for a query.

        :param query: the query text (tokenized with :meth:`tokenize`).
        :param doc_id: the document's external identifier.
        :param k1: BM25 term-frequency saturation parameter.
        :param b: BM25 document-length normalization parameter.
        :return: the BM25 score (higher is more relevant); ``0.0`` if no
            query term occurs in the document.
        """
        with self._session() as snapshot:
            return self._bm25_score(snapshot, self.tokenize(query), doc_id, k1, b)

    def _bm25_score(self, snapshot, tokens, doc_id, k1, b):
        score = 0.0
        for term in tokens:
            tf = snapshot.tf(term, doc_id)
            if tf == 0: continue

            df = snapshot.df(term)
            idf = math.log((snapshot.total_docs - df + 0.5) / (df + 0.5) + 1.0)

            doc_len = snapshot.doc_len(doc_id)
            numerator = tf * (k1 + 1)
            denominator = tf + k1 * (1 - b + b * (doc_len / snapshot.avg_doc_len))

            score += idf * (numerator / denominator)

        return score

    def search(self, query, top_k=10):
        """Search the corpus with BM25 and return the top-scoring documents.

        :param query: the query text.
        :param top_k: maximum number of results to return.
        :return: a list of ``(doc_id, score)`` tuples, sorted by descending
            score, of length at most ``top_k`` (only documents with a
            positive score are included).
        """
        with self._session() as snapshot:
            tokens = self.tokenize(query)
            scores = []
            for doc_id in snapshot.doc_ids:
                score = self._bm25_score(snapshot, tokens, doc_id, 1.5, 0.75)
                if score > 0:
                    scores.append((doc_id, score))

        scores.sort(key=lambda x: x[1], reverse=True)
        return scores[:top_k]

class NindIndexer:
    """Builds a nind binary index (lexicon + term + local files) from a list of text files.

    The only Python *writer* for the nind index-family formats (see the
    module docstring): it builds them via ``nind._native``'s
    ``is_writer=True`` constructors.
    """

    def __init__(self, index_dir, prefix="corpus"):
        """Prepare an indexer that will write into ``index_dir``.

        Does not touch the filesystem until :meth:`index_files` is called.

        :param index_dir: directory the index files will be written into
            (must already exist).
        :param prefix: filename prefix shared by the three index files
            (e.g. ``"corpus"`` -> ``corpus.nindlexiconindex`` etc.).
        """
        self.index_dir = index_dir
        self.prefix = prefix
        # Do not initialize engine here; files may not exist yet
        self.engine = None

    def index_files(self, file_paths, workers=None, shards=1):
        """Tokenize ``file_paths`` and write the resulting corpus as a nind index.

        Each file becomes one document, identified externally by its
        0-based position in ``file_paths``. Replaces any existing index with
        the same prefix in ``index_dir``.

        Always safe: the new index is built apart and only published, at
        once, when complete. Until then, :class:`NindEngine` instances keep
        answering from the previous index (and switch to the new one by
        themselves); if indexing fails, the previous index stays in place.
        Several ``index_files`` on the same index wait for each other.

        Files are tokenized in parallel worker processes, then the term and
        local index files (independent of each other once the lexicon is
        written) are written concurrently, each by its own single writer.

        :param file_paths: list of paths to UTF-8 (or UTF-8-decodable, with
            errors ignored) text files to index.
        :param workers: number of processes tokenizing the files (default:
            ``os.cpu_count()``). ``1``, or a corpus of fewer than
            ``PARALLEL_MIN_FILES`` files, tokenizes in this process instead,
            since starting worker processes would then cost more than it saves.
        :param shards: number of independent indexes (each over a contiguous
            part of ``file_paths``) built in parallel processes, then merged
            into this one by ``nind._native.merge_indexes``. ``1`` builds the
            index directly. At most ``workers`` shards are built at a time.
        """
        if native is None:
            raise ImportError('nind._native compiled extension is not available (build it via "uv sync")')
        shards = max(1, min(shards, len(file_paths)))
        with IndexStore(self.index_dir, self.prefix).new_generation() as base:
            if shards > 1:
                self._index_sharded(file_paths, workers, shards, base)
            else:
                self._write_index(self._tokenize_files(file_paths, workers), 0, base)

    def _write_index(self, tokenized, first_doc_id, base):
        # writes the index files at base (a path without extension)
        # tokenized: (tokens, counts) per document, the first one being document first_doc_id
        global_lexicon = Counter()
        for _, counts in tokenized:
            global_lexicon.update(counts)
        term_to_id, lexicon_identification = self._write_lexicon(sorted(global_lexicon.keys()), base)

        inverted_index = defaultdict(list)
        for doc_id, (_, counts) in enumerate(tokenized, first_doc_id):
            for term, freq in counts.items():
                inverted_index[term_to_id[term]].append((doc_id, freq))

        # One writer per file: the native writers release the GIL while they build and write.
        with ThreadPoolExecutor(max_workers=2) as pool:
            term_future = pool.submit(self._write_term_index, inverted_index, lexicon_identification, base)
            local_future = pool.submit(self._write_local_index, [tokens for tokens, _ in tokenized],
                                       term_to_id, lexicon_identification, first_doc_id, base)
            term_future.result()
            local_future.result()

    def _index_sharded(self, file_paths, workers, shards, base):
        if workers is None:
            workers = os.cpu_count() or 1
        # contiguous parts, so that each shard's documents keep their global ids
        bounds = [len(file_paths) * i // shards for i in range(shards + 1)]
        # inside the new generation, so they go away with it if anything fails
        shard_dir = tempfile.mkdtemp(prefix=".shards-", dir=os.path.dirname(base))
        try:
            shard_bases = [os.path.join(shard_dir, "shard%d" % i) for i in range(shards)]
            with ProcessPoolExecutor(max_workers=max(1, min(workers, shards)), initializer=_pool_worker_init) as pool:
                futures = [pool.submit(self._build_shard, shard_bases[i], file_paths[bounds[i]:bounds[i + 1]], bounds[i])
                           for i in range(shards)]
                for future in futures:
                    future.result()
            native.merge_indexes(shard_bases, base)
        finally:
            shutil.rmtree(shard_dir, ignore_errors=True)

    def _build_shard(self, shard_base, file_paths, first_doc_id):
        # Runs in a worker process (self is a copy: keeps an overridden tokenizer).
        self._write_index([self._tokenize_file(path) for path in file_paths], first_doc_id, shard_base)

    #: Below this many files, :meth:`index_files` tokenizes in-process.
    PARALLEL_MIN_FILES = 64

    def _tokenize_files(self, file_paths, workers):
        if workers is None:
            workers = os.cpu_count() or 1
        workers = min(workers, len(file_paths))
        if workers <= 1 or len(file_paths) < self.PARALLEL_MIN_FILES:
            return [self._tokenize_file(path) for path in file_paths]
        # Chunks amortize the inter-process round trips; results keep file_paths order.
        chunksize = max(1, len(file_paths) // (workers * 4))
        with ProcessPoolExecutor(max_workers=workers, initializer=_pool_worker_init) as pool:
            return list(pool.map(self._tokenize_file, file_paths, chunksize=chunksize))

    def _tokenize_file(self, path):
        # Runs in a worker process: also counts there, so the parent doesn't have to.
        with open(path, 'r', encoding='utf-8', errors='ignore') as f:
            tokens = self._default_tokenize(f.read())
        return tokens, Counter(tokens)

    def _default_tokenize(self, text):
        tokens = re.split(r'[^a-zA-Z0-9_]', text)
        refined = []
        for t in tokens:
            if not t: continue
            parts = re.findall(r'[A-Z]?[a-z]+|[A-Z]+(?=[A-Z][a-z]|\d|\W|$)|\d+', t)
            refined.extend(parts if parts else [t])
        return refined

    ########################################################################
    # .nindlexiconindex : one simple word per term, ids assigned by the writer.
    ########################################################################
    def _write_lexicon(self, terms, base):
        lexicon_writer = native.NindLexiconIndex(base, is_writer=True,
                                                   indirection_bloc_size=max(1, len(terms)))
        term_to_id = {}
        for term in terms:
            term_to_id[term] = lexicon_writer.add_word([term])
        return term_to_id, lexicon_writer.get_identification()

    ########################################################################
    # .nindtermindex : directly indexed by identifiant terme.
    ########################################################################
    def _write_term_index(self, inverted_index, lexicon_identification, base):
        max_term_id = max(inverted_index) if inverted_index else 0
        term_writer = native.NindTermIndex(base, is_writer=True,
                                            lexicon_identification=lexicon_identification,
                                            specifics_number=0,
                                            indirection_bloc_size=max_term_id + 1)
        # flat buffers: term i's postings are doc_ids/freqs[ends[i-1]:ends[i]], already in
        # increasing doc_id order (built by iterating documents in order)
        idents, ends, doc_ids, freqs = array('I'), array('I'), array('I'), array('I')
        for tid in sorted(inverted_index):
            postings = inverted_index[tid]
            idents.append(tid)
            doc_ids.extend(doc_id for doc_id, _ in postings)
            freqs.extend(freq for _, freq in postings)
            ends.append(len(doc_ids))
        term_writer.set_term_defs_arrays(idents, ends, doc_ids, freqs, lexicon_identification)

    ########################################################################
    # .nindlocalindex : directly indexed by identifiant document externe
    # (la traduction externe <-> interne est faite en interne par le C++).
    ########################################################################
    def _write_local_index(self, corpus_tokens, term_to_id, lexicon_identification, first_doc_id, base):
        local_writer = native.NindLocalIndex(base, is_writer=True,
                                              lexicon_identification=lexicon_identification,
                                              indirection_bloc_size=len(corpus_tokens) + 1)
        # one call per chunk of documents: the native writer builds and writes a chunk with
        # the GIL released while this thread is flattening the next one
        for first in range(0, len(corpus_tokens), self.LOCAL_CHUNK_DOCS):
            idents, ends, term_ids, positions = array('I'), array('I'), array('I'), array('I')
            for index in range(first, min(first + self.LOCAL_CHUNK_DOCS, len(corpus_tokens))):
                tokens = corpus_tokens[index]
                idents.append(first_doc_id + index)
                term_ids.extend(term_to_id[token] for token in tokens)
                positions.extend(range(len(tokens)))
                ends.append(len(term_ids))
            lengths = array('I', [1]) * len(term_ids)
            local_writer.set_local_defs_arrays(idents, ends, term_ids, positions, lengths,
                                               lexicon_identification)

    #: Number of documents per native call in :meth:`_write_local_index`.
    LOCAL_CHUNK_DOCS = 1024
