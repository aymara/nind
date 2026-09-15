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
import math
import os
import re
from collections import Counter, defaultdict
from .NindLexiconindex import NindLexiconindex
from .NindTermindex import NindTermindex
from .NindLocalindex import NindLocalindex
try:
    from . import _native as native
except ImportError:
    # nind._native is a compiled extension: not available when introspecting
    # the pure-Python source without building it (e.g. Sphinx autodoc, see
    # docs/conf.py). NindIndexer.index_files still requires it - only module
    # import is tolerant.
    native = None

class NindEngine:
    """BM25 search engine on top of a nind index produced by :class:`NindIndexer`.

    Opens the three index files (lexicon, term, local) for a corpus and
    exposes term/document-frequency lookups plus a ready-to-use
    :meth:`search`. Read-only: to build the index files this reads, use
    :class:`NindIndexer`.
    """

    def __init__(self, index_dir):
        """Open the index files found in ``index_dir``.

        :param index_dir: directory containing exactly one corpus's
            ``.nindlexiconindex``, ``.nindtermindex`` and
            ``.nindlocalindex`` files (same filename prefix).
        :raises FileNotFoundError: if no ``.nindlexiconindex`` file is
            found in ``index_dir``.
        """
        self.index_dir = index_dir

        files = os.listdir(index_dir)
        prefix = None
        for f in files:
            if f.endswith('.nindlexiconindex'):
                prefix = f.replace('.nindlexiconindex', '')
                break

        if not prefix:
            raise FileNotFoundError("Could not find .nindlexiconindex file in the specified directory.")

        self.lexicon = NindLexiconindex(os.path.join(index_dir, f"{prefix}.nindlexiconindex"))
        lexicon_identification = self.lexicon.donneIdentification()
        self.term_index = NindTermindex(os.path.join(index_dir, f"{prefix}.nindtermindex"), lexicon_identification)
        self.local_index = NindLocalindex(os.path.join(index_dir, f"{prefix}.nindlocalindex"), lexicon_identification)

        self.total_docs = len(self.local_index.donneidentifiantsExternes())
        self.avg_doc_len = self._calculate_avg_doc_len()

    def _calculate_avg_doc_len(self):
        """Calculate average document length (in tokens) across the corpus."""
        if self.total_docs == 0: return 0
        total_len = sum(self.get_doc_len(ext_id) for ext_id, _ in self.local_index.donneidentifiantsExternes())
        return total_len / self.total_docs

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
        return self.lexicon.donneIdentifiant([term])

    def get_tf(self, term, doc_id):
        """Get the raw term frequency of ``term`` in a specific document.

        :param term: the term to look up.
        :param doc_id: the document's external identifier.
        :return: the number of occurrences (summed across grammatical
            categories), or ``0`` if the term is unknown or absent from
            that document.
        """
        term_id = self.get_term_id(term)
        if term_id == 0: return 0

        term_defs = self.term_index.donneListeTermesCG(term_id)
        for term_cg in term_defs:
            for document in term_cg.documents:
                if document.ident == doc_id:
                    return document.frequency
        return 0

    def get_df(self, term):
        """Get the document frequency of ``term`` (how many documents contain it).

        :param term: the term to look up.
        :return: the number of documents containing the term, or ``0`` if
            it is unknown.
        """
        term_id = self.get_term_id(term)
        if term_id == 0: return 0

        term_defs = self.term_index.donneListeTermesCG(term_id)
        df = 0
        for term_cg in term_defs:
            df += len(term_cg.documents)
        return df

    def get_doc_len(self, doc_id):
        """Get the length (total token occurrences) of a document.

        :param doc_id: the document's external identifier.
        :return: the document's length in tokens.
        """
        return sum(len(term.localisation) for term in self.local_index.donneListeTermes(doc_id))

    def bm25_score(self, query, doc_id, k1=1.5, b=0.75):
        """Compute the Okapi BM25 relevance score of a document for a query.

        :param query: the query text (tokenized with :meth:`tokenize`).
        :param doc_id: the document's external identifier.
        :param k1: BM25 term-frequency saturation parameter.
        :param b: BM25 document-length normalization parameter.
        :return: the BM25 score (higher is more relevant); ``0.0`` if no
            query term occurs in the document.
        """
        score = 0.0
        tokens = self.tokenize(query)

        for term in tokens:
            tf = self.get_tf(term, doc_id)
            if tf == 0: continue

            df = self.get_df(term)
            idf = math.log((self.total_docs - df + 0.5) / (df + 0.5) + 1.0)

            doc_len = self.get_doc_len(doc_id)
            numerator = tf * (k1 + 1)
            denominator = tf + k1 * (1 - b + b * (doc_len / self.avg_doc_len))

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
        scores = []
        for doc_id, _ in self.local_index.donneidentifiantsExternes():
            score = self.bm25_score(query, doc_id)
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

    def index_files(self, file_paths):
        """Tokenize ``file_paths`` and write the resulting corpus as a nind index.

        Each file becomes one document, identified externally by its
        0-based position in ``file_paths``. Overwrites any existing index
        files with the same prefix in ``index_dir``.

        :param file_paths: list of paths to UTF-8 (or UTF-8-decodable, with
            errors ignored) text files to index.
        """
        corpus_tokens = []
        global_lexicon = Counter()

        for path in file_paths:
            with open(path, 'r', encoding='utf-8', errors='ignore') as f:
                tokens = self._default_tokenize(f.read())
                corpus_tokens.append(tokens)
                global_lexicon.update(tokens)

        term_to_id, lexicon_identification = self._write_lexicon(sorted(global_lexicon.keys()))

        inverted_index = defaultdict(list)
        for doc_id, tokens in enumerate(corpus_tokens):
            doc_counts = Counter(tokens)
            for term, freq in doc_counts.items():
                term_id = term_to_id[term]
                inverted_index[term_id].append((doc_id, freq))

        self._write_term_index(inverted_index, lexicon_identification)
        self._write_local_index(corpus_tokens, term_to_id, lexicon_identification)

    def _default_tokenize(self, text):
        tokens = re.split(r'[^a-zA-Z0-9_]', text)
        refined = []
        for t in tokens:
            if not t: continue
            parts = re.findall(r'[A-Z]?[a-z]+|[A-Z]+(?=[A-Z][a-z]|\d|\W|$)|\d+', t)
            refined.extend(parts if parts else [t])
        return refined

    def _base_path(self):
        # nind._native appends the format-specific extension itself.
        return os.path.join(self.index_dir, self.prefix)

    ########################################################################
    # .nindlexiconindex : one simple word per term, ids assigned by the writer.
    ########################################################################
    def _write_lexicon(self, terms):
        if native is None:
            raise ImportError('nind._native compiled extension is not available (build it via "uv sync")')
        lexicon_writer = native.NindLexiconIndex(self._base_path(), is_writer=True,
                                                   indirection_bloc_size=max(1, len(terms)))
        term_to_id = {}
        for term in terms:
            term_to_id[term] = lexicon_writer.add_word([term])
        return term_to_id, lexicon_writer.get_identification()

    ########################################################################
    # .nindtermindex : directly indexed by identifiant terme.
    ########################################################################
    def _write_term_index(self, inverted_index, lexicon_identification):
        max_term_id = max(inverted_index) if inverted_index else 0
        term_writer = native.NindTermIndex(self._base_path(), is_writer=True,
                                            lexicon_identification=lexicon_identification,
                                            specifics_number=0,
                                            indirection_bloc_size=max_term_id + 1)
        for tid in sorted(inverted_index):
            postings = sorted(inverted_index[tid])   # tri croissant par doc_id pour le delta
            term_cg = native.TermCG(cg=0, frequency=sum(freq for _, freq in postings))
            term_cg.documents = [native.Document(ident=doc_id, frequency=freq) for doc_id, freq in postings]
            term_writer.set_term_def(tid, [term_cg], lexicon_identification, [])

    ########################################################################
    # .nindlocalindex : directly indexed by identifiant document externe
    # (la traduction externe <-> interne est faite en interne par le C++).
    ########################################################################
    def _write_local_index(self, corpus_tokens, term_to_id, lexicon_identification):
        local_writer = native.NindLocalIndex(self._base_path(), is_writer=True,
                                              lexicon_identification=lexicon_identification,
                                              indirection_bloc_size=len(corpus_tokens) + 1)
        for doc_id, tokens in enumerate(corpus_tokens):
            positions_par_terme = defaultdict(list)
            for position, token in enumerate(tokens):
                positions_par_terme[term_to_id[token]].append(position)

            terms = []
            for tid in sorted(positions_par_terme):
                term = native.Term(term=tid, cg=0)
                term.localisation = [native.Localisation(position=p, length=1)
                                      for p in positions_par_terme[tid]]
                terms.append(term)
            local_writer.set_local_def(doc_id, terms, lexicon_identification)
