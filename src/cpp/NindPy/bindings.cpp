//
// Python bindings (pybind11) for the core nind C++ index stack.
//
// Exposes the writer/reader classes that have no Python-side equivalent writer
// (NindLexiconIndex, NindTermIndex, NindLocalIndex, NindRetrolexicon) plus the
// in-memory NindLexicon, as module `nind._native`.
//
// Author: Claude <noreply@anthropic.com>
//
// Copyright: 2014-2017 LATEJCON. See LICENCE.md file that comes with this distribution
// This file is part of NIND (as "nouvelle indexation").
// NIND is free software: you can redistribute it and/or modify it under the terms of the
// GNU Less General Public License (LGPL) as published by the Free Software Foundation,
// (see <http://www.gnu.org/licenses/>), either version 3 of the License, or any later version.
////////////////////////////////////////////////////////////
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/operators.h>
#include <sstream>
#include <queue>
#include <map>
#include <functional>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <limits>
#include "NindBasics/NindPadFile.h"
#include "NindRetrolexicon/NindRetrolexicon.h"
#include "NindLexicon/NindLexicon.h"
#include "NindIndex/NindLexiconIndex.h"
#include "NindIndex/NindTermIndex.h"
#include "NindIndex/NindLocalIndex.h"
#include "NindIndex/NindIndexMerge.h"
#include "NindIndex/NindIndexVerify.h"
#include "NindIndex/NindTokenizer.h"
#include "NindIndex/NindDocNames.h"
#include <unordered_map>
#include <cmath>
#include <mutex>
#include <thread>
#include <memory>
#include "NindExceptions.h"
////////////////////////////////////////////////////////////
namespace py = pybind11;
using namespace latecon::nindex;
////////////////////////////////////////////////////////////
namespace {
// 1-D contiguous buffer of 32-bit unsigned ints (array.array('I'), numpy
// uint32, ...), as used by the *_arrays bulk writers below: they build the
// C++ definition straight from flat buffers instead of going through one
// Python Term/Localisation/Document object per entry.
struct UIntView {
    py::buffer_info info;
    const uint32_t *data;
    size_t size;
};
UIntView uintView(const py::buffer &buffer, const char *name)
{
    py::buffer_info info = buffer.request();
    const std::string &format = info.format;
    const bool isUInt32 = info.itemsize == 4 && !format.empty() &&
                          (format.back() == 'I' || format.back() == 'L') &&
                          (format.size() == 1 || format[0] == '<' || format[0] == '=' || format[0] == '@');
    if (!isUInt32)
        throw py::value_error(std::string(name) + ": expected a buffer of 32-bit unsigned ints (e.g. array('I')), got format '" + format + "'");
    if (info.ndim != 1 || (info.shape[0] > 1 && info.strides[0] != 4))
        throw py::value_error(std::string(name) + ": expected a 1-D contiguous buffer");
    const uint32_t *data = static_cast<const uint32_t *>(info.ptr);
    const size_t size = static_cast<size_t>(info.shape[0]);
    return UIntView{std::move(info), data, size};
}
// document ids from Python: a buffer of 64-bit (array('Q')) or 32-bit (array('I')) unsigned ints,
// copied as 64-bit ids (NindDocId)
struct DocIdsView {
    std::vector<uint64_t> values;
    const uint64_t *data;
    size_t size;
};
DocIdsView docIdsView(const py::buffer &buffer, const char *name)
{
    py::buffer_info info = buffer.request();
    const std::string &format = info.format;
    const bool native = format.size() == 1 || format[0] == '<' || format[0] == '=' || format[0] == '@';
    const char code = format.empty() ? 0 : format.back();
    const bool isUInt32 = info.itemsize == 4 && native && (code == 'I' || code == 'L');
    const bool isUInt64 = info.itemsize == 8 && native && (code == 'Q' || code == 'L' || code == 'K');
    if (!isUInt32 && !isUInt64)
        throw py::value_error(std::string(name) + ": expected a buffer of 64-bit or 32-bit unsigned ints (e.g. array('Q')), got format '" + format + "'");
    if (info.ndim != 1 || (info.shape[0] > 1 && info.strides[0] != info.itemsize))
        throw py::value_error(std::string(name) + ": expected a 1-D contiguous buffer");
    DocIdsView view;
    const size_t size = static_cast<size_t>(info.shape[0]);
    view.values.resize(size);
    if (isUInt64) {
        const uint64_t *data = static_cast<const uint64_t *>(info.ptr);
        std::copy(data, data + size, view.values.begin());
    }
    else {
        const uint32_t *data = static_cast<const uint32_t *>(info.ptr);
        std::copy(data, data + size, view.values.begin());
    }
    view.data = view.values.data();
    view.size = size;
    return view;
}
// validates a batch's end offsets: ascending, the last one == itemsNb
void checkEnds(const UIntView &ends, const size_t itemsNb, const char *name)
{
    uint32_t prev = 0;
    for (size_t i = 0; i < ends.size; i++) {
        if (ends.data[i] < prev || ends.data[i] > itemsNb)
            throw py::value_error(std::string(name) + ": end offsets must be ascending and within the data");
        prev = ends.data[i];
    }
    if (prev != itemsNb)
        throw py::value_error(std::string(name) + ": last end offset must equal the data length");
}
// one term definition from its [begin, end) slice of (docIds, freqs)
std::list<NindTermIndex::TermCG> buildTermDef(const uint64_t *docIds, const uint32_t *freqs,
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
            throw py::value_error("doc_ids: duplicate document id " + std::to_string(postings[i].first));
        total += postings[i].second;
        termCG.documents.push_back(NindTermIndex::Document(postings[i].first, postings[i].second));
    }
    if (total > std::numeric_limits<unsigned int>::max())
        throw py::value_error("frequencies: total term frequency overflows 32 bits");
    termCG.frequency = static_cast<unsigned int>(total);
    termDef.push_back(std::move(termCG));
    return termDef;
}
// one local definition from its [begin, end) slice of occurrences: one Term
// per term id (ascending), localisations kept in occurrence order.
// <nbreLocalisations> is one byte, so a term occurring more than 255 times
// is split into several consecutive Terms with the same id (readers sum them)
std::list<NindLocalIndex::Term> buildLocalDef(const uint32_t *termIds, const uint32_t *positions,
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
}
namespace {
py::bytes uintBytes(const std::vector<uint32_t> &values)
{
    return py::bytes(reinterpret_cast<const char *>(values.data()), values.size() * sizeof(uint32_t));
}
py::bytes uint64Bytes(const std::vector<uint64_t> &values)
{
    return py::bytes(reinterpret_cast<const char *>(values.data()), values.size() * sizeof(uint64_t));
}

// Builds the term and local index files of a corpus from its documents, given as
// chunks of term ids (in document order): see the IndexBuilder binding below.
class IndexBuilder {
public:
    IndexBuilder(): m_docIds(), m_docEnds(), m_tokens(), m_maxTermId(0) {}

