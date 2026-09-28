//
// C++ Implementation: NindIndexMerge
//
// Description: Fusion hors ligne de plusieurs index nind (les "shards") en un seul index standard.
// voir NindIndexMerge.h
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
#include "NindIndexMerge.h"
#include "NindLexiconIndex.h"
#include "NindLocalIndex.h"
#include <algorithm>
#include <map>
#include <memory>
#include <utility>
#include <stdio.h>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
namespace {
//un terme d'un shard, repejrej par son identifiant fusionnej
struct ShardTerm {
    unsigned int mergedIdent;
    unsigned int shard;
    unsigned int shardIdent;
    ShardTerm(const unsigned int merged, const unsigned int sh, const unsigned int shardId):
        mergedIdent(merged), shard(sh), shardIdent(shardId) {}
    bool operator<(const ShardTerm &term2) const {
        if (mergedIdent != term2.mergedIdent) return mergedIdent < term2.mergedIdent;
        return shard < term2.shard;
    }
};
bool documentInfejrieur(const NindTermIndex::Document &doc1, const NindTermIndex::Document &doc2)
{
    return doc1.ident < doc2.ident;
}
const char *EXTENSIONS[] = {".nindlexiconindex", ".nindretrolexicon", ".nindtermindex", ".nindlocalindex"};
bool fileExists(const string &fileName)
{
    FILE *file = fopen(fileName.c_str(), "rb");
    if (file == 0) return false;
    fclose(file);
    return true;
}
}
////////////////////////////////////////////////////////////
//brief Prepare the merge of the specified shards
//param shardNames absolute path file names without extension of the shards
//param options indirection block sizes of the merged files */
NindIndexMerge::NindIndexMerge(const vector<string> &shardNames,
                               const Options &options):
    m_options(options),
    m_shardNames(shardNames),
    m_unionWords()
{
}
////////////////////////////////////////////////////////////
NindIndexMerge::~NindIndexMerge()
{
}
////////////////////////////////////////////////////////////
//brief Merge the shards into a new index
//param targetName absolute path file name without extension of the merged index (must not exist)
//return what was merged */
NindIndexMerge::Stats NindIndexMerge::merge(const string &targetName)
{
    //un ejcrivain ouvert sur un fichier existant l'enrichirait au lieu de le crejer
    for (unsigned int i = 0; i < 4; i++) {
        if (fileExists(targetName + EXTENSIONS[i]))
            throw NindIndexException("NindIndexMerge::merge target already exists : " + targetName + EXTENSIONS[i]);
    }
    for (size_t shard = 0; shard < m_shardNames.size(); shard++) {
        if (m_shardNames[shard] == targetName)
            throw NindIndexException("NindIndexMerge::merge target is also a shard : " + targetName);
    }
    //en cas d'ejchec, ne laisse pas un index partiel (les fichiers sont refermejs en sortant de doMerge)
    try {
        return doMerge(targetName);
    }
    catch (...) {
        for (unsigned int i = 0; i < 4; i++) remove((targetName + EXTENSIONS[i]).c_str());
        throw;
    }
}
////////////////////////////////////////////////////////////
//la fusion elle-mesme, sur une cible vejrifieje
NindIndexMerge::Stats NindIndexMerge::doMerge(const string &targetName)
{
    const unsigned int shardsNb = m_shardNames.size();
    const unsigned int specificsNumber = getTermSpecificsNumber();
    Stats stats;
    stats.shardsNb = shardsNb;

    //1) union des lexiques : un mot est identifiej par (prejfixe dans l'union, dernier composant)
    m_unionWords.clear();
    m_unionWords.push_back(UnionWord(0, ""));           //l'identifiant 0 n'est pas valide
    map<pair<unsigned int, string>, unsigned int> unionIdents;
    vector<NindPadFile::Identification> shardIdentifications(shardsNb);
    vector<vector<unsigned int> > shardToUnion(shardsNb);       //identifiant du shard -> identifiant d'union
    for (unsigned int shard = 0; shard < shardsNb; shard++) {
        NindLexiconIndex lexicon(m_shardNames[shard], false);
        vector<NindLexiconIndex::Entry> entries;
        lexicon.getEntries(entries);
        shardIdentifications[shard] = lexicon.getIdentification();
        vector<unsigned int> &toUnion = shardToUnion[shard];
        toUnion.assign(shardIdentifications[shard].lexiconWordsNb + 1, 0);
        //les entrejes sont par identifiant croissant : le prejfixe d'un mot composej est dejjah traitej
        for (size_t i = 0; i < entries.size(); i++) {
            const NindLexiconIndex::Entry &entry = entries[i];
            const unsigned int unionPrefix = (entry.prefixIdent == 0) ? 0 : toUnion[entry.prefixIdent];
            if (entry.prefixIdent != 0 && unionPrefix == 0)
                throw NindIndexException("NindIndexMerge::merge unknown prefix in lexicon : " + m_shardNames[shard]);
            const pair<unsigned int, string> key(unionPrefix, entry.lastComponent);
            map<pair<unsigned int, string>, unsigned int>::const_iterator it = unionIdents.find(key);
            if (it == unionIdents.end()) {
                it = unionIdents.insert(make_pair(key, (unsigned int)m_unionWords.size())).first;
                m_unionWords.push_back(UnionWord(unionPrefix, entry.lastComponent));
            }
            toUnion[entry.ident] = (*it).second;
        }
    }
    unionIdents.clear();
    const unsigned int unionWordsNb = m_unionWords.size() - 1;

    //les documents de tous les shards, vejrifiejs disjoints avant toute ejcriture
    vector<unique_ptr<NindLocalIndex> > shardLocalIndexes;
    vector<pair<unsigned int, unsigned int> > documents;       //(identifiant externe, shard)
    for (unsigned int shard = 0; shard < shardsNb; shard++) {
        shardLocalIndexes.push_back(unique_ptr<NindLocalIndex>(
            new NindLocalIndex(m_shardNames[shard], false, shardIdentifications[shard])));
        vector<unsigned int> docIdents;
        shardLocalIndexes.back()->getDocIdents(docIdents);
        for (size_t i = 0; i < docIdents.size(); i++) documents.push_back(make_pair(docIdents[i], shard));
    }
    sort(documents.begin(), documents.end());
    for (size_t i = 1; i < documents.size(); i++) {
        if (documents[i].first == documents[i - 1].first)
            throw NindIndexException("NindIndexMerge::merge document in several shards : " + to_string(documents[i].first));
    }

    //2) le lexique fusionnej, dimensionnej pour l'union
    NindLexiconIndex lexicon(targetName, true, m_options.withRetrolexicon,
                             m_options.lexiconIndirectionBlocSize ? m_options.lexiconIndirectionBlocSize : max(unionWordsNb, 1u),
                             m_options.retroIndirectionBlocSize ? m_options.retroIndirectionBlocSize : unionWordsNb + 1);
    vector<unsigned int> unionToMerged(unionWordsNb + 1, 0);
    vector<unsigned int> mergedToUnion(1, 0);
    for (unsigned int unionIdent = 1; unionIdent <= unionWordsNb; unionIdent++) {
        list<string> components;
        getUnionComponents(unionIdent, components);
        const unsigned int mergedIdent = lexicon.addWord(components);
        unionToMerged[unionIdent] = mergedIdent;
        if (mergedToUnion.size() <= mergedIdent) mergedToUnion.resize(mergedIdent + 1, 0);
        mergedToUnion[mergedIdent] = unionIdent;
    }
    const NindPadFile::Identification identification = lexicon.getIdentification();
    stats.wordsNb = identification.lexiconWordsNb;
    stats.identification = identification;
    //identifiant du shard -> identifiant fusionnej
    vector<vector<unsigned int> > shardToMerged(shardsNb);
    for (unsigned int shard = 0; shard < shardsNb; shard++) {
        const vector<unsigned int> &toUnion = shardToUnion[shard];
        vector<unsigned int> &toMerged = shardToMerged[shard];
        toMerged.assign(toUnion.size(), 0);
        for (size_t shardIdent = 1; shardIdent < toUnion.size(); shardIdent++)
            if (toUnion[shardIdent] != 0) toMerged[shardIdent] = unionToMerged[toUnion[shardIdent]];
    }
    shardToUnion.clear();

    //3) le fichier inverse : pour chaque terme fusionnej, rejunit les documents de tous les shards
    {
        vector<unique_ptr<NindTermIndex> > shardTermIndexes;
        for (unsigned int shard = 0; shard < shardsNb; shard++) {
            shardTermIndexes.push_back(unique_ptr<NindTermIndex>(
                new NindTermIndex(m_shardNames[shard], false, shardIdentifications[shard], specificsNumber)));
            if (shardTermIndexes.back()->getSpecificsSize() != specificsNumber * 4)
                throw NindIndexException("NindIndexMerge::merge unexpected term specifics : " + m_shardNames[shard]);
        }
        vector<ShardTerm> shardTerms;
        for (unsigned int shard = 0; shard < shardsNb; shard++) {
            const vector<unsigned int> &toMerged = shardToMerged[shard];
            for (size_t shardIdent = 1; shardIdent < toMerged.size(); shardIdent++)
                if (toMerged[shardIdent] != 0) shardTerms.push_back(ShardTerm(toMerged[shardIdent], shard, shardIdent));
        }
        sort(shardTerms.begin(), shardTerms.end());
        NindTermIndex termIndex(targetName, true, identification, specificsNumber,
                                m_options.termIndirectionBlocSize ? m_options.termIndirectionBlocSize : stats.wordsNb + 1);
        size_t first = 0;
        while (first < shardTerms.size()) {
            const unsigned int mergedIdent = shardTerms[first].mergedIdent;
            //une catejgorie grammaticale -> ses documents, tous shards confondus
            map<unsigned char, NindTermIndex::TermCG> byCg;
            size_t last = first;
            for (; last < shardTerms.size() && shardTerms[last].mergedIdent == mergedIdent; last++) {
                list<NindTermIndex::TermCG> shardDef;
                if (!shardTermIndexes[shardTerms[last].shard]->getTermDef(shardTerms[last].shardIdent, shardDef)) continue;
                for (list<NindTermIndex::TermCG>::iterator it = shardDef.begin(); it != shardDef.end(); it++) {
                    NindTermIndex::TermCG &termCG = byCg[(*it).cg];
                    termCG.cg = (*it).cg;
                    termCG.frequency += (*it).frequency;
                    termCG.documents.splice(termCG.documents.end(), (*it).documents);
                }
            }
            first = last;
            if (byCg.empty()) continue;
            list<NindTermIndex::TermCG> termDef;
            for (map<unsigned char, NindTermIndex::TermCG>::iterator it = byCg.begin(); it != byCg.end(); it++) {
                list<NindTermIndex::Document> &termDocuments = (*it).second.documents;
                termDocuments.sort(documentInfejrieur);
                for (list<NindTermIndex::Document>::const_iterator itdoc = termDocuments.begin(); itdoc != termDocuments.end(); itdoc++) {
                    list<NindTermIndex::Document>::const_iterator itnext = itdoc;
                    //documents disjoints entre shards : un doublon vient d'un fichier inverse incohejrent
                    if (++itnext != termDocuments.end() && (*itnext).ident == (*itdoc).ident)
                        throw NindIndexException("NindIndexMerge::merge document twice in a term : " + to_string((*itdoc).ident));
                }
                termDef.push_back(NindTermIndex::TermCG());
                swap(termDef.back(), (*it).second);
            }
            list<string> components;
            getUnionComponents(mergedToUnion[mergedIdent], components);
            countTerm(components, termDef);
            termIndex.setTermDef(mergedIdent, termDef, identification, getTermSpecifics());
            stats.termsNb++;
        }
        //les spejcifiques dejfinitifs, une fois tous les termes comptejs
        termIndex.setSpecificWords(getTermSpecifics(), identification);
    }

    //4) les index locaux : chaque document est recopiej avec ses termes renumejrotejs
    {
        NindLocalIndex localIndex(targetName, true, identification,
                                  m_options.localIndirectionBlocSize ? m_options.localIndirectionBlocSize : documents.size() + 1);
        for (size_t i = 0; i < documents.size(); i++) {
            const unsigned int shard = documents[i].second;
            const vector<unsigned int> &toMerged = shardToMerged[shard];
            list<NindLocalIndex::Term> localDef;
            if (!shardLocalIndexes[shard]->getLocalDef(documents[i].first, localDef)) continue;
            for (list<NindLocalIndex::Term>::iterator it = localDef.begin(); it != localDef.end(); it++) {
                if ((*it).term >= toMerged.size() || toMerged[(*it).term] == 0)
                    throw NindIndexException("NindIndexMerge::merge term not in lexicon : " + m_shardNames[shard]);
                (*it).term = toMerged[(*it).term];
            }
            localIndex.setLocalDef(documents[i].first, localDef, identification);
            stats.docsNb++;
        }
    }
    return stats;
}
////////////////////////////////////////////////////////////
//brief number of specific unsigned int of the term indexes (shards and merged) */
unsigned int NindIndexMerge::getTermSpecificsNumber() const
{
    return 0;
}
////////////////////////////////////////////////////////////
//brief called for every term written into the merged term index */
void NindIndexMerge::countTerm(const list<string> &,
                               const list<NindTermIndex::TermCG> &)
{
}
////////////////////////////////////////////////////////////
//brief specifics of the merged term index, once every term was counted */
list<unsigned int> NindIndexMerge::getTermSpecifics() const
{
    return list<unsigned int>();
}
////////////////////////////////////////////////////////////
//composants d'un mot de l'union (chaisne des prejfixes, toujours d'identifiants dejcroissants)
void NindIndexMerge::getUnionComponents(const unsigned int unionIdent,
                                        list<string> &components) const
{
    components.clear();
    unsigned int ident = unionIdent;
    while (ident != 0) {
        components.push_front(m_unionWords[ident].lastComponent);
        ident = m_unionWords[ident].prefix;
    }
}
////////////////////////////////////////////////////////////
