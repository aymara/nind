//
// C++ Implementation: NindBm25Scorer
//
// Description: Okapi BM25 sur le fichier inverse, exhaustif ou par blocs (MaxScore), voir NindBm25Scorer.h
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
#include "NindBm25Scorer.h"
#include "NindBasics/NindReadBuffer.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <unordered_map>
using namespace latecon::nindex;
////////////////////////////////////////////////////////////
namespace {
const uint32_t NO_POSITION = 0xFFFFFFFFu;       // a document id without position: not in the corpus
const uint64_t DENSE_SLACK = 1u << 20;          // a dense table up to 2 N + 1M entries
}
////////////////////////////////////////////////////////////
namespace latecon {
    namespace nindex {
// Okapi BM25 over the postings of a term index (see NindBm25Scorer.h).
// Flat arrays only: documents are numbered by their position in the corpus order (dense table
// from document id, or binary search when ids are too sparse for one), scores are accumulated
// in an array indexed by position (reset only where touched), postings are read with
// NindTermIndex::getTermPostings into reused vectors - no allocation nor hashing per posting.
class NindBm25Scorer::Impl {
public:
    // the documents of the corpus, in the order that breaks score ties
    // document ids: 64 bits (NindDocId); positions: 32 bits (at most 4.29G documents per index)
    Impl(const std::vector<NindDocId> &docIds, const std::vector<uint32_t> &lengths, const double avgDocLength):
        m_docIds(), m_lengths(), m_shortLengths(), m_minLength(0), m_identity(false), m_dense(), m_sparse(), m_docsNb(docIds.size()),
        m_avgDocLength(avgDocLength),
        m_poolMutex(), m_pool()
    {
        if (docIds.size() != lengths.size()) throw std::invalid_argument("doc_ids and lengths must have the same length");
        if (docIds.size() >= NO_POSITION) throw std::invalid_argument("too many documents");
        uint64_t maxId = 0;
        for (size_t i = 0; i < docIds.size(); i++) maxId = std::max(maxId, (uint64_t)docIds[i]);
        // a document id given twice: its last position counts (and only it is scored)
        if (docIds.size() != 0 && maxId < 2 * (uint64_t)docIds.size() + DENSE_SLACK) {
            m_dense.assign((size_t)maxId + 1, NO_POSITION);
            for (size_t i = 0; i < docIds.size(); i++) m_dense[docIds[i]] = (uint32_t)i;
        }
        else {
            m_sparse.reserve(docIds.size());
            for (size_t i = 0; i < docIds.size(); i++) m_sparse.push_back(std::make_pair((uint64_t)docIds[i], (uint32_t)i));
            std::stable_sort(m_sparse.begin(), m_sparse.end(),
                             [](const std::pair<uint64_t, uint32_t> &a, const std::pair<uint64_t, uint32_t> &b)
                             { return a.first < b.first; });
            std::vector<std::pair<uint64_t, uint32_t> > unique;
            unique.reserve(m_sparse.size());
            for (size_t i = 0; i < m_sparse.size(); i++) {
                if (!unique.empty() && unique.back().first == m_sparse[i].first) unique.back() = m_sparse[i];
                else unique.push_back(m_sparse[i]);
            }
            m_sparse.swap(unique);
        }
        m_docIds.assign(docIds.begin(), docIds.end());
        m_lengths.assign(lengths.begin(), lengths.end());
        //lengths under 2^16: 2 bytes each (more of them in the processor's caches)
        uint32_t maxLength = 0;
        for (size_t i = 0; i < lengths.size(); i++) maxLength = std::max(maxLength, lengths[i]);
        if (maxLength < 65536) {
            m_shortLengths.assign(lengths.begin(), lengths.end());
            std::vector<uint32_t>().swap(m_lengths);
        }
        m_identity = true;
        for (size_t i = 0; i < docIds.size() && m_identity; i++) m_identity = (docIds[i] == i);
        if (m_identity) std::vector<uint32_t>().swap(m_dense);
        m_minLength = 0;
        for (size_t i = 0; i < lengths.size(); i++)
            if (i == 0 || lengths[i] < m_minLength) m_minLength = lengths[i];
    }

