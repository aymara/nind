//
// C++ Implementation: NindIndexUpdate
//
// Description: Mise ah jour increjmentale d'un index nind (voir NindIndexUpdate.h)
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
#include "NindIndexUpdate.h"
#include "NindLexiconIndex.h"
#include "NindTermIndex.h"
#include "NindLocalIndex.h"
#include "NindIndexMerge.h"
#include "NindExceptions.h"
#include <algorithm>
#include <cstdio>
#include <memory>
#include <set>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
namespace {
const char *kExtensions[] = {".nindlexiconindex", ".nindretrolexicon", ".nindtermindex", ".nindlocalindex"};

bool fileExists(const string &fileName) {
    FILE *file = fopen(fileName.c_str(), "rb");
    if (file == 0) return false;
    fclose(file);
    return true;
}
}
////////////////////////////////////////////////////////////
NindIndexUpdate::NindIndexUpdate(const string &fileNameExtensionLess,
                                 const Options &options):
    m_fileName(fileNameExtensionLess),
    m_options(options),
    m_vocabulary(),
    m_vocabularyIndex(),
    m_documents(),
    m_lengths(),
    m_withLengths(false),
    m_done(false)
{
}
////////////////////////////////////////////////////////////
void NindIndexUpdate::setDocumentLengths(const vector<pair<NindDocId, unsigned int> > &lengths)
{
    m_lengths = lengths;
    m_withLengths = true;
}
////////////////////////////////////////////////////////////
uint32_t NindIndexUpdate::vocabularyIndex(const list<string> &word)
{
    const map<list<string>, uint32_t>::const_iterator it = m_vocabularyIndex.find(word);
    if (it != m_vocabularyIndex.end()) return it->second;
    const uint32_t index = m_vocabulary.size();
    m_vocabulary.push_back(word);
    m_vocabularyIndex[word] = index;
    return index;
}
////////////////////////////////////////////////////////////
void NindIndexUpdate::addDocument(const NindDocId ident,
                                  const vector<string> &words,
                                  const unsigned char cg,
                                  const long length)
{
    if (m_done) throw NindIndexException("NindIndexUpdate::addDocument after commit : " + m_fileName);
    Document &document = m_documents[ident];
    document = Document();
    document.removed = false;
    document.words.reserve(words.size());
    list<string> word(1, string());
    for (size_t i = 0; i < words.size(); i++) {
        word.front() = words[i];
        document.words.push_back(vocabularyIndex(word));
        document.cgs.push_back(cg);
        document.places.push_back(pair<unsigned int, unsigned int>((unsigned int)i, 1));
    }
    document.length = (length < 0) ? (unsigned int)words.size() : (unsigned int)length;
}
////////////////////////////////////////////////////////////
void NindIndexUpdate::addDocument(const NindDocId ident,
                                  const vector<Occurrence> &occurrences,
                                  const long length)
{
    if (m_done) throw NindIndexException("NindIndexUpdate::addDocument after commit : " + m_fileName);
    Document &document = m_documents[ident];
    document = Document();
    document.removed = false;
    for (size_t i = 0; i < occurrences.size(); i++) {
        if (occurrences[i].word.empty())
            throw NindIndexException("NindIndexUpdate::addDocument an occurrence without word : " + m_fileName);
        document.words.push_back(vocabularyIndex(occurrences[i].word));
        document.cgs.push_back(occurrences[i].cg);
        document.places.push_back(pair<unsigned int, unsigned int>(occurrences[i].position, occurrences[i].length));
    }
    document.length = (length < 0) ? (unsigned int)occurrences.size() : (unsigned int)length;
}
////////////////////////////////////////////////////////////
void NindIndexUpdate::removeDocument(const NindDocId ident)
{
    if (m_done) throw NindIndexException("NindIndexUpdate::removeDocument after commit : " + m_fileName);
    Document &document = m_documents[ident];
    document = Document();
    document.removed = true;
    document.length = 0;
}
////////////////////////////////////////////////////////////
//the index rebuilt by a merge onto itself, in the current format
void NindIndexUpdate::rebuild(const unsigned int lexiconBuckets)
{
    NindIndexMerge::Options options;
    options.withRetrolexicon = fileExists(m_fileName + ".nindretrolexicon");
    options.lexiconIndirectionBlocSize = lexiconBuckets;
    if (m_withLengths) options.documentLengths = m_lengths;
    const string rebuilt = m_fileName + ".rebuilt";
    for (size_t e = 0; e < 4; e++) remove((rebuilt + kExtensions[e]).c_str());
    NindIndexMerge(vector<string>(1, m_fileName), options).merge(rebuilt);
    for (size_t e = 0; e < 4; e++) {
        if (!fileExists(rebuilt + kExtensions[e])) continue;
        if (rename((rebuilt + kExtensions[e]).c_str(), (m_fileName + kExtensions[e]).c_str()) != 0)
            throw NindIndexException("NindIndexUpdate::rebuild can't replace " + m_fileName + kExtensions[e]);
    }
}
////////////////////////////////////////////////////////////
NindIndexUpdate::Stats NindIndexUpdate::commit()
{
    if (m_done) throw NindIndexException("NindIndexUpdate::commit twice : " + m_fileName);
    m_done = true;
    Stats stats;
    if (m_documents.empty()) return stats;
    const bool exists = fileExists(m_fileName + ".nindlexiconindex");
    const bool withLocalIndex = exists ? fileExists(m_fileName + ".nindlocalindex") : m_options.withLocalIndex;
    const bool withRetrolexicon = exists ? fileExists(m_fileName + ".nindretrolexicon") : m_options.withRetrolexicon;
    //the documents' lengths after the update
    if (m_withLengths || !exists) {
        map<NindDocId, unsigned int> lengths(m_lengths.begin(), m_lengths.end());
        for (map<NindDocId, Document>::const_iterator d = m_documents.begin(); d != m_documents.end(); d++) {
            if (d->second.removed || d->second.words.empty()) lengths.erase(d->first);
            else lengths[d->first] = d->second.length;
        }
        m_lengths.assign(lengths.begin(), lengths.end());
        m_withLengths = true;
    }
    else {
        //only the added documents' lengths are known
        m_lengths.clear();
        for (map<NindDocId, Document>::const_iterator d = m_documents.begin(); d != m_documents.end(); d++)
            if (!d->second.removed && !d->second.words.empty())
                m_lengths.push_back(pair<NindDocId, unsigned int>(d->first, d->second.length));
    }
    //1) an index of format v1: rebuilt in format v2 first
    unsigned int oldWordsNb = 0;
    unsigned int specificsNumber = 0;
    list<unsigned int> specifics;
    if (exists) {
        bool formatV2;
        {
            NindLexiconIndex lexicon(m_fileName, false, withRetrolexicon);
            formatV2 = lexicon.isFormatV2();
            oldWordsNb = lexicon.getIdentification().lexiconWordsNb;
        }
        if (!formatV2) {
            //(the lengths of the documents the update doesn't touch are needed for its blocks)
            const bool withLengths = m_withLengths;
            rebuild(max(m_options.lexiconMinBuckets, 2 * oldWordsNb));
            m_withLengths = withLengths;
            stats.migrated = true;
        }
        NindTermIndex reader(m_fileName, false, NindPadFile::Identification(0, 0), 0);
        specificsNumber = reader.getSpecificsSize() / 4;
        NindTermIndex withSpecifics(m_fileName, false, NindPadFile::Identification(0, 0), specificsNumber);
        withSpecifics.getSpecificWords(specifics);
    }
    const unsigned int vocabularySize = m_vocabulary.size();
    unsigned int wordsNb = 0, modulo = 0;
    {
    //2) the writers, term and local files opened with the lexicon's identification before the update
    NindLexiconIndex lexicon(m_fileName, true, withRetrolexicon,
                             max(m_options.lexiconMinBuckets, 2 * vocabularySize),
                             max(m_options.indirectionBlocMin, vocabularySize + 1));
    const NindPadFile::Identification oldIdentification = lexicon.getIdentification();
    NindTermIndex termIndex(m_fileName, true, oldIdentification, specificsNumber,
                            max(m_options.indirectionBlocMin, vocabularySize + 1));
    unique_ptr<NindLocalIndex> localIndex;
    if (withLocalIndex)
        localIndex.reset(new NindLocalIndex(m_fileName, true, oldIdentification,
                                            max(m_options.indirectionBlocMin, (unsigned int)m_documents.size() + 1)));
    //3) the terms the replaced and removed documents leave
    map<unsigned int, vector<NindDocId> > removed;
    if (exists) {
        if (localIndex) {
            for (map<NindDocId, Document>::const_iterator d = m_documents.begin(); d != m_documents.end(); d++) {
                set<unsigned int> termIdents;
                if (!localIndex->getTermIdents(d->first, termIdents)) continue;
                if (d->second.removed) stats.removedDocsNb++;
                for (set<unsigned int>::const_iterator t = termIdents.begin(); t != termIdents.end(); t++)
                    removed[*t].push_back(d->first);
            }
        }
        else {
            //no local index: every term's documents
            set<NindDocId> found;
            for (unsigned int t = 1; t <= oldWordsNb; t++) {
                list<NindTermIndex::TermCG> termDef;
                if (!termIndex.getTermDef(t, termDef)) continue;
                for (list<NindTermIndex::TermCG>::const_iterator cg = termDef.begin(); cg != termDef.end(); cg++)
                    for (list<NindTermIndex::Document>::const_iterator doc = cg->documents.begin(); doc != cg->documents.end(); doc++)
                        if (m_documents.count(doc->ident) != 0) {
                            removed[t].push_back(doc->ident);
                            found.insert(doc->ident);
                        }
            }
            for (set<NindDocId>::const_iterator d = found.begin(); d != found.end(); d++)
                if (m_documents[*d].removed) stats.removedDocsNb++;
        }
        for (map<unsigned int, vector<NindDocId> >::iterator t = removed.begin(); t != removed.end(); t++) {
            sort(t->second.begin(), t->second.end());
            t->second.erase(unique(t->second.begin(), t->second.end()), t->second.end());
        }
    }
    //4) bulk mode: nobody reads the files before the end
    lexicon.beginBulk();
    termIndex.beginBulk();
    if (localIndex) localIndex->beginBulk();
    //the new words, all at once
    vector<unsigned int> termIdents;
    lexicon.addWords(m_vocabulary, termIdents);
    const NindPadFile::Identification identification = lexicon.getIdentification();
    stats.newWordsNb = identification.lexiconWordsNb - oldIdentification.lexiconWordsNb;
    termIndex.setDocumentLengths(m_lengths);
    //5) the postings added, per term and category: (term, cg) -> document -> frequency
    map<pair<unsigned int, unsigned char>, vector<NindTermIndex::Document> > added;
    for (map<NindDocId, Document>::const_iterator d = m_documents.begin(); d != m_documents.end(); d++) {
        const Document &document = d->second;
        if (document.removed) continue;
        map<pair<unsigned int, unsigned char>, unsigned int> counts;
        for (size_t i = 0; i < document.words.size(); i++)
            counts[pair<unsigned int, unsigned char>(termIdents[document.words[i]], document.cgs[i])]++;
        for (map<pair<unsigned int, unsigned char>, unsigned int>::const_iterator c = counts.begin(); c != counts.end(); c++)
            added[c->first].push_back(NindTermIndex::Document(d->first, c->second));     //(documents in order)
        if (!document.words.empty()) stats.addedDocsNb++;
    }
    //every touched term, in increasing order: its removals with its first category's additions
    set<unsigned int> touched;
    for (map<unsigned int, vector<NindDocId> >::const_iterator t = removed.begin(); t != removed.end(); t++) touched.insert(t->first);
    for (map<pair<unsigned int, unsigned char>, vector<NindTermIndex::Document> >::const_iterator a = added.begin(); a != added.end(); a++)
        touched.insert(a->first.first);
    const vector<NindDocId> none;
    const vector<NindTermIndex::Document> nothing;
    for (set<unsigned int>::const_iterator t = touched.begin(); t != touched.end(); t++) {
        const map<unsigned int, vector<NindDocId> >::const_iterator r = removed.find(*t);
        const vector<NindDocId> &termRemoved = (r == removed.end()) ? none : r->second;
        map<pair<unsigned int, unsigned char>, vector<NindTermIndex::Document> >::const_iterator a =
            added.lower_bound(pair<unsigned int, unsigned char>(*t, 0));
        bool first = true, inPlace = true;
        for (; a != added.end() && a->first.first == *t; a++) {
            inPlace = termIndex.updateTermDef(*t, first ? termRemoved : none, a->first.second, a->second,
                                              identification, specifics) && inPlace;
            first = false;
        }
        if (first) inPlace = termIndex.updateTermDef(*t, termRemoved, 0, nothing, identification, specifics);
        stats.termsNb++;
        if (inPlace) stats.inPlaceTermsNb++;
    }
    //6) the local index: every added document (terms by increasing id, then category; their
    //localisations in occurrence order, by 255 at most), the removed ones erased
    if (localIndex) {
        for (map<NindDocId, Document>::const_iterator d = m_documents.begin(); d != m_documents.end(); d++) {
            const Document &document = d->second;
            map<pair<unsigned int, unsigned char>, vector<size_t> > byTerm;
            for (size_t i = 0; i < document.words.size(); i++)
                byTerm[pair<unsigned int, unsigned char>(termIdents[document.words[i]], document.cgs[i])].push_back(i);
            list<NindLocalIndex::Term> localDef;
            for (map<pair<unsigned int, unsigned char>, vector<size_t> >::const_iterator t = byTerm.begin(); t != byTerm.end(); t++) {
                for (size_t begin = 0; begin < t->second.size(); begin += 255) {
                    localDef.push_back(NindLocalIndex::Term(t->first.first, t->first.second));
                    for (size_t i = begin; i < min(begin + 255, t->second.size()); i++) {
                        const pair<unsigned int, unsigned int> &place = document.places[t->second[i]];
                        localDef.back().localisation.push_back(NindLocalIndex::Localisation(place.first, place.second));
                    }
                }
            }
            localIndex->setLocalDef(d->first, localDef, identification);
        }
    }
    if (localIndex) localIndex->endBulk();
    termIndex.endBulk();
    lexicon.endBulk();
    wordsNb = identification.lexiconWordsNb;
    modulo = lexicon.getModulo();
    }
    //7) a lexicon too full for its buckets (the writers closed): rebuilt with twice as many buckets as words
    if (wordsNb > (uint64_t)m_options.lexiconMaxLoad * modulo) {
        rebuild(2 * wordsNb);
        stats.rebuilt = true;
    }
    return stats;
}
////////////////////////////////////////////////////////////
