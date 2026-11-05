//
// C++ Implementation: NindIndexBuilder
//
// Description: Construction en masse des fichiers inverse et local d'un corpus (voir NindIndexBuilder.h)
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
#include "NindIndexBuilder.h"
#include <algorithm>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
using namespace latecon::nindex;
////////////////////////////////////////////////////////////
NindIndexBuilder::NindIndexBuilder():
    m_docIds(),
    m_docEnds(),
    m_tokens(),
    m_maxTermId(0)
{
}
////////////////////////////////////////////////////////////

    // documents firstDocId, firstDocId + 1... : document i is localIds[ends[i-1]:ends[i]],
    // local id x being the term idMap[x]
void NindIndexBuilder::addDocuments(const NindDocId firstDocId,
                                    const uint32_t *ends, const size_t docsNb,
                                    const uint32_t *localIds, const size_t tokensNb,
                                    const uint32_t *idMap, const size_t idMapSize)
{
    uint32_t prev = 0;
    for (size_t i = 0; i < docsNb; i++) {
        if (ends[i] < prev || ends[i] > tokensNb)
            throw std::invalid_argument("ends: end offsets must be ascending and within the data");
        prev = ends[i];
    }
    if (prev != tokensNb)
        throw std::invalid_argument("ends: last end offset must equal the data length");
    if (docsNb != 0 && !m_docIds.empty() && firstDocId <= m_docIds.back())
        throw std::invalid_argument("documents must be added in increasing id order");
    if (docsNb != 0 && firstDocId > std::numeric_limits<uint64_t>::max() - (docsNb - 1))
        throw std::invalid_argument("document id out of range");
    for (size_t i = 0; i < idMapSize; i++) {
        if (idMap[i] == 0) throw std::invalid_argument("id_map: 0 is not a valid term id");
    }
    for (size_t i = 0; i < tokensNb; i++) {
        if (localIds[i] >= idMapSize) throw std::invalid_argument("term_ids: local id beyond id_map");
    }
    const size_t base = m_tokens.size();
    m_tokens.reserve(base + tokensNb);
    for (size_t i = 0; i < tokensNb; i++) {
        const uint32_t termId = idMap[localIds[i]];
        m_tokens.push_back(termId);
        if (termId > m_maxTermId) m_maxTermId = termId;
    }
    for (size_t i = 0; i < docsNb; i++) {
        m_docIds.push_back(firstDocId + (uint64_t)i);
        m_docEnds.push_back(base + ends[i]);
    }
}

////////////////////////////////////////////////////////////
void NindIndexBuilder::writeTermIndex(NindTermIndex &termIndex,
                                      const NindPadFile::Identification &identification,
                                      const unsigned char cg) const
{
    const std::list<unsigned int> noSpecifics;
    //0) the documents' lengths (their tokens: what NindEngine scores with), for the blocks
    {
        std::vector<std::pair<NindDocId, unsigned int> > lengths(m_docIds.size());
        size_t previous = 0;
        for (size_t d = 0; d < m_docIds.size(); d++) {
            lengths[d] = std::make_pair((NindDocId)m_docIds[d], (unsigned int)(m_docEnds[d] - previous));
            previous = m_docEnds[d];
        }
        termIndex.setDocumentLengths(lengths);
    }
    //1) occurrences of each term in each document, counted per document
    std::vector<uint32_t> counts(m_maxTermId + 1, 0);
    std::vector<uint32_t> touched;
    std::vector<size_t> postingsNb(m_maxTermId + 2, 0);
    size_t begin = 0;
    for (size_t d = 0; d < m_docIds.size(); d++) {
        for (size_t i = begin; i < m_docEnds[d]; i++) {
            if (counts[m_tokens[i]]++ == 0) touched.push_back(m_tokens[i]);
        }
        for (size_t i = 0; i < touched.size(); i++) {
            postingsNb[touched[i] + 1]++;
            counts[touched[i]] = 0;
        }
        touched.clear();
        begin = m_docEnds[d];
    }
    //2) postings by term (CSR), documents in increasing id order
    for (size_t t = 1; t < postingsNb.size(); t++) postingsNb[t] += postingsNb[t - 1];
    std::vector<size_t> fill(postingsNb.begin(), postingsNb.end() - 1);
    std::vector<uint64_t> postingDocs(postingsNb.back());
    std::vector<uint32_t> postingFreqs(postingsNb.back());
    begin = 0;
    for (size_t d = 0; d < m_docIds.size(); d++) {
        for (size_t i = begin; i < m_docEnds[d]; i++) {
            if (counts[m_tokens[i]]++ == 0) touched.push_back(m_tokens[i]);
        }
        for (size_t i = 0; i < touched.size(); i++) {
            const size_t slot = fill[touched[i]]++;
            postingDocs[slot] = m_docIds[d];
            postingFreqs[slot] = counts[touched[i]];
            counts[touched[i]] = 0;
        }
        touched.clear();
        begin = m_docEnds[d];
    }
    //3) the term definitions, flat (documents already in increasing order: added so)
    std::vector<NindTermIndex::Document> documents;
    std::vector<NindTermIndex::CategoryPostings> categories;
    for (uint32_t t = 1; t <= m_maxTermId; t++) {
        if (postingsNb[t] == postingsNb[t + 1]) continue;
        documents.clear();
        uint64_t total = 0;
        for (size_t i = postingsNb[t]; i < postingsNb[t + 1]; i++) {
            documents.push_back(NindTermIndex::Document(postingDocs[i], postingFreqs[i]));
            total += postingFreqs[i];
        }
        if (total > std::numeric_limits<unsigned int>::max())
            throw std::invalid_argument("frequencies: total term frequency overflows 32 bits");
        categories.assign(1, NindTermIndex::CategoryPostings(cg, (unsigned int)total, documents.data(), documents.size()));
        termIndex.setTermPostings(t, categories, identification, noSpecifics);
    }
}

