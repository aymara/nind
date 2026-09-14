import math
import os
import re
import time
from collections import Counter, defaultdict
from .NindLexiconindex import NindLexiconindex
from .NindTermindex import NindTermindex
from .NindLocalindex import NindLocalindex
from . import NindFile

############################################################
# Flags and sizes from the nind binary format (see NindPadFile.py, NindIndex.py,
# NindLexiconindex.py, NindTermindex.py, NindLocalindex.py for the full grammar).
############################################################
FLAG_INDEXEJ = 47
FLAG_SPEJCIFIQUE = 57
FLAG_IDENTIFICATION = 53
TAILLE_INDIRECTION = 8         # <offsetDejfinition:5> <longueurDejfinition:3>

FLAG_LEXICON_DEJFINITION = 13
FLAG_TERME_DEJFINITION = 17
FLAG_CG = 61
FLAG_LOCAL_DEJFINITION = 19

class NindEngine:
    """
    High-level Python frontend for the Nind Indexing Module.
    Provides a simplified API for indexing and BM25-based search.
    """
    def __init__(self, index_dir):
        """
        Initialize the engine by opening the required index files.
        :param index_dir: Directory containing .nindlexiconindex, .nindtermindex, and .nindlocalindex files.
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
        self.term_index = NindTermindex(os.path.join(index_dir, f"{prefix}.nindtermindex"))
        self.local_index = NindLocalindex(os.path.join(index_dir, f"{prefix}.nindlocalindex"))

        self.total_docs = len(self.local_index.donneidentifiantsExternes())
        self.avg_doc_len = self._calculate_avg_doc_len()

    def _calculate_avg_doc_len(self):
        """Calculate average document length across the corpus."""
        if self.total_docs == 0: return 0
        total_len = sum(self.get_doc_len(ext_id) for ext_id, _ in self.local_index.donneidentifiantsExternes())
        return total_len / self.total_docs

    def tokenize(self, text):
        """
        Basic tokenizer for programming languages.
        Splits by non-alphanumeric characters and handles camelCase/snake_case.
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
        """Get the internal identifier for a term."""
        return self.lexicon.donneIdentifiant([term])

    def get_tf(self, term, doc_id):
        """Get the term frequency in a specific document."""
        term_id = self.get_term_id(term)
        if term_id == 0: return 0

        term_defs = self.term_index.donneListeTermesCG(term_id)
        for cg, freq, docs in term_defs:
            for d_id, d_freq in docs:
                if d_id == doc_id:
                    return d_freq
        return 0

    def get_df(self, term):
        """Get the document frequency of a term."""
        term_id = self.get_term_id(term)
        if term_id == 0: return 0

        term_defs = self.term_index.donneListeTermesCG(term_id)
        df = 0
        for cg, freq, docs in term_defs:
            df += len(docs)
        return df

    def get_doc_len(self, doc_id):
        """Get the length (total token occurrences) of a specific document, given its external id."""
        return sum(len(localisations) for _, _, localisations in self.local_index.donneListeTermes(doc_id))

    def bm25_score(self, query, doc_id, k1=1.5, b=0.75):
        """Calculate the BM25 score for a document given a query."""
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
        """Search for the top_k most relevant documents for a query."""
        scores = []
        for doc_id, _ in self.local_index.donneidentifiantsExternes():
            score = self.bm25_score(query, doc_id)
            if score > 0:
                scores.append((doc_id, score))

        scores.sort(key=lambda x: x[1], reverse=True)
        return scores[:top_k]