    // the documents with a positive score for the query terms (in query order, repeats
    // included, 0 = unknown term), by decreasing score, at most topK if topK >= 0
    std::vector<std::pair<uint64_t, double> > search(const NindTermIndex &termIndex,
                                                     const std::vector<unsigned int> &termIds,
                                                     const double k1, const double b,
                                                     const long topK, const bool prune) const
    {
        // scratch arrays and read buffer from the pool: concurrent searches each get their own
        ScratchLease lease(*this);
        NindReadBuffer &buffer = lease.scratch->buffer;
        std::vector<double> &scores = lease.scratch->scores;
        if (scores.size() != m_docsNb) scores.assign(m_docsNb, 0.0);
        std::vector<uint32_t> touched;          // positions whose score isn't 0 any more

        // the query's distinct terms; per occurrence (query order, repeats included) its term
        std::vector<unsigned int> distinctIds;
        std::vector<size_t> occurrences;
        std::unordered_map<unsigned int, size_t> distinct;
        for (size_t t = 0; t < termIds.size(); t++) {
            if (termIds[t] == 0) continue;
            const auto found = distinct.insert(std::make_pair(termIds[t], distinctIds.size()));
            if (found.second) distinctIds.push_back(termIds[t]);
            occurrences.push_back(found.first->second);
        }
        // per distinct term: its idf and the postings to score, (position, frequency): every posting
        // (exhaustive), or only those of the blocks that may reach the top k (block-max pruning)
        std::vector<double> idfs;
        std::vector<std::vector<std::pair<uint32_t, uint32_t> > > postings;
        if (prune && topK > 0 && k1 >= 0 && b >= 0 && b <= 1 && !distinctIds.empty()) {
            std::vector<std::pair<uint64_t, double> > result;
            if (searchMaxScore(termIndex, distinctIds, occurrences, k1, b, (size_t)topK, buffer, result)) {
                lease.clean = true;         //(scores untouched)
                return result;
            }
        }
        collectAll(termIndex, distinctIds, buffer, lease.scratch->stamps, lease.scratch->stamp, idfs, postings);
        const Params parameters = params(k1, b);
        // the scores, term occurrence by term occurrence in query order (as NindEngine._bm25_score)
        for (size_t o = 0; o < occurrences.size(); o++) {
            const double idf = idfs[occurrences[o]];
            const std::vector<std::pair<uint32_t, uint32_t> > &termPostings = postings[occurrences[o]];
            for (size_t i = 0; i < termPostings.size(); i++) {
                if (termPostings[i].second == 0) continue;
                const uint32_t position = termPostings[i].first;
                double &score = scores[position];
                if (score == 0.0) touched.push_back(position);
                score += contribution(idf, termPostings[i].second, lengthOf(position), parameters);
            }
        }
        //(score, position) of the documents with a positive score; scores reset for the next search
        std::vector<std::pair<double, uint32_t> > ranked;
        ranked.reserve(touched.size());
        for (size_t i = 0; i < touched.size(); i++) {
            const uint32_t position = touched[i];
            if (scores[position] > 0) ranked.push_back(std::make_pair(-scores[position], position));
            scores[position] = 0.0;
        }
        lease.clean = true;             //every score reset: the scratch arrays can be reused
        //decreasing score, then the order of the corpus
        const size_t kept = (topK >= 0 && (size_t)topK < ranked.size()) ? (size_t)topK : ranked.size();
        std::partial_sort(ranked.begin(), ranked.begin() + kept, ranked.end());
        std::vector<std::pair<uint64_t, double> > result(kept);
        for (size_t i = 0; i < kept; i++) result[i] = std::make_pair(m_docIds[ranked[i].second], -ranked[i].first);
        return result;
    }

