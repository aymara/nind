//
// C++ Interface: NindSegmentedUpdate
//
// Description: modification d'un index en segments : un nouveau segment, des documents masquejs dans
// les anciens, jamais de rejejcriture des segments existants ; fusions selon une politique par paliers.
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
#ifndef NindSegmentedUpdate_H
#define NindSegmentedUpdate_H
////////////////////////////////////////////////////////////
#include "NindBasics/NindDocId.h"
#include "NindCommonExport.h"
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
////////////////////////////////////////////////////////////
namespace latecon {
    namespace nindex {
////////////////////////////////////////////////////////////
/**\brief An update of an index made of segments (see NindSegmentedIndex): commit() writes the added
* and replaced documents into a new segment and masks the replaced and removed ones in theirs (their
* .ninddeletes); it costs what changes, whatever the size of the index.
* - The files are changed where they are: a caller that has readers works on a copy, in which
*   unchanged files may be hard links: commit() never writes into a segment's files, it replaces
*   the .ninddeletes it changes (remove, then create) and removes merged segments' files.
* - Merges, at the end of commit(): log-structured, as with Lucene's tiers. Segments are in the
*   tier of their number of live documents, tiers growing by mergeFactor; mergeFactor segments in
*   a tier are merged into one, or those of them that fit if that would make a segment of more than
*   maxMergedDocs. A segment of at least half of it is full: it stays as it is (rewriting it costs
*   more than it gains; a large build's segments are searched in parallel). So, however many
*   updates, at most about 2 N / maxMergedDocs full segments and mergeFactor per tier below them. A
*   segment over half deleted is rewritten alone. A document is rewritten about log(N) times over
*   its life, at most. forceMerge() merges everything, whatever the size. */
class DLLExportLexicon NindSegmentedUpdate {
public:
    struct Options {
        bool withLocalIndex;            //new segments with a local index (the index's segments: all or none)
        unsigned int mergeFactor;       //segments of a tier merged at once; 0: no merge
        uint64_t maxMergedDocs;         //no merge into a segment of more live documents; 0: no limit
        Options(): withLocalIndex(true), mergeFactor(10), maxMergedDocs(5000000) {}
    };
    /**\brief What commit() did */
    struct Stats {
        uint64_t addedDocsNb;           //documents added or replaced
        uint64_t removedDocsNb;         //documents removed or replaced that were live
        unsigned int segmentsNb;        //segments of the index, after the merges
        unsigned int mergesNb;          //merges made
        uint64_t mergedDocsNb;          //documents they rewrote
        Stats(): addedDocsNb(0), removedDocsNb(0), segmentsNb(0), mergesNb(0), mergedDocsNb(0) {}
    };

    /**\brief An update of the index at base (nothing read or written before commit) */
    explicit NindSegmentedUpdate(const std::string &base,
                                 const Options &options = Options());
    ~NindSegmentedUpdate();

    /**\brief Add a document, or replace the one with this id; no words: only removes it
    *\param name its name (empty: none) */
    void addDocument(const NindDocId ident,
                     const std::vector<std::string> &words,
                     const std::string &name = std::string());

    /**\brief addDocument() for many documents at once, given as a tokenizer's batch (see
    * NindTokenizer): document d is vocabulary[localIds[i]] for i in [ends[d - 1], ends[d]), named
    * names[d]; each word of the vocabulary is looked up once, not once per occurrence
    *\throws std::invalid_argument if idents and names differ in size, ends aren't ascending or
    * don't cover the words, or a local id is beyond the vocabulary */
    void addDocuments(const std::vector<NindDocId> &idents,
                      const uint32_t *ends,
                      const uint32_t *localIds,
                      const size_t wordsNb,
                      const std::vector<std::string> &vocabulary,
                      const std::vector<std::string> &names);

    /**\brief addDocuments() with the vocabulary given as its words' UTF-8 bytes one after the other
    * (word w: words[wordEnds[w - 1], wordEnds[w])), as NindIndexBuilder::addBatch
    *\throws std::invalid_argument as addDocuments, or on word ends not ascending */
    void addBatch(const std::vector<NindDocId> &idents,
                  const uint32_t *ends,
                  const uint32_t *localIds,
                  const size_t tokensNb,
                  const char *words,
                  const size_t wordsBytes,
                  const uint32_t *wordEnds,
                  const size_t wordsNb,
                  const std::vector<std::string> &names);

    /**\brief Remove a document (nothing if it isn't live) */
    void removeDocument(const NindDocId ident);

    /**\brief Write the changes
    *\throws FileException on I/O errors, std::invalid_argument on a name given twice */
    Stats commit();

    /**\brief Merge every segment of the index at base into one, its deleted documents dropped
    *\return what was done (nothing for an index of one segment without deletion) */
    static Stats forceMerge(const std::string &base);

    /**\brief The largest document id of the index at base, deleted ones included, without opening it
    *\return false if it has none */
    static bool getMaxDocIdent(const std::string &base,
                               NindDocId &docId);

private:
    NindSegmentedUpdate(const NindSegmentedUpdate &);
    NindSegmentedUpdate &operator=(const NindSegmentedUpdate &);
    struct Document {
        std::vector<uint32_t> words;                //their ids in m_words
        std::string name;
    };
    //the id of a word in m_words (added if new)
    uint32_t wordId(const std::string &word);
    std::string m_base;
    Options m_options;
    std::map<NindDocId, Document> m_documents;      //no words: removed
    std::vector<std::string> m_words;               //the words of the added documents, once each
    std::unordered_map<std::string, uint32_t> m_wordIds;
};
////////////////////////////////////////////////////////////
    } // end namespace
} // end namespace
#endif
////////////////////////////////////////////////////////////