////////////////////////////////////////////////////////////
void NindIndexBuilder::writeLocalIndex(NindLocalIndex &localIndex,
                                       const NindPadFile::Identification &identification,
                                       const unsigned char cg) const
{
    size_t maxLength = 0, begin = 0;
    for (size_t d = 0; d < m_docIds.size(); d++) {
        maxLength = std::max(maxLength, m_docEnds[d] - begin);
        begin = m_docEnds[d];
    }
    std::vector<uint32_t> positions(maxLength), lengths(maxLength, 1);
    for (size_t i = 0; i < maxLength; i++) positions[i] = (uint32_t)i;
    begin = 0;
    for (size_t d = 0; d < m_docIds.size(); d++) {
        const size_t length = m_docEnds[d] - begin;
        if (length != 0) {
            const std::list<NindLocalIndex::Term> definition =
                NindIndexBuilder::localDef(m_tokens.data() + begin, positions.data(), lengths.data(), 0, length, cg);
            localIndex.setLocalDef(m_docIds[d], definition, identification);
        }
        begin = m_docEnds[d];
    }
}

////////////////////////////////////////////////////////////
void NindIndexBuilder::docLengths(std::vector<NindDocId> &docIds,
                                  std::vector<uint32_t> &lengths) const
{
    docIds.assign(m_docIds.begin(), m_docIds.end());
    lengths.resize(m_docIds.size());
    size_t begin = 0;
    for (size_t d = 0; d < m_docIds.size(); d++) {
        lengths[d] = (uint32_t)(m_docEnds[d] - begin);
        begin = m_docEnds[d];
    }
}

////////////////////////////////////////////////////////////
//the checks of termDef on [begin, end) alone (no definition built): scratch is reused
void NindIndexBuilder::checkTermDef(const uint64_t *docIds, const uint32_t *freqs, const size_t begin, const size_t end,
                  std::vector<std::pair<uint64_t, uint32_t> > &scratch)
{
    scratch.assign(end - begin, std::pair<uint64_t, uint32_t>());
    uint64_t total = 0;
    for (size_t i = begin; i < end; i++) {
        scratch[i - begin] = std::make_pair(docIds[i], freqs[i]);
        total += freqs[i];
    }
    if (!std::is_sorted(scratch.begin(), scratch.end())) std::sort(scratch.begin(), scratch.end());
    for (size_t i = 1; i < scratch.size(); i++)
        if (scratch[i].first == scratch[i - 1].first)
            throw std::invalid_argument("doc_ids: duplicate document id " + std::to_string(scratch[i].first));
    if (total > std::numeric_limits<unsigned int>::max())
        throw std::invalid_argument("frequencies: total term frequency overflows 32 bits");
}
//one term definition from its [begin, end) slice of (docIds, freqs)
std::list<NindTermIndex::TermCG> NindIndexBuilder::termDef(const uint64_t *docIds, const uint32_t *freqs,
                                               const size_t begin, const size_t end,
                                               const unsigned char cg)
{
    std::vector<std::pair<uint64_t, uint32_t> > postings(end - begin);
    for (size_t i = begin; i < end; i++) postings[i - begin] = std::make_pair(docIds[i], freqs[i]);
    if (!std::is_sorted(postings.begin(), postings.end())) std::sort(postings.begin(), postings.end());
    std::list<NindTermIndex::TermCG> termDef;
    if (postings.empty()) return termDef;
    uint64_t total = 0;
    NindTermIndex::TermCG termCG(cg, 0);
    for (size_t i = 0; i < postings.size(); i++) {
        if (i > 0 && postings[i].first == postings[i - 1].first)
            throw std::invalid_argument("doc_ids: duplicate document id " + std::to_string(postings[i].first));
        total += postings[i].second;
        termCG.documents.push_back(NindTermIndex::Document(postings[i].first, postings[i].second));
    }
    if (total > std::numeric_limits<unsigned int>::max())
        throw std::invalid_argument("frequencies: total term frequency overflows 32 bits");
    termCG.frequency = static_cast<unsigned int>(total);
    termDef.push_back(std::move(termCG));
    return termDef;
}
//one local definition from its [begin, end) slice of occurrences: one Term
// per term id (ascending), localisations kept in occurrence order.
// <nbreLocalisations> is one byte, so a term occurring more than 255 times
// is split into several consecutive Terms with the same id (readers sum them)
std::list<NindLocalIndex::Term> NindIndexBuilder::localDef(const uint32_t *termIds, const uint32_t *positions,
                                               const uint32_t *lengths,
                                               const size_t begin, const size_t end,
                                               const unsigned char cg)
{
    std::map<unsigned int, std::vector<size_t> > byTerm;     //term id -> its occurrences
    for (size_t i = begin; i < end; i++) byTerm[termIds[i]].push_back(i);
    std::list<NindLocalIndex::Term> localDef;
    for (std::map<unsigned int, std::vector<size_t> >::const_iterator it = byTerm.begin(); it != byTerm.end(); it++) {
        const std::vector<size_t> &occurrences = it->second;
        for (size_t j = 0; j < occurrences.size(); j++) {
            if (j % 255 == 0) localDef.push_back(NindLocalIndex::Term(it->first, cg));
            localDef.back().localisation.push_back(
                NindLocalIndex::Localisation(positions[occurrences[j]], lengths[occurrences[j]]));
        }
    }
    return localDef;
}
////////////////////////////////////////////////////////////
