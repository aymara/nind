//
// C++ Interface: NindSegmentedIndex
//
// Description: un index fait de segments immuables (chacun un index nind standard), interrogej comme
// un seul : mesmes rejsultats qu'un index construit d'un seul tenant avec ses documents vivants.
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
#ifndef NindSegmentedIndex_H
#define NindSegmentedIndex_H
////////////////////////////////////////////////////////////
#include "NindBasics/NindDocId.h"
#include "NindBm25Scorer.h"
#include "NindCommonExport.h"
#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <utility>
#include <vector>
////////////////////////////////////////////////////////////
namespace latecon {
    namespace nindex {
////////////////////////////////////////////////////////////
/**\brief The files of an index's document lengths (<name>.ninddoclengths): every document's length
* in tokens, the documents of length 0 left out.
* <fichier> ::= <magic="NINDDL02"> <nombre> { <ident> } { <longueur> }
* <nombre>, <longueur>: Entier4; <ident>: Entier8, increasing (Entier4 in NINDDL01, still read).
* Little-endian. */
class DLLExportLexicon NindDocLengths {
public:
    /**\brief Read a file
    *\return false if there is no such file
    *\throws FormatFileException, ReadFileException if invalid */
    static bool read(const std::string &fileName,
                     std::vector<NindDocId> &docIds,
                     std::vector<uint32_t> &lengths);
    /**\brief read() without the document ids when they are consecutive (every file nind writes):
    * the file is mapped, only the lengths are copied (4 bytes a document)
    *\param firstId receives the first document's id
    *\param docIds receives the ids, left empty when they are firstId, firstId + 1...
    *\return false if there is no such file
    *\throws FormatFileException, ReadFileException if invalid */
    static bool readCompact(const std::string &fileName,
                            NindDocId &firstId,
                            std::vector<NindDocId> &docIds,
                            std::vector<uint32_t> &lengths);
    /**\brief The last (largest) document of a file, read alone
    *\return false if there is no such file or it's empty
    *\throws FormatFileException, ReadFileException if invalid */
    static bool readLast(const std::string &fileName,
                         NindDocId &docId);
    /**\brief Write a new file (must not exist)
    *\throws OpenFileException, WriteFileException on I/O errors */
    static void write(const std::string &fileName,
                      const std::vector<NindDocId> &docIds,
                      const std::vector<uint32_t> &lengths);
};
////////////////////////////////////////////////////////////
/**\brief The documents removed from a segment (<segment>.ninddeletes): segments never change, so a
* removed document stays in its files and is masked, until a merge drops it.
* <fichier> ::= <magic="NINDDE01"> <nombreDocs> <nombreTermes> { <ident> } { <terme> <nombre> }
* <nombreDocs>, <nombreTermes>, <terme>, <nombre>: Entier4; <ident>: Entier8, increasing.
* <terme> (increasing): a term of the segment's lexicon; <nombre>: how many of its postings belong to
* removed documents, for the exact document frequency without reading its list. Little-endian. */
struct DLLExportLexicon NindSegmentDeletes {
    std::vector<NindDocId> docIds;                                  //increasing
    std::vector<std::pair<unsigned int, uint32_t> > termCounts;     //(term, postings), by increasing term
    /**\brief Read a file
    *\return false if there is no such file
    *\throws FormatFileException, ReadFileException if invalid */
    bool read(const std::string &fileName);
    /**\brief Write a new file (must not exist)
    *\throws OpenFileException, WriteFileException on I/O errors */
    void write(const std::string &fileName) const;
};
////////////////////////////////////////////////////////////
/**\brief An index made of segments, searched as one. A segment is a standard nind index, never
* modified once written; the index at base is either
* - a manifest, <base>.nindsegments (first line "nindsegments 1", then a segment per line, a
*   directory next to it: its files are <dirname(base)>/<segment>/<basename(base)>.*), or
* - no manifest: base is the only segment.
* Next to each segment's index files: <name>.ninddoclengths (see NindDocLengths), optional
* <name>.ninddocnames (see NindDocNames) and <name>.ninddeletes (see NindSegmentDeletes).
* - Results: exactly those of one index built from the live documents: N, the average length and
*   every df are the corpus's, deletions subtracted; ties broken by increasing document id. A
*   document id is live in at most one segment.
* - Search: segments in parallel (threads), sharing theta (the k-th best score so far), so that
*   every segment prunes from what the others found.
* - Thread-safe: everything is const and reads the files on their const paths. */
class DLLExportLexicon NindSegmentedIndex {
public:
    /**\brief What a segment holds */
    struct SegmentInfo {
        std::string name;               //its directory (empty: the index without manifest)
        std::string base;               //the path of its files, without extension
        uint64_t docsNb;                //its documents, the deleted ones included
        uint64_t deletedNb;
    };