class NindIndexer:
    """
    Indexer to create Nind binary indices from a list of files.
    """
    def __init__(self, index_dir, prefix="corpus"):
        self.index_dir = index_dir
        self.prefix = prefix
        # Do not initialize engine here; files may not exist yet
        self.engine = None

    def index_files(self, file_paths):
        """
        Processes a list of files and writes them to the binary Nind format.
        """
        corpus_tokens = []
        global_lexicon = Counter()

        for path in file_paths:
            with open(path, 'r', encoding='utf-8', errors='ignore') as f:
                tokens = self._default_tokenize(f.read())
                corpus_tokens.append(tokens)
                global_lexicon.update(tokens)

        term_to_id = {}
        id_counter = 1
        for term in sorted(global_lexicon.keys()):
            term_to_id[term] = id_counter
            id_counter += 1

        inverted_index = defaultdict(list)
        for doc_id, tokens in enumerate(corpus_tokens):
            doc_counts = Counter(tokens)
            for term, freq in doc_counts.items():
                term_id = term_to_id[term]
                inverted_index[term_id].append((doc_id, freq))

        max_term_id = max(term_to_id.values()) if term_to_id else 0
        self._write_lexicon(term_to_id)
        self._write_term_index(inverted_index, max_term_id)
        self._write_local_index(corpus_tokens, term_to_id)

    def _default_tokenize(self, text):
        tokens = re.split(r'[^a-zA-Z0-9_]', text)
        refined = []
        for t in tokens:
            if not t: continue
            parts = re.findall(r'[A-Z]?[a-z]+|[A-Z]+(?=[A-Z][a-z]|\d|\W|$)|\d+', t)
            refined.extend(parts if parts else [t])
        return refined

    ########################################################################
    # Shared helpers for the nind "pad file" envelope: fixed header, a single
    # indexed block (indirection table), the definitions themselves ("en vrac"),
    # then the specifics + identification trailer required by NindPadFile.
    ########################################################################
    def _open_index_file(self, suffix, nombre_index, taille_specifiques):
        file_path = os.path.join(self.index_dir, f"{self.prefix}.{suffix}")
        nf = NindFile.NindFile(file_path, enEjcriture=True)
        nf.ejcritNombre1(TAILLE_INDIRECTION)    # tailleEntreje
        nf.ejcritNombre3(taille_specifiques)    # tailleSpejcifiques
        nf.ejcritNombre1(FLAG_INDEXEJ)
        nf.ejcritNombre5(0)                     # addrBlocSuivant : un seul bloc
        nf.ejcritNombre3(nombre_index)
        indirection_offset = nf.tell()
        for _ in range(nombre_index):
            nf.ejcritNombre5(0)
            nf.ejcritNombre3(0)
        return nf, indirection_offset

    def _patch_indirection(self, nf, indirection_offset, index, offset, length):
        nf.seek(indirection_offset + index * TAILLE_INDIRECTION, 0)
        nf.ejcritNombre5(offset)
        nf.ejcritNombre3(length)

    def _write_footer(self, nf, specifiques_entier4, max_identifiant):
        nf.seek(0, 2)
        nf.ejcritNombre1(FLAG_SPEJCIFIQUE)
        for valeur in specifiques_entier4:
            nf.ejcritNombre4(valeur)
        nf.ejcritNombre1(FLAG_IDENTIFICATION)
        nf.ejcritNombre4(max_identifiant)
        nf.ejcritNombre4(int(time.time()) & 0xFFFFFFFF)

    ########################################################################
    # .nindlexiconindex : hash table keyed by clefB(mot) % nombreIndirection.
    ########################################################################
    def _write_lexicon(self, term_to_id):
        nombre_buckets = max(1, len(term_to_id))
        buckets = defaultdict(list)
        for term, tid in term_to_id.items():
            buckets[NindFile.clefB(term) % nombre_buckets].append((term, tid))

        nf, indirection_offset = self._open_index_file('nindlexiconindex', nombre_buckets, 0)
        try:
            placements = []
            for bucket in sorted(buckets):
                def_start = nf.tell()
                nf.ejcritNombre1(FLAG_LEXICON_DEJFINITION)   # <flagDejfinition=13>
                nf.ejcritNombre3(bucket)                     # <identifiantHash>
                len_pos = nf.tell()
                nf.ejcritNombre3(0)                          # <longueurDonnejes> (placeholder)
                data_start = nf.tell()
                for term, tid in sorted(buckets[bucket], key=lambda t: t[1]):
                    encoded = term.encode('utf-8')
                    nf.ejcritNombre1(len(encoded))           # <motSimple> = <MotUtf8>
                    nf.ejcritChaine(term)
                    nf.ejcritNombre4(tid)                    # <identifiantS>
                    nf.ejcritNombreULat(0)                   # <nbreComposejs> : pas de mots composés
                data_end = nf.tell()
                nf.seek(len_pos, 0)
                nf.ejcritNombre3(data_end - data_start)
                nf.seek(0, 2)
                placements.append((bucket, def_start, data_end - def_start))

            for bucket, offset, length in placements:
                self._patch_indirection(nf, indirection_offset, bucket, offset, length)

            max_id = max(term_to_id.values()) if term_to_id else 0
            self._write_footer(nf, [], max_id)
        finally:
            nf.close()

    ########################################################################
    # .nindtermindex : directly indexed by identifiant terme (1..maxTermId).
    ########################################################################
    def _write_term_index(self, inverted_index, max_term_id):
        nombre_index = max_term_id + 1     # slot 0 inutilisé
        nf, indirection_offset = self._open_index_file('nindtermindex', nombre_index, 0)
        try:
            placements = []
            for tid in sorted(inverted_index):
                postings = sorted(inverted_index[tid])   # tri croissant par doc_id pour le delta
                def_start = nf.tell()
                nf.ejcritNombre1(FLAG_TERME_DEJFINITION)  # <flagDejfinition=17>
                nf.ejcritNombre4(tid)                     # <identifiantTerme>
                len_pos = nf.tell()
                nf.ejcritNombre3(0)                       # <longueurDonnejes> (placeholder)
                data_start = nf.tell()
                nf.ejcritNombre1(FLAG_CG)                 # <flagCg=61>
                nf.ejcritNombre1(0)                       # <catejgorie> : non utilisée
                nf.ejcritNombreULat(sum(f for _, f in postings))   # <frejquenceTerme>
                nf.ejcritNombreULat(len(postings))                 # <nbreDocs>
                noDocPrec = 0
                for doc_id, freq in postings:
                    nf.ejcritNombreULat(doc_id - noDocPrec)  # <identDocRelatif>
                    nf.ejcritNombreULat(freq)                # <frejquenceDoc>
                    noDocPrec = doc_id
                data_end = nf.tell()
                nf.seek(len_pos, 0)
                nf.ejcritNombre3(data_end - data_start)
                nf.seek(0, 2)
                placements.append((tid, def_start, data_end - def_start))

            for tid, offset, length in placements:
                self._patch_indirection(nf, indirection_offset, tid, offset, length)

            self._write_footer(nf, [], max_term_id)
        finally:
            nf.close()

    ########################################################################
    # .nindlocalindex : directly indexed by identifiant document interne
    # (1..nombreDocuments), avec table de traduction externe <-> interne.
    ########################################################################
    def _write_local_index(self, corpus_tokens, term_to_id):
        nombre_documents = len(corpus_tokens)
        nombre_index = nombre_documents + 1     # slot 0 inutilisé
        specifiques = [nombre_documents, nombre_documents]   # maxIdentifiantInterne, nombreDocuments

        nf, indirection_offset = self._open_index_file('nindlocalindex', nombre_index, 8)
        try:
            placements = []
            for doc_id, tokens in enumerate(corpus_tokens):
                noDocInterne = doc_id + 1
                identifiantExterne = doc_id

                positions_par_terme = defaultdict(list)
                for position, token in enumerate(tokens):
                    positions_par_terme[term_to_id[token]].append(position)

                def_start = nf.tell()
                nf.ejcritNombre1(FLAG_LOCAL_DEJFINITION)  # <flagDejfinition=19>
                nf.ejcritNombre3(noDocInterne)             # <identifiantDoc>
                nf.ejcritNombre4(identifiantExterne)       # <identifiantExterne>
                len_pos = nf.tell()
                nf.ejcritNombre3(0)                        # <longueurDonnejes> (placeholder)
                data_start = nf.tell()
                noTermePrec = 0
                for tid in sorted(positions_par_terme):
                    nf.ejcritNombreSLat(tid - noTermePrec)   # <identTermeRelatif>
                    noTermePrec = tid
                    nf.ejcritNombre1(0)                      # <catejgorie> : non utilisée
                    positions = positions_par_terme[tid]
                    nf.ejcritNombre1(len(positions))         # <nbreLocalisations>
                    localisationPrec = 0
                    for position in positions:
                        nf.ejcritNombreSLat(position - localisationPrec)   # <localisationRelatif>
                        localisationPrec = position
                        nf.ejcritNombre1(1)                                # <longueur> : non utilisée
                data_end = nf.tell()
                nf.seek(len_pos, 0)
                nf.ejcritNombre3(data_end - data_start)
                nf.seek(0, 2)
                placements.append((noDocInterne, def_start, data_end - def_start))

            for noDocInterne, offset, length in placements:
                self._patch_indirection(nf, indirection_offset, noDocInterne, offset, length)

            self._write_footer(nf, specifiques, nombre_documents)
        finally:
            nf.close()
