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
#include "NindThreadPool.h"
#include "NindBasics/NindReadBuffer.h"
#include "NindBasics/NindMappedFile.h"
#include <algorithm>
#include <atomic>
#include <exception>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <functional>
#include <future>
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
        m_docIds(), m_first(0), m_lengths(), m_shortLengths(), m_minLength(0), m_identity(false), m_dense(), m_sparse(), m_docsNb(docIds.size()),
        m_avgDocLength(avgDocLength),
        m_poolMutex(), m_pool()
    {
        if (docIds.size() != lengths.size()) throw std::invalid_argument("doc_ids and lengths must have the same length");
        if (docIds.size() >= NO_POSITION) throw std::invalid_argument("too many documents");
        uint64_t maxId = 0, minId = docIds.empty() ? 0 : docIds[0];
        for (size_t i = 0; i < docIds.size(); i++) {
            maxId = std::max(maxId, (uint64_t)docIds[i]);
            minId = std::min(minId, (uint64_t)docIds[i]);
        }
        m_first = minId;
        //ids first, first + 1... in order (an index NindIndexer built, or a part of it): no table
        m_identity = true;
        for (size_t i = 0; i < docIds.size() && m_identity; i++) m_identity = (docIds[i] == minId + i);
        // a document id given twice: its last position counts (and only it is scored)
        if (m_identity) {}
        else if (docIds.size() != 0 && maxId - minId < 2 * (uint64_t)docIds.size() + DENSE_SLACK) {
            m_dense.assign((size_t)(maxId - minId) + 1, NO_POSITION);
            for (size_t i = 0; i < docIds.size(); i++) m_dense[docIds[i] - minId] = (uint32_t)i;
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
        if (!m_identity) m_docIds.assign(docIds.begin(), docIds.end());
        setLengths(lengths);
    }
    // documents firstId, firstId + 1...: no table
    Impl(const NindDocId firstId, const std::vector<uint32_t> &lengths, const double avgDocLength):
        m_docIds(), m_first(firstId), m_lengths(), m_shortLengths(), m_minLength(0), m_identity(true), m_dense(), m_sparse(),
        m_docsNb(lengths.size()), m_avgDocLength(avgDocLength),
        m_poolMutex(), m_pool()
    {
        if (lengths.size() >= NO_POSITION) throw std::invalid_argument("too many documents");
        setLengths(lengths);
    }
    // documents firstId, firstId + 1..., their lengths in a mapped file (not copied)
    Impl(const NindDocId firstId, const std::shared_ptr<const NindMappedFile> &file, const uint32_t *lengths,
         const size_t count, const double avgDocLength):
        m_docIds(), m_first(firstId), m_lengths(), m_shortLengths(), m_minLength(0), m_identity(true), m_dense(), m_sparse(),
        m_docsNb(count), m_avgDocLength(avgDocLength),
        m_poolMutex(), m_pool()
    {
        if (count >= NO_POSITION) throw std::invalid_argument("too many documents");
        m_wide = lengths;
        m_mapped = file;
        for (size_t i = 0; i < count; i++)
            if (i == 0 || lengths[i] < m_minLength) m_minLength = lengths[i];
    }
    void setLengths(const std::vector<uint32_t> &lengths) {
        //lengths under 2^16: 2 bytes each (more of them in the processor's caches)
        uint32_t maxLength = 0;
        for (size_t i = 0; i < lengths.size(); i++) maxLength = std::max(maxLength, lengths[i]);
        if (maxLength < 65536) {
            m_shortLengths.assign(lengths.begin(), lengths.end());
            m_narrow = m_shortLengths.data();
        }
        else {
            m_lengths.assign(lengths.begin(), lengths.end());
            m_wide = m_lengths.data();
        }
        m_minLength = 0;
        for (size_t i = 0; i < lengths.size(); i++)
            if (i == 0 || lengths[i] < m_minLength) m_minLength = lengths[i];
    }

    // the documents with a positive score for the query terms (in query order, repeats
    // included, 0 = unknown term), by decreasing score, at most topK if topK >= 0
    std::vector<std::pair<uint64_t, double> > search(const NindTermIndex &termIndex,
                                                     const std::vector<unsigned int> &termIds,
                                                     const double k1, const double b,
                                                     const long topK, const bool prune,
                                                     const unsigned int threads,
                                                     const NindBm25Scorer::SearchContext *context) const
    {
        //this search's corpus: these documents, or the segmented index around them
        Corpus corpus;
        corpus.docsNb = context ? context->docsNb : m_docsNb;
        corpus.avgDocLength = context ? context->avgDocLength : m_avgDocLength;
        corpus.deleted = context ? context->deleted : 0;
        corpus.threshold = context ? context->threshold : 0;
        if (context && context->documentFrequencies && context->documentFrequencies->size() != termIds.size())
            throw std::invalid_argument("as many document frequencies as terms expected");
        // scratch arrays and read buffer from the pool: concurrent searches each get their own
        ScratchLease lease(*this);
        NindReadBuffer &buffer = lease.scratch->buffer;
        std::vector<uint32_t> touched;          // positions whose score isn't 0 any more

        // the query's distinct terms; per occurrence (query order, repeats included) its term
        std::vector<unsigned int> distinctIds;
        std::vector<size_t> occurrences;
        std::unordered_map<unsigned int, size_t> distinct;
        for (size_t t = 0; t < termIds.size(); t++) {
            if (termIds[t] == 0) continue;
            const auto found = distinct.insert(std::make_pair(termIds[t], distinctIds.size()));
            if (found.second) {
                distinctIds.push_back(termIds[t]);
                if (context && context->documentFrequencies) corpus.dfs.push_back((*context->documentFrequencies)[t]);
            }
            occurrences.push_back(found.first->second);
        }
        // per distinct term: its idf and the postings to score, (position, frequency): every posting
        // (exhaustive), or only those of the blocks that may reach the top k (block-max pruning)
        std::vector<double> idfs;
        std::vector<std::vector<std::pair<uint32_t, uint32_t> > > postings;
        if (prune && topK > 0 && k1 >= 0 && b >= 0 && b <= 1 && !distinctIds.empty()) {
            std::vector<std::pair<uint64_t, double> > result;
            if (searchMaxScore(termIndex, distinctIds, occurrences, k1, b, (size_t)topK, std::max(1u, threads), corpus, buffer, result)) {
                lease.clean = true;         //(scores untouched)
                return result;
            }
        }
        // one score per document, for the exhaustive search only (MaxScore keeps its candidates
        // apart): 8 bytes a document per concurrent search, 1 GB at 138M
        std::vector<double> &scores = lease.scratch->scores;
        if (scores.size() != m_docsNb) scores.assign(m_docsNb, 0.0);
        collectAll(termIndex, distinctIds, corpus, buffer, lease.scratch->stamps, lease.scratch->stamp, idfs, postings);
        const Params parameters = params(k1, b, corpus);
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
        for (size_t i = 0; i < kept; i++) result[i] = std::make_pair(docIdOf(ranked[i].second), -ranked[i].first);
        return result;
    }

    size_t docsNb() const { return m_docsNb; }

    // a term's documents in termIndex, every category counted
    uint64_t documentFrequency(const NindTermIndex &termIndex, const unsigned int termId, NindReadBuffer &buffer) const {
        const std::shared_ptr<const NindTermIndex::TermDirectory> directory = directoryOf(termIndex, termId, buffer);
        uint64_t df = 0;
        if (directory)
            for (size_t c = 0; c < directory->categories.size(); c++) df += directory->categories[c].docsNb;
        return df;
    }
    size_t positionOfDocument(const NindDocId docId) const {
        const uint32_t position = positionOf(docId);
        return position == NO_POSITION ? m_docsNb : position;
    }
    NindDocId docIdAt(const size_t position) const { return docIdOf((uint32_t)position); }
    uint32_t lengthAt(const size_t position) const { return (uint32_t)lengthOf((uint32_t)position); }

private:
    // what a search takes from its corpus (see SearchContext); dfs by distinct term, empty: this index's
    struct Corpus {
        uint64_t docsNb;
        double avgDocLength;
        std::vector<uint64_t> dfs;
        const std::vector<uint64_t> *deleted;
        std::atomic<double> *threshold;
        Corpus(): docsNb(0), avgDocLength(0), dfs(), deleted(0), threshold(0) {}
        bool isDeleted(const uint32_t position) const {
            return deleted != 0 && ((*deleted)[position / 64] >> (position % 64) & 1) != 0;
        }
    };
    // a query's BM25 constants: tf * (k1 + 1) / (tf + k1 * (1 - b) + k1 * b / avgDocLength * length)
    struct Params {
        double k1Plus1, base, perLength;
    };
    Params params(const double k1, const double b, const Corpus &corpus) const {
        Params p;
        p.k1Plus1 = k1 + 1;
        p.base = k1 * (1 - b);
        p.perLength = (corpus.avgDocLength > 0) ? k1 * b / corpus.avgDocLength : 0.0;
        return p;
    }
    // one term's BM25 contribution: the same function everywhere (scores and bounds): monotonic in
    // tf (increasing) and in the length (decreasing), so a bound computed at a larger tf and a
    // smaller length is never below a contribution
    static double contribution(const double idf, const double tf, const double docLength, const Params &p) {
        return idf * (tf * p.k1Plus1 / (tf + p.base + p.perLength * docLength));
    }
    static double idfOf(const uint64_t df, const uint64_t docsNb) {
        //same operations, in the same order, as NindEngine._bm25_score
        return std::log(((double)((int64_t)docsNb - (int64_t)df) + 0.5) / ((double)df + 0.5) + 1.0);
    }

    // every posting of every term (several categories: a document's first one only)
    void collectAll(const NindTermIndex &termIndex, const std::vector<unsigned int> &distinctIds,
                    const Corpus &corpus, NindReadBuffer &buffer, std::vector<uint32_t> &stamps, uint32_t &stamp,
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
            idfs.push_back(idfOf(corpus.dfs.empty() ? (uint64_t)read.size() : corpus.dfs[t], corpus.docsNb));
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
                if (position == NO_POSITION || corpus.isDeleted(position)) continue;
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
    static const size_t NO_BLOCK = (size_t)-1;
    static const size_t WINDOW_SPAN = 16384;            //a window's largest span of document ids (a multiple of 64)
    static const size_t MIN_WINDOW_SPAN = 4096;         //scanRangeByWindow's windows: at least this many
    static const size_t WINDOWED_TERMS = 5;             //from this many terms, scanRangeByWindow
    // a query term, as every thread of a search sees it (read only)
    struct TermInfo {
        std::shared_ptr<const NindTermIndex::TermDirectory> directory;     //(from the scorer's cache)
        unsigned int ident;
        double idf;
        unsigned int multiplicity;      //occurrences in the query
        bool inlineList;                //a short list: one pseudo-block, its inline postings
        size_t blocksNb;
        const NindTermIndex::Block *blocks;     //the directory's blocks, or ownBlock (an inline list)
        NindTermIndex::Block ownBlock;
        std::shared_ptr<const std::vector<double> > tf;     //per block, its bound without the idf
        TermInfo(): directory(), ident(0), idf(0), multiplicity(0), inlineList(false), blocksNb(0), blocks(0),
            ownBlock(), tf() {}
        NindDocId first(const size_t block) const { return blocks[block].first; }
        NindDocId last(const size_t block) const { return blocks[block].last; }
        //a block's bound: (its contribution's largest) * multiplicity, the same value as
        //multiplicity * contribution(idf, ...) (a product with a positive number keeps the order)
        double boundAt(const size_t block) const { return multiplicity * (idf * (*tf)[block]); }
    };
    // a thread's cursor on a term's postings, read lazily: blocks' bytes RAW_READ_AHEAD at a time,
    // one block decoded only when a window needs it
    struct Cursor {
        const TermInfo *info;
        size_t block;                   //the first block that may hold documents >= the window
        std::unique_ptr<NindReadBuffer> raw;    //the bytes of blocks [rawBegin, rawEnd)
        size_t rawBegin, rawEnd;
        std::vector<NindTermIndex::Document> docs;      //the decoded block (decoded: its index)
        size_t decoded;
        size_t pos;                     //cursor in docs
        NindDocId matchedDoc;           //the last document scored with this term, its frequency
        uint32_t matchedFrequency;
        bool matched;
        Cursor(): info(0), block(0), raw(), rawBegin(0), rawEnd(0), docs(), decoded(NO_BLOCK), pos(0),
            matchedDoc(0), matchedFrequency(0), matched(false) {}
    };
    // the first block of a term whose last document is at least lo
    static size_t firstBlockEndingFrom(const TermInfo &term, const NindDocId lo) {
        size_t low = 0, high = term.blocksNb;
        while (low < high) {
            const size_t middle = low + (high - low) / 2;
            if (term.blocks[middle].last < lo) low = middle + 1;
            else high = middle;
        }
        return low;
    }
    // the postings of block b of a term in docs (decoded once), the cursor at its first document
    void decode(const NindTermIndex &termIndex, Cursor &cursor, const size_t b) const {
        if (cursor.decoded == b) return;
        cursor.pos = 0;
        cursor.decoded = b;
        const TermInfo &info = *cursor.info;
        if (info.inlineList) {
            cursor.docs = info.directory->categories[0].inlinePostings;
            return;
        }
        if (b < cursor.rawBegin || b >= cursor.rawEnd) {
            cursor.rawBegin = b;
            cursor.rawEnd = std::min(b + RAW_READ_AHEAD, info.blocksNb);
            if (!cursor.raw) cursor.raw.reset(new NindReadBuffer());
            termIndex.readTermBlocks(info.ident, *info.directory, 0, cursor.rawBegin, cursor.rawEnd, *cursor.raw);
        }
        cursor.docs.clear();
        termIndex.decodeTermBlock(info.ident, *info.directory, 0, cursor.rawBegin, b, *cursor.raw, cursor.docs);
    }
    // what a thread found: its candidates (with their frequencies, termsNb each), its theta
    struct Found {
        std::vector<std::pair<uint32_t, double> > candidates;  //(position, approximation)
        std::vector<uint32_t> frequencies;
        double threshold;
        Found(): candidates(), frequencies(), threshold(0) {}
    };
    // theta shared by the threads of a search: the largest of their k-th best approximations (a
    // lower bound of the k-th best score of all)
    static void raise(std::atomic<double> &shared, const double value) {
        double current = shared.load(std::memory_order_relaxed);
        while (value > current && !shared.compare_exchange_weak(current, value, std::memory_order_relaxed)) {}
    }
    // Block-max MaxScore over windows, on the documents [lo, hi], document at a time (see
    // searchMaxScore): a query of few terms
    void scanRangeByDocument(const NindTermIndex &termIndex, const std::vector<TermInfo> &terms, const NindDocId lo,
                   const NindDocId hi, const size_t topK, const Params &parameters, const Corpus &corpus,
                   std::atomic<double> &shared, Found &found) const
    {
        const size_t termsNb = terms.size();
        std::vector<Cursor> cursors(termsNb);
        for (size_t t = 0; t < termsNb; t++) {
            cursors[t].info = &terms[t];
            //(the first block that may hold documents >= lo: a binary search, not a walk from 0)
            cursors[t].block = firstBlockEndingFrom(terms[t], lo);
        }
        std::priority_queue<double, std::vector<double>, std::greater<double> > best;  //the k best approximations
        double threshold = 0.0;                 //theta minus the margin
        // per window: the terms in it (by increasing bound), their bounds, the bounds' prefix sums
        std::vector<size_t> order;
        std::vector<double> windowBound(termsNb), prefix(termsNb);
        order.reserve(termsNb);
        NindDocId windowMin = ~(NindDocId)0;
        for (size_t t = 0; t < termsNb; t++) windowMin = std::min(windowMin, terms[t].first(0));
        windowMin = std::max(windowMin, lo);
        while (windowMin <= hi) {
            threshold = std::max(threshold, shared.load(std::memory_order_relaxed));
            // each term's block at or after windowMin; the window: up to the next block boundary
            NindDocId windowMax = hi;
            order.clear();
            for (size_t t = 0; t < termsNb; t++) {
                Cursor &cursor = cursors[t];
                const TermInfo &term = terms[t];
                while (cursor.block < term.blocksNb && term.last(cursor.block) < windowMin) cursor.block++;
                if (cursor.block == term.blocksNb) continue;
                if (term.first(cursor.block) > windowMin) windowMax = std::min(windowMax, term.first(cursor.block) - 1);
                else windowMax = std::min(windowMax, term.last(cursor.block));
                order.push_back(t);
            }
            if (order.empty()) break;
            // the terms with a block in the window, by increasing bound (the others contribute 0)
            size_t present = 0;
            for (size_t i = 0; i < order.size(); i++) {
                const size_t t = order[i];
                const TermInfo &term = terms[t];
                windowBound[t] = (term.first(cursors[t].block) <= windowMax) ? term.boundAt(cursors[t].block) : 0.0;
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
                    Cursor &cursor = cursors[order[i]];
                    decode(termIndex, cursor, cursor.block);
                    while (cursor.pos < cursor.docs.size() && cursor.docs[cursor.pos].ident < windowMin) cursor.pos++;
                }
                // the window's documents in the essential lists, in order: each document's score and
                // the next document in one pass
                NindDocId docId = 0;
                bool any = false;
                for (size_t i = essential; i < order.size(); i++) {
                    const Cursor &cursor = cursors[order[i]];
                    if (cursor.pos >= cursor.docs.size()) continue;
                    const NindDocId current = cursor.docs[cursor.pos].ident;
                    if (current <= windowMax && (!any || current < docId)) { docId = current; any = true; }
                }
                while (any) {
                    const uint32_t position = positionOf(docId);
                    const double length = (position != NO_POSITION) ? lengthOf(position) : 0.0;
                    double approximation = 0.0;
                    NindDocId nextDoc = 0;
                    bool anyNext = false;
                    for (size_t i = essential; i < order.size(); i++) {
                        Cursor &cursor = cursors[order[i]];
                        if (cursor.pos >= cursor.docs.size()) continue;
                        const NindTermIndex::Document &posting = cursor.docs[cursor.pos];
                        if (posting.ident == docId) {
                            cursor.matchedDoc = docId;
                            cursor.matchedFrequency = posting.frequency;
                            cursor.matched = true;
                            if (posting.frequency != 0)
                                approximation += cursor.info->multiplicity *
                                                 contribution(cursor.info->idf, posting.frequency, length, parameters);
                            if (++cursor.pos >= cursor.docs.size()) continue;
                            //the length of a document a few postings ahead, fetched early (a cache miss
                            //otherwise: the documents' lengths are scattered)
                            if (cursor.pos + PREFETCH_DISTANCE < cursor.docs.size())
                                prefetchLength(cursor.docs[cursor.pos + PREFETCH_DISTANCE].ident);
                        }
                        const NindDocId current = cursor.docs[cursor.pos].ident;
                        if (current <= windowMax && (!anyNext || current < nextDoc)) { nextDoc = current; anyNext = true; }
                    }
                    const NindDocId thisDoc = docId;
                    docId = nextDoc;
                    any = anyNext;
                    //(a deleted document never reaches theta, nor the candidates)
                    if (position == NO_POSITION || corpus.isDeleted(position)) continue;
                    // the non-essential terms, by decreasing bound, while the document may reach theta
                    bool rejected = false;
                    for (size_t i = essential; i-- > 0; ) {
                        if (approximation + prefix[i] < threshold) { rejected = true; break; }
                        Cursor &cursor = cursors[order[i]];
                        decode(termIndex, cursor, cursor.block);
                        while (cursor.pos < cursor.docs.size() && cursor.docs[cursor.pos].ident < thisDoc) cursor.pos++;
                        if (cursor.pos == cursor.docs.size() || cursor.docs[cursor.pos].ident != thisDoc) continue;
                        const uint32_t frequency = cursor.docs[cursor.pos].frequency;
                        cursor.matchedDoc = thisDoc;
                        cursor.matchedFrequency = frequency;
                        cursor.matched = true;
                        if (frequency != 0)
                            approximation += cursor.info->multiplicity * contribution(cursor.info->idf, frequency, length, parameters);
                    }
                    if (rejected || !(approximation > 0) || approximation < threshold) continue;
                    found.candidates.push_back(std::make_pair(position, approximation));
                    for (size_t t = 0; t < termsNb; t++)
                        found.frequencies.push_back((cursors[t].matched && cursors[t].matchedDoc == thisDoc) ?
                                                    cursors[t].matchedFrequency : 0);
                    if (best.size() < topK) best.push(approximation);
                    else if (approximation > best.top()) { best.pop(); best.push(approximation); }
                    if (best.size() == topK) {
                        const double mine = best.top() * (1 - PRUNING_MARGIN);
                        if (mine > threshold) {
                            threshold = mine;
                            raise(shared, mine);
                        }
                    }
                    //(a higher theta: fewer essential terms from the next window on)
                }
            }
            if (windowMax >= hi) break;
            windowMin = windowMax + 1;
        }
        found.threshold = threshold;
    }
    // Block-max MaxScore over windows, on the documents [lo, hi], term at a time in each window (see
    // searchMaxScore): a query of many terms. Windows of at least MIN_WINDOW_SPAN documents (a term
    // may have several blocks there: its bound, the largest of theirs), the essential terms' postings
    // added term after term into a dense array, the documents they touch then completed in order.
    // Against scanRangeByDocument, MS MARCO v2 138M, top 10: 9 terms and more 300 -> 141 ms, 5 terms
    // even, 2 terms 3.5 -> 8.3 ms (the window's machinery for few documents): hence WINDOWED_TERMS.
    void scanRangeByWindow(const NindTermIndex &termIndex, const std::vector<TermInfo> &terms, const NindDocId lo,
                   const NindDocId hi, const size_t topK, const Params &parameters, const Corpus &corpus,
                   std::atomic<double> &shared, Found &found) const
    {
        const size_t termsNb = terms.size();
        std::vector<Cursor> cursors(termsNb);
        for (size_t t = 0; t < termsNb; t++) {
            cursors[t].info = &terms[t];
            //(the first block that may hold documents >= lo: a binary search, not a walk from 0)
            cursors[t].block = firstBlockEndingFrom(terms[t], lo);
        }
        std::priority_queue<double, std::vector<double>, std::greater<double> > best;  //the k best approximations
        double threshold = 0.0;                 //theta minus the margin
        // per window: the terms in it (by increasing bound), their bounds, the bounds' prefix sums;
        // sorted: every term by its bound in the last window (from one window to the next, usually
        // one term changes block: an insertion sort from it is linear, from scratch quadratic, a
        // quarter of a long query's time)
        std::vector<size_t> order, sorted(termsNb);
        std::vector<double> windowBound(termsNb, 0.0), prefix(termsNb);
        order.reserve(termsNb);
        for (size_t t = 0; t < termsNb; t++) sorted[t] = t;
        // a window's approximations, by document, and the documents touched (at most WINDOW_SPAN)
        std::vector<double> windowScores(WINDOW_SPAN, 0.0);
        std::vector<uint64_t> touched(WINDOW_SPAN / 64, 0);
        std::vector<bool> essentialTerm(termsNb, false);
        std::vector<std::vector<std::pair<NindDocId, uint32_t> > > windowPostings(termsNb);   //an essential term's, in the window
        NindDocId windowMin = ~(NindDocId)0;
        for (size_t t = 0; t < termsNb; t++) windowMin = std::min(windowMin, terms[t].first(0));
        windowMin = std::max(windowMin, lo);
        while (windowMin <= hi) {
            threshold = std::max(threshold, shared.load(std::memory_order_relaxed));
            // each term's block at or after windowMin; the window: up to the next block boundary, at
            // most WINDOW_SPAN documents (a shorter window keeps the same bounds)
            NindDocId windowMax = std::min<NindDocId>(hi, windowMin + (WINDOW_SPAN - 1));
            bool anyLeft = false;
            for (size_t t = 0; t < termsNb; t++) {
                Cursor &cursor = cursors[t];
                const TermInfo &term = terms[t];
                while (cursor.block < term.blocksNb && term.last(cursor.block) < windowMin) cursor.block++;
                if (cursor.block == term.blocksNb) continue;
                if (term.first(cursor.block) > windowMin) windowMax = std::min(windowMax, term.first(cursor.block) - 1);
                else windowMax = std::min(windowMax, term.last(cursor.block));
                anyLeft = true;
            }
            if (!anyLeft) break;
            //at least MIN_WINDOW_SPAN documents: a term may then have several blocks in the window, its
            //bound there the largest of theirs (fewer windows, each with looser bounds)
            windowMax = std::max(windowMax, std::min<NindDocId>(hi, windowMin + (MIN_WINDOW_SPAN - 1)));
            // the terms with a block in the window, by increasing bound (the others: 0, first, left out)
            for (size_t t = 0; t < termsNb; t++) {
                const TermInfo &term = terms[t];
                double bound = 0.0;
                for (size_t b = cursors[t].block; b < term.blocksNb && term.first(b) <= windowMax; b++)
                    bound = std::max(bound, term.boundAt(b));
                windowBound[t] = bound;
            }
            for (size_t i = 1; i < termsNb; i++)
                for (size_t j = i; j > 0 && windowBound[sorted[j]] < windowBound[sorted[j - 1]]; j--)
                    std::swap(sorted[j], sorted[j - 1]);
            size_t absent = 0;
            while (absent < termsNb && !(windowBound[sorted[absent]] > 0)) absent++;
            order.assign(sorted.begin() + absent, sorted.end());
            double sum = 0.0;
            for (size_t i = 0; i < order.size(); i++) prefix[i] = (sum += windowBound[order[i]]);
            // the non-essential prefix: order[0..essential)
            size_t essential = 0;
            while (essential < order.size() && prefix[essential] < threshold) essential++;
            if (essential < order.size()) {
                // the essential terms' postings in the window, term after term (in the order of the
                // bounds, as a document-at-a-time sum would add them): their approximations in a
                // dense array, the documents they touch in a bitset
                for (size_t i = essential; i < order.size(); i++) {
                    const size_t t = order[i];
                    Cursor &cursor = cursors[t];
                    const TermInfo &term = terms[t];
                    std::vector<std::pair<NindDocId, uint32_t> > &own = windowPostings[t];
                    own.clear();
                    const double idf = term.idf, multiplicity = term.multiplicity;
                    for (size_t b = cursor.block; b < term.blocksNb && term.first(b) <= windowMax; b++) {
                        cursor.block = b;
                        decode(termIndex, cursor, b);
                        while (cursor.pos < cursor.docs.size() && cursor.docs[cursor.pos].ident < windowMin) cursor.pos++;
                        for (; cursor.pos < cursor.docs.size() && cursor.docs[cursor.pos].ident <= windowMax; cursor.pos++) {
                            const NindTermIndex::Document &posting = cursor.docs[cursor.pos];
                            const size_t offset = (size_t)(posting.ident - windowMin);
                            touched[offset / 64] |= (uint64_t)1 << (offset % 64);
                            own.push_back(std::make_pair(posting.ident, posting.frequency));
                            //the length of a document a few postings ahead, fetched early (scattered: a cache miss)
                            if (cursor.pos + PREFETCH_DISTANCE < cursor.docs.size())
                                prefetchLength(cursor.docs[cursor.pos + PREFETCH_DISTANCE].ident);
                            if (posting.frequency == 0) continue;
                            const uint32_t position = positionOf(posting.ident);
                            const double length = (position != NO_POSITION) ? lengthOf(position) : 0.0;
                            windowScores[offset] += multiplicity * contribution(idf, posting.frequency, length, parameters);
                        }
                        if (term.last(b) > windowMax) break;     //(its rest: the next windows')
                    }
                }
                bool flagged = false;           //essentialTerm set for this window (at its first candidate)
                // the touched documents, in order
                const size_t words = (size_t)(windowMax - windowMin) / 64 + 1;
                for (size_t w = 0; w < words; w++) {
                    for (uint64_t bits = touched[w]; bits != 0; bits &= bits - 1) {
                        const size_t offset = w * 64 + (size_t)__builtin_ctzll(bits);
                        const NindDocId thisDoc = windowMin + offset;
                        double approximation = windowScores[offset];
                        windowScores[offset] = 0.0;
                        const uint32_t position = positionOf(thisDoc);
                        //(a deleted document never reaches theta, nor the candidates)
                        if (position == NO_POSITION || corpus.isDeleted(position)) continue;
                        const double length = lengthOf(position);
                        // the non-essential terms, by decreasing bound, while the document may reach theta
                        bool rejected = false;
                        for (size_t i = essential; i-- > 0; ) {
                            if (approximation + prefix[i] < threshold) { rejected = true; break; }
                            Cursor &cursor = cursors[order[i]];
                            const TermInfo &term = terms[order[i]];
                            while (cursor.block < term.blocksNb && term.last(cursor.block) < thisDoc) cursor.block++;
                            if (cursor.block == term.blocksNb || term.first(cursor.block) > thisDoc) continue;
                            decode(termIndex, cursor, cursor.block);
                            while (cursor.pos < cursor.docs.size() && cursor.docs[cursor.pos].ident < thisDoc) cursor.pos++;
                            if (cursor.pos == cursor.docs.size() || cursor.docs[cursor.pos].ident != thisDoc) continue;
                            const uint32_t frequency = cursor.docs[cursor.pos].frequency;
                            cursor.matchedDoc = thisDoc;
                            cursor.matchedFrequency = frequency;
                            cursor.matched = true;
                            if (frequency != 0)
                                approximation += cursor.info->multiplicity * contribution(cursor.info->idf, frequency, length, parameters);
                        }
                        if (rejected || !(approximation > 0) || approximation < threshold) continue;
                        found.candidates.push_back(std::make_pair(position, approximation));
                        // its frequencies: the essential terms' from their postings in the window (a
                        // binary search: candidates are few), the others' from their probes
                        if (!flagged) {
                            for (size_t t = 0; t < termsNb; t++) essentialTerm[t] = false;
                            for (size_t i = essential; i < order.size(); i++) essentialTerm[order[i]] = true;
                            flagged = true;
                        }
                        for (size_t t = 0; t < termsNb; t++) {
                            uint32_t frequency = 0;
                            if (essentialTerm[t]) {
                                const std::vector<std::pair<NindDocId, uint32_t> > &own = windowPostings[t];
                                const std::vector<std::pair<NindDocId, uint32_t> >::const_iterator it = std::lower_bound(
                                    own.begin(), own.end(), std::make_pair(thisDoc, (uint32_t)0));
                                if (it != own.end() && it->first == thisDoc) frequency = it->second;
                            }
                            else if (cursors[t].matched && cursors[t].matchedDoc == thisDoc) frequency = cursors[t].matchedFrequency;
                            found.frequencies.push_back(frequency);
                        }
                        if (best.size() < topK) best.push(approximation);
                        else if (approximation > best.top()) { best.pop(); best.push(approximation); }
                        if (best.size() == topK) {
                            const double mine = best.top() * (1 - PRUNING_MARGIN);
                            if (mine > threshold) {
                                threshold = mine;
                                raise(shared, mine);
                            }
                        }
                        //(a higher theta: fewer non-essential terms needed from the next document on)
                    }
                    touched[w] = 0;
                }
            }
            if (windowMax >= hi) break;
            windowMin = windowMax + 1;
        }
        found.threshold = threshold;
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
    // threads > 1: the documents cut into as many ranges of ids, scanned by as many threads at once,
    // theta shared (the largest of their k-th best: a lower bound of the k-th best of all).
    // Scores are accumulated in another order than the exhaustive search's: every comparison with
    // theta has a margin far above the rounding errors, and the candidates kept are scored again
    // in query order, as the exhaustive search does (identical scores and ranks).
    // false (nothing done) when it doesn't apply: a term in several categories, an idf <= 0, a
    // long list without blocks (format v1).
    bool searchMaxScore(const NindTermIndex &termIndex, const std::vector<unsigned int> &distinctIds,
                        const std::vector<size_t> &occurrences, const double k1, const double b,
                        const size_t topK, const unsigned int threads, const Corpus &corpus, NindReadBuffer &buffer,
                        std::vector<std::pair<uint64_t, double> > &result) const
    {
        //(a segment's deleted documents count in its shortest length: never above the live ones')
        const double shortest = m_minLength;
        const Params parameters = params(k1, b, corpus);
        // the query's terms present in the index (index in distinctIds -> in terms)
        std::vector<TermInfo> terms;
        std::vector<size_t> termOf(distinctIds.size(), (size_t)-1);
        terms.reserve(distinctIds.size());
        for (size_t d = 0; d < distinctIds.size(); d++) {
            terms.push_back(TermInfo());
            TermInfo &term = terms.back();
            term.directory = directoryOf(termIndex, distinctIds[d], buffer);
            if (!term.directory) { terms.pop_back(); continue; }
            if (term.directory->categories.size() != 1) return false;
            const NindTermIndex::TermDirectory::Category &category = term.directory->categories[0];
            term.ident = distinctIds[d];
            term.idf = idfOf(corpus.dfs.empty() ? category.docsNb : corpus.dfs[d], corpus.docsNb);
            if (!(term.idf > 0)) return false;
            //a format v1 file: every list inline, a long one would be read whole as one block
            if (category.blocks.empty() && category.inlinePostings.size() > NindTermIndex::BLOCK_SIZE) return false;
            for (size_t o = 0; o < occurrences.size(); o++) if (occurrences[o] == d) term.multiplicity++;
            if (!category.blocks.empty()) {
                term.blocksNb = category.blocks.size();
                term.blocks = category.blocks.data();
                term.tf = blockBounds(termIndex, term.ident, term.directory, parameters, shortest);
            }
            else {
                if (category.inlinePostings.empty()) { terms.pop_back(); continue; }
                term.inlineList = true;
                term.blocksNb = 1;
                term.ownBlock.first = category.inlinePostings.front().ident;
                term.ownBlock.last = category.inlinePostings.back().ident;
                double bound = 0.0;
                for (size_t i = 0; i < category.inlinePostings.size(); i++) {
                    const NindTermIndex::Document &posting = category.inlinePostings[i];
                    const uint32_t position = positionOf(posting.ident);
                    if (position == NO_POSITION || posting.frequency == 0) continue;
                    bound = std::max(bound, contribution(1.0, posting.frequency, lengthOf(position), parameters));
                }
                term.tf = std::make_shared<const std::vector<double> >(1, bound);
            }
            termOf[d] = terms.size() - 1;
        }
        const size_t termsNb = terms.size();
        if (termsNb == 0) { result.clear(); return true; }
        for (size_t t = 0; t < termsNb; t++) if (terms[t].inlineList) terms[t].blocks = &terms[t].ownBlock;
        // the documents [first, last], cut into ranges, one per thread
        NindDocId first = ~(NindDocId)0, last = 0;
        for (size_t t = 0; t < termsNb; t++) {
            first = std::min(first, terms[t].first(0));
            last = std::max(last, terms[t].last(terms[t].blocksNb - 1));
        }
        const uint64_t span = last - first;
        const unsigned int slices = (unsigned int)std::max<uint64_t>(1, std::min<uint64_t>(threads, span / 4096 + 1));
        std::atomic<double> ownThreshold(0.0);
        std::atomic<double> &shared = corpus.threshold ? *corpus.threshold : ownThreshold;
        std::vector<Found> found(slices);
        const bool byWindow = termsNb >= WINDOWED_TERMS;
        const auto scan = [&](const NindDocId lo, const NindDocId hi, Found &into) {
            if (byWindow) scanRangeByWindow(termIndex, terms, lo, hi, topK, parameters, corpus, shared, into);
            else scanRangeByDocument(termIndex, terms, lo, hi, topK, parameters, corpus, shared, into);
        };
        if (slices == 1) scan(first, last, found[0]);
        else {
            //the other ranges to the scorer's pool of threads, the last one here
            std::vector<std::future<void> > done;
            std::exception_ptr error;
            for (unsigned int s = 0; s < slices; s++) {
                const NindDocId lo = first + span / slices * s + std::min<uint64_t>(s, span % slices);
                const NindDocId hi = (s + 1 == slices) ? last : first + span / slices * (s + 1) + std::min<uint64_t>(s + 1, span % slices) - 1;
                auto work = [&, s, lo, hi]() { scan(lo, hi, found[s]); };
                if (s + 1 < slices) done.push_back(m_threadPool.run(work, slices - 1));
                else {
                    try { work(); }
                    catch (...) { error = std::current_exception(); }
                }
            }
            //(every range finished before anything they use goes)
            for (size_t f = 0; f < done.size(); f++) {
                try { done[f].get(); }
                catch (...) { if (!error) error = std::current_exception(); }
            }
            if (error) std::rethrow_exception(error);
        }
        // the candidates that may be in the top k, scored exactly as the exhaustive search does
        double threshold = shared.load();
        for (unsigned int s = 0; s < slices; s++) threshold = std::max(threshold, found[s].threshold);
        std::vector<std::pair<double, uint32_t> > ranked;
        for (unsigned int s = 0; s < slices; s++) {
            const Found &f = found[s];
            for (size_t c = 0; c < f.candidates.size(); c++) {
                if (f.candidates[c].second < threshold) continue;
                const uint32_t position = f.candidates[c].first;
                double score = 0.0;
                for (size_t o = 0; o < occurrences.size(); o++) {
                    const size_t t = termOf[occurrences[o]];
                    if (t == (size_t)-1) continue;
                    const uint32_t frequency = f.frequencies[c * termsNb + t];
                    if (frequency == 0) continue;
                    score += contribution(terms[t].idf, frequency, lengthOf(position), parameters);
                }
                if (score > 0) ranked.push_back(std::make_pair(-score, position));
            }
        }
        const size_t kept = std::min(topK, ranked.size());
        std::partial_sort(ranked.begin(), ranked.begin() + kept, ranked.end());
        result.resize(kept);
        for (size_t i = 0; i < kept; i++) result[i] = std::make_pair(docIdOf(ranked[i].second), -ranked[i].first);
        return true;
    }

    mutable NindThreadPool m_threadPool;

    // A term's parsed directory, from the cache (read and parsed once per term: the scorer serves
    // one immutable index); null if the index hasn't the term. The cache is emptied when it holds
    // more than DIRECTORY_CACHE_BYTES, and when the scorer is used with another term index.
    static const size_t DIRECTORY_CACHE_BYTES = 256u << 20;
    // a cached directory, and its blocks' bounds without the idf for the last parameters asked
    struct CachedTerm {
        std::shared_ptr<const NindTermIndex::TermDirectory> directory;
        std::mutex mutex;
        double k1Plus1, base, perLength, shortest;
        std::shared_ptr<const std::vector<double> > tf;
        explicit CachedTerm(const std::shared_ptr<const NindTermIndex::TermDirectory> &d):
            directory(d), mutex(), k1Plus1(0), base(0), perLength(0), shortest(0), tf() {}
    };
    std::shared_ptr<CachedTerm> cachedTerm(const NindTermIndex &termIndex,
                                           const unsigned int ident,
                                           NindReadBuffer &buffer) const {
        {
            std::shared_lock<std::shared_timed_mutex> lock(m_cacheMutex);
            if (m_cachedIndex == &termIndex) {
                const auto it = m_directories.find(ident);
                if (it != m_directories.end()) return it->second;
            }
        }
        std::shared_ptr<NindTermIndex::TermDirectory> directory(new NindTermIndex::TermDirectory());
        if (!termIndex.getTermDirectory(ident, *directory, buffer)) directory.reset();
        size_t bytes = sizeof(NindTermIndex::TermDirectory) + 64;
        if (directory)
            for (size_t c = 0; c < directory->categories.size(); c++)
                bytes += directory->categories[c].blocks.size() * (sizeof(NindTermIndex::Block) + sizeof(double))
                         + directory->categories[c].inlinePostings.size() * sizeof(NindTermIndex::Document);
        std::shared_ptr<CachedTerm> entry(new CachedTerm(directory));
        std::unique_lock<std::shared_timed_mutex> lock(m_cacheMutex);
        if (m_cachedIndex != &termIndex || m_cacheBytes + bytes > DIRECTORY_CACHE_BYTES) {
            m_directories.clear();
            m_cacheBytes = 0;
            m_cachedIndex = &termIndex;
        }
        const auto inserted = m_directories.insert(std::make_pair(ident, entry));
        if (inserted.second) m_cacheBytes += bytes;
        return inserted.first->second;
    }
    std::shared_ptr<const NindTermIndex::TermDirectory> directoryOf(const NindTermIndex &termIndex,
                                                                    const unsigned int ident,
                                                                    NindReadBuffer &buffer) const {
        return cachedTerm(termIndex, ident, buffer)->directory;
    }
    // a term's blocks' bounds without the idf (its first category's), computed once for the same
    // parameters (each search computed them again: 5% of a search at 138M, a frequent term's
    // hundreds of thousands of blocks)
    std::shared_ptr<const std::vector<double> > blockBounds(const NindTermIndex &termIndex, const unsigned int ident,
                                                           const std::shared_ptr<const NindTermIndex::TermDirectory> &directory,
                                                           const Params &parameters, const double shortest) const {
        std::shared_ptr<CachedTerm> entry;
        {
            std::shared_lock<std::shared_timed_mutex> lock(m_cacheMutex);
            if (m_cachedIndex == &termIndex) {
                const auto it = m_directories.find(ident);
                if (it != m_directories.end() && it->second->directory == directory) entry = it->second;
            }
        }
        if (entry) {
            std::lock_guard<std::mutex> lock(entry->mutex);
            if (entry->tf && entry->k1Plus1 == parameters.k1Plus1 && entry->base == parameters.base &&
                entry->perLength == parameters.perLength && entry->shortest == shortest) return entry->tf;
        }
        const std::vector<NindTermIndex::Block> &blocks = directory->categories[0].blocks;
        std::shared_ptr<std::vector<double> > tf(new std::vector<double>(blocks.size()));
        for (size_t i = 0; i < blocks.size(); i++) {
            const NindTermIndex::Block &block = blocks[i];
            double bound = 0.0;
            if (block.impactsNb != 0) {
                //the block's competitive (frequency, length) pairs: its exact largest contribution
                for (unsigned int j = 0; j < block.impactsNb; j++)
                    bound = std::max(bound, contribution(1.0, block.impacts[j].first,
                                                         std::max((double)block.impacts[j].second, shortest), parameters));
            }
            else bound = contribution(1.0, block.maxFrequency, std::max((double)block.minLength, shortest), parameters);
            (*tf)[i] = bound;
        }
        if (entry) {
            std::lock_guard<std::mutex> lock(entry->mutex);
            entry->tf = tf;
            entry->k1Plus1 = parameters.k1Plus1;
            entry->base = parameters.base;
            entry->perLength = parameters.perLength;
            entry->shortest = shortest;
        }
        return tf;
    }
    mutable std::shared_timed_mutex m_cacheMutex;
    mutable std::unordered_map<unsigned int, std::shared_ptr<CachedTerm> > m_directories;
    mutable const NindTermIndex *m_cachedIndex = 0;
    mutable size_t m_cacheBytes = 0;

    uint32_t positionOf(const uint64_t docId) const {
        //(an id below m_first wraps to a large offset)
        const uint64_t offset = docId - m_first;
        //ids in order, without gap: the position is the offset
        if (m_identity) return offset < m_docsNb ? (uint32_t)offset : NO_POSITION;
        if (!m_dense.empty() || m_sparse.empty())
            return offset < m_dense.size() ? m_dense[offset] : NO_POSITION;
        const auto it = std::lower_bound(m_sparse.begin(), m_sparse.end(), docId,
                                         [](const std::pair<uint64_t, uint32_t> &a, const uint64_t id)
                                         { return a.first < id; });
        return (it != m_sparse.end() && it->first == docId) ? it->second : NO_POSITION;
    }

    uint64_t docIdOf(const uint32_t position) const { return m_identity ? m_first + position : m_docIds[position]; }

    std::vector<uint64_t> m_docIds;     //position -> document id (m_identity: none)
    uint64_t m_first;                   //the smallest document id
    std::vector<uint32_t> m_lengths;    //position -> length (or, all under 2^16:)
    std::vector<uint16_t> m_shortLengths;
    const uint32_t *m_wide = 0;         //the lengths read: m_lengths', a mapped file's, or
    const uint16_t *m_narrow = 0;       //m_shortLengths'
    std::shared_ptr<const NindMappedFile> m_mapped;     //the file m_wide is in, if mapped
    static const size_t PREFETCH_DISTANCE = 8;
    void prefetchLength(const NindDocId docId) const {
        const uint32_t position = positionOf(docId);
        if (position == NO_POSITION) return;
        if (m_narrow) __builtin_prefetch(m_narrow + position);
        else __builtin_prefetch(m_wide + position);
    }
    double lengthOf(const uint32_t position) const {
        return m_narrow ? (double)m_narrow[position] : (double)m_wide[position];
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
NindBm25Scorer::NindBm25Scorer(const NindDocId firstId,
                               const std::vector<uint32_t> &lengths,
                               const double avgDocLength):
    m_impl(new Impl(firstId, lengths, avgDocLength))
{
}
////////////////////////////////////////////////////////////
NindBm25Scorer::NindBm25Scorer(const NindDocId firstId,
                               const std::shared_ptr<const NindMappedFile> &file,
                               const uint32_t *lengths,
                               const size_t count,
                               const double avgDocLength):
    m_impl(new Impl(firstId, file, lengths, count, avgDocLength))
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
                                                                  const bool prune,
                                                                  const unsigned int threads) const
{
    return m_impl->search(termIndex, termIds, k1, b, topK, prune, threads, 0);
}
////////////////////////////////////////////////////////////
std::vector<std::pair<NindDocId, double> > NindBm25Scorer::search(const NindTermIndex &termIndex,
                                                                  const std::vector<unsigned int> &termIds,
                                                                  const double k1,
                                                                  const double b,
                                                                  const long topK,
                                                                  const bool prune,
                                                                  const unsigned int threads,
                                                                  const SearchContext &context) const
{
    return m_impl->search(termIndex, termIds, k1, b, topK, prune, threads, &context);
}
////////////////////////////////////////////////////////////
uint64_t NindBm25Scorer::documentFrequency(const NindTermIndex &termIndex,
                                           const unsigned int termId,
                                           NindReadBuffer &buffer) const
{
    return m_impl->documentFrequency(termIndex, termId, buffer);
}
////////////////////////////////////////////////////////////
size_t NindBm25Scorer::position(const NindDocId docId) const
{
    return m_impl->positionOfDocument(docId);
}
////////////////////////////////////////////////////////////
NindDocId NindBm25Scorer::docIdAt(const size_t position) const
{
    return m_impl->docIdAt(position);
}
////////////////////////////////////////////////////////////
uint32_t NindBm25Scorer::lengthAt(const size_t position) const
{
    return m_impl->lengthAt(position);
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
