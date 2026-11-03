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
    with NindIndexer("indices", prefix="corpus").update() as update:
        update.add_file("c.py")             # incremental: add, replace, remove
    engine = NindEngine("indices")
    for doc_id, score in engine.search("def foo"):
        print(doc_id, score)
"""
import contextlib
import json
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
from ._index_store import IndexStore, INDEX_EXTENSIONS
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

class _CancelUpdate(Exception):
    """Raised into the generation of an update that changes nothing, so that it isn't published."""


def _check_doc_id(doc_id):
    import operator
    doc_id = operator.index(doc_id)
    if not 0 <= doc_id <= 0xFFFFFFFF:
        raise ValueError("document id out of range (0 .. 2**32-1): %d" % doc_id)
    return doc_id


class IndexUpdate:
    """One batch of incremental changes to an index, published at once when it ends.

    Obtained from :meth:`NindIndexer.update` and used as a context manager::

        with indexer.update() as update:
            for path in paths:
                doc_id = update.add_file(path)
            update.remove(3)

    Documents can be added (or replaced: adding an id the index already has
    replaces that document) and removed; the index may not exist yet. Nothing
    is visible until the ``with`` block ends without an exception: the
    changes are then published in one step, :class:`NindEngine` instances
    switching to them by themselves. If the block raises, the index is left
    as it was. An update holds the index's writer lock from start to end:
    other updates and :meth:`NindIndexer.index_files` on the same index wait.

    The published index is copied when the update starts, so an update costs
    at least a copy of the index files: group many documents in one update
    rather than making one update per document on a large index.

    Not thread-safe: use an update from one thread at a time.
    """

    #: Hash buckets of the lexicon of a new index (at least).
    LEXICON_MIN_BUCKETS = 4096
    #: Above this many words per hash bucket, the lexicon is rebuilt with twice
    #: as many buckets as words (the bucket count is fixed in the file).
    LEXICON_MAX_LOAD = 4
    #: Entries in each new indirection block of the term and local files (at least).
    INDIRECTION_BLOC_MIN = 4096

    def __init__(self, indexer):
        self._indexer = indexer
        self._stack = None
        self._writers = None

    # -- context ------------------------------------------------------------
    def __enter__(self):
        if native is None:
            raise ImportError('nind._native compiled extension is not available (build it via "uv sync")')
        if self._stack is not None:
            raise RuntimeError("this update is already in progress")
        store = IndexStore(self._indexer.index_dir, self._indexer.prefix)
        with contextlib.ExitStack() as stack:
            self._base = stack.enter_context(store.new_generation())
            self._existing = set()
            self._with_retrolexicon = False
            self._is_new = True
            source = store.published_base()
            if source is not None:
                # the published generation is never modified: work on a copy of it
                for extension in INDEX_EXTENSIONS:
                    if os.path.exists(source + extension):
                        shutil.copyfile(source + extension, self._base + extension)
                self._with_retrolexicon = os.path.exists(self._base + ".nindretrolexicon")
                self._is_new = False
                lexicon = native.NindLexiconIndex(self._base, is_writer=False)
                local_index = native.NindLocalIndex(self._base, is_writer=False,
                                                    lexicon_identification=lexicon.get_identification())
                self._existing = set(local_index.get_doc_idents())
                del local_index, lexicon
            self._stack = stack.pop_all()
        self._pending = {}      # doc id -> its tokens, or None to remove it
        self._next_id = max(self._existing) + 1 if self._existing else 0
        return self

    def __exit__(self, exc_type, exc, tb):
        stack, self._stack = self._stack, None
        if exc_type is not None:
            self._close_writers()
            return stack.__exit__(exc_type, exc, tb)
        try:
            if not self._pending:
                raise _CancelUpdate()       # nothing changes: nothing to publish
            self._commit()
            self._close_writers()
            self._compact_lexicon()
        except BaseException as error:
            self._close_writers()
            try:
                if not stack.__exit__(type(error), error, error.__traceback__):
                    raise
            except _CancelUpdate:
                pass
            return False
        stack.close()
        return False

    # -- changes ------------------------------------------------------------
    def add_file(self, path, doc_id=None):
        """Index the text file ``path`` as one document.

        :param path: path to a UTF-8 (or UTF-8-decodable, with errors
            ignored) text file.
        :param doc_id: the document's identifier; replaces the document with
            this id if there is one. Default: one more than the highest id
            in the index or in this update (``0`` for the first document).
        :return: the document's identifier.
        """
        tokens, _ = self._indexer._tokenize_file(path)
        return self.add_tokens(tokens, doc_id)

    def add_text(self, text, doc_id=None):
        """Index ``text`` as one document; see :meth:`add_file`."""
        return self.add_tokens(self._indexer._default_tokenize(text), doc_id)

    def add_tokens(self, tokens, doc_id=None):
        """Index an already tokenized document; see :meth:`add_file`.

        A document without any token isn't stored (an existing document with
        that id is removed).
        """
        self._check_active()
        doc_id = self._next_id if doc_id is None else _check_doc_id(doc_id)
        self._pending[doc_id] = list(tokens)
        self._next_id = max(self._next_id, doc_id + 1)
        return doc_id

    def remove(self, doc_id):
        """Remove the document ``doc_id`` from the index.

        :return: ``True`` if there was such a document (in the index or
            added earlier in this update), ``False`` otherwise.
        """
        self._check_active()
        doc_id = _check_doc_id(doc_id)
        if doc_id in self._existing:
            found = self._pending.get(doc_id, True) is not None
            self._pending[doc_id] = None
            return found
        return self._pending.pop(doc_id, None) is not None

    def _check_active(self):
        if self._stack is None:
            raise RuntimeError("an IndexUpdate must be used inside a 'with' block")

    # -- writing ------------------------------------------------------------
    def _commit(self):
        pending = self._pending
        vocabulary = sorted({token for tokens in pending.values() if tokens for token in tokens})
        lexicon, term_index, local_index = self._open_writers(len(vocabulary), len(pending))
        # the previous postings of every replaced or removed document go
        removed = defaultdict(set)
        for doc_id in pending:
            if doc_id in self._existing:
                for term_id in local_index.get_term_idents(doc_id) or ():
                    removed[term_id].add(doc_id)
        # term/local files are opened with the lexicon's identification: they're written after it
        term_to_id = {term: lexicon.add_word([term]) for term in vocabulary}
        identification = lexicon.get_identification()
        added = defaultdict(dict)
        for doc_id, tokens in pending.items():
            for term, freq in Counter(tokens or ()).items():
                added[term_to_id[term]][doc_id] = freq
        doc_ids = sorted(pending)
        with ThreadPoolExecutor(max_workers=2) as pool:
            term_future = pool.submit(self._update_term_index, term_index, removed, added, identification)
            local_future = pool.submit(self._update_local_index, local_index, doc_ids, pending,
                                       term_to_id, identification)
            term_future.result()
            local_future.result()
        self._modulo = lexicon.get_modulo()
        self._words_nb = identification.lexicon_words_nb

    def _open_writers(self, vocabulary_size, docs_nb):
        lexicon = native.NindLexiconIndex(
            self._base, is_writer=True, with_retrolexicon=self._with_retrolexicon,
            indirection_bloc_size=max(self.LEXICON_MIN_BUCKETS, 2 * vocabulary_size),
            retro_indirection_bloc_size=max(self.INDIRECTION_BLOC_MIN, vocabulary_size + 1))
        self._writers = [lexicon]
        identification = lexicon.get_identification()
        # an existing file keeps its blocks; these sizes are for the blocks added from now on
        term_index = native.NindTermIndex(self._base, is_writer=True, lexicon_identification=identification,
                                          specifics_number=0,
                                          indirection_bloc_size=max(self.INDIRECTION_BLOC_MIN, vocabulary_size + 1))
        self._writers.append(term_index)
        local_index = native.NindLocalIndex(self._base, is_writer=True, lexicon_identification=identification,
                                            indirection_bloc_size=max(self.INDIRECTION_BLOC_MIN, docs_nb + 1))
        self._writers.append(local_index)
        return lexicon, term_index, local_index

    def _close_writers(self):
        # the native writers close their files when destroyed
        self._writers = None

    @staticmethod
    def _update_term_index(term_index, removed, added, identification):
        # every touched term is rewritten whole: previous postings, minus removed documents, plus added ones
        idents, ends, doc_ids, freqs = array('I'), array('I'), array('I'), array('I')
        for term_id in sorted(removed.keys() | added.keys()):
            postings = {}
            for term_cg in term_index.get_term_def(term_id) or ():
                for document in term_cg.documents:
                    postings[document.ident] = postings.get(document.ident, 0) + document.frequency
            for doc_id in removed.get(term_id, ()):
                postings.pop(doc_id, None)
            postings.update(added.get(term_id, {}))
            idents.append(term_id)
            for doc_id in sorted(postings):         # no postings left: the term is erased
                doc_ids.append(doc_id)
                freqs.append(postings[doc_id])
            ends.append(len(doc_ids))
        term_index.set_term_defs_arrays(idents, ends, doc_ids, freqs, identification)

    @staticmethod
    def _update_local_index(local_index, doc_ids, pending, term_to_id, identification):
        idents, ends, term_ids, positions = array('I'), array('I'), array('I'), array('I')
        for doc_id in doc_ids:
            tokens = pending[doc_id] or ()          # no tokens: the document is erased
            idents.append(doc_id)
            term_ids.extend(term_to_id[token] for token in tokens)
            positions.extend(range(len(tokens)))
            ends.append(len(term_ids))
        lengths = array('I', [1]) * len(term_ids)
        local_index.set_local_defs_arrays(idents, ends, term_ids, positions, lengths, identification)

    def _compact_lexicon(self):
        # the hash bucket count is fixed in the file: past LEXICON_MAX_LOAD words per bucket, the
        # index is rebuilt (term ids change, document ids don't) with twice as many buckets as words
        if self._words_nb <= self.LEXICON_MAX_LOAD * self._modulo:
            return
        rebuild_dir = tempfile.mkdtemp(prefix=".rebuild-", dir=os.path.dirname(self._base))
        try:
            rebuilt = os.path.join(rebuild_dir, os.path.basename(self._base))
            native.merge_indexes([self._base], rebuilt, with_retrolexicon=self._with_retrolexicon,
                                 lexicon_indirection_bloc_size=2 * self._words_nb)
            for extension in INDEX_EXTENSIONS:
                if os.path.exists(rebuilt + extension):
                    os.replace(rebuilt + extension, self._base + extension)
        finally:
            shutil.rmtree(rebuild_dir, ignore_errors=True)