    size_t docsNb() const { return m_docsNb; }

private:
    // a query's BM25 constants: tf * (k1 + 1) / (tf + k1 * (1 - b) + k1 * b / avgDocLength * length)
    struct Params {
        double k1Plus1, base, perLength;
    };
    Params params(const double k1, const double b) const {
        Params p;
        p.k1Plus1 = k1 + 1;
        p.base = k1 * (1 - b);
        p.perLength = (m_avgDocLength > 0) ? k1 * b / m_avgDocLength : 0.0;
        return p;
    }
    // one term's BM25 contribution: the same function everywhere (scores and bounds): monotonic in
    // tf (increasing) and in the length (decreasing), so a bound computed at a larger tf and a
    // smaller length is never below a contribution
    static double contribution(const double idf, const double tf, const double docLength, const Params &p) {
        return idf * (tf * p.k1Plus1 / (tf + p.base + p.perLength * docLength));
    }
    double idfOf(const uint64_t df) const {
        //same operations, in the same order, as NindEngine._bm25_score
        return std::log(((double)((int64_t)m_docsNb - (int64_t)df) + 0.5) / ((double)df + 0.5) + 1.0);
    }

    // every posting of every term (several categories: a document's first one only)
    void collectAll(const NindTermIndex &termIndex, const std::vector<unsigned int> &distinctIds,
                    NindReadBuffer &buffer, std::vector<uint32_t> &stamps, uint32_t &stamp,
                    std::vector<double> &idfs,
                    std::vector<std::vector<std::pair<uint32_t, uint32_t> > > &postings) const
    {
        idfs.clear();
        postings.assign(distinctIds.size(), std::vector<std::pair<uint32_t, uint32_t> >());
        std::vector<NindTermIndex::Document> read;
        std::vector<size_t> categoryEnds;
        for (size_t t = 0; t < distinctIds.size(); t++) {
            termIndex.getTermPostings(distinctIds[t], read, categoryEnds, buffer);
            // df counts the documents of every category
            idfs.push_back(idfOf((uint64_t)read.size()));
            std::vector<std::pair<uint32_t, uint32_t> > &termPostings = postings[t];
            termPostings.reserve(read.size());
            const bool severalCategories = categoryEnds.size() > 1;
            if (severalCategories) {
                if (stamps.size() != m_docsNb) stamps.assign(m_docsNb, 0);
                if (++stamp == 0) {         // wrapped: forget every mark
                    std::fill(stamps.begin(), stamps.end(), 0);
                    stamp = 1;
                }
            }
            for (size_t i = 0; i < read.size(); i++) {
                const uint32_t position = positionOf(read[i].ident);
                if (position == NO_POSITION) continue;
                if (severalCategories) {
                    if (stamps[position] == stamp) continue;    // already in a previous category
                    stamps[position] = stamp;
                }
                termPostings.push_back(std::make_pair(position, (uint32_t)read[i].frequency));
            }
        }
    }

