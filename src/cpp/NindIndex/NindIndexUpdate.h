//
// C++ Interface: NindIndexUpdate
//
// Description: Mise ah jour increjmentale d'un index nind (lexique + fichier inverse + index local
// facultatif) : des documents ajoutejs, remplacejs ou supprimejs, ejcrits ensemble. Seuls les termes
// touchejs sont rejcrits, et un terme qui ne gagne que des documents aprehs son dernier ne rejcrit que
// son dernier bloc, en place (format v2).
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
#ifndef NindIndexUpdate_H
#define NindIndexUpdate_H
////////////////////////////////////////////////////////////
#include "NindBasics/NindDocId.h"
#include "NindCommonExport.h"
#include <cstdint>
#include <list>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
////////////////////////////////////////////////////////////
namespace latecon {
    namespace nindex {
////////////////////////////////////////////////////////////
/**\brief Incremental update of an index (prefix.nindlexiconindex, .nindtermindex, optional
* .nindlocalindex; created if there is none): documents added, replaced (adding a document the
* index has replaces it) and removed, written together by commit().
* - Only the touched terms are rewritten: a term that only gains documents after its last one
*   rewrites its last block in place, in the room an earlier rewrite left (see
*   NindTermIndex::updateTermDef); the other touched terms are rewritten whole, with room.
* - The terms of a replaced or removed document come from the local index, or, without one, from
*   a scan of every term (slower).
* - An index written in format v1 (an older nind) is first rebuilt in format v2 (NindIndexMerge
*   onto itself: term ids change, document ids don't); so is an index whose lexicon holds more than
*   lexiconMaxLoad words per hash bucket (the bucket count is fixed in the file), after the update,
*   with twice as many buckets as words.
* - The term index's specifics are kept as they are (Amose's counters aren't recomputed).
* Single writer: nothing may read or write the files during commit() (a rebuild replaces them). */
class DLLExportLexicon NindIndexUpdate {
public:
    struct Options {
        bool withLocalIndex;                    //a new index: with a local index (an existing one: as it is)
        bool withRetrolexicon;                  //a new index: with a retro lexicon (an existing one: as it is)
        unsigned int lexiconMinBuckets;         //a new lexicon: at least this many hash buckets
        unsigned int lexiconMaxLoad;            //words per bucket above which the index is rebuilt
        unsigned int indirectionBlocMin;        //entries in each new indirection block (at least)
        Options(): withLocalIndex(true), withRetrolexicon(false), lexiconMinBuckets(4096), lexiconMaxLoad(4),
            indirectionBlocMin(4096) {}
    };
    /**\brief One occurrence of a word in a document: its components (one: a simple word, more: a
    * compound word), its category, its place */
    struct Occurrence {
        std::list<std::string> word;
        unsigned char cg;
        unsigned int position;
        unsigned int length;
    };
    /**\brief What commit() did */
    struct Stats {
        uint64_t addedDocsNb;           //documents added or replaced
        uint64_t removedDocsNb;         //documents removed (that were in the index)
        unsigned int newWordsNb;        //words the lexicon didn't have
        unsigned int termsNb;           //term definitions changed
        unsigned int inPlaceTermsNb;    //of which rewritten in place (their last block)
        bool migrated;                  //a format v1 index, rebuilt in format v2 first
        bool rebuilt;                   //rebuilt for a fuller lexicon (term ids changed)
        Stats(): addedDocsNb(0), removedDocsNb(0), newWordsNb(0), termsNb(0), inPlaceTermsNb(0),
            migrated(false), rebuilt(false) {}
    };

    /**\brief An update of the index at fileNameExtensionLess (nothing read or written before commit) */
    NindIndexUpdate(const std::string &fileNameExtensionLess,
                    const Options &options = Options());

    /**\brief The lengths of the index's documents (as a search scores with them), for the blocks'
    * <longueurMin> (see NindTermIndex::setDocumentLengths); the added documents' are given with
    * them. Without them, rewritten blocks with documents of unknown length say 0 (unknown). */
    void setDocumentLengths(const std::vector<std::pair<NindDocId, unsigned int> > &lengths);

    /**\brief Add (or replace) a document made of simple words, in order: category cg, word i at
    * position i, length 1
    *\param length the document's length (for the blocks' <longueurMin>; default: words.size()) */
    void addDocument(const NindDocId ident,
                     const std::vector<std::string> &words,
                     const unsigned char cg = 0,
                     const long length = -1);

    /**\brief Add (or replace) a document from its occurrences (compound words, categories,
    * localisations); <nbreLocalisations> is one byte: a term with more than 255 occurrences in a
    * document is written as several local entries
    *\param length the document's length (default: occurrences.size()) */
    void addDocument(const NindDocId ident,
                     const std::vector<Occurrence> &occurrences,
                     const long length = -1);

    /**\brief Remove a document (nothing if the index doesn't have it) */
    void removeDocument(const NindDocId ident);

    /**\brief Number of documents added, replaced or removed so far */
    size_t pendingNb() const { return m_documents.size(); }

    /**\brief Write every change (nothing if there is none); the update can't be used again
    *\return what was done */
    Stats commit();

private:
    struct Document {
        bool removed;
        std::vector<uint32_t> words;            //into m_vocabulary
        std::vector<unsigned char> cgs;
        std::vector<std::pair<unsigned int, unsigned int> > places;     //(position, length)
        unsigned int length;
    };
    uint32_t vocabularyIndex(const std::list<std::string> &word);
    //the index rebuilt by a merge onto itself (lexiconBuckets: 0 = the merge's default)
    void rebuild(const unsigned int lexiconBuckets);

    std::string m_fileName;
    Options m_options;
    std::vector<std::list<std::string> > m_vocabulary;
    std::map<std::list<std::string>, uint32_t> m_vocabularyIndex;
    std::map<NindDocId, Document> m_documents;
    std::vector<std::pair<NindDocId, unsigned int> > m_lengths;
    bool m_withLengths;
    bool m_done;
};
////////////////////////////////////////////////////////////
    } // end namespace
} // end namespace
#endif
////////////////////////////////////////////////////////////