class NindIndexer:
    """Builds a nind binary index (lexicon + term + local files) from text files.

    The only Python *writer* for the nind index-family formats (see the
    module docstring): it builds them via ``nind._native``'s
    ``is_writer=True`` constructors.

    Two ways of indexing: :meth:`index_files` (re)builds the whole index from
    a list of files, in parallel; :meth:`update` (and its shortcuts
    :meth:`add_file`, :meth:`add_files`, :meth:`remove_documents`) changes an
    existing index - or creates one - incrementally.
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

    def index_jsonl(self, path, id_field="id", text_field="contents", limit=None, workers=None):
        """Index a JSON Lines corpus (one document per line) as a nind index.

        The format read by pyserini's ``JsonCollection``: each non-blank line
        is a JSON object such as ``{"id": "d49309", "contents": "..."}``.
        The ``n``-th record becomes document ``n`` (0-based), as the ``n``-th
        file does in :meth:`index_files`; the records' own identifiers are
        returned, in document order, for the caller to map results back.
        Replaces any existing index, with the same safety guarantees as
        :meth:`index_files`.

        The file is parsed and tokenized in parallel worker processes, each
        reading its own line-aligned part of it.

        :param path: path to the UTF-8 ``.jsonl`` file.
        :param id_field: name of the field holding a record's identifier.
        :param text_field: name of the field holding the text to index.
        :param limit: index only the first ``limit`` records (default: all).
        :param workers: number of processes parsing and tokenizing the file
            (default: ``os.cpu_count()``); ``1``, or a file smaller than
            ``JSONL_PARALLEL_MIN_BYTES``, works in this process.
        :return: the ``id_field`` value of every indexed record: element
            ``n`` is the identifier of document ``n``.
        :raises ValueError: if a line isn't a JSON object with both fields.
        """
        if native is None:
            raise ImportError('nind._native compiled extension is not available (build it via "uv sync")')
        if limit is not None and limit < 0:
            raise ValueError("limit must be positive or None")
        end = self._jsonl_end(path, limit)
        if workers is None:
            workers = os.cpu_count() or 1
        if workers <= 1 or end < self.JSONL_PARALLEL_MIN_BYTES:
            parts = [self._tokenize_jsonl_range(path, 0, end, id_field, text_field)]
        else:
            ranges = self._jsonl_ranges(path, end, workers * 4)
            with ProcessPoolExecutor(max_workers=min(workers, len(ranges)), initializer=_pool_worker_init) as pool:
                parts = list(pool.map(self._tokenize_jsonl_range, [path] * len(ranges),
                                      [start for start, _ in ranges], [stop for _, stop in ranges],
                                      [id_field] * len(ranges), [text_field] * len(ranges)))
        ids = [doc_ident for part in parts for doc_ident, _ in part]
        tokenized = [document for part in parts for _, document in part]
        with IndexStore(self.index_dir, self.prefix).new_generation() as base:
            self._write_index(tokenized, 0, base)
        return ids

    #: Below this many bytes of JSON Lines, :meth:`index_jsonl` works in-process.
    JSONL_PARALLEL_MIN_BYTES = 1 << 20

    @staticmethod
    def _jsonl_end(path, limit):
        # offset just past the limit-th non-blank line (the whole file without a limit)
        if limit is None:
            return os.path.getsize(path)
        records = 0
        with open(path, 'rb') as f:
            while records < limit:
                line = f.readline()
                if not line:
                    break
                if line.strip():
                    records += 1
            return f.tell()

    @staticmethod
    def _jsonl_ranges(path, end, pieces):
        # [start, stop) byte ranges covering [0, end), each starting at the beginning of a line
        bounds = [0]
        with open(path, 'rb') as f:
            for i in range(1, pieces):
                target = end * i // pieces
                if target <= bounds[-1]:
                    continue
                f.seek(target - 1)
                f.readline()            # finish the line target - 1 is in: the next one starts a range
                if f.tell() >= end:
                    break
                bounds.append(f.tell())
        bounds.append(end)
        return [(bounds[i], bounds[i + 1]) for i in range(len(bounds) - 1)]

    def _tokenize_jsonl_range(self, path, start, stop, id_field, text_field):
        # Runs in a worker process: [(record id, (tokens, counts))] of the lines in [start, stop).
        with open(path, 'rb') as f:
            f.seek(start)
            data = f.read(stop - start)
        documents = []
        for line_number, line in enumerate(data.split(b'\n')):
            if not line.strip():
                continue
            try:
                record = json.loads(line)
                doc_ident, text = record[id_field], record[text_field]
            except (ValueError, KeyError, TypeError) as error:
                raise ValueError("%s, at byte %d (+%d lines): not a JSON object with fields %r and %r: %s"
                                 % (path, start, line_number, id_field, text_field, error)) from None
            if not isinstance(text, str):
                raise ValueError("%s, at byte %d (+%d lines): field %r is not a string"
                                 % (path, start, line_number, text_field))
            tokens = self._default_tokenize(text)
            documents.append((doc_ident, (tokens, Counter(tokens))))
        return documents

    def update(self):
        """Start an incremental update of the index (created if there is none yet).

        :return: an :class:`IndexUpdate`, to use as a context manager; its
            changes are published together when the ``with`` block ends.
        """
        return IndexUpdate(self)

    def add_files(self, file_paths, doc_ids=None, workers=None):
        """Add (or replace) documents incrementally: one :meth:`update` for all of ``file_paths``.

        :param file_paths: paths of the text files to index, one document each.
        :param doc_ids: their identifiers (an existing document with the same
            id is replaced); default: new ids following the highest one.
        :param workers: processes tokenizing the files, as in :meth:`index_files`.
        :return: the documents' identifiers, in ``file_paths`` order.
        """
        if doc_ids is not None and len(doc_ids) != len(file_paths):
            raise ValueError("doc_ids and file_paths must have the same length")
        tokenized = self._tokenize_files(list(file_paths), workers)
        with self.update() as update:
            return [update.add_tokens(tokens, None if doc_ids is None else doc_ids[i])
                    for i, (tokens, _) in enumerate(tokenized)]

    def add_file(self, path, doc_id=None):
        """Add (or replace) one document incrementally; see :meth:`add_files`.

        Each call publishes a new version of the index, which costs a copy
        of the index: to add many files, use :meth:`add_files` or one
        :meth:`update` for all of them.

        :return: the document's identifier.
        """
        with self.update() as update:
            return update.add_file(path, doc_id)

    def remove_documents(self, doc_ids):
        """Remove documents from the index, in one :meth:`update`.

        :return: for each id, whether there was such a document.
        """
        with self.update() as update:
            return [update.remove(doc_id) for doc_id in doc_ids]

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
