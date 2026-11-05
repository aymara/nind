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
        //la table est dimensionneje par les identifiants rejellement prejsents (bornejs par le fichier),
        //pas par le nombre de mots de l'identification, lu dans le fichier : corrompu, il ferait
        //allouer jusqu'ah 16 Go (et autant pour toMerged)
        unsigned int maxIdent = 0;
        for (size_t i = 0; i < entries.size(); i++)
            maxIdent = max(maxIdent, max(entries[i].ident, entries[i].prefixIdent));
        if (maxIdent > shardIdentifications[shard].lexiconWordsNb)
            throw NindIndexException("NindIndexMerge::merge word ident beyond the lexicon's words number : " + m_shardNames[shard]);
        toUnion.assign((size_t)maxIdent + 1, 0);
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

    //les index locaux sont facultatifs, mais tous les shards en ont ou aucun
    unsigned int localIndexesNb = 0;
    for (unsigned int shard = 0; shard < shardsNb; shard++)
        if (fileExists(m_shardNames[shard] + ".nindlocalindex")) localIndexesNb++;
    if (localIndexesNb != 0 && localIndexesNb != shardsNb)
        throw NindIndexException("NindIndexMerge::merge some shards have a local index, some don't");
    const bool withLocalIndex = (localIndexesNb != 0);

    //les documents de tous les shards, vejrifiejs disjoints avant toute ejcriture
    //(sans index local, un document dans deux shards est vu dans le fichier inverse)
    //les shards d'intervalles d'identifiants disjoints (le cas d'un dejcoupage en parties contiguës) :
    //leurs documents dans l'ordre des shards par premier document, sans tri global
    vector<unique_ptr<NindLocalIndex> > shardLocalIndexes;
    vector<vector<NindDocId> > shardDocuments(shardsNb);      //par ordre croissant (getDocIdents)
    for (unsigned int shard = 0; shard < shardsNb && withLocalIndex; shard++) {
        shardLocalIndexes.push_back(unique_ptr<NindLocalIndex>(
            new NindLocalIndex(m_shardNames[shard], false, shardIdentifications[shard])));
        shardLocalIndexes.back()->getDocIdents(shardDocuments[shard]);
    }
    vector<unsigned int> shardOrder;
    for (unsigned int shard = 0; shard < shardsNb; shard++)
        if (!shardDocuments[shard].empty()) shardOrder.push_back(shard);
    sort(shardOrder.begin(), shardOrder.end(), [&shardDocuments](const unsigned int a, const unsigned int b) {
        return shardDocuments[a].front() < shardDocuments[b].front(); });
    bool disjointRanges = true;
    for (size_t i = 1; i < shardOrder.size(); i++)
        if (shardDocuments[shardOrder[i]].front() <= shardDocuments[shardOrder[i - 1]].back()) disjointRanges = false;
    //sinon, (identifiant externe, shard) de tous les documents, trijs, vejrifiejs disjoints
    vector<pair<NindDocId, unsigned int> > documents;
    if (!disjointRanges) {
        for (unsigned int shard = 0; shard < shardsNb; shard++) {
            for (size_t i = 0; i < shardDocuments[shard].size(); i++) documents.push_back(make_pair(shardDocuments[shard][i], shard));
            vector<NindDocId>().swap(shardDocuments[shard]);
        }
        sort(documents.begin(), documents.end());
        for (size_t i = 1; i < documents.size(); i++) {
            if (documents[i].first == documents[i - 1].first)
                throw NindIndexException("NindIndexMerge::merge document in several shards : " + to_string(documents[i].first));
        }
    }
    size_t documentsNb = documents.size();
    for (unsigned int shard = 0; shard < shardsNb; shard++) documentsNb += shardDocuments[shard].size();

    //2) le lexique fusionnej, dimensionnej pour l'union
    NindLexiconIndex lexicon(targetName, true, m_options.withRetrolexicon,
                             m_options.lexiconIndirectionBlocSize ? m_options.lexiconIndirectionBlocSize : max(unionWordsNb, 1u),
                             m_options.retroIndirectionBlocSize ? m_options.retroIndirectionBlocSize : unionWordsNb + 1);
    vector<unsigned int> unionToMerged(unionWordsNb + 1, 0);
    vector<unsigned int> mergedToUnion(1, 0);
    {
        //tous les mots en une fois, le fichier neuf en mode massif
        vector<list<string> > words(unionWordsNb);
        for (unsigned int unionIdent = 1; unionIdent <= unionWordsNb; unionIdent++)
            getUnionComponents(unionIdent, words[unionIdent - 1]);
        vector<unsigned int> mergedIdents;
        lexicon.beginBulk();
        lexicon.addWords(words, mergedIdents);
        lexicon.endBulk();
        for (unsigned int unionIdent = 1; unionIdent <= unionWordsNb; unionIdent++) {
            const unsigned int mergedIdent = mergedIdents[unionIdent - 1];
            unionToMerged[unionIdent] = mergedIdent;
            if (mergedToUnion.size() <= mergedIdent) mergedToUnion.resize(mergedIdent + 1, 0);
            mergedToUnion[mergedIdent] = unionIdent;
        }
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
        termIndex.setDocumentLengths(m_options.documentLengths);
        termIndex.beginBulk();
        //une catejgorie grammaticale du terme fusionnej : ses documents, tous shards confondus, ah plat
        //(16 octets par document : un terme dans des centaines de millions de documents tient en mejmoire)
        struct MergedCategory {
            unsigned char cg;
            uint64_t frequency;
            vector<NindTermIndex::Document> documents;
            bool sorted;                //documents dejjah en ordre croissant (shards d'intervalles ordonnejs)
        };
        vector<MergedCategory> byCg;    //par catejgorie croissante ; rejutilisej d'un terme ah l'autre
        size_t categoriesNb = 0;
        vector<NindTermIndex::Document> shardPostings;
        vector<size_t> shardEnds;
        vector<pair<unsigned char, unsigned int> > shardCategories;
        vector<NindTermIndex::CategoryPostings> categories;
        size_t first = 0;
        while (first < shardTerms.size()) {
            const unsigned int mergedIdent = shardTerms[first].mergedIdent;
            categoriesNb = 0;
            size_t last = first;
            for (; last < shardTerms.size() && shardTerms[last].mergedIdent == mergedIdent; last++) {
                if (!shardTermIndexes[shardTerms[last].shard]->getTermPostings(shardTerms[last].shardIdent,
                                                                               shardPostings, shardEnds, shardCategories)) continue;
                for (size_t c = 0, begin = 0; c < shardCategories.size(); begin = shardEnds[c], c++) {
                    const unsigned char cg = shardCategories[c].first;
                    size_t k = 0;
                    while (k < categoriesNb && byCg[k].cg < cg) k++;
                    if (k == categoriesNb || byCg[k].cg != cg) {
                        //une nouvelle catejgorie, insejreje ah sa place (les vecteurs sont rejutilisejs)
                        if (categoriesNb == byCg.size()) byCg.push_back(MergedCategory());
                        for (size_t j = categoriesNb; j > k; j--) swap(byCg[j], byCg[j - 1]);
                        categoriesNb++;
                        byCg[k].cg = cg;
                        byCg[k].frequency = 0;
                        byCg[k].documents.clear();
                        byCg[k].sorted = true;
                    }
                    MergedCategory &category = byCg[k];
                    category.frequency += shardCategories[c].second;
                    if (begin == shardEnds[c]) continue;
                    if (!category.documents.empty() && shardPostings[begin].ident <= category.documents.back().ident)
                        category.sorted = false;
                    category.documents.insert(category.documents.end(), shardPostings.begin() + begin, shardPostings.begin() + shardEnds[c]);
                }
            }
            first = last;
            if (categoriesNb == 0) continue;
            categories.clear();
            for (size_t k = 0; k < categoriesNb; k++) {
                MergedCategory &category = byCg[k];
                vector<NindTermIndex::Document> &documents = category.documents;
                if (!category.sorted) {
                    sort(documents.begin(), documents.end(), documentInfejrieur);
                    //documents disjoints entre shards : un doublon vient d'un fichier inverse incohejrent
                    for (size_t d = 1; d < documents.size(); d++) {
                        if (documents[d].ident == documents[d - 1].ident)
                            throw NindIndexException("NindIndexMerge::merge document twice in a term : " + to_string(documents[d].ident));
                    }
                }
                if (category.frequency > 0xFFFFFFFFull)
                    throw NindIndexException("NindIndexMerge::merge term frequency over 32 bits : " + to_string(mergedIdent));
                categories.push_back(NindTermIndex::CategoryPostings(category.cg, (unsigned int)category.frequency,
                                                                     documents.data(), documents.size()));
            }
            list<string> components;
            getUnionComponents(mergedToUnion[mergedIdent], components);
            countTerm(components, categories);
            termIndex.setTermPostings(mergedIdent, categories, identification, getTermSpecifics());
            stats.termsNb++;
        }
        //les spejcifiques dejfinitifs, une fois tous les termes comptejs
        termIndex.setSpecificWords(getTermSpecifics(), identification);
        termIndex.endBulk();
    }

    //4) les index locaux : chaque document est recopiej avec ses termes renumejrotejs
    if (withLocalIndex) {
        NindLocalIndex localIndex(targetName, true, identification,
                                  m_options.localIndirectionBlocSize ? m_options.localIndirectionBlocSize : documentsNb + 1);
        localIndex.beginBulk();
        list<NindLocalIndex::Term> localDef;
        //un document du shard, recopiej
        auto copyDocument = [&](const NindDocId docIdent, const unsigned int shard) {
            const vector<unsigned int> &toMerged = shardToMerged[shard];
            if (!shardLocalIndexes[shard]->getLocalDef(docIdent, localDef)) return;
            for (list<NindLocalIndex::Term>::iterator it = localDef.begin(); it != localDef.end(); it++) {
                if ((*it).term >= toMerged.size() || toMerged[(*it).term] == 0)
                    throw NindIndexException("NindIndexMerge::merge term not in lexicon : " + m_shardNames[shard]);
                (*it).term = toMerged[(*it).term];
            }
            localIndex.setLocalDef(docIdent, localDef, identification);
            stats.docsNb++;
        };
        //par identifiant externe croissant
        if (disjointRanges) {
            for (size_t i = 0; i < shardOrder.size(); i++) {
                const vector<NindDocId> &docIdents = shardDocuments[shardOrder[i]];
                for (size_t d = 0; d < docIdents.size(); d++) copyDocument(docIdents[d], shardOrder[i]);
            }
        }
        else {
            for (size_t i = 0; i < documents.size(); i++) copyDocument(documents[i].first, documents[i].second);
        }
        localIndex.endBulk();
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
                               const vector<NindTermIndex::CategoryPostings> &)
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