    // documents firstDocId, firstDocId + 1... : document i is localIds[ends[i-1]:ends[i]],
    // local id x being the term idMap[x]
    void addDocuments(const uint64_t firstDocId, const UIntView &ends, const UIntView &localIds,
                      const UIntView &idMap)
    {
        checkEnds(ends, localIds.size, "ends");
        if (ends.size != 0 && !m_docIds.empty() && firstDocId <= m_docIds.back())
            throw py::value_error("documents must be added in increasing id order");
        if (ends.size != 0 && firstDocId > std::numeric_limits<uint64_t>::max() - (ends.size - 1))
            throw py::value_error("document id out of range");
        for (size_t i = 0; i < idMap.size; i++) {
            if (idMap.data[i] == 0) throw py::value_error("id_map: 0 is not a valid term id");
        }
        for (size_t i = 0; i < localIds.size; i++) {
            if (localIds.data[i] >= idMap.size) throw py::value_error("term_ids: local id beyond id_map");
        }
        py::gil_scoped_release release;
        const size_t base = m_tokens.size();
        m_tokens.reserve(base + localIds.size);
        for (size_t i = 0; i < localIds.size; i++) {
            const uint32_t termId = idMap.data[localIds.data[i]];
            m_tokens.push_back(termId);
            if (termId > m_maxTermId) m_maxTermId = termId;
        }
        for (size_t i = 0; i < ends.size; i++) {
            m_docIds.push_back(firstDocId + (uint64_t)i);
            m_docEnds.push_back(base + ends.data[i]);
        }
    }

    // writes every term's postings, terms in increasing id order (GIL released)
    void writeTermIndex(NindTermIndex &termIndex, const NindPadFile::Identification &identification,
                        const unsigned char cg) const
    {
        py::gil_scoped_release release;
        const std::list<unsigned int> noSpecifics;
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
        //3) the term definitions
        for (uint32_t t = 1; t <= m_maxTermId; t++) {
            if (postingsNb[t] == postingsNb[t + 1]) continue;
            const std::list<NindTermIndex::TermCG> termDef =
                buildTermDef(postingDocs.data(), postingFreqs.data(), postingsNb[t], postingsNb[t + 1], cg);
            termIndex.setTermDef(t, termDef, identification, noSpecifics);
        }
    }

    // writes every document: its term occurrences at positions 0, 1... (length 1) (GIL released)
    void writeLocalIndex(NindLocalIndex &localIndex, const NindPadFile::Identification &identification,
                         const unsigned char cg) const
    {
        py::gil_scoped_release release;
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
                const std::list<NindLocalIndex::Term> localDef =
                    buildLocalDef(m_tokens.data() + begin, positions.data(), lengths.data(), 0, length, cg);
                localIndex.setLocalDef(m_docIds[d], localDef, identification);
            }
            begin = m_docEnds[d];
        }
    }

    // (document ids as uint64 bytes, lengths in tokens as uint32 bytes)
    py::tuple docLengths() const
    {
        std::vector<uint32_t> lengths(m_docIds.size());
        size_t begin = 0;
        for (size_t d = 0; d < m_docIds.size(); d++) {
            lengths[d] = (uint32_t)(m_docEnds[d] - begin);
            begin = m_docEnds[d];
        }
        return py::make_tuple(uint64Bytes(m_docIds), uintBytes(lengths));
    }

    size_t docsNb() const { return m_docIds.size(); }
    size_t tokensNb() const { return m_tokens.size(); }
    uint32_t maxTermId() const { return m_maxTermId; }

private:
    std::vector<uint64_t> m_docIds;     //in increasing order (external document ids, 64 bits)
    std::vector<size_t> m_docEnds;      //document d is m_tokens[m_docEnds[d-1]:m_docEnds[d]]
    std::vector<uint32_t> m_tokens;     //term ids
    uint32_t m_maxTermId;
};

// The thread-safe (const) reads of readers each use a buffer of their own (see NindFile::readAt):
// a local one for a single lookup, the pooled scratch's for a search. Not a thread_local: in a
// dynamically loaded module, every access to one may cost a __tls_get_addr call, and with
// link-time optimization the compiler recomputed its address in the decoding loops (11% of a
// search's instructions).

