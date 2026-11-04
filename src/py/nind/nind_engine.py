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
    NindIndexer("indices", prefix="docs").index_jsonl("corpus.jsonl")    # pyserini's format
    with NindIndexer("indices", prefix="corpus").update() as update:
        update.add_file("c.py")             # incremental: add, replace, remove
    engine = NindEngine("indices")
    for doc_id, score in engine.search("def foo"):
        print(doc_id, score)
"""
import contextlib
import copy
import json
import math
import os
import re
import shutil
import struct
import sys
import tempfile
import threading
import time
from array import array
from collections import Counter, defaultdict
from concurrent.futures import ProcessPoolExecutor, ThreadPoolExecutor
from .NindLexiconindex import NindLexiconindex
from .NindTermindex import NindTermindex
from .NindLocalindex import NindLocalindex
from ._index_store import IndexStore, INDEX_EXTENSIONS, META_EXTENSION, DOC_LENGTHS_EXTENSION, DOC_NAMES_EXTENSION
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


# ---------------------------------------------------------------------------
# Tokenization
# ---------------------------------------------------------------------------

#: Default tokenizer options of :class:`NindIndexer` (see its ``tokenizer`` argument).
TOKENIZER_DEFAULTS = {
    "split_camel_case": True,   # "fooBar" -> "foo" "Bar", "HTTPServer" -> "HTTP" "Server"
    "split_underscore": True,   # "foo_bar" -> "foo" "bar"
    "split_digits": True,       # "utf8" -> "utf" "8"
    "lowercase": False,         # ASCII letters only
    "min_length": 1,            # shorter tokens are dropped (in bytes)
    "max_length": 254,          # longer tokens are dropped (in bytes; a lexicon word is at most 254)
}

#: Tokenizer of indexes written before tokenizer options were stored with the
#: index: the original pure-Python tokenizer, kept to query and update them as built.
LEGACY_TOKENIZER = "legacy"


def _tokenizer_options(options):
    """Complete and check tokenizer options (``None``: the defaults)."""
    if options is None:
        return dict(TOKENIZER_DEFAULTS)
    if options == LEGACY_TOKENIZER:
        return LEGACY_TOKENIZER
    unknown = set(options) - set(TOKENIZER_DEFAULTS)
    if unknown:
        raise ValueError("unknown tokenizer options: %s" % ", ".join(sorted(unknown)))
    result = dict(TOKENIZER_DEFAULTS)
    result.update(options)
    for name in ("split_camel_case", "split_underscore", "split_digits", "lowercase"):
        result[name] = bool(result[name])
    result["min_length"], result["max_length"] = int(result["min_length"]), int(result["max_length"])
    if not 1 <= result["min_length"] <= result["max_length"] <= 254:
        raise ValueError("tokenizer options: need 1 <= min_length <= max_length <= 254")
    return result


_native_tokenizers = {}     # per process: options -> native.Tokenizer


def _native_tokenizer(options):
    key = tuple(sorted(options.items()))
    tokenizer = _native_tokenizers.get(key)
    if tokenizer is None:
        tokenizer = _native_tokenizers[key] = native.Tokenizer(native.TokenizerOptions(**options))
    return tokenizer


def _legacy_tokenize(text):
    tokens = re.split(r'[^a-zA-Z0-9_]', text)
    refined = []
    for t in tokens:
        if not t: continue
        parts = re.findall(r'[A-Z]?[a-z]+|[A-Z]+(?=[A-Z][a-z]|\d|\W|$)|\d+', t)
        refined.extend(parts if parts else [t])
    return refined


def _encodable(text):
    """``text``, its characters not encodable in UTF-8 (lone surrogates, which
    ``json.loads`` accepts as escapes) replaced with ``?``."""
    try:
        text.encode("utf-8")
        return text
    except UnicodeEncodeError:
        return text.encode("utf-8", "replace").decode("utf-8")


def _tokenize_text(options, text):
    """Tokens of ``text`` with tokenizer ``options`` (a dict, or LEGACY_TOKENIZER)."""
    if options == LEGACY_TOKENIZER:
        return _legacy_tokenize(text)
    return _native_tokenizer(options).tokenize(_encodable(text))


def _tokenize_batch(options, texts):
    tokenizer = _native_tokenizer(options)
    try:
        return tokenizer.tokenize_batch(texts)
    except (TypeError, UnicodeEncodeError):
        # a text not encodable in UTF-8: only then are the texts checked one by one
        return tokenizer.tokenize_batch([_encodable(text) for text in texts])


def _storable(token):
    # a lexicon word is 1 to 254 bytes long, in UTF-8
    try:
        return 0 < len(token) and len(token.encode("utf-8")) <= 254
    except UnicodeEncodeError:
        return False


def _chunk_from_token_lists(token_lists):
    """(vocabulary, term ids, ends) of documents given as token lists, as
    native.Tokenizer.tokenize_batch returns them; unstorable tokens are dropped."""
    local_ids, vocabulary = {}, []
    term_ids, ends = array('I'), array('I')
    for tokens in token_lists:
        for token in tokens:
            local_id = local_ids.get(token)
            if local_id is None:
                if not _storable(token):
                    continue
                local_id = local_ids[token] = len(vocabulary)
                vocabulary.append(token)
            term_ids.append(local_id)
        ends.append(len(term_ids))
    return vocabulary, term_ids.tobytes(), ends.tobytes()


def _uint_array(data):
    values = array('I')
    values.frombytes(data)
    return values


# ---------------------------------------------------------------------------
# Index metadata and document lengths, stored next to the nind files
# ---------------------------------------------------------------------------

def _write_meta(base, tokenizer_options, local_index):
    with open(base + META_EXTENSION, "w", encoding="utf-8") as f:
        json.dump({"format": 1, "tokenizer": tokenizer_options, "local_index": bool(local_index)}, f)


def _read_meta(base):
    """The metadata of the index at ``base`` (``None`` for an index written before it existed)."""
    try:
        with open(base + META_EXTENSION, "r", encoding="utf-8") as f:
            meta = json.load(f)
    except FileNotFoundError:
        return None
    if not isinstance(meta, dict) or meta.get("format") != 1:
        raise ValueError("Unsupported %s" % (base + META_EXTENSION))
    meta["tokenizer"] = _tokenizer_options(meta.get("tokenizer"))
    return meta


_DOC_LENGTHS_MAGIC = b"NINDDL01"


def _write_doc_lengths(base, lengths):
    """Write ``{document id: length in tokens}`` (documents of length 0 are left out)."""
    doc_ids = array('I', sorted(doc_id for doc_id, length in lengths.items() if length))
    values = array('I', (lengths[doc_id] for doc_id in doc_ids))
    if sys.byteorder == "big":
        doc_ids.byteswap()
        values.byteswap()
    with open(base + DOC_LENGTHS_EXTENSION, "wb") as f:
        f.write(_DOC_LENGTHS_MAGIC + struct.pack("<I", len(doc_ids)))
        f.write(doc_ids.tobytes())
        f.write(values.tobytes())


def _read_doc_lengths(base):
    """``{document id: length in tokens}`` (``None`` if the index has no such file)."""
    try:
        with open(base + DOC_LENGTHS_EXTENSION, "rb") as f:
            data = f.read()
    except FileNotFoundError:
        return None
    header = len(_DOC_LENGTHS_MAGIC) + 4
    if len(data) < header or not data.startswith(_DOC_LENGTHS_MAGIC):
        raise ValueError("Corrupt %s" % (base + DOC_LENGTHS_EXTENSION))
    (count,) = struct.unpack_from("<I", data, len(_DOC_LENGTHS_MAGIC))
    if len(data) != header + 8 * count:
        raise ValueError("Corrupt %s" % (base + DOC_LENGTHS_EXTENSION))
    doc_ids, values = array('I'), array('I')
    doc_ids.frombytes(data[header:header + 4 * count])
    values.frombytes(data[header + 4 * count:])
    if sys.byteorder == "big":
        doc_ids.byteswap()
        values.byteswap()
    return dict(zip(doc_ids, values))


def _write_doc_names(base, names):
    """Write ``{document id: name}`` (names: unique strings; other values are converted with str)."""
    doc_ids = sorted(names)
    native.NindDocNames.write(base + DOC_NAMES_EXTENSION, array('I', doc_ids),
                              [name if isinstance(name, str) else str(name) for name in map(names.__getitem__, doc_ids)])


def _read_doc_names(base):
    """The document names of the index at ``base`` (``None`` if it has none)."""
    if not os.path.exists(base + DOC_NAMES_EXTENSION):
        return None
    return native.NindDocNames(base + DOC_NAMES_EXTENSION)


def _local_index_lengths(base):
    # the document lengths of an index written before they were stored apart
    lexicon = native.NindLexiconIndex(base, is_writer=False)
    local_index = native.NindLocalIndex(base, is_writer=False, lexicon_identification=lexicon.get_identification())
    lengths = {}
    for doc_id in local_index.get_doc_idents():
        lengths[doc_id] = sum(len(term.localisation) for term in local_index.get_local_def(doc_id) or ())
    return lengths


class _Snapshot:
    """One opened generation of an index: every file from the same build."""

    def __init__(self, base):
        # every file is opened here, while the generation can't be deleted (see _index_store)
        self.lexicon = NindLexiconindex(base + ".nindlexiconindex")
        lexicon_identification = self.lexicon.donneIdentification()
        self.term_index = NindTermindex(base + ".nindtermindex", lexicon_identification)
        # an index built with local_index=False has none
        self.local_index = NindLocalindex(base + ".nindlocalindex", lexicon_identification) \
            if os.path.exists(base + ".nindlocalindex") else None
        meta = _read_meta(base)
        # an index written before tokenizer options were stored: the original tokenizer
        self.tokenizer_options = meta["tokenizer"] if meta is not None else LEGACY_TOKENIZER
        lengths = _read_doc_lengths(base)
        if lengths is not None:
            self.doc_ids = sorted(lengths)
        else:
            # written before document lengths were stored apart: read from the local index
            if self.local_index is None:
                raise FileNotFoundError("No document lengths nor local index for %s" % base)
            self.doc_ids = [ext_id for ext_id, _ in self.local_index.donneidentifiantsExternes()]
            lengths = {doc_id: sum(len(term.localisation) for term in self.local_index.donneListeTermes(doc_id))
                       for doc_id in self.doc_ids}
        self._doc_lengths = lengths
        self.total_docs = len(self.doc_ids)
        total_len = sum(lengths.values())
        self.avg_doc_len = total_len / self.total_docs if self.total_docs else 0
        self.scorer = native.Bm25Scorer(array('I', self.doc_ids), array('I', (lengths[d] for d in self.doc_ids)),
                                        self.avg_doc_len)
        self.doc_names = _read_doc_names(base)

    def search(self, terms, k1, b, top_k):
        # the scores of the documents containing the terms, by decreasing score (see native.Bm25Scorer)
        term_ids = [self.term_id(term) for term in terms]
        # the native term index reader behind self.term_index
        return self.scorer.search(self.term_index._native, term_ids, k1, b, top_k)

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
        return self._doc_lengths.get(doc_id, 0)


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
        """The :class:`~nind.NindLocalindex.NindLocalindex` of the current build
        (``None`` for an index built with ``local_index=False``)."""
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
        """Split source-code-like text into tokens, as the index's documents were.

        Uses the tokenizer options the index was built with (see
        :class:`NindIndexer`): queries must be tokenized like the corpus for
        search results to be meaningful.

        :param text: the text to tokenize.
        :return: the list of tokens.
        """
        if not hasattr(self, "_store"):
            return _tokenize_text(_tokenizer_options(None), text)
        with self._session() as snapshot:
            return _tokenize_text(snapshot.tokenizer_options, text)

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

    def doc_name(self, doc_id):
        """The name of a document: its identifier in the corpus (e.g. the ``id`` of its
        record for :meth:`NindIndexer.index_jsonl`).

        :param doc_id: the document's identifier in the index.
        :return: its name, or ``None`` if it has none.
        """
        with self._session() as snapshot:
            return snapshot.doc_names.name(doc_id) if snapshot.doc_names is not None else None

    def doc_id(self, name):
        """The identifier in the index of the document named ``name`` (see :meth:`doc_name`).

        :return: the document's identifier, or ``None`` if no document has that name.
        """
        with self._session() as snapshot:
            return snapshot.doc_names.doc_id(name) if snapshot.doc_names is not None else None

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

    def search(self, query, top_k=10, doc_names=False, k1=1.5, b=0.75):
        """Search the corpus with BM25 and return the top-scoring documents.

        :param query: the query text.
        :param top_k: maximum number of results to return.
        :param doc_names: return the documents' names (see :meth:`doc_name`;
            ``None`` for a document without name) instead of their
            identifiers in the index - like pyserini's hits' ``docid``.
        :param k1: BM25 term-frequency saturation parameter.
        :param b: BM25 document-length normalization parameter.
        :return: a list of ``(doc_id, score)`` tuples (``(name, score)``
            with ``doc_names``), sorted by descending score, of length at
            most ``top_k`` (only documents with a positive score are included).

        Only the documents containing a query term are scored, from the
        terms' postings lists (in native code): the cost depends on how many
        documents contain the query terms, not on the size of the corpus.
        Scores are those of :meth:`bm25_score`; equal scores keep the order
        of the documents in the index.
        """
        limit = top_k if isinstance(top_k, int) and top_k >= 0 else -1
        with self._session() as snapshot:
            results = snapshot.search(self.tokenize(query), k1, b, limit)
            if limit < 0:
                results = results[:top_k]
            if doc_names:
                names = snapshot.doc_names.names([doc_id for doc_id, _ in results]) \
                    if snapshot.doc_names is not None else [None] * len(results)
                results = [(name, score) for name, (_, score) in zip(names, results)]
        return results

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

    Documents are tokenized as the index's were (its stored tokenizer
    options); an index built without local index stays without (replacing
    or removing documents then reads every term of the index).

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
        indexer = self._indexer
        store = IndexStore(indexer.index_dir, indexer.prefix)
        with contextlib.ExitStack() as stack:
            self._base = stack.enter_context(store.new_generation())
            self._with_retrolexicon = False
            self._tokenizer_options = indexer.tokenizer_options
            self._local = indexer.local_index is not False
            self._lengths = {}
            source = store.published_base()
            if source is not None:
                # the published generation is never modified: work on a copy of it
                for extension in INDEX_EXTENSIONS:
                    if os.path.exists(source + extension):
                        shutil.copyfile(source + extension, self._base + extension)
                self._with_retrolexicon = os.path.exists(self._base + ".nindretrolexicon")
                # the index keeps the tokenizer and layout it was built with
                meta = _read_meta(self._base)
                options = meta["tokenizer"] if meta is not None else LEGACY_TOKENIZER
                if indexer._explicit_tokenizer and options != indexer.tokenizer_options:
                    raise ValueError("the index was built with other tokenizer options: %r" % (options,))
                local = os.path.exists(self._base + ".nindlocalindex")
                if indexer.local_index is not None and indexer.local_index != local:
                    raise ValueError("the index was built %s local index" % ("with" if local else "without"))
                self._tokenizer_options, self._local = options, local
                lengths = _read_doc_lengths(self._base)
                self._lengths = lengths if lengths is not None else _local_index_lengths(self._base)
            self._stack = stack.pop_all()
        # tokenizes like the index: the indexer, with the index's tokenizer options
        self._tokenizing_indexer = copy.copy(indexer)
        self._tokenizing_indexer.tokenizer_options = self._tokenizer_options
        self._existing = set(self._lengths)
        self._pending = {}      # doc id -> its tokens, or None to remove it
        self._next_id = max(self._existing) + 1 if self._existing else 0
        # document names: doc id -> name and back
        names = _read_doc_names(self._base)
        self._had_names = names is not None
        self._names = {}
        if names is not None:
            doc_ids, doc_names = names.entries()
            self._names = dict(zip(_uint_array(doc_ids), doc_names))
        self._ids_by_name = {name: doc_id for doc_id, name in self._names.items()}
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
    def add_file(self, path, doc_id=None, name=None):
        """Index the text file ``path`` as one document.

        :param path: path to a UTF-8 (or UTF-8-decodable, with errors
            ignored) text file.
        :param doc_id: the document's identifier; replaces the document with
            this id if there is one. Default: the document named ``name`` if
            there is one, else one more than the highest id in the index or
            in this update (``0`` for the first document).
        :param name: the document's name (see :meth:`NindEngine.doc_name`),
            unique in the index: adding a document with the name of another
            one replaces it (unless ``doc_id`` is another document: then
            ``ValueError``). Default: a replaced document keeps its name.
        :return: the document's identifier.
        """
        self._check_active()
        return self.add_tokens(self._tokenizing_indexer._tokenize_file(path), doc_id, name)

    def add_text(self, text, doc_id=None, name=None):
        """Index ``text`` as one document; see :meth:`add_file`."""
        self._check_active()
        return self.add_tokens(self._tokenizing_indexer._default_tokenize(text), doc_id, name)

    def add_tokens(self, tokens, doc_id=None, name=None):
        """Index an already tokenized document; see :meth:`add_file`.

        A document without any token isn't stored (an existing document with
        that id is removed). Tokens a lexicon can't store (empty, or longer
        than 254 bytes in UTF-8) are dropped.
        """
        self._check_active()
        if name is not None:
            name = name if isinstance(name, str) else str(name)
            owner = self._ids_by_name.get(name)
            if doc_id is None:
                doc_id = owner
            elif owner is not None and owner != _check_doc_id(doc_id):
                raise ValueError("another document (%d) is named %r" % (owner, name))
        doc_id = self._next_id if doc_id is None else _check_doc_id(doc_id)
        self._pending[doc_id] = [token for token in tokens if _storable(token)]
        self._next_id = max(self._next_id, doc_id + 1)
        if name is not None:
            self._set_name(doc_id, name)
        return doc_id

    def doc_id(self, name):
        """The identifier of the document named ``name`` (``None`` if there is none),
        changes of this update included."""
        self._check_active()
        return self._ids_by_name.get(name)

    def _set_name(self, doc_id, name):
        previous = self._names.pop(doc_id, None)
        if previous is not None:
            del self._ids_by_name[previous]
        if name is not None:
            self._names[doc_id] = name
            self._ids_by_name[name] = doc_id

    def remove(self, doc_id):
        """Remove the document ``doc_id`` from the index.

        :return: ``True`` if there was such a document (in the index or
            added earlier in this update), ``False`` otherwise.
        """
        self._check_active()
        doc_id = _check_doc_id(doc_id)
        self._set_name(doc_id, None)
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
        replaced = [doc_id for doc_id in pending if doc_id in self._existing]
        if replaced and local_index is not None:
            for doc_id in replaced:
                for term_id in local_index.get_term_idents(doc_id) or ():
                    removed[term_id].add(doc_id)
        elif replaced:
            # no local index: find their terms in the whole term index
            replaced = set(replaced)
            for term_id in range(1, lexicon.get_identification().lexicon_words_nb + 1):
                for term_cg in term_index.get_term_def(term_id) or ():
                    for document in term_cg.documents:
                        if document.ident in replaced:
                            removed[term_id].add(document.ident)
        # term/local files are opened with the lexicon's identification: they're written after it
        term_to_id = dict(zip(vocabulary, lexicon.add_simple_words(vocabulary)))
        identification = lexicon.get_identification()
        added = defaultdict(dict)
        for doc_id, tokens in pending.items():
            for term, freq in Counter(tokens or ()).items():
                added[term_to_id[term]][doc_id] = freq
        doc_ids = sorted(pending)
        with ThreadPoolExecutor(max_workers=2) as pool:
            futures = [pool.submit(self._update_term_index, term_index, removed, added, identification)]
            if local_index is not None:
                futures.append(pool.submit(self._update_local_index, local_index, doc_ids, pending,
                                           term_to_id, identification))
            for future in futures:
                future.result()
        for writer in self._writers:
            writer.end_bulk()
        self._modulo = lexicon.get_modulo()
        self._words_nb = identification.lexicon_words_nb
        for doc_id, tokens in pending.items():
            if tokens:
                self._lengths[doc_id] = len(tokens)
            else:
                self._lengths.pop(doc_id, None)
        _write_doc_lengths(self._base, self._lengths)
        _write_meta(self._base, self._tokenizer_options, self._local)
        if self._names or self._had_names:
            # the names of documents that are no longer stored (no tokens left) go too
            if os.path.exists(self._base + DOC_NAMES_EXTENSION):
                os.remove(self._base + DOC_NAMES_EXTENSION)
            _write_doc_names(self._base, {doc_id: name for doc_id, name in self._names.items()
                                          if doc_id in self._lengths})

    def _open_writers(self, vocabulary_size, docs_nb):
        # in bulk mode: nobody reads this generation before it is published
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
        local_index = None
        if self._local:
            local_index = native.NindLocalIndex(self._base, is_writer=True, lexicon_identification=identification,
                                                indirection_bloc_size=max(self.INDIRECTION_BLOC_MIN, docs_nb + 1))
            self._writers.append(local_index)
        for writer in self._writers:
            writer.begin_bulk()
        return lexicon, term_index, local_index

    def _close_writers(self):
        # the native writers close their files when destroyed (ending bulk mode if needed)
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

    Two ways of indexing: :meth:`index_files` (or :meth:`index_jsonl`)
    (re)builds the whole index, in parallel; :meth:`update` (and its
    shortcuts :meth:`add_file`, :meth:`add_files`, :meth:`remove_documents`)
    changes an existing index - or creates one - incrementally.

    Texts are tokenized by the native tokenizer (``nind._native.Tokenizer``),
    with the options given at construction, stored with the index so that
    :class:`NindEngine` tokenizes queries the same way. To tokenize
    differently, a subclass may override :meth:`_default_tokenize` (a
    function of a text returning its list of tokens): it is then used
    instead, in pure Python (slower).
    """

    def __init__(self, index_dir, prefix="corpus", tokenizer=None, local_index=None):
        """Prepare an indexer that will write into ``index_dir``.

        Does not touch the filesystem until it indexes.

        :param index_dir: directory the index files will be written into
            (must already exist).
        :param prefix: filename prefix shared by the index files
            (e.g. ``"corpus"`` -> ``corpus.nindlexiconindex`` etc.).
        :param tokenizer: tokenizer options, a dict overriding some of
            :data:`TOKENIZER_DEFAULTS`: ``split_camel_case``,
            ``split_underscore``, ``split_digits`` (each kind of split of
            identifiers is optional), ``lowercase``, ``min_length`` and
            ``max_length`` (in bytes; longer or shorter tokens are dropped).
            Default: the defaults for a new index, and the index's own options
            for an :meth:`update` (which refuses explicitly different ones).
        :param local_index: whether to write the local index (per document:
            its terms and their positions). The search engine doesn't need it
            (it only needs document lengths, stored apart), and it makes up
            most of an index's size and a good part of its build time; other
            readers of nind indexes (e.g. Amose) may need it. Default: with a
            local index for a new index, the index's own choice for an
            :meth:`update` (which refuses an explicitly different one).
        """
        self.index_dir = index_dir
        self.prefix = prefix
        self._explicit_tokenizer = tokenizer is not None
        self.tokenizer_options = _tokenizer_options(tokenizer)
        self.local_index = None if local_index is None else bool(local_index)
        # Do not initialize engine here; files may not exist yet
        self.engine = None

    def index_files(self, file_paths, workers=None, shards=1, doc_names=None):
        """Tokenize ``file_paths`` and write the resulting corpus as a nind index.

        Each file becomes one document, identified externally by its
        0-based position in ``file_paths``. Replaces any existing index with
        the same prefix in ``index_dir``.

        Always safe: the new index is built apart and only published, at
        once, when complete. Until then, :class:`NindEngine` instances keep
        answering from the previous index (and switch to the new one by
        themselves); if indexing fails, the previous index stays in place.
        Several ``index_files`` on the same index wait for each other.

        Files are tokenized in parallel worker processes, then the lexicon is
        written, then the term and local index files (independent of each
        other once the lexicon is written) are written concurrently, each by
        its own single writer, in bulk mode.

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
        :param doc_names: the documents' names (their identifiers in the
            corpus, unique), in ``file_paths`` order; :meth:`NindEngine.doc_name`
            and ``search(..., doc_names=True)`` return them. Default: none.
        """
        if native is None:
            raise ImportError('nind._native compiled extension is not available (build it via "uv sync")')
        file_paths = list(file_paths)
        if doc_names is not None:
            doc_names = list(doc_names)
            if len(doc_names) != len(file_paths):
                raise ValueError("doc_names and file_paths must have the same length")
        shards = max(1, min(shards, len(file_paths)))
        with IndexStore(self.index_dir, self.prefix).new_generation() as base:
            if shards > 1:
                self._index_sharded(file_paths, workers, shards, base)
            else:
                self._write_chunks(self._tokenize_chunks(file_paths, workers), 0, base)
            _write_meta(base, self.tokenizer_options, self._with_local_index)
            if doc_names is not None:
                _write_doc_names(base, dict(enumerate(doc_names)))

    @property
    def _with_local_index(self):
        return self.local_index is not False

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
        ids = [doc_ident for record_ids, _ in parts for doc_ident in record_ids]
        with IndexStore(self.index_dir, self.prefix).new_generation() as base:
            self._write_chunks([chunk for _, chunk in parts], 0, base)
            _write_meta(base, self.tokenizer_options, self._with_local_index)
            # the records' ids are the documents' names (converted to strings, unique)
            _write_doc_names(base, dict(enumerate(ids)))
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
        # Runs in a worker process: (record ids, chunk) of the lines in [start, stop).
        with open(path, 'rb') as f:
            f.seek(start)
            data = f.read(stop - start)
        record_ids, texts = [], []
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
            record_ids.append(doc_ident)
            texts.append(text)
        return record_ids, self._tokenize_texts(texts)

    def update(self):
        """Start an incremental update of the index (created if there is none yet).

        :return: an :class:`IndexUpdate`, to use as a context manager; its
            changes are published together when the ``with`` block ends.
        """
        return IndexUpdate(self)

    def add_files(self, file_paths, doc_ids=None, workers=None, doc_names=None):
        """Add (or replace) documents incrementally: one :meth:`update` for all of ``file_paths``.

        :param file_paths: paths of the text files to index, one document each.
        :param doc_ids: their identifiers (an existing document with the same
            id is replaced); default: new ids following the highest one, or
            the document with the same name.
        :param workers: processes tokenizing the files, as in :meth:`index_files`.
        :param doc_names: their names (see :meth:`IndexUpdate.add_file`).
        :return: the documents' identifiers, in ``file_paths`` order.
        """
        if doc_ids is not None and len(doc_ids) != len(file_paths):
            raise ValueError("doc_ids and file_paths must have the same length")
        if doc_names is not None and len(doc_names) != len(file_paths):
            raise ValueError("doc_names and file_paths must have the same length")
        with self.update() as update:
            # tokenized like the index
            tokenized = update._tokenizing_indexer._tokenize_files(list(file_paths), workers)
            return [update.add_tokens(tokens, None if doc_ids is None else doc_ids[i],
                                      None if doc_names is None else doc_names[i])
                    for i, tokens in enumerate(tokenized)]

    def add_file(self, path, doc_id=None, name=None):
        """Add (or replace) one document incrementally; see :meth:`add_files`.

        Each call publishes a new version of the index, which costs a copy
        of the index: to add many files, use :meth:`add_files` or one
        :meth:`update` for all of them.

        :return: the document's identifier.
        """
        with self.update() as update:
            return update.add_file(path, doc_id, name)

    def remove_documents(self, doc_ids):
        """Remove documents from the index, in one :meth:`update`.

        :return: for each id, whether there was such a document.
        """
        with self.update() as update:
            return [update.remove(doc_id) for doc_id in doc_ids]

    def _write_chunks(self, chunks, first_doc_id, base):
        # writes the index files at base (a path without extension), plus the document lengths
        # chunks: (vocabulary, term ids, ends) of consecutive documents, the first one being
        # document first_doc_id (see native.Tokenizer.tokenize_batch)
        vocabulary = sorted(set().union(*(chunk[0] for chunk in chunks)))
        term_to_id, identification = self._write_lexicon(vocabulary, base)
        builder = native.IndexBuilder()
        doc_id = first_doc_id
        for chunk_vocabulary, term_ids, ends in chunks:
            ends = _uint_array(ends)
            builder.add_documents(doc_id, ends, _uint_array(term_ids),
                                  array('I', map(term_to_id.__getitem__, chunk_vocabulary)))
            doc_id += len(ends)
        del chunks
        writers = [native.NindTermIndex(base, is_writer=True, lexicon_identification=identification,
                                        specifics_number=0, indirection_bloc_size=len(vocabulary) + 1)]
        if self._with_local_index:
            writers.append(native.NindLocalIndex(base, is_writer=True, lexicon_identification=identification,
                                                 indirection_bloc_size=max(1, builder.docs_nb + 1)))

        def write(writer, method):
            # one writer per file, in its own thread: the native writers release the GIL
            writer.begin_bulk()
            method(writer, identification)
            writer.end_bulk()

        with ThreadPoolExecutor(max_workers=2) as pool:
            futures = [pool.submit(write, writers[0], builder.write_term_index)]
            if len(writers) > 1:
                futures.append(pool.submit(write, writers[1], builder.write_local_index))
            for future in futures:
                future.result()
        del writers
        doc_ids, lengths = builder.doc_lengths()
        _write_doc_lengths(base, dict(zip(_uint_array(doc_ids), _uint_array(lengths))))

    def _write_index(self, token_lists, first_doc_id, base):
        # writes the index of documents given as token lists (first one: document first_doc_id)
        self._write_chunks([_chunk_from_token_lists(token_lists)], first_doc_id, base)

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
            # disjoint documents: the shards' lengths together
            lengths = {}
            for shard_base in shard_bases:
                lengths.update(_read_doc_lengths(shard_base))
            _write_doc_lengths(base, lengths)
        finally:
            shutil.rmtree(shard_dir, ignore_errors=True)

    def _build_shard(self, shard_base, file_paths, first_doc_id):
        # Runs in a worker process (self is a copy: keeps an overridden tokenizer).
        self._write_chunks(self._tokenize_chunks(file_paths, 1), first_doc_id, shard_base)

    #: Below this many files, :meth:`index_files` tokenizes in-process.
    PARALLEL_MIN_FILES = 64
    #: At most this many files per tokenizing task.
    CHUNK_MAX_FILES = 4096

    def _tokenize_chunks(self, file_paths, workers):
        # the files as chunks (see _write_chunks), tokenized in worker processes
        if workers is None:
            workers = os.cpu_count() or 1
        workers = min(workers, len(file_paths))
        if workers <= 1 or len(file_paths) < self.PARALLEL_MIN_FILES:
            size = self.CHUNK_MAX_FILES
        else:
            size = max(1, min(self.CHUNK_MAX_FILES, -(-len(file_paths) // (workers * 4))))
        pieces = [file_paths[i:i + size] for i in range(0, len(file_paths), size)]
        if workers <= 1 or len(file_paths) < self.PARALLEL_MIN_FILES:
            return [self._tokenize_paths(piece) for piece in pieces]
        with ProcessPoolExecutor(max_workers=workers, initializer=_pool_worker_init) as pool:
            return list(pool.map(self._tokenize_paths, pieces))

    def _tokenize_paths(self, file_paths):
        # Runs in a worker process: one chunk (see _write_chunks) of the files.
        texts = []
        for path in file_paths:
            with open(path, 'r', encoding='utf-8', errors='ignore') as f:
                texts.append(f.read())
        return self._tokenize_texts(texts)

    def _tokenize_texts(self, texts):
        # one chunk (see _write_chunks): natively, unless _default_tokenize is overridden
        if type(self)._default_tokenize is NindIndexer._default_tokenize and \
                self.tokenizer_options != LEGACY_TOKENIZER:
            return _tokenize_batch(self.tokenizer_options, texts)
        return _chunk_from_token_lists([self._default_tokenize(text) for text in texts])

    def _tokenize_files(self, file_paths, workers):
        # the files' token lists (for updates)
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
        with open(path, 'r', encoding='utf-8', errors='ignore') as f:
            return self._default_tokenize(f.read())

    def _default_tokenize(self, text):
        """The tokens of ``text`` (see the class documentation to override it)."""
        return _tokenize_text(self.tokenizer_options, text)

    ########################################################################
    # .nindlexiconindex : one simple word per term, ids assigned by the writer.
    ########################################################################
    def _write_lexicon(self, terms, base):
        # terms get the idents 1, 2... in the order given
        lexicon_writer = native.NindLexiconIndex(base, is_writer=True,
                                                 indirection_bloc_size=max(1, len(terms)))
        lexicon_writer.begin_bulk()
        term_to_id = dict(zip(terms, lexicon_writer.add_simple_words(terms)))
        lexicon_writer.end_bulk()
        return term_to_id, lexicon_writer.get_identification()

    ########################################################################
    # .nindtermindex : directly indexed by identifiant terme.
    # (_write_term_index and _write_local_index: writers of index files from
    # Python structures, used by the reader tests; indexing uses IndexBuilder)
    ########################################################################
    def _write_term_index(self, inverted_index, lexicon_identification, base):
        # inverted_index: term id -> [(doc id, frequency)] in increasing doc id order
        max_term_id = max(inverted_index) if inverted_index else 0
        term_writer = native.NindTermIndex(base, is_writer=True,
                                           lexicon_identification=lexicon_identification,
                                           specifics_number=0,
                                           indirection_bloc_size=max_term_id + 1)
        idents, ends, doc_ids, freqs = array('I'), array('I'), array('I'), array('I')
        for tid in sorted(inverted_index):
            postings = inverted_index[tid]
            idents.append(tid)
            doc_ids.extend(doc_id for doc_id, _ in postings)
            freqs.extend(freq for _, freq in postings)
            ends.append(len(doc_ids))
        term_writer.begin_bulk()
        term_writer.set_term_defs_arrays(idents, ends, doc_ids, freqs, lexicon_identification)
        term_writer.end_bulk()

    ########################################################################
    # .nindlocalindex : directly indexed by identifiant document externe
    # (la traduction externe <-> interne est faite en interne par le C++).
    ########################################################################
    def _write_local_index(self, corpus_tokens, term_to_id, lexicon_identification, first_doc_id, base):
        local_writer = native.NindLocalIndex(base, is_writer=True,
                                             lexicon_identification=lexicon_identification,
                                             indirection_bloc_size=len(corpus_tokens) + 1)
        idents, ends, term_ids, positions = array('I'), array('I'), array('I'), array('I')
        for index, tokens in enumerate(corpus_tokens):
            idents.append(first_doc_id + index)
            term_ids.extend(term_to_id[token] for token in tokens)
            positions.extend(range(len(tokens)))
            ends.append(len(term_ids))
        lengths = array('I', [1]) * len(term_ids)
        local_writer.begin_bulk()
        local_writer.set_local_defs_arrays(idents, ends, term_ids, positions, lengths, lexicon_identification)
        local_writer.end_bulk()
