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
#include <unordered_map>
using namespace latecon::nindex;
////////////////////////////////////////////////////////////
NindIndexBuilder::NindIndexBuilder():
    m_docIds(),
    m_docEnds(),
    m_tokens(),
    m_maxTermId(0),
    m_words(),
    m_wordIds(),
    m_mode(NO_DOCUMENT),
    m_writtenWordsNb(0)
{
}
////////////////////////////////////////////////////////////
void NindIndexBuilder::reserve(const size_t tokensNb, const size_t docsNb)
{
    m_tokens.reserve(m_tokens.size() + tokensNb);
    m_docIds.reserve(m_docIds.size() + docsNb);
    m_docEnds.reserve(m_docEnds.size() + docsNb);
}
////////////////////////////////////////////////////////////
void NindIndexBuilder::addDocument(const NindDocId docId,
                                   const uint32_t *termIds, const size_t tokensNb)
{
    if (!m_docIds.empty() && docId <= m_docIds.back())
        throw std::invalid_argument("documents must be added in increasing id order");
    if (m_mode == BATCHES || m_mode == WRITTEN) throw std::invalid_argument("lexicon ids after batches");
    m_mode = LEXICON_IDS;
    if (m_docIds.size() >= std::numeric_limits<uint32_t>::max())
        throw std::invalid_argument("too many documents for one build (at most 2^32 - 1)");
    for (size_t i = 0; i < tokensNb; i++) {
        if (termIds[i] == 0) throw std::invalid_argument("0 is not a valid term id");
        if (termIds[i] > m_maxTermId) m_maxTermId = termIds[i];
    }
    m_tokens.insert(m_tokens.end(), termIds, termIds + tokensNb);
    m_docIds.push_back(docId);
    m_docEnds.push_back(m_tokens.size());
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
    if (m_docIds.size() + docsNb > std::numeric_limits<uint32_t>::max())
        throw std::invalid_argument("too many documents for one build (at most 2^32 - 1)");
    if (m_mode == BATCHES || m_mode == WRITTEN) throw std::invalid_argument("lexicon ids after batches");
    m_mode = LEXICON_IDS;
    const size_t base = m_tokens.size();
    //no exact reserve here: it would copy the whole buffer at every batch (see reserve)
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
void NindIndexBuilder::addBatch(const NindDocId firstDocId,
                                const uint32_t *ends, const size_t docsNb,
                                const uint32_t *localIds, const size_t tokensNb,
                                const char *words, const size_t wordsBytes,
                                const uint32_t *wordEnds, const size_t wordsNb)
{
    if (m_mode == LEXICON_IDS || m_mode == WRITTEN) throw std::invalid_argument("a batch after lexicon ids or writeLexicon");
    uint32_t prev = 0;
    for (size_t i = 0; i < docsNb; i++) {
        if (ends[i] < prev || ends[i] > tokensNb)
            throw std::invalid_argument("ends: end offsets must be ascending and within the data");
        prev = ends[i];
    }
    if (prev != tokensNb) throw std::invalid_argument("ends: last end offset must equal the data length");
    if (docsNb != 0 && !m_docIds.empty() && firstDocId <= m_docIds.back())
        throw std::invalid_argument("documents must be added in increasing id order");
    if (docsNb != 0 && firstDocId > std::numeric_limits<uint64_t>::max() - (docsNb - 1))
        throw std::invalid_argument("document id out of range");
    if (m_docIds.size() + docsNb > std::numeric_limits<uint32_t>::max())
        throw std::invalid_argument("too many documents for one build (at most 2^32 - 1)");
    //the batch's words: their builder ids (each word looked up once)
    std::vector<uint32_t> ids(wordsNb);
    std::string word;
    for (size_t w = 0, begin = 0; w < wordsNb; begin = wordEnds[w], w++) {
        if (wordEnds[w] < begin || wordEnds[w] > wordsBytes) throw std::invalid_argument("word_ends: ascending, within the words");
        if (wordEnds[w] == begin || wordEnds[w] - begin > 254) throw std::invalid_argument("words: 1 to 254 bytes each");
        word.assign(words + begin, wordEnds[w] - begin);
        const std::pair<std::unordered_map<std::string, uint32_t>::iterator, bool> found =
            m_wordIds.insert(std::make_pair(word, (uint32_t)m_words.size() + 1));
        if (found.second) {
            if (m_words.size() + 1 >= std::numeric_limits<uint32_t>::max()) throw std::invalid_argument("too many words");
            m_words.push_back(word);
        }
        ids[w] = found.first->second;
    }
    for (size_t i = 0; i < tokensNb; i++)
        if (localIds[i] >= wordsNb) throw std::invalid_argument("term_ids: local id beyond the words");
    m_mode = BATCHES;
    const size_t base = m_tokens.size();
    for (size_t i = 0; i < tokensNb; i++) {
        const uint32_t id = ids[localIds[i]];
        m_tokens.push_back(id);
        if (id > m_maxTermId) m_maxTermId = id;
    }
    for (size_t i = 0; i < docsNb; i++) {
        m_docIds.push_back(firstDocId + (uint64_t)i);
        m_docEnds.push_back(base + ends[i]);
    }
}
////////////////////////////////////////////////////////////
void NindIndexBuilder::writeLexicon(NindLexiconIndex &lexicon)
{
    if (m_mode == LEXICON_IDS || m_mode == WRITTEN) throw std::invalid_argument("writeLexicon: no batches to write");
    //by increasing bytes (UTF-8: code points, the order NindIndexer gave them before): their idents
    std::vector<uint32_t> order(m_words.size());
    for (size_t w = 0; w < order.size(); w++) order[w] = (uint32_t)w;
    std::sort(order.begin(), order.end(), [this](const uint32_t a, const uint32_t b) { return m_words[a] < m_words[b]; });
    std::vector<unsigned int> idents;
    {
        std::unordered_map<std::string, uint32_t>().swap(m_wordIds);
        std::vector<std::string> sorted(order.size());
        for (size_t w = 0; w < order.size(); w++) sorted[w].swap(m_words[order[w]]);
        std::vector<std::string>().swap(m_words);
        lexicon.addSimpleWords(sorted, idents);     //(sorted, unique: each bucket written from its words)
    }
    //builder id (1..) -> lexicon ident, then every token renumbered
    std::vector<uint32_t> identOf(order.size() + 1, 0);
    for (size_t w = 0; w < order.size(); w++) identOf[order[w] + 1] = idents[w];
    m_maxTermId = 0;
    for (size_t i = 0; i < m_tokens.size(); i++) {
        m_tokens[i] = identOf[m_tokens[i]];
        if (m_tokens[i] > m_maxTermId) m_maxTermId = m_tokens[i];
    }
    m_writtenWordsNb = order.size();
    m_mode = WRITTEN;
}
////////////////////////////////////////////////////////////
void NindIndexBuilder::writeTermIndex(NindTermIndex &termIndex,
                                      const NindPadFile::Identification &identification,
                                      const unsigned char cg) const
{
    if (m_mode == BATCHES) throw std::invalid_argument("writeTermIndex: writeLexicon first");
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
    //2) postings by term (CSR), documents in increasing id order: 5 bytes a posting, the document's
    // position (32 bits, its id is m_docIds[position]) and its frequency (8 bits; 255 and more: in
    // overflows, at most one per 255 tokens)
    for (size_t t = 1; t < postingsNb.size(); t++) postingsNb[t] += postingsNb[t - 1];
    std::vector<size_t> fill(postingsNb.begin(), postingsNb.end() - 1);
    std::vector<uint32_t> postingDocs(postingsNb.back());
    std::vector<uint8_t> postingFreqs(postingsNb.back());
    std::unordered_map<size_t, uint32_t> overflows;
    begin = 0;
    for (size_t d = 0; d < m_docIds.size(); d++) {
        for (size_t i = begin; i < m_docEnds[d]; i++) {
            if (counts[m_tokens[i]]++ == 0) touched.push_back(m_tokens[i]);
        }
        for (size_t i = 0; i < touched.size(); i++) {
            const size_t slot = fill[touched[i]]++;
            const uint32_t count = counts[touched[i]];
            postingDocs[slot] = (uint32_t)d;
            if (count < MAX_SMALL_FREQ) postingFreqs[slot] = (uint8_t)count;
            else {
                postingFreqs[slot] = (uint8_t)MAX_SMALL_FREQ;
                overflows[slot] = count;
            }
            counts[touched[i]] = 0;
        }
        touched.clear();
        begin = m_docEnds[d];
    }
    std::vector<uint32_t>().swap(counts);
    std::vector<uint32_t>().swap(touched);
    std::vector<size_t>().swap(fill);
    //3) the term definitions, flat (documents already in increasing order: added so); consecutive
    // ids (every batch of addDocuments) without looking them up (scattered reads)
    const bool consecutive = m_docIds.empty() || m_docIds.back() - m_docIds.front() == m_docIds.size() - 1;
    const uint64_t firstId = m_docIds.empty() ? 0 : m_docIds.front();
    std::vector<NindTermIndex::Document> documents;
    std::vector<NindTermIndex::CategoryPostings> categories;
    for (uint32_t t = 1; t <= m_maxTermId; t++) {
        if (postingsNb[t] == postingsNb[t + 1]) continue;
        documents.clear();
        uint64_t total = 0;
        for (size_t i = postingsNb[t]; i < postingsNb[t + 1]; i++) {
            const uint32_t frequency = postingFreqs[i] == MAX_SMALL_FREQ ? overflows[i] : postingFreqs[i];
            documents.push_back(NindTermIndex::Document(consecutive ? firstId + postingDocs[i] : m_docIds[postingDocs[i]],
                                                        frequency));
            total += frequency;
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
    if (m_mode == BATCHES) throw std::invalid_argument("writeLocalIndex: writeLexicon first");
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
