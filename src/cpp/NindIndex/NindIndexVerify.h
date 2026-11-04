//
// C++ Interface: NindIndexVerify
//
// Description: Vejrification d'un index nind (lexique + fichier inverse + index local facultatif),
// formats v1 et v2 : chaque entreje se dejcode, par les deux chemins de lecture, les fichiers sont
// apparillejs au lexique, les listes sont croissantes, les descripteurs de blocs (v2) disent vrai,
// et, sur demande, le fichier inverse et l'index local disent la meme chose.
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
#ifndef NindIndexVerify_H
#define NindIndexVerify_H
////////////////////////////////////////////////////////////
#include "NindCommonExport.h"
#include <cstdint>
#include <string>
#include <vector>
////////////////////////////////////////////////////////////
namespace latecon {
    namespace nindex {
////////////////////////////////////////////////////////////
/**\brief Checks an index (prefix.nindlexiconindex, .nindtermindex, optional .nindlocalindex),
* format v1 or v2, and reports what is wrong with it instead of throwing:
* - the lexicon's words decode (NindLexiconIndex::getEntries), with idents 1..its words number;
* - the term and local files are paired with the lexicon (identification);
* - every term definition decodes, the same through both read paths; per category, document ids
*   strictly increase, frequencies aren't 0 and sum to the category's frequency; no definition for
*   a term the lexicon doesn't have;
* - format v2: every block of a long list matches its descriptor (first and last documents,
*   count, largest frequency: what a top-k search trusts to skip a block it doesn't read), and the
*   blocks read through the directory are the definition's postings;
* - every document of the local index decodes, the same through both read paths, with terms of
*   the lexicon;
* - with crossCheck, the term index and the local index hold the same (document, term, frequency)
*   triples (a term's frequency in a document: its localisations), and no block's shortest length
*   (<longueurMin>) is above one of its documents' length (its localisations), in memory: about
*   40 bytes per posting. */
class DLLExportLexicon NindIndexVerify {
public:
    struct Report {
        bool formatV2;                  //the lexicon's format
        unsigned int wordsNb;           //the lexicon's
        unsigned int termsNb;           //terms with a definition
        uint64_t postingsNb;            //of every definition
        uint64_t documentsNb;           //of the local index
        bool withLocalIndex;
        uint64_t problemsNb;            //every problem found (problems holds the first maxProblems)
        std::vector<std::string> problems;
        Report(): formatV2(false), wordsNb(0), termsNb(0), postingsNb(0), documentsNb(0),
            withLocalIndex(false), problemsNb(0), problems() {}
        bool ok() const { return problemsNb == 0; }
    };

    /**\brief Verify the index at fileNameExtensionLess
    *\param fileNameExtensionLess the index's files without their extension
    *\param crossCheck also compare the term index with the local index (if there is one)
    *\param maxProblems at most this many problems described in the report (all are counted)
    *\return the report: ok() if nothing is wrong */
    static Report verify(const std::string &fileNameExtensionLess,
                         const bool crossCheck = false,
                         const size_t maxProblems = 100);
};
////////////////////////////////////////////////////////////
    } // end namespace
} // end namespace
#endif
////////////////////////////////////////////////////////////
