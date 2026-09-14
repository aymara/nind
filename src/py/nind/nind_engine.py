import math
import os
import re
from collections import Counter, defaultdict
from .NindLexiconindex import NindLexiconindex
from .NindTermindex import NindTermindex
from .NindLocalindex import NindLocalindex
from . import NindFile

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
        
        self.total_docs = self.local_index.getDocCount()
        self.avg_doc_len = self._calculate_avg_doc_len()

    def _calculate_avg_doc_len(self):
        """Calculate average document length across the corpus."""
        total_len = 0
        for i in range(self.total_docs):
            length = 0
            if self.local_index.getLocalLength(i, length):
                total_len += length
        return total_len / self.total_docs if self.total_docs > 0 else 0

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
        """Get the length of a specific document."""
        length = 0
        if self.local_index.getLocalLength(doc_id, length):
            return length
        return 0

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
        for doc_id in range(self.total_docs):
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
        
        self._write_lexicon(term_to_id)
        self._write_term_index(inverted_index)
        self._write_local_index(corpus_tokens)

    def _default_tokenize(self, text):
        import re
        tokens = re.split(r'[^a-zA-Z0-9_]', text)
        refined = []
        for t in tokens:
            if not t: continue
            parts = re.findall(r'[A-Z]?[a-z]+|[A-Z]+(?=[A-Z][a-z]|\d|\W|$)|\d+', t)
            refined.extend(parts if parts else [t])
        return refined

    def _write_lexicon(self, term_to_id):
        file_path = os.path.join(self.index_dir, f"{self.prefix}.nindlexiconindex")
        nf = NindFile.NindFile(file_path, enEjcriture=True)
        try:
            # Write file header: tailleEntreje (1 byte) + tailleSpejcifiques (3 bytes)
            # For lexicon index, tailleEntreje is 8 (5 bytes offset + 3 bytes length)
            nf.ejcritNombre1(8)  # tailleEntreje
            nf.ejcritNombre3(0)  # tailleSpejcifiques
            
            # Write first (and only) index block
            nf.ejcritNombre1(47)  # FLAG_INDEXEJ
            nf.ejcritNombre5(0)   # addrBlocSuivant (0 means no next block)
            nf.ejcritNombre3(len(term_to_id))  # nombreIndex
            
            # Write indirection entries for each term
            # Each entry is 8 bytes: offset (5) + length (3)
            # We'll write placeholder offsets/lengths first, then come back to update them
            indirection_offsets = []
            for i in range(len(term_to_id)):
                pos = nf.tell()
                indirection_offsets.append(pos)
                nf.ejcritNombre5(0)  # placeholder offset
                nf.ejcritNombre3(0)  # placeholder length
            
            # Write all definitions
            definition_offsets = []
            for term, tid in term_to_id.items():
                def_start = nf.tell()
                definition_offsets.append(def_start)
                
                # Write definition: <flagDejfinition=13> <identifiantHash> <longueurDonnejes> <donnejesHash>
                nf.ejcritNombre1(13)  # FLAG_DEJFINITION
                nf.ejcritNombre3(tid)  # identifiantHash
                data_start = nf.tell()
                nf.ejcritNombre3(0)  # placeholder for longueurDonnejes
                
                # Write the data: <motSimple> <identifiantS> <nbreComposejs>
                encoded_term = term.encode('utf-8')
                nf.ejcritNombre1(len(encoded_term))
                nf.ejcritChaine(term)
                nf.ejcritNombre4(tid)
                nf.ejcritNombreULat(0)  # nbreComposejs (0 for simple words)
                data_end = nf.tell()
                data_length = data_end - data_start - 3  # Subtract the 3 bytes for longueurDonnejes itself
                
                # Go back and update the definition length
                nf.seek(data_start, 0)
                nf.ejcritNombre3(data_length)
                nf.seek(0, 2)  # Seek to end of file
                
            # Now update all indirection entries with actual offsets and lengths
            for i, (term, tid) in enumerate(term_to_id.items()):
                nf.seek(indirection_offsets[i], 0)
                nf.ejcritNombre5(definition_offsets[i])  # offset
                # Calculate length from definition start to next definition (or end of file)
                if i + 1 < len(definition_offsets):
                    length = definition_offsets[i+1] - definition_offsets[i]
                else:
                    nf.seek(0, 2)
                    end_of_file = nf.tell()
                    length = end_of_file - definition_offsets[i]
                nf.ejcritNombre3(length)
                
            # Write specific data and identification
            # For simplicity, we'll write minimal specific data
            nf.seek(0, 2)  # Go to end
            # Write FLAG_SPEJCIFIQUE
            nf.ejcritNombre1(57)
            # No specific data
            # Write FLAG_IDENTIFICATION
            nf.ejcritNombre1(53)
            nf.ejcritNombre4(len(term_to_id))  # maxIdentifiant
            import time
            nf.ejcritNombre4(int(time.time()))  # identifieurUnique
        finally:
            nf.close()

    def _write_term_index(self, inverted_index):
        file_path = os.path.join(self.index_dir, f"{self.prefix}.nindtermindex")
        nf = NindFile.NindFile(file_path, enEjcriture=True)
        try:
            nf.ejcritNombre1(47)
            nf.ejcritNombre5(0)
            nf.ejcritNombre3(len(inverted_index))
            for tid in inverted_index:
                nf.ejcritNombre5(0)
                nf.ejcritNombre3(0)
            for tid, postings in inverted_index.items():
                nf.ejcritNombre1(17)
                nf.ejcritNombre4(tid)
                total_freq = sum(f for d, f in postings)
                len_def = 1 + 1 + 4 + 4 + len(postings) * 8
                nf.ejcritNombre3(len_def)
                nf.ejcritNombre1(61)
                nf.ejcritNombre1(0)
                nf.ejcritNombreULat(total_freq)
                nf.ejcritNombreULat(len(postings))
                for doc_id, freq in postings:
                    nf.ejcritNombreULat(doc_id)
                    nf.ejcritNombreULat(freq)
        finally:
            nf.close()

    def _write_local_index(self, corpus_tokens):
        file_path = os.path.join(self.index_dir, f"{self.prefix}.nindlocalindex")
        with NindFile.NindFile(file_path, enEjcriture=True) as nf:
            nf.ejcritNombre1(47)
            nf.ejcritNombre5(0)
            nf.ejcritNombre3(len(corpus_tokens))
            for _ in corpus_tokens:
                nf.ejcritNombre5(0)
                nf.ejcritNombre3(0)
            for tokens in corpus_tokens:
                nf.ejcritNombre1(17)
                nf.ejcritNombre4(len(tokens))
                nf.ejcritNombre3(len(tokens) * 4)
                for t in tokens:
                    nf.ejcritNombre4(1)
