//
// C++ Interface: NindAmoseMerge
//
// Description: Fusion hors ligne d'index Amose (voir NindIndexMerge) : le lexique fusionnej a
// toujours un retro lexique, et les compteurs du fichier inverse (termes uniques et occurrences
// par type) sont recalculejs sur l'index fusionnej, le type d'un terme ejtant dejduit de ses
// composants comme le fait NindLexiconAmose::getWord.
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
#ifndef NindAmoseMerge_H
#define NindAmoseMerge_H
////////////////////////////////////////////////////////////
#include "NindIndex/NindIndexMerge.h"
#include "NindLexiconAmose.h"
#include "NindCommonExport.h"
#include <string>
#include <list>
#include <vector>
////////////////////////////////////////////////////////////
namespace latecon {
    namespace nindex {
////////////////////////////////////////////////////////////
/**\brief Merges several Amose indexes (shards over disjoint sets of documents) into one */
class DLLExportLexicon NindAmoseMerge : public NindIndexMerge {
public:
    /**\brief Prepare the merge of the specified shards
    *\param shardNames absolute path file names without extension of the shards
    *\param options indirection block sizes of the merged files (withRetrolexicon is forced) */
    NindAmoseMerge(const std::vector<std::string> &shardNames,
                   const Options &options = Options());

    virtual ~NindAmoseMerge();

protected:
    virtual unsigned int getTermSpecificsNumber() const;
    virtual void countTerm(const std::list<std::string> &components,
                           const std::list<NindTermIndex::TermCG> &termDef);
    virtual std::list<unsigned int> getTermSpecifics() const;

private:
    std::vector<unsigned int> m_uniqueTermCount;        //par AmoseTypes
    std::vector<unsigned int> m_termOccurrences;        //par AmoseTypes
};
////////////////////////////////////////////////////////////
    } // end namespace
} // end namespace
#endif
////////////////////////////////////////////////////////////
