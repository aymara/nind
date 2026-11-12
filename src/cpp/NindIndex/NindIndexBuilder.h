//
// C++ Interface: NindIndexBuilder
//
// Description: Construction en masse des fichiers inverse et local d'un corpus, ah partir de ses
// documents donnejs par lots d'identifiants de termes (dans l'ordre des documents).
//
// Author: Claude <noreply@anthropic.com>
//
// Copyright: 2014-2017 LATEJCON. See LICENCE.md file that comes with this distribution
// This file is part of NIND (as "nouvelle indexation").
// NIND is free software: you can redistribute it and/or modify it under the terms of the
// GNU Less General Public License (LGPL) as published by the Free Software Foundation,
// (see <http://www.gnu.org/licenses/>), either version 3 of the License, or any later version.
// NIND is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
// even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Less General Public License for more details.
////////////////////////////////////////////////////////////
#ifndef NindIndexBuilder_H
#define NindIndexBuilder_H
////////////////////////////////////////////////////////////
#include "NindTermIndex.h"
#include "NindLocalIndex.h"
#include "NindLexiconIndex.h"
#include "NindBasics/NindDocId.h"
#include "NindCommonExport.h"
#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
////////////////////////////////////////////////////////////
namespace latecon {
    namespace nindex {
////////////////////////////////////////////////////////////
/**\brief Builds the term and local index files of a new corpus at once, from its documents
* given as batches of term ids, in document order (the lexicon written apart, e.g. with
* NindLexiconIndex::addWords). Memory: about 4 bytes per token plus 16 per document (reserve
* avoids growing them by copies); writing the term index builds every term's postings (5 bytes
* per posting, at most one per token) once, or a range of terms at a time (setPostingsBudget). At most 2^32 - 1 documents. Each block of the term
* index gets its shortest document's length (a document's length: its tokens). The writers are
* best in bulk mode (NindIndex::beginBulk), as nind's NindIndexer uses it; term and local index
* can be written from two threads (they are different files). */
class DLLExportLexicon NindIndexBuilder {
public:
    NindIndexBuilder();

    /**\brief Room for tokensNb more tokens and docsNb more documents, when the caller knows them:
    * no copy of the buffers as they grow, no unused capacity */
    void reserve(const size_t tokensNb, const size_t docsNb);

    /**\brief Documents firstDocId, firstDocId + 1...: document i is localIds[ends[i-1]:ends[i]],
    * local id x being the term idMap[x]
    *\throws std::invalid_argument on ends not ascending or not covering the tokens, documents not
    * in increasing id order, a term id 0, a local id beyond idMap */
    void addDocuments(const NindDocId firstDocId,
                      const uint32_t *ends, const size_t docsNb,
                      const uint32_t *localIds, const size_t tokensNb,
                      const uint32_t *idMap, const size_t idMapSize);

    /**\brief Documents firstDocId, firstDocId + 1...: a tokenizer's batch (NindTokenizer), its words
    * given as their UTF-8 bytes one after the other (word w: words[wordEnds[w - 1], wordEnds[w])),
    * document i being words localIds[ends[i - 1]:ends[i]]. The builder numbers the words itself, each
    * looked up once a batch (about 50 bytes a distinct word, none per occurrence), and writes them to
    * the lexicon (writeLexicon) before the term and local indexes. Not with addDocuments' lexicon ids.
    *\throws std::invalid_argument as addDocuments, on word ends not ascending, or a word too long
    * (more than 254 bytes) or empty */
    void addBatch(const NindDocId firstDocId,
                  const uint32_t *ends, const size_t docsNb,
                  const uint32_t *localIds, const size_t tokensNb,
                  const char *words, const size_t wordsBytes,
                  const uint32_t *wordEnds, const size_t wordsNb);

    /**\brief The batches' words into a new lexicon (in bulk mode or not), by increasing bytes (their
    * idents 1, 2...), the documents' words renumbered so: then writeTermIndex, writeLocalIndex */
    void writeLexicon(NindLexiconIndex &lexicon);

