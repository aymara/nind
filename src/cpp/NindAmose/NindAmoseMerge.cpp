//
// C++ Implementation: NindAmoseMerge
//
// Description: Fusion hors ligne d'index Amose, voir NindAmoseMerge.h
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
#include "NindAmoseMerge.h"
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
//mesme nombre et mesme ordre que NindTermAmose
#define NOMBRE_COMPTEURS 8
////////////////////////////////////////////////////////////
static NindIndexMerge::Options forceRetrolexicon(const NindIndexMerge::Options &options)
{
    NindIndexMerge::Options result = options;
    result.withRetrolexicon = true;      //NindLexiconAmose a toujours un retro lexique
    return result;
}
////////////////////////////////////////////////////////////
//brief Prepare the merge of the specified shards
//param shardNames absolute path file names without extension of the shards
//param options indirection block sizes of the merged files (withRetrolexicon is forced) */
NindAmoseMerge::NindAmoseMerge(const vector<string> &shardNames,
                               const Options &options):
    NindIndexMerge(shardNames, forceRetrolexicon(options)),
    m_uniqueTermCount(4, 0),
    m_termOccurrences(4, 0)
{
}
////////////////////////////////////////////////////////////
NindAmoseMerge::~NindAmoseMerge()
{
}
////////////////////////////////////////////////////////////
unsigned int NindAmoseMerge::getTermSpecificsNumber() const
{
    return NOMBRE_COMPTEURS;
}
////////////////////////////////////////////////////////////
//compte le terme selon son type, dejduit de ses composants comme NindLexiconAmose::getWord
void NindAmoseMerge::countTerm(const list<string> &components,
                               const list<NindTermIndex::TermCG> &termDef)
{
    AmoseTypes type = (components.size() == 1) ? SIMPLE_TERM : MULTI_TERM;
    if (!components.empty() && components.front() == "§") type = NAMED_ENTITY;
    unsigned int occurrences = 0;
    for (list<NindTermIndex::TermCG>::const_iterator it = termDef.begin(); it != termDef.end(); it++)
        occurrences += (*it).frequency;
    m_uniqueTermCount[type] += 1;
    m_uniqueTermCount[ALL] += 1;
    m_termOccurrences[type] += occurrences;
    m_termOccurrences[ALL] += occurrences;
}
////////////////////////////////////////////////////////////
//mesme ordre que NindTermAmose::setCountsAsList
list<unsigned int> NindAmoseMerge::getTermSpecifics() const
{
    list<unsigned int> result;
    const AmoseTypes types[] = {ALL, SIMPLE_TERM, MULTI_TERM, NAMED_ENTITY};
    for (unsigned int i = 0; i < 4; i++) {
        result.push_back(m_uniqueTermCount[types[i]]);
        result.push_back(m_termOccurrences[types[i]]);
    }
    return result;
}
////////////////////////////////////////////////////////////