    static constexpr double PRUNING_MARGIN = 1e-9;
    static const size_t RAW_READ_AHEAD = 64;            //blocks whose bytes are read at once
    // a query term's postings, read lazily: blocks' bytes RAW_READ_AHEAD at a time, one block
    // decoded only when a window needs it
    struct MaxScoreTerm {
        NindTermIndex::TermDirectory directory;
        unsigned int ident;
        double idf;
        unsigned int multiplicity;      //occurrences in the query
        bool inlineList;                //a short list: one pseudo-block, its inline postings
        size_t blocksNb;
        std::vector<NindDocId> blockFirst, blockLast;
        std::vector<double> blockBounds;
        double bound;                   //the largest of blockBounds
        size_t block;                   //the first block that may hold documents >= the window
        std::unique_ptr<NindReadBuffer> raw;    //the bytes of blocks [rawBegin, rawEnd)
        size_t rawBegin, rawEnd;
        std::vector<NindTermIndex::Document> docs;      //the decoded block (decoded: its index)
        size_t decoded;
        size_t pos;                     //cursor in docs
        NindDocId matchedDoc;           //the last document scored with this term, its frequency
        uint32_t matchedFrequency;
        bool matched;
        MaxScoreTerm(): directory(), ident(0), idf(0), multiplicity(0), inlineList(false), blocksNb(0),
            blockFirst(), blockLast(), blockBounds(), bound(0), block(0), raw(), rawBegin(0), rawEnd(0),
            docs(), decoded(NO_BLOCK), pos(0), matchedDoc(0), matchedFrequency(0), matched(false) {}
    };
    static const size_t NO_BLOCK = (size_t)-1;
    // the postings of block b of a term in docs (decoded once), the cursor at its first document
    void decode(const NindTermIndex &termIndex, MaxScoreTerm &term, const size_t b) const {
        if (term.decoded == b) return;
        term.pos = 0;
        term.decoded = b;
        if (term.inlineList) return;            //(docs holds the inline postings)
        if (b < term.rawBegin || b >= term.rawEnd) {
            term.rawBegin = b;
            term.rawEnd = std::min(b + RAW_READ_AHEAD, term.blocksNb);
            if (!term.raw) term.raw.reset(new NindReadBuffer());
            termIndex.readTermBlocks(term.ident, term.directory, 0, term.rawBegin, term.rawEnd, *term.raw);
        }
        term.docs.clear();
        termIndex.decodeTermBlock(term.ident, term.directory, 0, term.rawBegin, b, *term.raw, term.docs);
    }
    // Block-max MaxScore over windows, exact. The documents are taken window by window, a window
    // ending at the next block boundary of any term, so that every term has at most one block in
    // it: a document's score there is at most the sum of those blocks' bounds. In each window, the
    // terms by increasing block bound; the longest prefix of them whose bounds sum is below theta
    // (the k-th best score so far) is non-essential: a document in none of the others can't reach
    // the top k. A window whose bounds sum is below theta is skipped (nothing decoded). Otherwise the
    // essential terms' blocks are decoded and read in document order; a document is then completed by
    // the non-essential terms, by decreasing bound, while it may still reach theta (each of their
    // blocks decoded only if needed).
    // Scores are accumulated in another order than the exhaustive search's: every comparison with
    // theta has a margin far above the rounding errors, and the candidates kept are scored again
    // in query order, as the exhaustive search does (identical scores and ranks).
    // false (nothing done) when it doesn't apply: a term in several categories, an idf <= 0, a
    // long list without blocks (format v1).
    bool searchMaxScore(const NindTermIndex &termIndex, const std::vector<unsigned int> &distinctIds,
                        const std::vector<size_t> &occurrences, const double k1, const double b,
                        const size_t topK, NindReadBuffer &buffer,
                        std::vector<std::pair<uint64_t, double> > &result) const
    {
        const double shortest = m_minLength;
        const Params parameters = params(k1, b);
        // the query's terms present in the index (index in distinctIds -> in terms)
        std::vector<MaxScoreTerm> terms;
        std::vector<size_t> termOf(distinctIds.size(), (size_t)-1);
        terms.reserve(distinctIds.size());
        for (size_t d = 0; d < distinctIds.size(); d++) {
            terms.push_back(MaxScoreTerm());
            MaxScoreTerm &term = terms.back();
            if (!termIndex.getTermDirectory(distinctIds[d], term.directory, buffer)) { terms.pop_back(); continue; }
            if (term.directory.categories.size() != 1) return false;
            const NindTermIndex::TermDirectory::Category &category = term.directory.categories[0];
            term.ident = distinctIds[d];
            term.idf = idfOf(category.docsNb);
            if (!(term.idf > 0)) return false;
            //a format v1 file: every list inline, a long one would be read whole as one block
            if (category.blocks.empty() && category.inlinePostings.size() > NindTermIndex::BLOCK_SIZE) return false;
            for (size_t o = 0; o < occurrences.size(); o++) if (occurrences[o] == d) term.multiplicity++;
            if (!category.blocks.empty()) {
                term.blocksNb = category.blocks.size();
                for (size_t i = 0; i < category.blocks.size(); i++) {
                    const NindTermIndex::Block &block = category.blocks[i];
                    term.blockFirst.push_back(block.first);
                    term.blockLast.push_back(block.last);
                    const double length = std::max((double)block.minLength, shortest);
                    term.blockBounds.push_back(term.multiplicity *
                                               contribution(term.idf, block.maxFrequency, length, parameters));
                }
            }
            else {
                if (category.inlinePostings.empty()) { terms.pop_back(); continue; }
                term.inlineList = true;
                term.blocksNb = 1;
                term.blockFirst.push_back(category.inlinePostings.front().ident);
                term.blockLast.push_back(category.inlinePostings.back().ident);
                double bound = 0.0;
                for (size_t i = 0; i < category.inlinePostings.size(); i++) {
                    const NindTermIndex::Document &posting = category.inlinePostings[i];
                    const uint32_t position = positionOf(posting.ident);
                    if (position == NO_POSITION || posting.frequency == 0) continue;
                    bound = std::max(bound, contribution(term.idf, posting.frequency, lengthOf(position), parameters));
                }
                term.blockBounds.push_back(term.multiplicity * bound);
                term.docs = category.inlinePostings;
            }
            term.bound = *std::max_element(term.blockBounds.begin(), term.blockBounds.end());
            termOf[d] = terms.size() - 1;
        }
        const size_t termsNb = terms.size();
        if (termsNb == 0) { result.clear(); return true; }
        std::priority_queue<double, std::vector<double>, std::greater<double> > best;  //the k best approximations
        double threshold = 0.0;                 //theta minus the margin
        struct Candidate { uint32_t position; double approximation; };
        std::vector<Candidate> candidates;
        std::vector<uint32_t> candidateFrequencies;   //termsNb per candidate
        // per window: the terms in it (by increasing bound), their bounds, the bounds' prefix sums
        std::vector<size_t> order;
        std::vector<double> windowBound(termsNb), prefix(termsNb);
        order.reserve(termsNb);
        NindDocId windowMin = ~(NindDocId)0;
        for (size_t t = 0; t < termsNb; t++) windowMin = std::min(windowMin, terms[t].blockFirst[0]);
        while (true) {
            // each term's block at or after windowMin; the window: up to the next block boundary
            NindDocId windowMax = ~(NindDocId)0;
            order.clear();
            for (size_t t = 0; t < termsNb; t++) {
                MaxScoreTerm &term = terms[t];
                while (term.block < term.blocksNb && term.blockLast[term.block] < windowMin) term.block++;
                if (term.block == term.blocksNb) continue;
                if (term.blockFirst[term.block] > windowMin) windowMax = std::min(windowMax, term.blockFirst[term.block] - 1);
                else windowMax = std::min(windowMax, term.blockLast[term.block]);
                order.push_back(t);
            }
            if (order.empty()) break;
            // the terms with a block in the window, by increasing bound (the others contribute 0)
            size_t present = 0;
            for (size_t i = 0; i < order.size(); i++) {
                const size_t t = order[i];
                windowBound[t] = (terms[t].blockFirst[terms[t].block] <= windowMax) ? terms[t].blockBounds[terms[t].block] : 0.0;
                if (windowBound[t] > 0) order[present++] = t;
            }
            order.resize(present);
            for (size_t i = 1; i < order.size(); i++)           //(few terms: insertion sort)
                for (size_t j = i; j > 0 && windowBound[order[j]] < windowBound[order[j - 1]]; j--)
                    std::swap(order[j], order[j - 1]);
            double sum = 0.0;
            for (size_t i = 0; i < order.size(); i++) prefix[i] = (sum += windowBound[order[i]]);
            // the non-essential prefix: order[0..essential)
            size_t essential = 0;
            while (essential < order.size() && prefix[essential] < threshold) essential++;
            if (essential < order.size()) {
                // the essential terms' blocks, each cursor at its first document in the window
                for (size_t i = essential; i < order.size(); i++) {
                    MaxScoreTerm &term = terms[order[i]];
                    decode(termIndex, term, term.block);
                    while (term.pos < term.docs.size() && term.docs[term.pos].ident < windowMin) term.pos++;
                }
                // the window's documents in the essential lists, in order: each document's score and
                // the next document in one pass
                NindDocId docId = 0;
                bool any = false;
                for (size_t i = essential; i < order.size(); i++) {
                    const MaxScoreTerm &term = terms[order[i]];
                    if (term.pos >= term.docs.size()) continue;
                    const NindDocId current = term.docs[term.pos].ident;
                    if (current <= windowMax && (!any || current < docId)) { docId = current; any = true; }
                }
                while (any) {
                    const uint32_t position = positionOf(docId);
                    const double length = (position != NO_POSITION) ? lengthOf(position) : 0.0;
                    double approximation = 0.0;
                    NindDocId nextDoc = 0;
                    bool anyNext = false;
                    for (size_t i = essential; i < order.size(); i++) {
                        MaxScoreTerm &term = terms[order[i]];
                        if (term.pos >= term.docs.size()) continue;
                        const NindTermIndex::Document &posting = term.docs[term.pos];
                        if (posting.ident == docId) {
                            term.matchedDoc = docId;
                            term.matchedFrequency = posting.frequency;
                            term.matched = true;
                            if (posting.frequency != 0)
                                approximation += term.multiplicity * contribution(term.idf, posting.frequency, length, parameters);
                            if (++term.pos >= term.docs.size()) continue;
                        }
                        const NindDocId current = term.docs[term.pos].ident;
                        if (current <= windowMax && (!anyNext || current < nextDoc)) { nextDoc = current; anyNext = true; }
                    }
                    const NindDocId thisDoc = docId;
                    docId = nextDoc;
                    any = anyNext;
                    if (position == NO_POSITION) continue;
                    // the non-essential terms, by decreasing bound, while the document may reach theta
                    bool rejected = false;
                    for (size_t i = essential; i-- > 0; ) {
                        if (approximation + prefix[i] < threshold) { rejected = true; break; }
                        MaxScoreTerm &term = terms[order[i]];
                        decode(termIndex, term, term.block);
                        while (term.pos < term.docs.size() && term.docs[term.pos].ident < thisDoc) term.pos++;
                        if (term.pos == term.docs.size() || term.docs[term.pos].ident != thisDoc) continue;
                        const uint32_t frequency = term.docs[term.pos].frequency;
                        term.matchedDoc = thisDoc;
                        term.matchedFrequency = frequency;
                        term.matched = true;
                        if (frequency != 0)
                            approximation += term.multiplicity * contribution(term.idf, frequency, length, parameters);
                    }
                    if (rejected || !(approximation > 0) || approximation < threshold) continue;
                    candidates.push_back(Candidate{position, approximation});
                    for (size_t t = 0; t < termsNb; t++)
                        candidateFrequencies.push_back((terms[t].matched && terms[t].matchedDoc == thisDoc) ? terms[t].matchedFrequency : 0);
                    if (best.size() < topK) best.push(approximation);
                    else if (approximation > best.top()) { best.pop(); best.push(approximation); }
                    if (best.size() == topK) threshold = best.top() * (1 - PRUNING_MARGIN);
                    //(a higher theta: fewer essential terms from the next window on)
                }
            }
            if (windowMax == ~(NindDocId)0) break;
            windowMin = windowMax + 1;
        }
        // the candidates that may be in the top k, scored exactly as the exhaustive search does
        std::vector<std::pair<double, uint32_t> > ranked;
        for (size_t c = 0; c < candidates.size(); c++) {
            if (candidates[c].approximation < threshold) continue;
            const uint32_t position = candidates[c].position;
            double score = 0.0;
            for (size_t o = 0; o < occurrences.size(); o++) {
                const size_t t = termOf[occurrences[o]];
                if (t == (size_t)-1) continue;
                const uint32_t frequency = candidateFrequencies[c * termsNb + t];
                if (frequency == 0) continue;
                score += contribution(terms[t].idf, frequency, lengthOf(position), parameters);
            }
            if (score > 0) ranked.push_back(std::make_pair(-score, position));
        }
        const size_t kept = std::min(topK, ranked.size());
        std::partial_sort(ranked.begin(), ranked.begin() + kept, ranked.end());
        result.resize(kept);
        for (size_t i = 0; i < kept; i++) result[i] = std::make_pair(m_docIds[ranked[i].second], -ranked[i].first);
        return true;
    }

