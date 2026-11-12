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
import collections
import contextlib
import copy
import json
import math
import os
import re
import secrets
import shutil
import struct
import sys
import tempfile
import threading
import time
from array import array
from collections.abc import Sequence
from concurrent.futures import ProcessPoolExecutor, ThreadPoolExecutor
from itertools import accumulate, compress
from .NindLexiconindex import NindLexiconindex
from .NindTermindex import NindTermindex
from .NindLocalindex import NindLocalindex
from ._index_store import IndexStore, META_EXTENSION, DOC_LENGTHS_EXTENSION, DOC_NAMES_EXTENSION
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

    Otherwise a killed indexer leaves its workers behind, idle forever. Also keeps glibc's mmap
    threshold fixed (see _fix_mmap_threshold).
    """
    _fix_mmap_threshold()
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

#: A top-k search uses block-max MaxScore (identical results) from this many documents
#: per result up (top_k * PRUNING_DOCS_PER_RESULT <= documents), the exhaustive one below:
#: measured faster above, slower below.
PRUNING_DOCS_PER_RESULT = 4000


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
    # a chunk: (words' UTF-8 bytes, their ends, term ids, document ends), see _write_chunks
    tokenizer = _native_tokenizer(options)
    try:
        return tokenizer.tokenize_batch_packed(texts)
    except (TypeError, UnicodeEncodeError):
        # a text not encodable in UTF-8: only then are the texts checked one by one
        return tokenizer.tokenize_batch_packed([_encodable(text) for text in texts])


def _storable(token):
    # a lexicon word is 1 to 254 bytes long, in UTF-8
    try:
        return 0 < len(token) and len(token.encode("utf-8")) <= 254
    except UnicodeEncodeError:
        return False


def _chunk_from_token_lists(token_lists):
    """A chunk (words' UTF-8 bytes, their ends, term ids, document ends) of documents given as token
    lists, as native.Tokenizer.tokenize_batch_packed returns it; unstorable tokens are dropped."""
    local_ids, words, word_ends = {}, bytearray(), array('I')
    term_ids, ends = array('I'), array('I')
    for tokens in token_lists:
        for token in tokens:
            local_id = local_ids.get(token)
            if local_id is None:
                if not _storable(token):
                    continue
                local_id = local_ids[token] = len(word_ends)
                words += token.encode("utf-8")
                word_ends.append(len(words))
            term_ids.append(local_id)
        ends.append(len(term_ids))
    return bytes(words), word_ends.tobytes(), term_ids.tobytes(), ends.tobytes()


def _chunk_words(chunk):
    """The words of a chunk (see _chunk_from_token_lists), as bytes."""
    words, ends = chunk[0], _uint_array(chunk[1])
    return [words[begin:end] for begin, end in zip([0] + list(ends[:-1]), ends)]


_glibc = None


def _get_glibc():
    """glibc's malloc tuning functions (malloc_trim, mallopt), or None elsewhere."""
    global _glibc
    if _glibc is None:
        _glibc = False
        if sys.platform.startswith("linux"):
            try:
                import ctypes
                libc = ctypes.CDLL("libc.so.6")
                libc.malloc_trim, libc.mallopt      # AttributeError if not glibc
                _glibc = libc
            except (OSError, AttributeError):
                pass
    return _glibc or None


_M_MMAP_THRESHOLD = -3                      # glibc's mallopt parameter
_MMAP_THRESHOLD = 128 * 1024                # glibc's default, kept fixed


def _fix_mmap_threshold():
    """Keep glibc's mmap threshold at its default (for good: glibc can't make it dynamic again),
    in a pool worker or, for a direct build, this process (NindIndexer.FIX_MMAP_THRESHOLD). glibc
    raises it to the size of each mmapped block freed (up to 32 MiB): a build's later buffers then
    came from the heap, fragmented, instead of mmap (MS MARCO v2, 4.8M passages: a worker's first
    shard 1.45 GiB, the next ones ~2.0, fixed 1.27 each; a direct build 2.13 GiB, fixed 1.51; as
    fast)."""
    libc = _get_glibc()
    if libc:
        libc.mallopt(_M_MMAP_THRESHOLD, _MMAP_THRESHOLD)


def _release_freed_memory():
    """Give the memory freed by a build back to the system (glibc keeps it otherwise: 0.4 GiB after
    a shard of 3M documents, which a pool worker's next shard would start from)."""
    libc = _get_glibc()
    if libc:
        libc.malloc_trim(0)


def _uint_array(data):
    values = array('I')
    values.frombytes(data)
    return values


def _doc_id_array(data):
    """Document ids (64 bits, NindDocId) from the bytes of a native uint64 vector."""
    values = array('Q')
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


# NINDDL02: 8-byte document ids (64-bit NindDocId), 4-byte lengths; NINDDL01 (4-byte ids) still read
_DOC_LENGTHS_MAGIC = b"NINDDL02"
_DOC_LENGTHS_MAGIC_V1 = b"NINDDL01"


def _write_doc_length_arrays(base, doc_ids, values):
    """Write the lengths of documents: arrays by increasing id, no length 0 (no dict: ~100 bytes a document)."""
    if sys.byteorder == "big":
        doc_ids, values = array('Q', doc_ids), array('I', values)
        doc_ids.byteswap()
        values.byteswap()
    with open(base + DOC_LENGTHS_EXTENSION, "wb") as f:
        f.write(_DOC_LENGTHS_MAGIC + struct.pack("<I", len(doc_ids)))
        f.write(doc_ids.tobytes())
        f.write(values.tobytes())


def _read_doc_length_arrays(base):
    """(document ids, lengths in tokens) arrays (``None`` if the index has no such file)."""
    try:
        with open(base + DOC_LENGTHS_EXTENSION, "rb") as f:
            data = f.read()
    except FileNotFoundError:
        return None
    header = len(_DOC_LENGTHS_MAGIC) + 4
    if data.startswith(_DOC_LENGTHS_MAGIC):
        doc_ids, id_size = array('Q'), 8
    elif data.startswith(_DOC_LENGTHS_MAGIC_V1):
        doc_ids, id_size = array('I'), 4
    else:
        raise ValueError("Corrupt %s" % (base + DOC_LENGTHS_EXTENSION))
    if len(data) < header:
        raise ValueError("Corrupt %s" % (base + DOC_LENGTHS_EXTENSION))
    (count,) = struct.unpack_from("<I", data, len(_DOC_LENGTHS_MAGIC))
    if len(data) != header + (id_size + 4) * count:
        raise ValueError("Corrupt %s" % (base + DOC_LENGTHS_EXTENSION))
    values = array('I')
    doc_ids.frombytes(data[header:header + id_size * count])
    values.frombytes(data[header + id_size * count:])
    if sys.byteorder == "big":
        doc_ids.byteswap()
        values.byteswap()
    return doc_ids, values


def _write_doc_names(base, names):
    """Write ``{document id: name}`` (names: unique strings; other values are converted with str)."""
    doc_ids = sorted(names)
    native.NindDocNames.write(base + DOC_NAMES_EXTENSION, array('Q', doc_ids),
                              [name if isinstance(name, str) else str(name) for name in map(names.__getitem__, doc_ids)])


#: Names as their UTF-8 bytes one after the other (``names``) and their lengths (``lengths``,
#: ``array('I')``): what ``NindDocNames.write_packed`` writes, without a string per name.
_PackedNames = collections.namedtuple("_PackedNames", "names lengths")


def _pack_names(names):
    """``_PackedNames`` of strings."""
    encoded = [name.encode() for name in names]
    return _PackedNames(b''.join(encoded), array('I', map(len, encoded)))


def _unpack_names(names):
    """The strings of ``_PackedNames`` (anything else: as it is)."""
    if not isinstance(names, _PackedNames):
        return names
    unpacked, offset = [], 0
    for length in names.lengths:
        unpacked.append(names.names[offset:offset + length].decode())
        offset += length
    return unpacked


class _RecordNames(Sequence):
    """The record ids :meth:`NindIndexer.index_jsonl` returns (string ids), as strings: element
    ``n`` is the name of document ``n``, read from the build's names file at the first access
    (hundreds of millions of ids don't fit in a Python list). A later build of the index
    removes that file, and with it any element not read yet."""

    def __init__(self, base, count):
        self._base = base
        self._count = count
        self._names = None

    def __len__(self):
        return self._count

    def __eq__(self, other):
        # as a list compares (Sequence defines no __eq__): against any sequence, element by element
        if not isinstance(other, Sequence) or isinstance(other, (str, bytes)):
            return NotImplemented
        return len(other) == self._count and all(a == b for a, b in zip(self, other))

    __hash__ = None

    def __getitem__(self, index):
        if isinstance(index, slice):
            return [self[i] for i in range(*index.indices(self._count))]
        if index < 0:
            index += self._count
        if not 0 <= index < self._count:
            raise IndexError(index)
        if self._names is None:
            self._names = native.SegmentedIndex(self._base, map_lengths=True)       #(names only)
        return self._names.name(index)


class _Snapshot:
    """One opened generation of an index: every file from the same build."""

    def __init__(self, base, previous=None, map_lengths=False):
        # every file is opened here, while the generation can't be deleted (see _index_store);
        # the segments the previous snapshot had are shared, not read again
        self.base = base
        self.index = native.SegmentedIndex(base, previous.index if previous is not None else None,
                                           map_lengths=map_lengths)
        meta = _read_meta(base)
        # an index written before tokenizer options were stored: the original tokenizer
        self.tokenizer_options = meta["tokenizer"] if meta is not None else LEGACY_TOKENIZER
        self.total_docs = self.index.docs_nb
        self.avg_doc_len = self.index.avg_doc_len
        # the files of an index of one segment, without deleted documents, for direct access
        self.lexicon = self.term_index = self.local_index = None
        segments = self.index.segments()
        self.single = len(segments) == 1 and segments[0].deleted_nb == 0
        if self.single:
            single = segments[0].base
            self.lexicon = NindLexiconindex(single + ".nindlexiconindex")
            lexicon_identification = self.lexicon.donneIdentification()
            self.term_index = NindTermindex(single + ".nindtermindex", lexicon_identification)
            # an index built with local_index=False has none
            self.local_index = NindLocalindex(single + ".nindlocalindex", lexicon_identification) \
                if os.path.exists(single + ".nindlocalindex") else None

    def search(self, terms, k1, b, top_k, threads=1):
        # the scores of the documents containing the terms, by decreasing score (see
        # native.SegmentedIndex); block-max MaxScore (same results) when k is small for the corpus:
        # measured faster from 4000 documents per result up (MS MARCO 1M/4M, FiQA), slower below (see
        # docs/proposals/format-v2-blocks.md)
        prune = 0 < top_k and top_k * PRUNING_DOCS_PER_RESULT <= self.total_docs
        return self.index.search(terms, k1, b, top_k, prune=prune, threads=threads)

    def term_id(self, term):
        if not self.single:
            raise ValueError("term ids are per segment: this index has several, or deleted documents "
                             "(NindIndexer.force_merge makes it one index)")
        return self.lexicon.donneIdentifiant([term])

    def tf(self, term, doc_id):
        return self.index.term_frequency(term, doc_id)

    def df(self, term):
        return self.index.document_frequency(term)

    def doc_len(self, doc_id):
        return self.index.document_length(doc_id)

    @property
    def doc_ids(self):
        return _doc_id_array(self.index.documents()[0])


class NindEngine:
    """BM25 search engine on top of a nind index produced by :class:`NindIndexer`.

    Opens the three index files (lexicon, term, local) for a corpus and
    exposes term/document-frequency lookups plus a ready-to-use
    :meth:`search`. Read-only: to build the index files this reads, use
    :class:`NindIndexer`.

    Safe to use while the index is being rebuilt, and from several threads:
    each call answers from one complete build of the index, and switches to
    a newer build as soon as one is published - nothing to reopen or close.
    Calls from several threads run in parallel (the native reads are
    thread-safe and release the GIL); a call started before a new build is
    published finishes on the build it started with. The low-level objects
    returned by :attr:`lexicon`, :attr:`term_index` and :attr:`local_index`
    are thread-safe for their lookups (``donneIdentifiant``,
    ``donneListeTermesCG``, ``donneListeTermes``), not for their diagnostic
    methods (``dumpeFichier``, ``afficheTerme``...).
    """

    def __init__(self, index_dir, prefix=None, search_threads=1, map_lengths=False):
        """Open the index in ``index_dir``.

        :param index_dir: directory the index was written into by
            :class:`NindIndexer` (or containing the flat
            ``.nindlexiconindex``/``.nindtermindex``/``.nindlocalindex``
            files of an index written by an older version).
        :param prefix: the index's filename prefix; only needed when
            ``index_dir`` holds several indexes.
        :param search_threads: threads each :meth:`search` uses by default
            (its ``threads`` argument overrides it). ``1`` (the default)
            suits a server answering many queries at once: run them in
            parallel instead, every call releases the GIL. More lowers the
            latency of a single query on idle cores (MS MARCO, 8.8M
            documents, top 10, median: 1.5 ms with 1 thread, 0.9 ms with
            4), for block-max searches only (a ``top_k`` small for the
            corpus).
        :param map_lengths: read the documents' lengths where they are, in their
            mapped files (shared by every process searching the index: none of
            this process's memory, 0.5 GB less for MS MARCO v2's 138M passages),
            instead of a copy (searches 3-6% faster). For many search processes
            on one index.
        :raises FileNotFoundError: if there is no index in ``index_dir``.
        :raises ValueError: if there are several and no ``prefix`` is given.
        """
        if not isinstance(search_threads, int) or search_threads < 1:
            raise ValueError("search_threads must be a positive integer")
        self.search_threads = search_threads
        self.map_lengths = bool(map_lengths)
        self.index_dir = index_dir
        self._store = IndexStore(index_dir, prefix or IndexStore.find_prefix(index_dir))
        # (snapshot, version), replaced as a whole when a new build is published
        self._state = self._store.open_current(lambda base: _Snapshot(base, None, self.map_lengths))
        self._switch_lock = threading.Lock()
        # the snapshot of the outermost public call running in each thread (nested calls reuse it)
        self._calls = threading.local()

    @property
    def _snapshot(self):
        return self._state[0]

    @contextlib.contextmanager
    def _session(self):
        calls = self._calls
        if getattr(calls, "snapshot", None) is not None:
            # a public call inside another one, in this thread: same snapshot
            yield calls.snapshot
            return
        # outermost public call: switch to a newly published build (one stat when there's none),
        # then answer from one snapshot; other threads' calls run meanwhile on theirs
        if self._store.current_version() != self._state[1]:
            with self._switch_lock:
                if self._store.current_version() != self._state[1]:
                    previous = self._state[0]
                    self._state = self._store.open_current(lambda base: _Snapshot(base, previous, self.map_lengths))
        calls.snapshot = self._state[0]
        try:
            yield calls.snapshot
        finally:
            calls.snapshot = None

    @staticmethod
    def _one_index(snapshot):
        if not snapshot.single:
            raise ValueError("this index has several segments, or deleted documents: "
                             "NindIndexer.force_merge makes it one index")
        return snapshot

    @property
    def lexicon(self):
        """The :class:`~nind.NindLexiconindex.NindLexiconindex` of the current build, if it
        is one index (else ``ValueError``; see :meth:`NindIndexer.force_merge`)."""
        with self._session() as snapshot:
            return self._one_index(snapshot).lexicon

    @property
    def term_index(self):
        """The :class:`~nind.NindTermindex.NindTermindex` of the current build, if it is one
        index (else ``ValueError``; see :meth:`NindIndexer.force_merge`)."""
        with self._session() as snapshot:
            return self._one_index(snapshot).term_index

    @property
    def local_index(self):
        """The :class:`~nind.NindLocalindex.NindLocalindex` of the current build, if it is
        one index (else ``ValueError``; see :meth:`NindIndexer.force_merge`); ``None`` for an
        index built with ``local_index=False``."""
        with self._session() as snapshot:
            return self._one_index(snapshot).local_index

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
            return snapshot.index.name(doc_id)

    def doc_id(self, name):
        """The identifier in the index of the document named ``name`` (see :meth:`doc_name`).

        :return: the document's identifier, or ``None`` if no document has that name.
        """
        with self._session() as snapshot:
            return snapshot.index.doc_id(name)

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

    def search(self, query, top_k=10, doc_names=False, k1=1.5, b=0.75, threads=None):
        """Search the corpus with BM25 and return the top-scoring documents.

        :param query: the query text.
        :param top_k: maximum number of results to return.
        :param doc_names: return the documents' names (see :meth:`doc_name`;
            ``None`` for a document without name) instead of their
            identifiers in the index - like pyserini's hits' ``docid``.
        :param k1: BM25 term-frequency saturation parameter.
        :param b: BM25 document-length normalization parameter.
        :param threads: threads searching this one query at once (default:
            the engine's ``search_threads``): the index's segments in parallel,
            and for a ``top_k`` small for the corpus, more threads than segments
            within them, with the same results. For many queries at once,
            calling ``search`` from several threads scales better (each
            call releases the GIL).
        :return: a list of ``(doc_id, score)`` tuples (``(name, score)``
            with ``doc_names``), sorted by descending score, of length at
            most ``top_k`` (only documents with a positive score are included).

        Only the documents containing a query term are scored, from the
        terms' postings lists (in native code): the cost depends on how many
        documents contain the query terms, not on the size of the corpus.
        Scores are those of :meth:`bm25_score` (to rounding: the native code
        computes the same formula, not the same operations); equal scores keep
        the order of the documents in the index.
        """
        limit = top_k if isinstance(top_k, int) and top_k >= 0 else -1
        if threads is None:
            threads = self.search_threads
        elif not isinstance(threads, int) or threads < 1:
            raise ValueError("threads must be a positive integer")
        with self._session() as snapshot:
            results = snapshot.search(self.tokenize(query), k1, b, limit, threads)
            if limit < 0:
                results = results[:top_k]
            if doc_names:
                results = [(snapshot.index.name(doc_id), score) for doc_id, score in results]
        return results

class _CancelUpdate(Exception):
    """Raised into the generation of an update that changes nothing, so that it isn't published."""


#: IndexUpdate._pending's value for a document of a batch (IndexUpdate.add_jsonl), written apart
_IN_BATCH = object()


def _check_doc_id(doc_id):
    import operator
    doc_id = operator.index(doc_id)
    # 64-bit external ids (NindDocId); the v1 index formats store 32 bits and refuse larger ones
    if not 0 <= doc_id <= 0xFFFFFFFFFFFFFFFF:
        raise ValueError("document id out of range (0 .. 2**64-1): %d" % doc_id)
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

    An update costs what it changes, whatever the size of the index: its
    documents are written into a new segment, the replaced and removed ones
    are masked in theirs, the published segments stay as they are (hard links);
    segments are merged now and then (see ``MERGE_FACTOR``), which makes each
    document be rewritten about log(N) times over its life. The results are
    those of an index built at once from the live documents.

    Documents are tokenized as the index's were (its stored tokenizer
    options); an index built without local index stays without (replacing
    or removing documents then reads every term's directory in their segments).

    Not thread-safe: use an update from one thread at a time.
    """

    #: Segments of a size tier merged into one (tiers grow by this factor): more segments cost
    #: searches more (MS MARCO 8.8M, one thread: +16% with 8, +69% with 32; with 8 threads, nothing).
    #: ``0``: never merge (see :meth:`NindIndexer.force_merge`).
    MERGE_FACTOR = 10
    #: No merge into a segment of more live documents than this; at half of it a segment is full and
    #: stays as it is (rewriting it costs more than it gains: a large build's segments are left
    #: alone). ``0``: no limit.
    MAX_MERGED_DOCS = 5000000

    def __init__(self, indexer):
        self._indexer = indexer
        self._stack = None
        #: What the last commit did (``nind._native.SegmentedUpdateStats``: ``added_docs_nb``,
        #: ``removed_docs_nb``, ``segments_nb``, ``merges_nb``, ``merged_docs_nb``), ``None`` before.
        self.stats = None

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
            self._tokenizer_options = indexer.tokenizer_options
            self._local = indexer.local_index is not False
            source = store.published_base()
            if source is not None:
                # the published segments never change: this generation links them
                store.link_generation(source, self._base)
                # the index keeps the tokenizer and layout it was built with
                meta = _read_meta(self._base)
                options = meta["tokenizer"] if meta is not None else LEGACY_TOKENIZER
                if indexer._explicit_tokenizer and options != indexer.tokenizer_options:
                    raise ValueError("the index was built with other tokenizer options: %r" % (options,))
                segments = native.SegmentedIndex.read_manifest(self._base)
                if segments:
                    local = any(os.path.exists(native.SegmentedIndex.segment_base(self._base, segment) + ".nindlocalindex")
                                for segment in segments)
                else:   # no document left
                    local = meta["local_index"] if meta is not None else self._local
                if indexer.local_index is not None and indexer.local_index != local:
                    raise ValueError("the index was built %s local index" % ("with" if local else "without"))
                self._tokenizer_options, self._local = options, local
            else:
                native.SegmentedIndex.write_manifest(self._base, [])
            self._stack = stack.pop_all()
        # tokenizes like the index: the indexer, with the index's tokenizer options
        self._tokenizing_indexer = copy.copy(indexer)
        self._tokenizing_indexer.tokenizer_options = self._tokenizer_options
        self._pending = {}      # doc id -> its tokens, None to remove it, or _IN_BATCH
        self._batches = []      # add_jsonl's: (doc ids, tokenizer batch, names), in order
        self._published = None  # the index before this update, opened when first needed
        self._max_published = native.SegmentedUpdate.max_doc_id(self._base)
        self._next_id = self._max_published + 1 if self._max_published is not None else 0
        # names given in this update: doc id -> name (None: none), and back
        self._names = {}
        self._ids_by_name = {}
        return self

    def __exit__(self, exc_type, exc, tb):
        stack, self._stack = self._stack, None
        if exc_type is not None:
            return stack.__exit__(exc_type, exc, tb)
        try:
            if not self._pending:
                raise _CancelUpdate()       # nothing changes: nothing to publish
            self._commit()
        except BaseException as error:
            try:
                if not stack.__exit__(type(error), error, error.__traceback__):
                    raise
            except _CancelUpdate:
                pass
            return False
        stack.close()
        return False

    # -- the index before this update -----------------------------------------
    def _before(self):
        # (reads its documents' lengths: an update that only adds new documents doesn't)
        if self._published is None:
            self._published = native.SegmentedIndex(self._base, map_lengths=True)    #(lookups only)
        return self._published

    def _was_published(self, doc_id):
        return self._max_published is not None and doc_id <= self._max_published and self._before().contains(doc_id)

    def _owner(self, name):
        # the document named name now, changes of this update included
        if name in self._ids_by_name:
            return self._ids_by_name[name]
        if self._max_published is None:
            return None
        doc_id = self._before().doc_id(name)
        # (a published document whose name this update changed, or that it removed, has lost it)
        if doc_id is None or doc_id in self._names or self._pending.get(doc_id, ()) is None:
            return None
        return doc_id

    # -- changes ------------------------------------------------------------
    def add_file(self, path, doc_id=None, name=None):
        """Index the text file ``path`` as one document.

        :param path: path to a UTF-8 (or UTF-8-decodable, with errors
            ignored) text file.
        :param doc_id: the document's identifier; replaces the document with
            this id if there is one. Default: the document named ``name`` if
            there is one, else one more than the highest id ever given in the
            index or in this update (``0`` for the first document).
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
            owner = self._owner(name)
            if doc_id is None:
                doc_id = owner
            elif owner is not None and owner != _check_doc_id(doc_id):
                raise ValueError("another document (%d) is named %r" % (owner, name))
        doc_id = self._next_id if doc_id is None else _check_doc_id(doc_id)
        # (ASCII: its length is its size in UTF-8, no encoding needed)
        self._pending[doc_id] = [token for token in tokens
                                 if (token.isascii() and 0 < len(token) <= 254) or _storable(token)]
        self._next_id = max(self._next_id, doc_id + 1)
        if name is not None:
            self._set_name(doc_id, name)
        return doc_id

    def add_jsonl(self, path, id_field="id", text_field="contents", workers=None):
        """Add the records of a JSON Lines corpus (the format of
        :meth:`NindIndexer.index_jsonl`), each named by its ``id_field``: a
        record with the name of a document replaces it (as :meth:`add_tokens`
        with ``name``), the others get new identifiers.

        The way to add many documents: the file is parsed and tokenized in
        worker processes, and the documents go to the C++ API in batches,
        nothing done per token in Python.

        :param workers: processes parsing and tokenizing the file (default:
            ``os.cpu_count()``); ``1``, or a file smaller than
            ``NindIndexer.JSONL_PARALLEL_MIN_BYTES``, works in this process.
        :return: the documents' identifiers, in the file's order.
        :raises ValueError: if a line isn't a JSON object with both fields.
        """
        self._check_active()
        indexer = self._tokenizing_indexer
        end = indexer._jsonl_end(path, None)
        if workers is None:
            workers = os.cpu_count() or 1
        doc_ids = []
        if workers <= 1 or end < indexer.JSONL_PARALLEL_MIN_BYTES:
            self._add_batch(indexer._tokenize_jsonl_range(path, 0, end, id_field, text_field), doc_ids)
            return doc_ids
        ranges = indexer._jsonl_ranges(path, end, workers * 4)
        with ProcessPoolExecutor(max_workers=min(workers, len(ranges)), initializer=_pool_worker_init) as pool:
            for part in pool.map(indexer._tokenize_jsonl_range, [path] * len(ranges),
                                 [start for start, _ in ranges], [stop for _, stop in ranges],
                                 [id_field] * len(ranges), [text_field] * len(ranges)):
                self._add_batch(part, doc_ids)
        return doc_ids

    def _add_batch(self, part, doc_ids):
        # one tokenizer batch of add_jsonl: (record ids, chunk)
        record_ids, chunk = part
        ids, names = array('Q'), []
        for record_id in record_ids:
            name = record_id if isinstance(record_id, str) else str(record_id)
            doc_id = self._owner(name)
            if doc_id is None:
                doc_id = self._next_id
            self._next_id = max(self._next_id, doc_id + 1)
            self._pending[doc_id] = _IN_BATCH
            self._set_name(doc_id, name)
            ids.append(doc_id)
            names.append(name)
        self._batches.append((ids, chunk, names))
        doc_ids.extend(ids)

    def doc_id(self, name):
        """The identifier of the document named ``name`` (``None`` if there is none),
        changes of this update included."""
        self._check_active()
        return self._owner(name)

    def _set_name(self, doc_id, name):
        previous = self._names.get(doc_id)
        if previous is not None:
            self._ids_by_name.pop(previous, None)
        self._names[doc_id] = name
        if name is not None:
            self._ids_by_name[name] = doc_id

    def remove(self, doc_id):
        """Remove the document ``doc_id`` from the index.

        :return: ``True`` if there was such a document (in the index or
            added earlier in this update), ``False`` otherwise.
        """
        self._check_active()
        doc_id = _check_doc_id(doc_id)
        self._set_name(doc_id, None)
        if self._was_published(doc_id) or self._pending.get(doc_id) is _IN_BATCH:
            # (a batch's document is written with its batch: removed after it)
            found = self._pending.get(doc_id, True) is not None
            self._pending[doc_id] = None
            return found
        return self._pending.pop(doc_id, None) is not None

    def _check_active(self):
        if self._stack is None:
            raise RuntimeError("an IndexUpdate must be used inside a 'with' block")

    # -- writing ------------------------------------------------------------
    def _commit(self):
        # the changes, written by the C++ API (nind._native.SegmentedUpdate): a new segment, the
        # replaced and removed documents masked in theirs, the merges the policy asks for
        update = native.SegmentedUpdate(self._base, with_local_index=self._local, merge_factor=self.MERGE_FACTOR,
                                        max_merged_docs=self.MAX_MERGED_DOCS)
        # the batches first: a later change of one of their documents is in _pending
        for doc_ids, (words, word_ends, term_ids, ends), names in self._batches:
            update.add_batch(doc_ids, _uint_array(ends), _uint_array(term_ids), words, _uint_array(word_ends), names)
        self._batches = []
        for doc_id, tokens in self._pending.items():
            if tokens is _IN_BATCH:
                continue
            if not tokens:
                update.remove_document(doc_id)
                continue
            if doc_id in self._names:
                name = self._names[doc_id]
            else:
                # a replaced document keeps its name
                name = self._before().name(doc_id) if self._max_published is not None and doc_id <= self._max_published else None
            update.add_document(doc_id, tokens, name or "")
        self._published = None
        self.stats = update.commit()
        _write_meta(self._base, self._tokenizer_options, self._local)


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

    def index_files(self, file_paths, workers=None, shards=1, doc_names=None, merge=False, memory_gb=None):
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
            part of ``file_paths``) built in parallel processes, at most
            ``workers`` at a time, which become the index's segments. ``1`` builds
            the index directly. See :meth:`index_jsonl`.
        :param merge: with ``shards > 1``, merge them into one index. Default:
            they stay segments (see :meth:`index_jsonl`).
        :param doc_names: the documents' names (their identifiers in the
            corpus, unique), in ``file_paths`` order; :meth:`NindEngine.doc_name`
            and ``search(..., doc_names=True)`` return them. Default: none.
        :param memory_gb: the memory the build may use, in GiB, as in
            :meth:`index_jsonl`: the tokens and postings estimated from a few
            runs of consecutive files spread over ``file_paths``, scaled by the
            files' sizes (not counted: ``doc_names`` and the list of paths,
            which the caller holds anyway).
        """
        if native is None:
            raise ImportError('nind._native compiled extension is not available (build it via "uv sync")')
        file_paths = list(file_paths)
        if doc_names is not None:
            doc_names = list(doc_names)
            if len(doc_names) != len(file_paths):
                raise ValueError("doc_names and file_paths must have the same length")
        if workers is None:
            workers = os.cpu_count() or 1
        if memory_gb is not None:
            shards, workers, postings_budget = self._plan_files_memory(file_paths, workers, shards, memory_gb)
        else:
            postings_budget = 0
        shards = max(1, min(shards, len(file_paths)))
        with IndexStore(self.index_dir, self.prefix).new_generation() as base:
            if shards > 1:
                self._index_sharded(file_paths, workers, shards, base, doc_names, merge, postings_budget)
            else:
                if self.FIX_MMAP_THRESHOLD:
                    _fix_mmap_threshold()
                self._write_chunks(self._tokenize_chunks(file_paths, workers), 0, base, postings_budget)
                if doc_names is not None:
                    _write_doc_names(base, dict(enumerate(doc_names)))
            _write_meta(base, self.tokenizer_options, self._with_local_index)

    @property
    def _with_local_index(self):
        return self.local_index is not False

    def index_jsonl(self, path, id_field="id", text_field="contents", limit=None, workers=None, shards=1, merge=False,
                    memory_gb=None):
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

        Large corpora (more than about 10M documents): give ``memory_gb``, or
        ``shards``. A direct build holds the whole corpus in memory (about 8
        bytes per token, 35 per document and 100 per distinct term: 2.9 GiB
        for 8M MS MARCO passages; about 5 per token when its postings are
        built in 4 passes, as ``memory_gb`` may do; see
        ``FIX_MMAP_THRESHOLD``) and is slower; shards bound
        the memory and build in parallel, and their segments give the same
        results.

        :param path: path to the UTF-8 ``.jsonl`` file.
        :param id_field: name of the field holding a record's identifier.
        :param text_field: name of the field holding the text to index.
        :param limit: index only the first ``limit`` records (default: all).
        :param workers: number of processes parsing and tokenizing the file
            (default: ``os.cpu_count()``); ``1``, or a file smaller than
            ``JSONL_PARALLEL_MIN_BYTES``, works in this process.
        :param shards: number of independent indexes, each over a line-aligned
            part of the file of about the same bytes, built in worker processes
            (at most ``workers`` at a time), which become the index's segments:
            the same search results as a direct build, searched in parallel
            with ``threads``, and no time spent merging (MS MARCO 8.8M: 25 s
            with 8 or 32 shards, 45 s direct; searches on one thread cost 16%
            more with 8, 69% with 32; with 8 threads, nothing more). This is the
            way to index a corpus too large to build at once: the peak is about
            ``workers`` times a shard's build (see above).
        :param merge: with ``shards > 1``, merge them into one index at the end
            (the merge's memory: ~30 bytes per document, plus the longest term's
            postings at 16 bytes each, not counted by ``memory_gb``; shards and
            merged index on disk together).
        :param memory_gb: the memory the build may use, in GiB: the fewest
            shards (at least ``shards``; one, a direct build, if it fits) whose
            builds, ``workers`` at a time beside this process, fit in
            ``MEMORY_GB_MARGIN`` of it, estimated from the tokens, postings,
            documents and vocabulary of a few samples of the file
            (``BUILD_BYTES_PER_*``, ``HEAPS_EXPONENT``); each build's postings
            in up to ``MAX_BUILD_PASSES`` ranges of terms, the fewest that fit
            (a scan of its tokens each, about 2 s for 16M passages). Fewer
            workers if each wouldn't get twice ``BUILD_BYTES_PER_PROCESS``; fewer workers also mean fewer
            shards, i.e. faster searches on one thread.
        :return: the ``id_field`` value of every indexed record: element
            ``n`` is the identifier of document ``n``. A read-only sequence of
            them as strings, read from the index files at the first access (no
            list of every id in memory); a list of the values themselves for a
            direct build with ids that aren't all strings (numbers).
        :raises ValueError: if a line isn't a JSON object with both fields.
        """
        if native is None:
            raise ImportError('nind._native compiled extension is not available (build it via "uv sync")')
        if limit is not None and limit < 0:
            raise ValueError("limit must be positive or None")
        end = self._jsonl_end(path, limit)
        if workers is None:
            workers = os.cpu_count() or 1
        if memory_gb is not None:
            shards, workers, postings_budget = self._plan_jsonl_memory(path, end, id_field, text_field, workers,
                                                                       shards, memory_gb)
        else:
            postings_budget = 0
        if shards > 1:
            return self._index_jsonl_sharded(path, end, id_field, text_field, workers, shards, merge, postings_budget)
        if self.FIX_MMAP_THRESHOLD:
            _fix_mmap_threshold()
        if workers <= 1 or end < self.JSONL_PARALLEL_MIN_BYTES:
            parts = [self._tokenize_jsonl_range_packed(path, 0, end, id_field, text_field)]
        else:
            # pieces of at most JSONL_SHARD_PIECE_BYTES: what a worker holds (1/32 of 138M passages
            # was 5 GiB each)
            ranges = self._jsonl_ranges(path, end, max(workers * 4, -(-end // self.JSONL_SHARD_PIECE_BYTES)))
            with ProcessPoolExecutor(max_workers=min(workers, len(ranges)), initializer=_pool_worker_init) as pool:
                parts = list(pool.map(self._tokenize_jsonl_range_packed, [path] * len(ranges),
                                      [start for start, _ in ranges], [stop for _, stop in ranges],
                                      [id_field] * len(ranges), [text_field] * len(ranges)))
        chunks = [chunk for _, chunk in parts]
        names = [record_names for record_names, _ in parts]
        del parts                       # _write_chunks frees each chunk once added
        with IndexStore(self.index_dir, self.prefix).new_generation() as base:
            # the records' ids are the documents' names (converted to strings, unique); string
            # ids, packed by the workers, are written first and read back from the file, not kept
            # beside the build
            if all(isinstance(part, _PackedNames) for part in names):
                count = sum(len(part.lengths) for part in names)
                packed = _PackedNames(b''.join(part.names for part in names), array('I'))
                for part in names:
                    packed.lengths.extend(part.lengths)
                del names
                native.NindDocNames.write_packed(base + DOC_NAMES_EXTENSION, 0, packed.names, packed.lengths)
                del packed
                ids = None
            else:
                ids = [doc_ident for part in names for doc_ident in _unpack_names(part)]
                del names
                native.NindDocNames.write(base + DOC_NAMES_EXTENSION, array('Q', range(len(ids))),
                                          [doc_ident if isinstance(doc_ident, str) else str(doc_ident) for doc_ident in ids])
            self._write_chunks(chunks, 0, base, postings_budget)
            _write_meta(base, self.tokenizer_options, self._with_local_index)
        return _RecordNames(base, count) if ids is None else ids

    #: A direct build (:meth:`index_jsonl`, :meth:`index_files`) fixes glibc's mmap threshold at its
    #: default in this process, for good (a third less memory: see _fix_mmap_threshold); False
    #: leaves it dynamic (the planner's model then underestimates a direct build, ~1.4 times).
    #: Worker processes always fix it.
    FIX_MMAP_THRESHOLD = True
    #: Below this many bytes of JSON Lines, :meth:`index_jsonl` works in-process.
    JSONL_PARALLEL_MIN_BYTES = 1 << 20
    #: A shard of :meth:`index_jsonl` parses at most this many bytes of the file at a time.
    JSONL_SHARD_PIECE_BYTES = 1 << 26
    #: The memory of one build (direct, or a shard's), as ``index_jsonl(memory_gb=...)`` estimates
    #: it: bytes per token, per posting (a document's distinct term: the postings built at once,
    #: the whole build's divided by its passes, see ``IndexBuilder.postings_budget``), per document,
    #: per distinct term (``HEAPS_EXPONENT``: a build's vocabulary grows as its tokens to this
    #: power, from the samples' vocabulary) and per process. MS MARCO v2, shards of 1M to 9.5M
    #: passages built by a pool worker, 1 to 5 passes: 0 to 6% over (1M: 14%).
    BUILD_BYTES_PER_TOKEN = 4.0
    BUILD_BYTES_PER_POSTING = 5
    BUILD_BYTES_PER_DOCUMENT = 35
    BUILD_BYTES_PER_TERM = 100
    BUILD_BYTES_PER_PROCESS = 250 << 20
    HEAPS_EXPONENT = 0.7
    #: At most this many passes over a build's tokens, each building the postings of a range of
    #: terms: a pass costs a scan of the tokens (2 s for 16M passages), and from about 4 the
    #: tokenizing phase is the peak.
    MAX_BUILD_PASSES = 4
    #: ``index_jsonl(memory_gb=...)`` plans its builds in this part of the budget: equal shards
    #: started together reach their peaks together (138M, 8 workers, 16 GiB: 20 shards, 12.7 to
    #: 13.0 GiB from one run to another).
    MEMORY_GB_MARGIN = 0.9
    #: ``index_jsonl(memory_gb=...)`` counts the tokens, documents and distinct terms of this many
    #: pieces of the file, of this many bytes each, spread over it (the whole file when smaller).
    JSONL_SAMPLE_PIECES = 8
    JSONL_SAMPLE_BYTES = 1 << 21

    def _plan_jsonl_memory(self, path, end, id_field, text_field, workers, shards, memory_gb):
        # (shards, workers, postings budget) for index_jsonl(memory_gb=...), see _plan_memory
        direct_workers = 0 if workers <= 1 or end < self.JSONL_PARALLEL_MIN_BYTES else workers
        return self._plan_memory(lambda: self._jsonl_sample(path, end, id_field, text_field), workers, shards,
                                 memory_gb, direct_workers)

    def _plan_files_memory(self, file_paths, workers, shards, memory_gb):
        # (shards, workers, postings budget) for index_files(memory_gb=...), see _plan_memory
        direct_workers = 0 if workers <= 1 or len(file_paths) < self.PARALLEL_MIN_FILES else min(workers, len(file_paths))
        shards, workers, postings_budget = self._plan_memory(lambda: self._files_sample(file_paths), workers, shards,
                                                             memory_gb, direct_workers)
        return min(shards, max(1, len(file_paths))), workers, postings_budget

    def _plan_memory(self, sample, workers, shards, memory_gb, direct_workers):
        # (shards, workers, postings budget) of a build in memory_gb: the fewest shards, at least
        # `shards`, whose builds, `workers` at a time beside this process, fit in memory_gb by the
        # model above in at most MAX_BUILD_PASSES passes; fewer workers when each wouldn't have
        # twice a process's own memory; then the fewest passes (the postings budget: 0 for one).
        # sample: a function giving the corpus's _Sample; direct_workers: the processes tokenizing
        # beside a direct build
        budget = memory_gb * 2 ** 30 * self.MEMORY_GB_MARGIN
        process = self.BUILD_BYTES_PER_PROCESS
        if budget < 3 * process:
            raise ValueError("memory_gb: at least %.1f to build an index"
                             % (3 * process / 2 ** 30 / self.MEMORY_GB_MARGIN))
        sample = sample()

        def postings_bytes(parts):
            return self.BUILD_BYTES_PER_POSTING * sample.postings / parts

        def build(parts, passes):
            # the memory of one build over 1/parts of the file, its postings in `passes` ranges
            tokens = sample.tokens / parts
            terms = sample.sampled_terms * (tokens / sample.sampled_tokens) ** self.HEAPS_EXPONENT \
                if sample.sampled_tokens else 0
            return (self.BUILD_BYTES_PER_TOKEN * tokens + postings_bytes(parts) / passes
                    + self.BUILD_BYTES_PER_DOCUMENT * sample.documents / parts
                    + self.BUILD_BYTES_PER_TERM * terms + process)

        def plan(parts, workers, beside):
            # the fewest passes that fit beside `beside` bytes, the budget they give
            for passes in range(1, self.MAX_BUILD_PASSES + 1):
                if beside + min(workers, parts) * build(parts, passes) <= budget:
                    return parts, workers, 0 if passes == 1 else math.ceil(postings_bytes(parts) / passes)
            return None
        # a direct build: its workers tokenize beside it, a process each
        if shards <= 1 and plan(1, 1, direct_workers * process) is not None:
            return plan(1, workers, direct_workers * process)
        workers = max(1, min(workers, int((budget - process) // (2 * process))))

        def fits(parts):
            return plan(parts, workers, process) is not None
        # the fewest: doubling, then halving the interval (a build shrinks with its share)
        low, high = max(shards, 2) - 1, max(shards, 2)
        while not fits(high):
            low, high = high, high * 2
        while high - low > 1:
            middle = (low + high) // 2
            low, high = (low, middle) if fits(middle) else (middle, high)
        return plan(high, workers, process)

    _Sample = collections.namedtuple("_Sample", "tokens documents sampled_tokens sampled_terms postings")

    def _jsonl_sample(self, path, end, id_field, text_field):
        # the tokens, documents and postings (distinct terms of each document) of [0, end),
        # estimated from JSONL_SAMPLE_PIECES line-aligned pieces spread over it (counted exactly
        # when it is smaller than them), and the tokens and distinct terms of those pieces
        if end <= self.JSONL_SAMPLE_PIECES * self.JSONL_SAMPLE_BYTES:
            pieces = [(0, end)]
        else:
            pieces = []
            with open(path, 'rb') as f:
                for start, _ in self._jsonl_ranges(path, end, self.JSONL_SAMPLE_PIECES):
                    f.seek(start + self.JSONL_SAMPLE_BYTES - 1)
                    f.readline()        # finish the line: the piece ends with it
                    pieces.append((start, min(f.tell(), end)))
        tokens = documents = postings = sampled = 0
        terms = set()
        for start, stop in pieces:
            record_ids, chunk = self._tokenize_jsonl_range(path, start, stop, id_field, text_field)
            documents += len(record_ids)
            chunk_tokens, chunk_postings = self._count_chunk(chunk, terms)
            tokens += chunk_tokens
            postings += chunk_postings
            sampled += stop - start
        if sampled == 0:
            return self._Sample(0, 0, 0, 0, 0)
        scale = end / sampled
        return self._Sample(tokens * scale, documents * scale, tokens, len(terms), postings * scale)

    def _files_sample(self, file_paths):
        # as _jsonl_sample, for index_files: the documents counted, the tokens and postings
        # estimated by the files' sizes from JSONL_SAMPLE_PIECES runs of consecutive files spread
        # over the list, of JSONL_SAMPLE_BYTES each (every file when they are smaller than them)
        sizes = [os.path.getsize(file_path) for file_path in file_paths]
        total = sum(sizes)
        if total <= self.JSONL_SAMPLE_PIECES * self.JSONL_SAMPLE_BYTES:
            picked = range(len(file_paths))
        else:
            picked = set()
            for piece in range(self.JSONL_SAMPLE_PIECES):
                i, size = len(file_paths) * piece // self.JSONL_SAMPLE_PIECES, 0
                while i < len(file_paths) and size < self.JSONL_SAMPLE_BYTES:
                    picked.add(i)
                    size += sizes[i]
                    i += 1
            picked = sorted(picked)
        sampled = sum(sizes[i] for i in picked)
        if sampled == 0:
            return self._Sample(0, len(file_paths), 0, 0, 0)
        terms = set()
        tokens, postings = self._count_chunk(self._tokenize_paths([file_paths[i] for i in picked]), terms)
        scale = total / sampled
        return self._Sample(tokens * scale, len(file_paths), tokens, len(terms), postings * scale)

    @staticmethod
    def _count_chunk(chunk, terms):
        # (tokens, postings: each document's distinct terms) of a chunk; its words added to terms
        terms.update(_chunk_words(chunk))
        term_ids, begin, postings = _uint_array(chunk[2]), 0, 0
        for document_end in _uint_array(chunk[3]):
            postings += len(set(term_ids[begin:document_end]))
            begin = document_end
        return len(term_ids), postings

    def _index_jsonl_sharded(self, path, end, id_field, text_field, workers, shards, merge, postings_budget=0):
        ranges = self._jsonl_ranges(path, end, shards)
        with IndexStore(self.index_dir, self.prefix).new_generation() as base:
            # inside the new generation, so they go away with it if anything fails
            shard_dir = tempfile.mkdtemp(prefix=".shards-", dir=os.path.dirname(base))
            try:
                if merge:
                    shard_bases = [os.path.join(shard_dir, "shard%d" % i) for i in range(len(ranges))]
                else:
                    segments = ["seg-%06d-%s" % (i, secrets.token_hex(4)) for i in range(len(ranges))]
                    shard_bases = [native.SegmentedIndex.segment_base(base, segment) for segment in segments]
                    for shard_base in shard_bases:
                        os.mkdir(os.path.dirname(shard_base))
                with ProcessPoolExecutor(max_workers=max(1, min(workers, len(ranges))),
                                         initializer=_pool_worker_init) as pool:
                    # the documents before each shard: its documents' global ids
                    counts = list(pool.map(self._count_jsonl_range, [path] * len(ranges),
                                           [start for start, _ in ranges], [stop for _, stop in ranges]))
                    firsts = list(accumulate([0] + counts[:-1]))
                    futures = [pool.submit(self._build_jsonl_shard, shard_bases[i], path, ranges[i][0], ranges[i][1],
                                           id_field, text_field, firsts[i], postings_budget)
                               for i in range(len(ranges))]
                    for future in futures:
                        future.result()
                if merge:
                    self._merge_shards(shard_bases, base, with_names=True)
                else:
                    self._check_names_unique(shard_bases)
                    native.SegmentedIndex.write_manifest(base, segments)
            finally:
                shutil.rmtree(shard_dir, ignore_errors=True)
            _write_meta(base, self.tokenizer_options, self._with_local_index)
        return _RecordNames(base, sum(counts))

    @staticmethod
    def _count_jsonl_range(path, start, stop):
        # Runs in a worker process: the records of the lines in [start, stop).
        count, position = 0, start
        with open(path, 'rb') as f:
            f.seek(start)
            while position < stop:
                line = f.readline()
                if not line:
                    break
                position += len(line)
                if line.strip():
                    count += 1
        return count

    def _build_jsonl_shard(self, shard_base, path, start, stop, id_field, text_field, first_doc_id, postings_budget=0):
        # Runs in a worker process: the index, lengths and names of the lines in [start, stop).
        self._write_jsonl_shard(shard_base, path, start, stop, id_field, text_field, first_doc_id, postings_budget)
        _release_freed_memory()             # the worker's next shard starts from a clean slate

    def _write_jsonl_shard(self, shard_base, path, start, stop, id_field, text_field, first_doc_id, postings_budget=0):
        pieces = max(1, -(-(stop - start) // self.JSONL_SHARD_PIECE_BYTES))
        names, lengths, chunks = [], array('I'), []
        for piece_start, piece_stop in self._jsonl_ranges(path, stop, pieces, start):
            piece_ids, chunk = self._tokenize_jsonl_range(path, piece_start, piece_stop, id_field, text_field)
            packed = _pack_names(map(str, piece_ids))
            names.append(packed.names)
            lengths.extend(packed.lengths)
            chunks.append(chunk)
        native.NindDocNames.write_packed(shard_base + DOC_NAMES_EXTENSION, first_doc_id, b''.join(names), lengths)
        del names, lengths
        self._write_chunks(chunks, first_doc_id, shard_base, postings_budget)

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
    def _jsonl_ranges(path, end, pieces, begin=0):
        # [start, stop) byte ranges covering [begin, end) (begin: at the beginning of a line), each
        # starting at the beginning of a line
        bounds = [begin]
        with open(path, 'rb') as f:
            for i in range(1, pieces):
                target = begin + (end - begin) * i // pieces
                if target <= bounds[-1]:
                    continue
                f.seek(target - 1)
                f.readline()            # finish the line target - 1 is in: the next one starts a range
                if f.tell() >= end:
                    break
                bounds.append(f.tell())
        bounds.append(end)
        return [(bounds[i], bounds[i + 1]) for i in range(len(bounds) - 1)]

    def _tokenize_jsonl_range_packed(self, path, start, stop, id_field, text_field):
        # Runs in a worker process: _tokenize_jsonl_range, string record ids packed (_PackedNames:
        # ~24 bytes a document, a list of str ~80); other ids as they are, a list
        record_ids, chunk = self._tokenize_jsonl_range(path, start, stop, id_field, text_field)
        if all(isinstance(doc_ident, str) for doc_ident in record_ids):
            return _pack_names(record_ids), chunk
        return record_ids, chunk

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

    def force_merge(self):
        """Merge the index's segments into one standard index, its deleted documents dropped.

        Searches on one thread are at their fastest then, and
        :attr:`NindEngine.lexicon` and the other direct accesses work. Costs a
        rewrite of the index; published at once, as a build.

        :return: what was done (``nind._native.SegmentedUpdateStats``).
        """
        if native is None:
            raise ImportError('nind._native compiled extension is not available (build it via "uv sync")')
        store = IndexStore(self.index_dir, self.prefix)
        with store.new_generation() as base:
            source = store.published_base()
            if source is None:
                raise FileNotFoundError("No nind index %s in %s" % (self.prefix, self.index_dir))
            store.link_generation(source, base)
            return native.SegmentedUpdate.force_merge(base)

    def update(self):
        """Start an incremental update of the index (created if there is none yet).

        An index written in format v1 (by a nind older than format v2) stays
        readable as a segment; merges rewrite it in format v2.

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

    def _write_chunks(self, chunks, first_doc_id, base, postings_budget=0):
        # writes the index files at base (a path without extension), plus the document lengths
        # chunks: (words' bytes, their ends, term ids, ends) of consecutive documents, the first one
        # being document first_doc_id (see native.Tokenizer.tokenize_batch_packed); the list is
        # emptied as they are added (the caller should hold no other reference to them: they would
        # stay in memory). The builder numbers the words (nothing per word in Python) and writes the
        # lexicon, its words by increasing bytes; postings_budget: see IndexBuilder.postings_budget.
        builder = native.IndexBuilder()
        builder.reserve(sum(len(chunk[2]) for chunk in chunks) // 4, sum(len(chunk[3]) for chunk in chunks) // 4)
        builder.postings_budget = postings_budget
        doc_id = first_doc_id
        chunks.reverse()
        while chunks:
            words, word_ends, term_ids, ends = chunks.pop()
            ends = _uint_array(ends)
            builder.add_batch(doc_id, ends, _uint_array(term_ids), words, _uint_array(word_ends))
            doc_id += len(ends)
            del words, word_ends, term_ids, ends
        terms_nb = builder.words_nb
        lexicon_writer = native.NindLexiconIndex(base, is_writer=True, indirection_bloc_size=max(1, terms_nb))
        lexicon_writer.begin_bulk()
        builder.write_lexicon(lexicon_writer)
        lexicon_writer.end_bulk()
        identification = lexicon_writer.get_identification()
        del lexicon_writer
        writers = [native.NindTermIndex(base, is_writer=True, lexicon_identification=identification,
                                        specifics_number=0, indirection_bloc_size=terms_nb + 1)]
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
        lengths = _uint_array(lengths)
        _write_doc_length_arrays(base, array('Q', compress(_doc_id_array(doc_ids), lengths)),
                                 array('I', filter(None, lengths)))

    def _write_index(self, token_lists, first_doc_id, base):
        # writes the index of documents given as token lists (first one: document first_doc_id)
        self._write_chunks([_chunk_from_token_lists(token_lists)], first_doc_id, base)

    def _index_sharded(self, file_paths, workers, shards, base, doc_names, merge, postings_budget=0):
        if workers is None:
            workers = os.cpu_count() or 1
        # contiguous parts, so that each shard's documents keep their global ids
        bounds = [len(file_paths) * i // shards for i in range(shards + 1)]
        # inside the new generation, so they go away with it if anything fails
        shard_dir = tempfile.mkdtemp(prefix=".shards-", dir=os.path.dirname(base))
        try:
            if merge:
                shard_bases = [os.path.join(shard_dir, "shard%d" % i) for i in range(shards)]
            else:
                segments = ["seg-%06d-%s" % (i, secrets.token_hex(4)) for i in range(shards)]
                shard_bases = [native.SegmentedIndex.segment_base(base, segment) for segment in segments]
                for shard_base in shard_bases:
                    os.mkdir(os.path.dirname(shard_base))
            with ProcessPoolExecutor(max_workers=max(1, min(workers, shards)), initializer=_pool_worker_init) as pool:
                futures = [pool.submit(self._build_shard, shard_bases[i], file_paths[bounds[i]:bounds[i + 1]], bounds[i],
                                       None if doc_names is None else doc_names[bounds[i]:bounds[i + 1]],
                                       postings_budget)
                           for i in range(shards)]
                for future in futures:
                    future.result()
            if merge:
                self._merge_shards(shard_bases, base, with_names=doc_names is not None)
            else:
                if doc_names is not None:
                    self._check_names_unique(shard_bases)
                native.SegmentedIndex.write_manifest(base, segments)
        finally:
            shutil.rmtree(shard_dir, ignore_errors=True)

    @staticmethod
    def _check_names_unique(shard_bases):
        # a name in two segments would make name -> document ambiguous (ValueError)
        native.NindDocNames.check_unique([shard_base + DOC_NAMES_EXTENSION for shard_base in shard_bases])

    @staticmethod
    def _merge_shards(shard_bases, base, with_names=False):
        # shards of contiguous documents, in document order: their lengths concatenated
        doc_ids, lengths = array('Q'), array('I')
        for shard_base in shard_bases:
            shard_ids, shard_lengths = _read_doc_length_arrays(shard_base)
            doc_ids.extend(shard_ids)
            lengths.extend(shard_lengths)
        native.merge_indexes(shard_bases, base, doc_ids=doc_ids, doc_lengths=lengths)
        _write_doc_length_arrays(base, doc_ids, lengths)
        if with_names:
            native.NindDocNames.merge([shard_base + DOC_NAMES_EXTENSION for shard_base in shard_bases],
                                      base + DOC_NAMES_EXTENSION)

    def _build_shard(self, shard_base, file_paths, first_doc_id, doc_names=None, postings_budget=0):
        # Runs in a worker process (self is a copy: keeps an overridden tokenizer).
        self._write_chunks(self._tokenize_chunks(file_paths, 1), first_doc_id, shard_base, postings_budget)
        if doc_names is not None:
            _write_doc_names(shard_base, dict(enumerate(doc_names, first_doc_id)))
        _release_freed_memory()

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
        idents, ends, doc_ids, freqs = array('I'), array('I'), array('Q'), array('I')
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
        idents, ends, term_ids, positions = array('Q'), array('I'), array('I'), array('I')
        for index, tokens in enumerate(corpus_tokens):
            idents.append(first_doc_id + index)
            term_ids.extend(term_to_id[token] for token in tokens)
            positions.extend(range(len(tokens)))
            ends.append(len(term_ids))
        lengths = array('I', [1]) * len(term_ids)
        local_writer.begin_bulk()
        local_writer.set_local_defs_arrays(idents, ends, term_ids, positions, lengths, lexicon_identification)
        local_writer.end_bulk()
