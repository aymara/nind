//
// C++ Interface: NindIndexMerge
//
// Description: Fusion hors ligne de plusieurs index nind (les "shards") en un seul index standard.
// Chaque shard est un ensemble lexique + fichier inverse + index locaux, ejcrit par son propre
// ejcrivain sur une partition des documents. La fusion rejunit les lexiques (les identifiants de
// termes sont renumejrotejs), concatehne les listes de documents de chaque terme et recopie les
// index locaux en y renumejrotant les termes. Le rejsultat est lisible par tous les lecteurs existants.
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
#ifndef NindIndexMerge_H
#define NindIndexMerge_H
////////////////////////////////////////////////////////////
#include "NindTermIndex.h"
#include "NindCommonExport.h"
#include "NindExceptions.h"
#include <string>
#include <list>
#include <vector>
////////////////////////////////////////////////////////////
namespace latecon {
    namespace nindex {
////////////////////////////////////////////////////////////
/**\brief Merges several nind indexes (shards over disjoint sets of documents) into one.
* Shards must have no term specifics (see NindAmoseMerge for Amose indexes), and no
* document may appear in more than one shard. The target files must not exist yet. */
class DLLExportLexicon NindIndexMerge {
public:
    /**\brief Merge parameters: indirection block sizes of the merged files (0 = sized to fit) */
    struct Options {
        bool withRetrolexicon;                  //also write a retro lexicon for the merged lexicon
        unsigned int lexiconIndirectionBlocSize;    //also the hash modulo of the lexicon (0 = merged words count)
        unsigned int retroIndirectionBlocSize;      //(0 = merged words count + 1)
        unsigned int termIndirectionBlocSize;       //(0 = merged words count + 1)
        unsigned int localIndirectionBlocSize;      //(0 = documents count + 1)
        Options(): withRetrolexicon(false), lexiconIndirectionBlocSize(0), retroIndirectionBlocSize(0),
            termIndirectionBlocSize(0), localIndirectionBlocSize(0) {}
    };

    /**\brief What was merged */
    struct Stats {
        unsigned int shardsNb;
        unsigned int wordsNb;       //words of the merged lexicon
        unsigned int termsNb;       //terms with at least one document in the merged term index
        unsigned int docsNb;        //documents of the merged local index
        NindPadFile::Identification identification;     //of the merged lexicon
        Stats(): shardsNb(0), wordsNb(0), termsNb(0), docsNb(0), identification() {}
    };

    /**\brief Prepare the merge of the specified shards
    *\param shardNames absolute path file names without extension of the shards
    *\param options indirection block sizes of the merged files */
    NindIndexMerge(const std::vector<std::string> &shardNames,
                   const Options &options = Options());

    virtual ~NindIndexMerge();

    /**\brief Merge the shards into a new index
    *\param targetName absolute path file name without extension of the merged index (must not exist)
    *\return what was merged
    *\throws NindIndexException if the target exists, a document is in several shards,
    * or shards are inconsistent (plus any exception raised by reading the shards).
    * On any exception, the target files are removed. */
    Stats merge(const std::string &targetName);

protected:
    /**\brief number of specific unsigned int of the term indexes (shards and merged) */
    virtual unsigned int getTermSpecificsNumber() const;

    /**\brief called for every term written into the merged term index
    *\param components components of the term (1 component = simple word, more components = compound word)
    *\param termDef merged definition of the term */
    virtual void countTerm(const std::list<std::string> &components,
                           const std::list<NindTermIndex::TermCG> &termDef);

    /**\brief specifics of the merged term index, once every term was counted
    *\return getTermSpecificsNumber() specific unsigned int */
    virtual std::list<unsigned int> getTermSpecifics() const;

    Options m_options;

private:
    //un mot de l'union des lexiques : son prejfixe (0 si mot simple) et son dernier composant
    struct UnionWord {
        unsigned int prefix;
        std::string lastComponent;
        UnionWord(const unsigned int pref, const std::string &last): prefix(pref), lastComponent(last) {}
    };

    //la fusion elle-mesme, sur une cible vejrifieje
    Stats doMerge(const std::string &targetName);

    //composants d'un mot de l'union (chaisne des prejfixes, toujours d'identifiants dejcroissants)
    void getUnionComponents(const unsigned int unionIdent,
                            std::list<std::string> &components) const;

    std::vector<std::string> m_shardNames;
    std::vector<UnionWord> m_unionWords;        //indexej par identifiant d'union (0 inutilisej)
};
////////////////////////////////////////////////////////////
    } // end namespace
} // end namespace
#endif
////////////////////////////////////////////////////////////