    /**\brief The segments of the index at base, in the order of the manifest
    *\return (none if no manifest) */
    static std::vector<std::string> readManifest(const std::string &base);
    /**\brief Write the manifest of the index at base (replaces it) */
    static void writeManifest(const std::string &base,
                              const std::vector<std::string> &segments);
    /**\brief The path, without extension, of a segment of the index at base */
    static std::string segmentBase(const std::string &base,
                                   const std::string &segment);

    /**\brief Open the index at base
    *\param base path without extension
    *\param previous an index opened earlier (or null): its segments of the same names are shared,
    * not read again (segments never change; the deletions are read again)
    *\throws FileException if a file is missing or invalid */
    explicit NindSegmentedIndex(const std::string &base,
                                const NindSegmentedIndex *previous = 0);
    ~NindSegmentedIndex();

    /**\brief The documents with a positive score for the query's words, by decreasing score, then
    * increasing document (see NindBm25Scorer::search)
    *\param words the query's words in query order, repeats included: each word's components
    *\param threads threads this search uses, this one included
    *\return (document, score) pairs */
    std::vector<std::pair<NindDocId, double> > search(const std::vector<std::list<std::string> > &words,
                                                      const double k1,
                                                      const double b,
                                                      const long topK,
                                                      const bool prune,
                                                      const unsigned int threads = 1) const;

    /**\brief The live documents with a word, every category counted */
    uint64_t documentFrequency(const std::list<std::string> &word) const;

    /**\brief A word's occurrences in a live document, as searches score it: in the first of its
    * categories having the document (0: none) */
    uint64_t termFrequency(const std::list<std::string> &word,
                           const NindDocId docId) const;

    /**\brief A live document's length (0: unknown) */
    uint32_t documentLength(const NindDocId docId) const;

    /**\brief Whether a document is live here */
    bool contains(const NindDocId docId) const;

    /**\brief Where a live document is
    *\param segment receives its segment, as in segments()
    *\param position receives its place in the segment's documents (from 0)
    *\return false if it isn't live here */
    bool locate(const NindDocId docId,
                size_t &segment,
                size_t &position) const;

    /**\brief The name of a live document
    *\return false if it has none */
    bool getName(const NindDocId docId,
                 std::string &name) const;

    /**\brief The live document of a name
    *\return false if none has it */
    bool getDocIdent(const std::string &name,
                     NindDocId &docId) const;

    /**\brief Whether a segment has names */
    bool hasNames() const;

    /**\brief The largest document id ever given, deleted ones included (found: false if no document) */
    bool getMaxDocIdent(NindDocId &docId) const;

    /**\brief The live documents and their lengths, by increasing document */
    void getDocuments(std::vector<NindDocId> &docIds,
                      std::vector<uint32_t> &lengths) const;

    /**\brief The live documents */
    uint64_t docsNb() const { return m_docsNb; }

    /**\brief Their average length */
    double avgDocLength() const { return m_avgDocLength; }

    /**\brief The segments */
    std::vector<SegmentInfo> segments() const;

private:
    NindSegmentedIndex(const NindSegmentedIndex &);
    NindSegmentedIndex &operator=(const NindSegmentedIndex &);
    class Impl;
    std::unique_ptr<Impl> m_impl;
    uint64_t m_docsNb;
    double m_avgDocLength;
};
////////////////////////////////////////////////////////////
    } // end namespace
} // end namespace
#endif
////////////////////////////////////////////////////////////