    uint32_t positionOf(const uint64_t docId) const {
        //ids 0..N-1 in order (an index NindIndexer built): the position is the id
        if (m_identity) return docId < m_docsNb ? (uint32_t)docId : NO_POSITION;
        if (!m_dense.empty() || m_sparse.empty())
            return docId < m_dense.size() ? m_dense[docId] : NO_POSITION;
        const auto it = std::lower_bound(m_sparse.begin(), m_sparse.end(), docId,
                                         [](const std::pair<uint64_t, uint32_t> &a, const uint64_t id)
                                         { return a.first < id; });
        return (it != m_sparse.end() && it->first == docId) ? it->second : NO_POSITION;
    }

    std::vector<uint64_t> m_docIds;     //position -> document id
    std::vector<uint32_t> m_lengths;    //position -> length (or, all under 2^16:)
    std::vector<uint16_t> m_shortLengths;
    double lengthOf(const uint32_t position) const {
        return m_shortLengths.empty() ? (double)m_lengths[position] : (double)m_shortLengths[position];
    }
    uint32_t m_minLength;               //the shortest document
    bool m_identity;                    //document i at position i: no table
    std::vector<uint32_t> m_dense;      //document id -> position (NO_POSITION: none), or empty
    std::vector<std::pair<uint64_t, uint32_t> > m_sparse;   //(document id, position), by id, when not dense
    size_t m_docsNb;
    double m_avgDocLength;
    // scratch arrays reused by successive searches (all zero / unmarked between searches): a
    // pool, so that concurrent searches each have their own; at most one per hardware thread kept
    struct Scratch {
        std::vector<double> scores;
        std::vector<uint32_t> stamps;
        uint32_t stamp;
        NindReadBuffer buffer;          //for the term index's const reads
        Scratch(): scores(), stamps(), stamp(0), buffer() {}
    };
    struct ScratchLease {
        const Impl &scorer;
        std::unique_ptr<Scratch> scratch;
        bool clean;
        explicit ScratchLease(const Impl &s): scorer(s), scratch(), clean(false) {
            {
                std::lock_guard<std::mutex> lock(scorer.m_poolMutex);
                if (!scorer.m_pool.empty()) {
                    scratch = std::move(scorer.m_pool.back());
                    scorer.m_pool.pop_back();
                }
            }
            if (!scratch) scratch.reset(new Scratch());
        }
        ~ScratchLease() {
            //only clean scratch arrays go back (search sets clean after resetting its scores;
            //an exception may have left scores behind)
            if (!scratch || !clean) return;
            scratch->buffer.shrink();       //a large definition's storage isn't kept
            std::lock_guard<std::mutex> lock(scorer.m_poolMutex);
            const size_t maxKept = std::max(1u, std::thread::hardware_concurrency());
            if (scorer.m_pool.size() < maxKept) scorer.m_pool.push_back(std::move(scratch));
        }
    };
    mutable std::mutex m_poolMutex;
    mutable std::vector<std::unique_ptr<Scratch> > m_pool;
};
////////////////////////////////////////////////////////////
NindBm25Scorer::NindBm25Scorer(const std::vector<NindDocId> &docIds,
                               const std::vector<uint32_t> &lengths,
                               const double avgDocLength):
    m_impl(new Impl(docIds, lengths, avgDocLength))
{
}
////////////////////////////////////////////////////////////
NindBm25Scorer::~NindBm25Scorer()
{
}
////////////////////////////////////////////////////////////
std::vector<std::pair<NindDocId, double> > NindBm25Scorer::search(const NindTermIndex &termIndex,
                                                                  const std::vector<unsigned int> &termIds,
                                                                  const double k1,
                                                                  const double b,
                                                                  const long topK,
                                                                  const bool prune) const
{
    return m_impl->search(termIndex, termIds, k1, b, topK, prune);
}
////////////////////////////////////////////////////////////
size_t NindBm25Scorer::docsNb() const
{
    return m_impl->docsNb();
}
////////////////////////////////////////////////////////////
    } // end namespace
} // end namespace
////////////////////////////////////////////////////////////