// Okapi BM25 over the postings of a term index: see the Bm25Scorer binding below.
// Flat arrays only: documents are numbered by their position in the corpus order (dense table
// from document id, or binary search when ids are too sparse for one), scores are accumulated
// in an array indexed by position (reset only where touched), postings are read with
// NindTermIndex::getTermPostings into reused vectors - no allocation nor hashing per posting.
const uint32_t NO_POSITION = 0xFFFFFFFFu;       // a document id without position: not in the corpus
const uint64_t DENSE_SLACK = 1u << 20;          // a dense table up to 2 N + 1M entries
class Bm25Scorer {
public:
    // the documents of the corpus, in the order that breaks score ties
    // document ids: 64 bits (NindDocId); positions: 32 bits (at most 4.29G documents per index)
    Bm25Scorer(const DocIdsView &docIds, const UIntView &lengths, const double avgDocLength):
        m_docIds(), m_lengths(), m_dense(), m_sparse(), m_docsNb(docIds.size), m_avgDocLength(avgDocLength),
        m_poolMutex(), m_pool()
    {
        if (docIds.size != lengths.size) throw py::value_error("doc_ids and lengths must have the same length");
        if (docIds.size >= NO_POSITION) throw py::value_error("too many documents");
        uint64_t maxId = 0;
        for (size_t i = 0; i < docIds.size; i++) maxId = std::max(maxId, docIds.data[i]);
        // a document id given twice: its last position counts (and only it is scored)
        if (docIds.size != 0 && maxId < 2 * (uint64_t)docIds.size + DENSE_SLACK) {
            m_dense.assign((size_t)maxId + 1, NO_POSITION);
            for (size_t i = 0; i < docIds.size; i++) m_dense[docIds.data[i]] = (uint32_t)i;
        }
        else {
            m_sparse.reserve(docIds.size);
            for (size_t i = 0; i < docIds.size; i++) m_sparse.push_back(std::make_pair(docIds.data[i], (uint32_t)i));
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
        m_docIds.assign(docIds.data, docIds.data + docIds.size);
        m_lengths.assign(lengths.data, lengths.data + lengths.size);
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
        py::gil_scoped_release release;
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
        const Bm25Scorer &scorer;
        std::unique_ptr<Scratch> scratch;
        bool clean;
        explicit ScratchLease(const Bm25Scorer &s): scorer(s), scratch(), clean(false) {
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
}
////////////////////////////////////////////////////////////
PYBIND11_MODULE(_native, m) {
    m.doc() = "Native (C++) implementation of the nind core index stack";

    py::register_exception<FileException>(m, "NindError", PyExc_RuntimeError);

    py::class_<NindPadFile::Identification>(m, "Identification")
        .def(py::init<>())
        .def(py::init<unsigned int, unsigned int>(),
             py::arg("lexicon_words_nb"), py::arg("lexicon_time"))
        .def_readwrite("lexicon_words_nb", &NindPadFile::Identification::lexiconWordsNb)
        .def_readwrite("lexicon_time", &NindPadFile::Identification::lexiconTime)
        .def(py::self == py::self)
        .def(py::self != py::self)
        .def("__repr__", [](const NindPadFile::Identification &id) {
            return "Identification(lexicon_words_nb=" + std::to_string(id.lexiconWordsNb) +
                   ", lexicon_time=" + std::to_string(id.lexiconTime) + ")";
        });

    // ---- Shared diagnostics structures (NindPadFile / NindIndex) --------
    py::class_<NindPadFile::Repartition>(m, "Repartition")
        .def(py::init<>())
        .def_readwrite("count", &NindPadFile::Repartition::count)
        .def_readwrite("min_value", &NindPadFile::Repartition::minValue)
        .def_readwrite("max_value", &NindPadFile::Repartition::maxValue)
        .def_readwrite("sum", &NindPadFile::Repartition::sum)
        .def_readwrite("mean", &NindPadFile::Repartition::mean)
        .def_readwrite("stddev", &NindPadFile::Repartition::stddev);

    py::class_<NindPadFile::HoleStats>(m, "HoleStats")
        .def(py::init<>())
        .def_readwrite("holes_count", &NindPadFile::HoleStats::holesCount)
        .def_readwrite("holes_size", &NindPadFile::HoleStats::holesSize)
        .def_readwrite("hole_size_histogram", &NindPadFile::HoleStats::holeSizeHistogram)
        .def_readwrite("occupied_size_histogram", &NindPadFile::HoleStats::occupiedSizeHistogram);

    py::class_<NindPadFile::BlockStats>(m, "BlockStats")
        .def(py::init<>())
        .def_readwrite("block_addr", &NindPadFile::BlockStats::blockAddr)
        .def_readwrite("block_num", &NindPadFile::BlockStats::blockNum)
        .def_readwrite("entries_used", &NindPadFile::BlockStats::entriesUsed)
        .def_readwrite("entries_total", &NindPadFile::BlockStats::entriesTotal)
        .def_readwrite("en_vrac_addr", &NindPadFile::BlockStats::enVracAddr)
        .def_readwrite("en_vrac_size", &NindPadFile::BlockStats::enVracSize);

    py::class_<NindPadFile::PadFileStats>(m, "PadFileStats")
        .def(py::init<>())
        .def_readwrite("data_entry_size", &NindPadFile::PadFileStats::dataEntrySize)
        .def_readwrite("specifics_size", &NindPadFile::PadFileStats::specificsSize)
        .def_readwrite("blocks", &NindPadFile::PadFileStats::blocks)
        .def_readwrite("index_total_size", &NindPadFile::PadFileStats::indexTotalSize)
        .def_readwrite("en_vrac_total_size", &NindPadFile::PadFileStats::enVracTotalSize)
        .def_readwrite("identification", &NindPadFile::PadFileStats::identification)
        .def_readwrite("file_size", &NindPadFile::PadFileStats::fileSize);

    py::class_<NindIndex::IndexStats>(m, "IndexStats")
        .def(py::init<>())
        .def_readwrite("max_ident", &NindIndex::IndexStats::maxIdent)
        .def_readwrite("used_count", &NindIndex::IndexStats::usedCount)
        .def_readwrite("holes", &NindIndex::IndexStats::holes)
        .def_readwrite("definition_sizes", &NindIndex::IndexStats::definitionSizes);

    // ---- NindLexiconIndex -------------------------------------------------
    py::class_<NindLexiconIndex>(m, "NindLexiconIndex")
        .def(py::init<const std::string &, bool, bool, unsigned int, unsigned int>(),
             py::arg("file_name_extensionless"), py::arg("is_writer"),
             py::arg("with_retrolexicon") = false,
             py::arg("indirection_bloc_size") = 0,
             py::arg("retro_indirection_bloc_size") = 0)
        .def("add_word", &NindLexiconIndex::addWord, py::arg("components"))
        // many words at once (same idents as add_word on each in turn, each hash bucket
        // read and written once): words is a list of component lists
        .def("add_words", [](NindLexiconIndex &self, const std::vector<std::list<std::string> > &words) {
            std::vector<unsigned int> idents;
            py::gil_scoped_release release;
            self.addWords(words, idents);
            return idents;
        }, py::arg("words"))
        // the same for simple words: words is a list of strings
        .def("add_simple_words", [](NindLexiconIndex &self, const std::vector<std::string> &words) {
            std::vector<unsigned int> idents;
            py::gil_scoped_release release;
            std::vector<std::list<std::string> > lists(words.size());
            for (size_t i = 0; i < words.size(); i++) lists[i].push_back(words[i]);
            self.addWords(lists, idents);
            return idents;
        }, py::arg("words"))
        // bulk mode (see NindIndex::beginBulk): only for a file no reader opens before
        // end_bulk(), which must then be called (the destructor does it too, ignoring errors)
        .def("begin_bulk", &NindIndex::beginBulk, py::call_guard<py::gil_scoped_release>())
        .def("end_bulk", &NindIndex::endBulk, py::call_guard<py::gil_scoped_release>())
        .def("is_bulk", &NindIndex::isBulk)
        .def("is_format_v2", &NindIndex::isFormatV2)
        .def("get_max_ident", &NindPadFile::getMaxIdentAt)   //the indirections' capacity
        .def("get_word_id", [](NindLexiconIndex &self, const std::list<std::string> &components) {
            // a reader: thread-safe const path, GIL released; a writer: the sequential API
            if (self.isWriter()) return self.getWordId(components);
            py::gil_scoped_release release;
            NindReadBuffer buffer;
            return self.getWordId(components, buffer);
        }, py::arg("components"))
        .def("get_identification", &NindLexiconIndex::getIdentification)
        .def("get_modulo", &NindLexiconIndex::getModulo)
        .def("get_components", [](NindLexiconIndex &self, unsigned int ident) -> py::object {
            std::list<std::string> components;
            if (!self.getComponents(ident, components)) return py::none();
            return py::cast(components);
        }, py::arg("ident"))
        .def("get_retrolexicon_file_name", &NindLexiconIndex::getRetrolexiconFileName)
        // every word as (ident, prefix_ident, last_component), in increasing ident order:
        // prefix_ident is 0 for a simple word, else the ident of the compound word's prefix
        .def("get_entries", [](NindLexiconIndex &self) {
            std::vector<NindLexiconIndex::Entry> entries;
            self.getEntries(entries);
            py::list result;
            for (size_t i = 0; i < entries.size(); i++)
                result.append(py::make_tuple(entries[i].ident, entries[i].prefixIdent, entries[i].lastComponent));
            return result;
        })
        .def("get_file_name", &NindPadFile::getFileName)
        .def("analyse_pad_file", &NindPadFile::analysePadFile)
        .def("analyse_index", &NindIndex::analyseIndex);

    // ---- NindTermIndex ------------------------------------------------
    py::class_<NindTermIndex::Document>(m, "Document")
        .def(py::init<>())
        .def(py::init<NindDocId, unsigned int>(), py::arg("ident"), py::arg("frequency"))
        .def_readwrite("ident", &NindTermIndex::Document::ident)
        .def_readwrite("frequency", &NindTermIndex::Document::frequency)
        .def("__repr__", [](const NindTermIndex::Document &d) {
            return "Document(ident=" + std::to_string(d.ident) +
                   ", frequency=" + std::to_string(d.frequency) + ")";
        });

    py::class_<NindTermIndex::TermCG>(m, "TermCG")
        .def(py::init<>())
        .def(py::init<unsigned char, unsigned int>(), py::arg("cg"), py::arg("frequency"))
        .def_readwrite("cg", &NindTermIndex::TermCG::cg)
        .def_readwrite("frequency", &NindTermIndex::TermCG::frequency)
        .def_readwrite("documents", &NindTermIndex::TermCG::documents);

    py::class_<NindTermIndex>(m, "NindTermIndex")
        .def(py::init<const std::string &, bool, const NindPadFile::Identification &, unsigned int, unsigned int>(),
             py::arg("file_name_extensionless"), py::arg("is_writer"),
             py::arg("lexicon_identification"), py::arg("specifics_number"),
             py::arg("indirection_bloc_size") = 0)
        .def("get_term_def", [](NindTermIndex &self, unsigned int ident) -> py::object {
            std::list<NindTermIndex::TermCG> termDef;
            bool found;
            if (self.isWriter()) found = self.getTermDef(ident, termDef);
            else {
                py::gil_scoped_release release;
                NindReadBuffer buffer;
                found = self.getTermDef(ident, termDef, buffer);
            }
            if (!found) return py::none();
            return py::cast(termDef);
        }, py::arg("ident"))
        .def("get_specific_words", [](NindTermIndex &self) {
            std::list<unsigned int> specifics;
            self.getSpecificWords(specifics);
            return specifics;
        })
        // bulk mode (see NindIndex::beginBulk): only for a file no reader opens before
        // end_bulk(), which must then be called (the destructor does it too, ignoring errors)
        .def("begin_bulk", &NindIndex::beginBulk, py::call_guard<py::gil_scoped_release>())
        .def("end_bulk", &NindIndex::endBulk, py::call_guard<py::gil_scoped_release>())
        .def("is_bulk", &NindIndex::isBulk)
        .def("is_format_v2", &NindIndex::isFormatV2)
        .def("get_max_ident", &NindPadFile::getMaxIdentAt)   //the indirections' capacity
        .def("set_term_def", &NindTermIndex::setTermDef,
             py::arg("ident"), py::arg("term_def"), py::arg("file_identification"), py::arg("specifics"),
             py::call_guard<py::gil_scoped_release>())
        // Bulk form of set_term_def for a single-category term: documents
        // given as two parallel uint32 buffers; sorted by document id here
        // if they aren't already (duplicates are rejected), frequency is
        // their sum. Releases the GIL while writing, so a writer must not
        // be shared between Python threads.
        .def("set_term_def_arrays", [](NindTermIndex &self, unsigned int ident,
                                       const py::buffer &docIds, const py::buffer &frequencies,
                                       const NindPadFile::Identification &fileIdentification,
                                       unsigned char cg, const std::list<unsigned int> &specifics) {
            const DocIdsView ids = docIdsView(docIds, "doc_ids");
            const UIntView freqs = uintView(frequencies, "frequencies");
            if (ids.size != freqs.size)
                throw py::value_error("doc_ids and frequencies must have the same length");
            const std::list<NindTermIndex::TermCG> termDef = buildTermDef(ids.data, freqs.data, 0, ids.size, cg);
            py::gil_scoped_release release;
            self.setTermDef(ident, termDef, fileIdentification, specifics);
        }, py::arg("ident"), py::arg("doc_ids"), py::arg("frequencies"), py::arg("file_identification"),
           py::arg("cg") = 0, py::arg("specifics") = std::list<unsigned int>())
        // Whole-batch form of set_term_def_arrays: term i is idents[i], its
        // documents are doc_ids/frequencies[ends[i-1]:ends[i]]. Terms are
        // written in the given order; the batch is validated up front, so a
        // ValueError means nothing was written.
        .def("set_term_defs_arrays", [](NindTermIndex &self, const py::buffer &idents,
                                        const py::buffer &ends, const py::buffer &docIds,
                                        const py::buffer &frequencies,
                                        const NindPadFile::Identification &fileIdentification,
                                        unsigned char cg, const std::list<unsigned int> &specifics) {
            const UIntView terms = uintView(idents, "idents");
            const UIntView termEnds = uintView(ends, "ends");
            const DocIdsView ids = docIdsView(docIds, "doc_ids");
            const UIntView freqs = uintView(frequencies, "frequencies");
            if (ids.size != freqs.size)
                throw py::value_error("doc_ids and frequencies must have the same length");
            if (terms.size != termEnds.size)
                throw py::value_error("idents and ends must have the same length");
            checkEnds(termEnds, ids.size, "ends");
            std::vector<std::list<NindTermIndex::TermCG> > termDefs(terms.size);
            size_t begin = 0;
            for (size_t i = 0; i < terms.size; i++) {
                termDefs[i] = buildTermDef(ids.data, freqs.data, begin, termEnds.data[i], cg);
                begin = termEnds.data[i];
            }
            py::gil_scoped_release release;
            for (size_t i = 0; i < terms.size; i++)
                self.setTermDef(terms.data[i], termDefs[i], fileIdentification, specifics);
        }, py::arg("idents"), py::arg("ends"), py::arg("doc_ids"), py::arg("frequencies"),
           py::arg("file_identification"), py::arg("cg") = 0,
           py::arg("specifics") = std::list<unsigned int>())
        .def("get_file_name", &NindPadFile::getFileName)
        .def("analyse_pad_file", &NindPadFile::analysePadFile)
        .def("analyse_index", &NindIndex::analyseIndex);

    // ---- NindLocalIndex -----------------------------------------------
    py::class_<NindLocalIndex::Localisation>(m, "Localisation")
        .def(py::init<>())
        .def(py::init<unsigned int, unsigned int>(), py::arg("position"), py::arg("length"))
        .def_readwrite("position", &NindLocalIndex::Localisation::position)
        .def_readwrite("length", &NindLocalIndex::Localisation::length);

    py::class_<NindLocalIndex::Term>(m, "Term")
        .def(py::init<>())
        .def(py::init<unsigned int, unsigned char>(), py::arg("term"), py::arg("cg"))
        .def_readwrite("term", &NindLocalIndex::Term::term)
        .def_readwrite("cg", &NindLocalIndex::Term::cg)
        .def_readwrite("localisation", &NindLocalIndex::Term::localisation);

    py::class_<NindLocalIndex>(m, "NindLocalIndex")
        .def(py::init<const std::string &, bool, const NindPadFile::Identification &, unsigned int>(),
             py::arg("file_name_extensionless"), py::arg("is_writer"),
             py::arg("lexicon_identification"), py::arg("indirection_bloc_size") = 0)
        .def("get_local_def", [](NindLocalIndex &self, NindDocId ident) -> py::object {
            std::list<NindLocalIndex::Term> localDef;
            bool found;
            if (self.isWriter()) found = self.getLocalDef(ident, localDef);
            else {
                py::gil_scoped_release release;
                NindReadBuffer buffer;
                found = self.getLocalDef(ident, localDef, buffer);
            }
            if (!found) return py::none();
            return py::cast(localDef);
        }, py::arg("ident"))
        .def("get_local_length", [](NindLocalIndex &self, NindDocId ident) -> py::object {
            unsigned int length = 0;
            if (!self.getLocalLength(ident, length)) return py::none();
            return py::cast(length);
        }, py::arg("ident"))
        .def("get_term_idents", [](NindLocalIndex &self, NindDocId ident) -> py::object {
            std::set<unsigned int> termIdents;
            if (!self.getTermIdents(ident, termIdents)) return py::none();
            return py::cast(termIdents);
        }, py::arg("ident"))
        // bulk mode (see NindIndex::beginBulk): only for a file no reader opens before
        // end_bulk(), which must then be called (the destructor does it too, ignoring errors)
        .def("begin_bulk", &NindIndex::beginBulk, py::call_guard<py::gil_scoped_release>())
        .def("end_bulk", &NindIndex::endBulk, py::call_guard<py::gil_scoped_release>())
        .def("is_bulk", &NindIndex::isBulk)
        .def("is_format_v2", &NindIndex::isFormatV2)
        .def("get_max_ident", &NindPadFile::getMaxIdentAt)   //the indirections' capacity
        .def("set_local_def", &NindLocalIndex::setLocalDef,
             py::arg("ident"), py::arg("local_def"), py::arg("file_identification"),
             py::call_guard<py::gil_scoped_release>())
        // Bulk form of set_local_def: one entry per occurrence, as three
        // parallel uint32 buffers in document order; grouped here into one
        // Term per term id (ascending, all with category cg), each keeping
        // its localisations in occurrence order. Releases the GIL while
        // writing, so a writer must not be shared between Python threads.
        .def("set_local_def_arrays", [](NindLocalIndex &self, NindDocId ident,
                                        const py::buffer &termIds, const py::buffer &positions,
                                        const py::buffer &lengths,
                                        const NindPadFile::Identification &fileIdentification,
                                        unsigned char cg) {
            const UIntView terms = uintView(termIds, "term_ids");
            const UIntView pos = uintView(positions, "positions");
            const UIntView lens = uintView(lengths, "lengths");
            if (terms.size != pos.size || terms.size != lens.size)
                throw py::value_error("term_ids, positions and lengths must have the same length");
            const std::list<NindLocalIndex::Term> localDef =
                buildLocalDef(terms.data, pos.data, lens.data, 0, terms.size, cg);
            py::gil_scoped_release release;
            self.setLocalDef(ident, localDef, fileIdentification);
        }, py::arg("ident"), py::arg("term_ids"), py::arg("positions"), py::arg("lengths"),
           py::arg("file_identification"), py::arg("cg") = 0)
        // Whole-batch form of set_local_def_arrays: document i is idents[i],
        // its occurrences are term_ids/positions/lengths[ends[i-1]:ends[i]].
        // Documents are written in the given order, built one at a time with
        // the GIL released; the offsets are validated up front, so a
        // ValueError means nothing was written.
        .def("set_local_defs_arrays", [](NindLocalIndex &self, const py::buffer &idents,
                                         const py::buffer &ends, const py::buffer &termIds,
                                         const py::buffer &positions, const py::buffer &lengths,
                                         const NindPadFile::Identification &fileIdentification,
                                         unsigned char cg) {
            const DocIdsView docs = docIdsView(idents, "idents");
            const UIntView docEnds = uintView(ends, "ends");
            const UIntView terms = uintView(termIds, "term_ids");
            const UIntView pos = uintView(positions, "positions");
            const UIntView lens = uintView(lengths, "lengths");
            if (terms.size != pos.size || terms.size != lens.size)
                throw py::value_error("term_ids, positions and lengths must have the same length");
            if (docs.size != docEnds.size)
                throw py::value_error("idents and ends must have the same length");
            checkEnds(docEnds, terms.size, "ends");
            py::gil_scoped_release release;
            size_t begin = 0;
            for (size_t i = 0; i < docs.size; i++) {
                const std::list<NindLocalIndex::Term> localDef =
                    buildLocalDef(terms.data, pos.data, lens.data, begin, docEnds.data[i], cg);
                self.setLocalDef(docs.data[i], localDef, fileIdentification);
                begin = docEnds.data[i];
            }
        }, py::arg("idents"), py::arg("ends"), py::arg("term_ids"), py::arg("positions"),
           py::arg("lengths"), py::arg("file_identification"), py::arg("cg") = 0)
        .def("get_doc_count", &NindLocalIndex::getDocCount)
        .def("get_doc_ident_pairs", [](NindLocalIndex &self) {
            std::vector<std::pair<NindDocId, unsigned int> > docIdents;
            self.getDocIdentPairs(docIdents);
            return docIdents;
        })
        .def("get_doc_idents", [](NindLocalIndex &self) {
            std::vector<NindDocId> docIdents;
            self.getDocIdents(docIdents);
            return docIdents;
        })
        .def("get_file_name", &NindPadFile::getFileName)
        .def("analyse_pad_file", &NindPadFile::analysePadFile)
        .def("analyse_index", &NindIndex::analyseIndex);

    // ---- NindIndexMerge -------------------------------------------------
    py::class_<NindIndexMerge::Stats>(m, "MergeStats")
        .def_readonly("shards_nb", &NindIndexMerge::Stats::shardsNb)
        .def_readonly("words_nb", &NindIndexMerge::Stats::wordsNb)
        .def_readonly("terms_nb", &NindIndexMerge::Stats::termsNb)
        .def_readonly("docs_nb", &NindIndexMerge::Stats::docsNb)
        .def_readonly("identification", &NindIndexMerge::Stats::identification)
        .def("__repr__", [](const NindIndexMerge::Stats &stats) {
            return "MergeStats(shards_nb=" + std::to_string(stats.shardsNb) + ", words_nb=" + std::to_string(stats.wordsNb) +
                   ", terms_nb=" + std::to_string(stats.termsNb) + ", docs_nb=" + std::to_string(stats.docsNb) + ")";
        });

    // Offline merge of index shards (disjoint sets of documents, no term specifics) into
    // a new index. Indirection block sizes of 0 are sized to fit. Releases the GIL.
    m.def("merge_indexes", [](const std::vector<std::string> &shards, const std::string &target,
                              const bool withRetrolexicon, const unsigned int lexiconIndirectionBlocSize,
                              const unsigned int retroIndirectionBlocSize, const unsigned int termIndirectionBlocSize,
                              const unsigned int localIndirectionBlocSize) {
        NindIndexMerge::Options options;
        options.withRetrolexicon = withRetrolexicon;
        options.lexiconIndirectionBlocSize = lexiconIndirectionBlocSize;
        options.retroIndirectionBlocSize = retroIndirectionBlocSize;
        options.termIndirectionBlocSize = termIndirectionBlocSize;
        options.localIndirectionBlocSize = localIndirectionBlocSize;
        py::gil_scoped_release release;
        return NindIndexMerge(shards, options).merge(target);
    }, py::arg("shards"), py::arg("target"), py::arg("with_retrolexicon") = false,
       py::arg("lexicon_indirection_bloc_size") = 0, py::arg("retro_indirection_bloc_size") = 0,
       py::arg("term_indirection_bloc_size") = 0, py::arg("local_indirection_bloc_size") = 0);

    // ---- NindIndexVerify ------------------------------------------------
    py::class_<NindIndexVerify::Report>(m, "VerifyReport")
        .def_readonly("format_v2", &NindIndexVerify::Report::formatV2)
        .def_readonly("words_nb", &NindIndexVerify::Report::wordsNb)
        .def_readonly("terms_nb", &NindIndexVerify::Report::termsNb)
        .def_readonly("postings_nb", &NindIndexVerify::Report::postingsNb)
        .def_readonly("documents_nb", &NindIndexVerify::Report::documentsNb)
        .def_readonly("with_local_index", &NindIndexVerify::Report::withLocalIndex)
        .def_readonly("problems_nb", &NindIndexVerify::Report::problemsNb)
        .def_readonly("problems", &NindIndexVerify::Report::problems)
        .def_property_readonly("ok", &NindIndexVerify::Report::ok)
        .def("__bool__", &NindIndexVerify::Report::ok)
        .def("__repr__", [](const NindIndexVerify::Report &r) {
            return "<VerifyReport " + std::string(r.ok() ? "ok" : std::to_string(r.problemsNb) + " problems")
                + ", format v" + (r.formatV2 ? "2" : "1") + ", " + std::to_string(r.wordsNb) + " words, "
                + std::to_string(r.termsNb) + " terms, " + std::to_string(r.documentsNb) + " documents>";
        });
    // Checks an index (see NindIndexVerify.h): never throws on a damaged index, reports it.
    // Releases the GIL.
    m.def("verify_index", [](const std::string &base, const bool crossCheck, const size_t maxProblems) {
        py::gil_scoped_release release;
        return NindIndexVerify::verify(base, crossCheck, maxProblems);
    }, py::arg("base"), py::arg("cross_check") = false, py::arg("max_problems") = 100);

    // ---- NindRetrolexicon -----------------------------------------------
    // ---- NindTokenizer ----------------------------------------------------
    py::class_<NindTokenizer::Options>(m, "TokenizerOptions")
        .def(py::init([](bool splitCamelCase, bool splitUnderscore, bool splitDigits, bool lowercase,
                         unsigned int minLength, unsigned int maxLength) {
            NindTokenizer::Options options;
            options.splitCamelCase = splitCamelCase;
            options.splitUnderscore = splitUnderscore;
            options.splitDigits = splitDigits;
            options.lowercase = lowercase;
            options.minLength = minLength;
            options.maxLength = maxLength;
            return options;
        }), py::arg("split_camel_case") = true, py::arg("split_underscore") = true,
            py::arg("split_digits") = true, py::arg("lowercase") = false,
            py::arg("min_length") = 1, py::arg("max_length") = 254)
        .def_readwrite("split_camel_case", &NindTokenizer::Options::splitCamelCase)
        .def_readwrite("split_underscore", &NindTokenizer::Options::splitUnderscore)
        .def_readwrite("split_digits", &NindTokenizer::Options::splitDigits)
        .def_readwrite("lowercase", &NindTokenizer::Options::lowercase)
        .def_readwrite("min_length", &NindTokenizer::Options::minLength)
        .def_readwrite("max_length", &NindTokenizer::Options::maxLength);

    py::class_<NindTokenizer>(m, "Tokenizer")
        .def(py::init<const NindTokenizer::Options &>(), py::arg("options") = NindTokenizer::Options())
        .def("get_options", &NindTokenizer::getOptions)
        .def("tokenize", [](const NindTokenizer &self, const std::string &text) {
            std::vector<std::string> tokens;
            self.tokenize(text, tokens);
            return tokens;
        }, py::arg("text"))
        // Tokenize many documents at once, for IndexBuilder: returns (vocabulary,
        // term_ids, ends) where vocabulary lists the distinct tokens (in order of first
        // occurrence), term_ids (uint32 bytes) is every token as its index in vocabulary,
        // and document i is term_ids[ends[i-1]:ends[i]] (ends: uint32 bytes).
        .def("tokenize_batch", [](const NindTokenizer &self, const std::vector<std::string> &texts) {
            std::vector<std::string> vocabulary;
            std::vector<uint32_t> termIds, ends;
            {
                py::gil_scoped_release release;
                std::unordered_map<std::string, uint32_t> localIds;
                std::vector<std::pair<size_t, size_t> > spans;
                std::string token;
                for (size_t d = 0; d < texts.size(); d++) {
                    self.tokenize(texts[d].data(), texts[d].size(), spans);
                    for (size_t i = 0; i < spans.size(); i++) {
                        self.getToken(texts[d].data(), spans[i], token);
                        std::unordered_map<std::string, uint32_t>::const_iterator it = localIds.find(token);
                        if (it == localIds.end()) {
                            it = localIds.insert(std::make_pair(token, (uint32_t)vocabulary.size())).first;
                            vocabulary.push_back(token);
                        }
                        termIds.push_back((*it).second);
                    }
                    if (termIds.size() > std::numeric_limits<uint32_t>::max())
                        throw py::value_error("tokenize_batch: too many tokens in one batch");
                    ends.push_back((uint32_t)termIds.size());
                }
            }
            return py::make_tuple(vocabulary, uintBytes(termIds), uintBytes(ends));
        }, py::arg("texts"));

    // ---- IndexBuilder -----------------------------------------------------
    // Accumulates a corpus as term ids, then writes its term index and local index:
    // add_documents() chunk by chunk in increasing document id order, then
    // write_term_index() and write_local_index() (both release the GIL and only read
    // the builder: they can run concurrently, each on its own writer).
    py::class_<IndexBuilder>(m, "IndexBuilder")
        .def(py::init<>())
        .def("add_documents", [](IndexBuilder &self, uint64_t firstDocId, const py::buffer &ends,
                                 const py::buffer &termIds, const py::buffer &idMap) {
            self.addDocuments(firstDocId, uintView(ends, "ends"), uintView(termIds, "term_ids"),
                              uintView(idMap, "id_map"));
        }, py::arg("first_doc_id"), py::arg("ends"), py::arg("term_ids"), py::arg("id_map"))
        .def("write_term_index", &IndexBuilder::writeTermIndex,
             py::arg("term_index"), py::arg("file_identification"), py::arg("cg") = 0)
        .def("write_local_index", &IndexBuilder::writeLocalIndex,
             py::arg("local_index"), py::arg("file_identification"), py::arg("cg") = 0)
        .def("doc_lengths", &IndexBuilder::docLengths)
        .def_property_readonly("docs_nb", &IndexBuilder::docsNb)
        .def_property_readonly("tokens_nb", &IndexBuilder::tokensNb)
        .def_property_readonly("max_term_id", &IndexBuilder::maxTermId);

    // ---- NindDocNames -----------------------------------------------------
    // The names of an index's documents (their identifiers in the corpus), both ways.
    py::class_<NindDocNames>(m, "NindDocNames")
        .def(py::init<const std::string &>(), py::arg("file_name"))
        // writes a new file (never overwrites one); ValueError on a repeated ident or name
        .def_static("write", [](const std::string &fileName, const py::buffer &docIds,
                                const std::vector<std::string> &names) {
            const DocIdsView ids = docIdsView(docIds, "doc_ids");
            py::gil_scoped_release release;
            NindDocNames::write(fileName, ids.values, names);
        }, py::arg("file_name"), py::arg("doc_ids"), py::arg("names"))
        .def("name", [](const NindDocNames &self, uint64_t docId) -> py::object {
            std::string name;
            if (!self.getName(docId, name)) return py::none();
            return py::str(name);
        }, py::arg("doc_id"))
        // the names of many documents (None for a document without name)
        .def("names", [](const NindDocNames &self, const std::vector<uint64_t> &docIds) {
            py::list result;
            std::string name;
            for (size_t i = 0; i < docIds.size(); i++) {
                if (self.getName(docIds[i], name)) result.append(py::str(name));
                else result.append(py::none());
            }
            return result;
        }, py::arg("doc_ids"))
        .def("doc_id", [](const NindDocNames &self, const std::string &name) -> py::object {
            NindDocId docId = 0;
            if (!self.getDocIdent(name, docId)) return py::none();
            return py::int_(docId);
        }, py::arg("name"))
        // every (document, name): (doc ids as uint64 bytes, names), in increasing doc id order
        .def("entries", [](const NindDocNames &self) {
            std::vector<NindDocId> docIds;
            std::vector<std::string> names;
            self.getEntries(docIds, names);
            return py::make_tuple(uint64Bytes(docIds), names);
        })
        .def("__len__", &NindDocNames::size);

    // ---- Bm25Scorer -------------------------------------------------------
    // Okapi BM25 search driven by the postings of the query terms (only the documents
    // containing them are scored). Built once per index from its documents' lengths;
    // search() reads the term index on its thread-safe path (NindFile::readAt): several threads
    // can search one reader at once, each with its own scratch arrays (one pool per scorer)
    // with the GIL released.
    py::class_<Bm25Scorer>(m, "Bm25Scorer")
        .def(py::init([](const py::buffer &docIds, const py::buffer &lengths, double avgDocLength) {
            return new Bm25Scorer(docIdsView(docIds, "doc_ids"), uintView(lengths, "lengths"), avgDocLength);
        }), py::arg("doc_ids"), py::arg("lengths"), py::arg("avg_doc_len"))
        .def("search", &Bm25Scorer::search, py::arg("term_index"), py::arg("term_ids"),
             py::arg("k1") = 1.5, py::arg("b") = 0.75, py::arg("top_k") = -1,
             py::arg("prune") = false)
        .def_property_readonly("docs_nb", &Bm25Scorer::docsNb);

    py::class_<NindRetrolexicon::RetroWord>(m, "RetroWord")
        .def(py::init<>())
        .def(py::init<unsigned int, std::string>(), py::arg("identifiant"), py::arg("mot_simple"))
        .def(py::init<unsigned int, unsigned int, unsigned int>(),
             py::arg("identifiant"), py::arg("identifiant_a"), py::arg("identifiant_s"))
        .def_readwrite("identifiant", &NindRetrolexicon::RetroWord::identifiant)
        .def_readwrite("mot_simple", &NindRetrolexicon::RetroWord::motSimple)
        .def_readwrite("identifiant_a", &NindRetrolexicon::RetroWord::identifiantA)
        .def_readwrite("identifiant_s", &NindRetrolexicon::RetroWord::identifiantS);

    py::class_<NindRetrolexicon>(m, "NindRetrolexicon")
        .def(py::init<const std::string &, bool, const NindPadFile::Identification &, unsigned int>(),
             py::arg("file_name_extensionless"), py::arg("is_writer"),
             py::arg("lexicon_identification"), py::arg("indirection_bloc_size") = 0)
        .def("add_retro_words", &NindRetrolexicon::addRetroWords,
             py::arg("retro_words"), py::arg("lexicon_identification"))
        .def("get_components", [](NindRetrolexicon &self, unsigned int ident) -> py::object {
            std::list<std::string> components;
            if (!self.getComponents(ident, components)) return py::none();
            return py::cast(components);
        }, py::arg("ident"))
        .def("get_file_name", &NindRetrolexicon::getFileName)
        .def("analyse_pad_file", &NindPadFile::analysePadFile);

    // ---- NindLexicon (in-memory) ------------------------------------------
    py::class_<NindLexicon::LexiconChar>(m, "LexiconChar")
        .def(py::init<>())
        .def_readwrite("is_ok", &NindLexicon::LexiconChar::isOk)
        .def_readwrite("sw_nb", &NindLexicon::LexiconChar::swNb)
        .def_readwrite("cw_nb", &NindLexicon::LexiconChar::cwNb)
        .def_readwrite("words_nb", &NindLexicon::LexiconChar::wordsNb)
        .def_readwrite("identification", &NindLexicon::LexiconChar::identification);

    py::class_<NindLexicon>(m, "NindLexicon")
        .def(py::init<const std::string &, bool>(), py::arg("file_name"), py::arg("is_writer"))
        .def("add_word", &NindLexicon::addWord, py::arg("components"))
        .def("get_id", &NindLexicon::getId, py::arg("components"))
        .def("get_identification", [](NindLexicon &self) {
            unsigned int wordsNb = 0, identification = 0;
            self.getIdentification(wordsNb, identification);
            return py::make_tuple(wordsNb, identification);
        })
        .def("integrity_and_counts", [](NindLexicon &self) {
            NindLexicon::LexiconChar lexiconChar;
            self.integrityAndCounts(lexiconChar);
            return lexiconChar;
        })
        .def("dump", [](NindLexicon &self) {
            std::ostringstream oss;
            self.dump(oss);
            return oss.str();
        });
}
////////////////////////////////////////////////////////////
