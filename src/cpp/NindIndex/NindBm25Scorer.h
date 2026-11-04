//
// C++ Interface: NindBm25Scorer
//
// Description: Okapi BM25 sur le fichier inverse : exhaustif, ou par blocs (block-max MaxScore,
// format v2) pour un petit top k ; mesmes rejsultats.
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
#ifndef NindBm25Scorer_H
#define NindBm25Scorer_H
////////////////////////////////////////////////////////////
#include "NindTermIndex.h"
#include "NindBasics/NindDocId.h"
#include "NindCommonExport.h"
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>
////////////////////////////////////////////////////////////
namespace latecon {
    namespace nindex {
////////////////////////////////////////////////////////////
/**\brief Okapi BM25 search over a term index, driven by the postings of the query terms (only
* the documents containing them are scored), on flat arrays: no allocation nor hashing per
* posting. Built once per index from its documents' lengths.
* - Scores: for each occurrence of a query term, in query order (a term repeated counts again),
*   idf * tf * (k1 + 1) / (tf + k1 * (1 - b + b * length / avgDocLength)), idf =
*   log((N - df + 0.5) / (df + 0.5) + 1), df counting the documents of every category; a document
*   in several categories of a term counts in its first one only. Ties are broken by the order of
*   the documents given to the constructor. (nind's Python NindEngine scores exactly so: compile
*   with -ffp-contract=off for bit-identical scores.)
* - prune: block-max MaxScore, document at a time, over format v2 block descriptors (max
*   frequency, shortest length): the same results as the exhaustive search, faster for a small
*   top k on a large corpus (NindEngine uses it when topK * 4000 <= documents). It falls back
*   to the exhaustive search for a term in several categories, an idf <= 0, a long list without
*   blocks (format v1), topK <= 0, k1 < 0 or b outside [0, 1].
* - Thread-safe: search() is const and reads the term index on its const path (each search with
*   its own scratch arrays and read buffer, from a pool); one scorer serves many threads. */
class DLLExportLexicon NindBm25Scorer {
public:
    /**\brief The corpus: its documents, in the order that breaks score ties, and their lengths
    *\param docIds document ids (64 bits; at most 4.29G documents)
    *\param lengths their lengths, in the same order
    *\param avgDocLength the average document length
    *\throws std::invalid_argument if the sizes differ or there are too many documents */
    NindBm25Scorer(const std::vector<NindDocId> &docIds,
                   const std::vector<uint32_t> &lengths,
                   const double avgDocLength);
    ~NindBm25Scorer();

    /**\brief The documents with a positive score for the query terms, by decreasing score
    *\param termIndex the index to search (its const read path)
    *\param termIds the query's terms, in query order, repeats included (0: an unknown term)
    *\param topK at most this many documents (all if < 0)
    *\param prune block-max MaxScore (same results; see above)
    *\return (document id, score) pairs */
    std::vector<std::pair<NindDocId, double> > search(const NindTermIndex &termIndex,
                                                      const std::vector<unsigned int> &termIds,
                                                      const double k1,
                                                      const double b,
                                                      const long topK,
                                                      const bool prune) const;

    /**\brief Number of documents of the corpus */
    size_t docsNb() const;

private:
    NindBm25Scorer(const NindBm25Scorer &);
    NindBm25Scorer &operator=(const NindBm25Scorer &);
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
////////////////////////////////////////////////////////////
    } // end namespace
} // end namespace
#endif
////////////////////////////////////////////////////////////
