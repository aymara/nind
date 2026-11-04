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
#include "NindBasics/NindDocId.h"
#include "NindCommonExport.h"
#include <cstdint>
#include <list>
#include <utility>
#include <vector>
////////////////////////////////////////////////////////////
namespace latecon {
    namespace nindex {
////////////////////////////////////////////////////////////
/**\brief Builds the term and local index files of a new corpus at once, from its documents
* given as batches of term ids, in document order (the lexicon written apart, e.g. with
* NindLexiconIndex::addWords). Memory: about 4 bytes per token plus 16 per document; writing the
* term index builds every term's postings (12 bytes per posting) once. Each block of the term
* index gets its shortest document's length (a document's length: its tokens). The writers are
* best in bulk mode (NindIndex::beginBulk), as nind's NindIndexer uses it; term and local index
* can be written from two threads (they are different files). */
class DLLExportLexicon NindIndexBuilder {
public:
    NindIndexBuilder();

    /**\brief Documents firstDocId, firstDocId + 1...: document i is localIds[ends[i-1]:ends[i]],
    * local id x being the term idMap[x]
    *\throws std::invalid_argument on ends not ascending or not covering the tokens, documents not
    * in increasing id order, a term id 0, a local id beyond idMap */
    void addDocuments(const NindDocId firstDocId,
                      const uint32_t *ends, const size_t docsNb,
                      const uint32_t *localIds, const size_t tokensNb,
                      const uint32_t *idMap, const size_t idMapSize);

    /**\brief Write every term's postings (category cg), terms in increasing id order */
    void writeTermIndex(NindTermIndex &termIndex,
                        const NindPadFile::Identification &identification,
                        const unsigned char cg = 0) const;

    /**\brief Write every document: its term occurrences at positions 0, 1... (length 1) */
    void writeLocalIndex(NindLocalIndex &localIndex,
                         const NindPadFile::Identification &identification,
                         const unsigned char cg = 0) const;

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
    std::vector<uint64_t> m_docIds;     //in increasing order (external document ids, 64 bits)
    std::vector<size_t> m_docEnds;      //document d is m_tokens[m_docEnds[d-1]:m_docEnds[d]]
    std::vector<uint32_t> m_tokens;     //term ids
    uint32_t m_maxTermId;
};
////////////////////////////////////////////////////////////
    } // end namespace
} // end namespace
#endif
////////////////////////////////////////////////////////////
