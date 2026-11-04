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
        m_docIds(), m_lengths(), m_dense(), m_sparse(), m_docsNb(docIds.size()), m_avgDocLength(avgDocLength),
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
        m_minLength = 0;
        for (size_t i = 0; i < m_lengths.size(); i++)
            if (i == 0 || m_lengths[i] < m_minLength) m_minLength = m_lengths[i];
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
        // the scores, term occurrence by term occurrence in query order (as NindEngine._bm25_score)
        for (size_t o = 0; o < occurrences.size(); o++) {
            const double idf = idfs[occurrences[o]];
            const std::vector<std::pair<uint32_t, uint32_t> > &termPostings = postings[occurrences[o]];
            for (size_t i = 0; i < termPostings.size(); i++) {
                if (termPostings[i].second == 0) continue;
                const uint32_t position = termPostings[i].first;
                double &score = scores[position];
                if (score == 0.0) touched.push_back(position);
                score += contribution(idf, termPostings[i].second, m_lengths[position], k1, b);
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
    // one term's BM25 contribution: the same operations, in the same order, everywhere (scores and
    // bounds; -ffp-contract=off): each operation is monotonic, so a bound computed at a larger tf
    // and a smaller length is never below a contribution
    double contribution(const double idf, const double tf, const double docLength,
                        const double k1, const double b) const {
        const double numerator = tf * (k1 + 1);
        const double denominator = tf + k1 * (1 - b + b * (docLength / m_avgDocLength));
        return idf * (numerator / denominator);
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

    // Dynamic block-max MaxScore, document at a time, exact. The terms by increasing upper bound
    // (their largest contribution); the shortest prefix of them whose bounds sum is below theta (the
    // k-th best score so far) is non-essential: a document in none of the others can't reach the top
    // k. The essential lists are read in document order (blocks read a few at a time); a document is
    // then completed by the non-essential terms, by decreasing bound, each one first checked against
    // its block's bound and read (that block only) if the document may still reach theta.
    // Scores are accumulated in another order than NindEngine._bm25_score: every comparison with
    // theta has a margin far above the rounding errors, and the candidates kept are scored again,
    // exactly, in query order (identical scores and ranks).
    // false (nothing done) when it doesn't apply: a term in several categories, an idf <= 0, a
    // long list without blocks (format v1).
    static constexpr double PRUNING_MARGIN = 1e-9;
    static const size_t ESSENTIAL_READ_AHEAD = 16;      //blocks read at once from an essential list
    struct MaxScoreTerm {
        NindTermIndex::TermDirectory directory;
        double idf;
        unsigned int multiplicity;      //occurrences in the query
        size_t blocksNb;                //a short list: 1 (its inline postings)
        std::vector<NindDocId> blockFirst, blockLast;
        std::vector<double> blockBounds;
        double bound;                   //the largest of blockBounds
        size_t chunkBegin, chunkEnd;    //blocks decoded in chunk
        std::vector<NindTermIndex::Document> chunk;
        size_t pos;                     //cursor in chunk
        size_t probeBlock;              //(non-essential) the first block that may hold the next document
        MaxScoreTerm(): directory(), idf(0), multiplicity(0), blocksNb(0), blockFirst(), blockLast(),
            blockBounds(), bound(0), chunkBegin(0), chunkEnd(0), chunk(), pos(0), probeBlock(0) {}
        bool exhausted() const { return pos >= chunk.size(); }     //(essential: see nextBlocks)
    };
    void readBlocks(const NindTermIndex &termIndex, const unsigned int ident, MaxScoreTerm &term,
                    const size_t first, const size_t end, NindReadBuffer &buffer) const {
        term.chunk.clear();
        termIndex.getTermBlocks(ident, term.directory, 0, first, end, term.chunk, buffer);
        term.chunkBegin = first;
        term.chunkEnd = end;
        term.pos = 0;
    }
    // an essential list's next blocks, until a posting (or the end)
    void nextBlocks(const NindTermIndex &termIndex, const unsigned int ident, MaxScoreTerm &term,
                    NindReadBuffer &buffer) const {
        while (term.pos >= term.chunk.size() && term.chunkEnd < term.blocksNb)
            readBlocks(termIndex, ident, term, term.chunkEnd,
                       std::min(term.chunkEnd + ESSENTIAL_READ_AHEAD, term.blocksNb), buffer);
    }
    bool searchMaxScore(const NindTermIndex &termIndex, const std::vector<unsigned int> &distinctIds,
                        const std::vector<size_t> &occurrences, const double k1, const double b,
                        const size_t topK, NindReadBuffer &buffer,
                        std::vector<std::pair<uint64_t, double> > &result) const
    {
        const double shortest = m_minLength;
        // the query's terms present in the index (index in distinctIds -> in terms)
        std::vector<MaxScoreTerm> terms;
        std::vector<unsigned int> termIdents;
        std::vector<size_t> termOf(distinctIds.size(), (size_t)-1);
        for (size_t d = 0; d < distinctIds.size(); d++) {
            MaxScoreTerm term;
            if (!termIndex.getTermDirectory(distinctIds[d], term.directory, buffer)) continue;
            if (term.directory.categories.size() != 1) return false;
            const NindTermIndex::TermDirectory::Category &category = term.directory.categories[0];
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
                                               contribution(term.idf, block.maxFrequency, length, k1, b));
                }
            }
            else {
                if (category.inlinePostings.empty()) continue;
                term.blocksNb = 1;
                term.blockFirst.push_back(category.inlinePostings.front().ident);
                term.blockLast.push_back(category.inlinePostings.back().ident);
                double bound = 0.0;
                for (size_t i = 0; i < category.inlinePostings.size(); i++) {
                    const NindTermIndex::Document &posting = category.inlinePostings[i];
                    const uint32_t position = positionOf(posting.ident);
                    if (position == NO_POSITION || posting.frequency == 0) continue;
                    bound = std::max(bound, contribution(term.idf, posting.frequency, m_lengths[position], k1, b));
                }
                term.blockBounds.push_back(term.multiplicity * bound);
                term.chunk = category.inlinePostings;
                term.chunkEnd = 1;
            }
            term.bound = *std::max_element(term.blockBounds.begin(), term.blockBounds.end());
            termOf[d] = terms.size();
            terms.push_back(std::move(term));
            termIdents.push_back(distinctIds[d]);
        }
        const size_t termsNb = terms.size();
        if (termsNb == 0) { result.clear(); return true; }
        // by increasing bound; prefix[i]: the bounds sum of order[0..i]
        std::vector<size_t> order(termsNb);
        for (size_t i = 0; i < termsNb; i++) order[i] = i;
        std::stable_sort(order.begin(), order.end(),
                         [&](const size_t x, const size_t y) { return terms[x].bound < terms[y].bound; });
        std::vector<double> prefix(termsNb);
        for (size_t i = 0; i < termsNb; i++) prefix[i] = (i ? prefix[i - 1] : 0.0) + terms[order[i]].bound;
        // the long lists: their first blocks
        for (size_t i = 0; i < termsNb; i++) nextBlocks(termIndex, termIdents[i], terms[i], buffer);
        std::priority_queue<double, std::vector<double>, std::greater<double> > best;  //the k best approximations
        double threshold = 0.0;                 //theta minus the margin
        size_t nonEssential = 0;                //order[0..nonEssential) are non-essential
        struct Candidate { uint32_t position; double approximation; };
        std::vector<Candidate> candidates;
        std::vector<uint32_t> candidateFrequencies;   //termsNb per candidate
        std::vector<uint32_t> frequencies(termsNb);
        while (nonEssential < termsNb) {
            // the next document: the smallest current one of the essential lists
            bool any = false;
            NindDocId docId = 0;
            for (size_t i = nonEssential; i < termsNb; i++) {
                const MaxScoreTerm &term = terms[order[i]];
                if (term.exhausted()) continue;
                const NindDocId current = term.chunk[term.pos].ident;
                if (!any || current < docId) { docId = current; any = true; }
            }
            if (!any) break;
            const uint32_t position = positionOf(docId);
            std::fill(frequencies.begin(), frequencies.end(), 0);
            double approximation = 0.0;
            for (size_t i = nonEssential; i < termsNb; i++) {
                const size_t t = order[i];
                MaxScoreTerm &term = terms[t];
                if (term.exhausted() || term.chunk[term.pos].ident != docId) continue;
                frequencies[t] = term.chunk[term.pos].frequency;
                term.pos++;
                nextBlocks(termIndex, termIdents[t], term, buffer);
                if (position != NO_POSITION && frequencies[t] != 0)
                    approximation += term.multiplicity * contribution(term.idf, frequencies[t], m_lengths[position], k1, b);
            }
            if (position == NO_POSITION) continue;
            // the non-essential terms, by decreasing bound, while the document may reach theta
            bool rejected = false;
            for (size_t i = nonEssential; i-- > 0; ) {
                if (approximation + prefix[i] < threshold) { rejected = true; break; }
                const size_t t = order[i];
                MaxScoreTerm &term = terms[t];
                while (term.probeBlock < term.blocksNb && term.blockLast[term.probeBlock] < docId) term.probeBlock++;
                if (term.probeBlock == term.blocksNb || term.blockFirst[term.probeBlock] > docId) continue;   //not in it
                const double others = i ? prefix[i - 1] : 0.0;
                if (approximation + term.blockBounds[term.probeBlock] + others < threshold) { rejected = true; break; }
                if (term.probeBlock < term.chunkBegin || term.probeBlock >= term.chunkEnd)
                    readBlocks(termIndex, termIdents[t], term, term.probeBlock, term.probeBlock + 1, buffer);
                while (term.pos < term.chunk.size() && term.chunk[term.pos].ident < docId) term.pos++;
                if (term.pos == term.chunk.size() || term.chunk[term.pos].ident != docId) continue;
                frequencies[t] = term.chunk[term.pos].frequency;
                if (frequencies[t] != 0)
                    approximation += term.multiplicity * contribution(term.idf, frequencies[t], m_lengths[position], k1, b);
            }
            if (rejected || !(approximation > 0) || approximation < threshold) continue;
            candidates.push_back(Candidate{position, approximation});
            candidateFrequencies.insert(candidateFrequencies.end(), frequencies.begin(), frequencies.end());
            if (best.size() < topK) best.push(approximation);
            else if (approximation > best.top()) { best.pop(); best.push(approximation); }
            if (best.size() == topK) {
                threshold = best.top() * (1 - PRUNING_MARGIN);
                while (nonEssential < termsNb && prefix[nonEssential] < threshold) nonEssential++;
            }
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
                score += contribution(terms[t].idf, frequency, m_lengths[position], k1, b);
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
        if (!m_dense.empty() || m_sparse.empty())
            return docId < m_dense.size() ? m_dense[docId] : NO_POSITION;
        const auto it = std::lower_bound(m_sparse.begin(), m_sparse.end(), docId,
                                         [](const std::pair<uint64_t, uint32_t> &a, const uint64_t id)
                                         { return a.first < id; });
        return (it != m_sparse.end() && it->first == docId) ? it->second : NO_POSITION;
    }

    std::vector<uint64_t> m_docIds;     //position -> document id
    std::vector<uint32_t> m_lengths;    //position -> length
    uint32_t m_minLength;               //the shortest document
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