    /**\brief The batches' distinct words (also after writeLexicon) */
    size_t wordsNb() const { return m_mode == WRITTEN ? m_writtenWordsNb : m_words.size(); }

    /**\brief writeTermIndex builds the postings of a range of terms at a time, at most this many
    * bytes of them (5 a posting; a term larger than that alone), the tokens scanned again for each
    * range (a sequential read of 4 bytes a token); 0, the default: all terms at once. The same file */
    void setPostingsBudget(const size_t bytes) { m_postingsBudget = bytes; }
    size_t postingsBudget() const { return m_postingsBudget; }

    /**\brief Write every term's postings (category cg), terms in increasing id order */
    void writeTermIndex(NindTermIndex &termIndex,
                        const NindPadFile::Identification &identification,
                        const unsigned char cg = 0) const;

    /**\brief Write every document: its term occurrences at positions 0, 1... (length 1) */
    void writeLocalIndex(NindLocalIndex &localIndex,
                         const NindPadFile::Identification &identification,
                         const unsigned char cg = 0) const;

    /**\brief One document of the given terms (ids of the lexicon), in order; ids increasing: for
    * documents added one by one (addDocuments checks its whole map at each call) */
    void addDocument(const NindDocId docId,
                     const uint32_t *termIds,
                     const size_t tokensNb);

    /**\brief The documents and their lengths (in tokens) */
    void docLengths(std::vector<NindDocId> &docIds,
                    std::vector<uint32_t> &lengths) const;

    size_t docsNb() const { return m_docIds.size(); }
    size_t tokensNb() const { return m_tokens.size(); }
    uint32_t maxTermId() const { return m_maxTermId; }

    /**\brief One term definition (one category, cg) from the [begin, end) slice of (docIds,
    * freqs), in any order
    *\throws std::invalid_argument on a document twice or a total frequency over 32 bits */
    static std::list<NindTermIndex::TermCG> termDef(const uint64_t *docIds, const uint32_t *freqs,
                                                    const size_t begin, const size_t end,
                                                    const unsigned char cg);
    /**\brief termDef's checks alone (nothing built); scratch is reused between calls */
    static void checkTermDef(const uint64_t *docIds, const uint32_t *freqs, const size_t begin, const size_t end,
                             std::vector<std::pair<uint64_t, uint32_t> > &scratch);
    /**\brief One local definition from the [begin, end) slice of occurrences: one Term per term id
    * (ascending), localisations in occurrence order; a term occurring more than 255 times is split
    * into several consecutive Terms with the same id (<nbreLocalisations> is one byte) */
    static std::list<NindLocalIndex::Term> localDef(const uint32_t *termIds, const uint32_t *positions,
                                                    const uint32_t *lengths,
                                                    const size_t begin, const size_t end,
                                                    const unsigned char cg);

private:
    static const uint32_t MAX_SMALL_FREQ = 255;     //writeTermIndex: frequencies from it are kept apart
    static const size_t POSTING_BYTES = 5;          //writeTermIndex: a posting's position and frequency
    std::vector<uint64_t> m_docIds;     //in increasing order (external document ids, 64 bits)
    std::vector<size_t> m_docEnds;      //document d is m_tokens[m_docEnds[d-1]:m_docEnds[d]]
    std::vector<uint32_t> m_tokens;     //term ids (batches: the builder's word ids, until writeLexicon)
    uint32_t m_maxTermId;
    //the batches' words, by the builder's id (cleared by writeLexicon)
    std::vector<std::string> m_words;
    std::unordered_map<std::string, uint32_t> m_wordIds;
    enum { NO_DOCUMENT, LEXICON_IDS, BATCHES, WRITTEN } m_mode;
    size_t m_writtenWordsNb;            //the words writeLexicon wrote
    size_t m_postingsBudget;            //writeTermIndex: the postings of a range of terms (0: all)
};
////////////////////////////////////////////////////////////
    } // end namespace
} // end namespace
#endif
////////////////////////////////////////////////////////////
