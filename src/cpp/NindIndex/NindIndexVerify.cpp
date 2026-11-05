//
// C++ Implementation: NindIndexVerify
//
// Description: Vejrification d'un index nind (voir NindIndexVerify.h)
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
#include "NindIndexVerify.h"
#include "NindLexiconIndex.h"
#include "NindTermIndex.h"
#include "NindLocalIndex.h"
#include "NindBasics/NindReadBuffer.h"
#include "NindExceptions.h"
#include <algorithm>
#include <cstdio>
#include <list>
#include <map>
#include <memory>
#include <sstream>
#include <tuple>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
namespace {
typedef NindTermIndex::TermCG TermCG;
typedef NindTermIndex::Document Document;
typedef NindLocalIndex::Term Term;
typedef NindLocalIndex::Localisation Localisation;
//(document, term, frequency)
typedef tuple<NindDocId, unsigned int, uint64_t> Triple;

bool fileExists(const string &fileName) {
    FILE *file = fopen(fileName.c_str(), "rb");
    if (file == 0) return false;
    fclose(file);
    return true;
}

class Problems {
public:
    Problems(NindIndexVerify::Report &report, const size_t maxProblems): m_report(report), m_max(maxProblems) {}
    void add(const string &problem) {
        m_report.problemsNb++;
        if (m_report.problems.size() < m_max) m_report.problems.push_back(problem);
    }
private:
    NindIndexVerify::Report &m_report;
    const size_t m_max;
};

string str(const uint64_t value) {
    ostringstream out;
    out << value;
    return out.str();
}

bool sameLocalDefs(const list<Term> &a, const list<Term> &b) {
    if (a.size() != b.size()) return false;
    for (list<Term>::const_iterator x = a.begin(), y = b.begin(); x != a.end(); x++, y++) {
        if (x->term != y->term || x->cg != y->cg || x->localisation.size() != y->localisation.size()) return false;
        for (list<Localisation>::const_iterator lx = x->localisation.begin(), ly = y->localisation.begin(); lx != x->localisation.end(); lx++, ly++)
            if (lx->position != ly->position || lx->length != ly->length) return false;
    }
    return true;
}

// a list read checked against the flat postings
bool sameAsPostings(const list<TermCG> &termDef, const vector<Document> &postings, const vector<size_t> &categoryEnds,
                    const vector<pair<unsigned char, unsigned int> > &categories) {
    if (termDef.size() != categories.size()) return false;
    size_t c = 0, p = 0;
    for (list<TermCG>::const_iterator cg = termDef.begin(); cg != termDef.end(); cg++, c++) {
        if (cg->cg != categories[c].first || cg->frequency != categories[c].second
            || cg->documents.size() != categoryEnds[c] - p) return false;
        for (list<Document>::const_iterator d = cg->documents.begin(); d != cg->documents.end(); d++, p++)
            if (d->ident != postings[p].ident || d->frequency != postings[p].frequency) return false;
    }
    return true;
}

// the list read paths are also checked on definitions of at most this many postings (a list takes
// ~48 bytes per posting; they decode the same blocks as the flat paths)
const size_t LIST_PATHS_MAX_POSTINGS = 1000000;

// one term's definition: decoded both ways, its lists checked; its triples for the cross check.
// Flat arrays: ~32 bytes per posting of the term, at most (hundreds of millions of postings)
void verifyTerm(NindTermIndex &termIndex, const unsigned int ident, const unsigned int wordsNb,
                NindReadBuffer &buffer, Problems &problems, NindIndexVerify::Report &report,
                vector<Triple> *triples, vector<Triple> *lengthClaims) {
    const string term = "term " + str(ident) + ": ";
    vector<Document> postings;
    vector<size_t> categoryEnds;
    vector<pair<unsigned char, unsigned int> > categories;
    bool found;
    try {
        found = termIndex.getTermPostings(ident, postings, categoryEnds, categories);
    }
    catch (const FileException &e) { problems.add(term + "doesn't decode: " + e.what()); return; }
    try {
        vector<Document> positioned;
        vector<size_t> positionedEnds;
        bool same = termIndex.getTermPostings(ident, positioned, positionedEnds, buffer) == found
            && positionedEnds == categoryEnds && positioned.size() == postings.size();
        for (size_t p = 0; same && p < postings.size(); p++)
            same = positioned[p].ident == postings[p].ident && positioned[p].frequency == postings[p].frequency;
        vector<Document>().swap(positioned);
        if (same && postings.size() <= LIST_PATHS_MAX_POSTINGS) {
            list<TermCG> sequentialList, positionedList;
            same = termIndex.getTermDef(ident, sequentialList) == found
                && termIndex.getTermDef(ident, positionedList, buffer) == found
                && sameAsPostings(sequentialList, postings, categoryEnds, categories)
                && sameAsPostings(positionedList, postings, categoryEnds, categories);
        }
        if (!same) problems.add(term + "the two read paths disagree");
    }
    catch (const FileException &e) { problems.add(term + "doesn't decode (const path): " + e.what()); }
    if (!found) return;
    report.termsNb++;
    if (ident == 0 || ident > wordsNb) problems.add(term + "a definition for a term the lexicon doesn't have");
    size_t begin = 0;
    for (size_t c = 0; c < categories.size(); begin = categoryEnds[c], c++) {
        uint64_t sum = 0;
        for (size_t p = begin; p < categoryEnds[c]; p++) {
            if (p > begin && postings[p].ident <= postings[p - 1].ident)
                problems.add(term + "category " + str(categories[c].first) + ": document ids not increasing at " + str(postings[p].ident));
            if (postings[p].frequency == 0) problems.add(term + "document " + str(postings[p].ident) + " with a frequency of 0");
            sum += postings[p].frequency;
            report.postingsNb++;
        }
        if (sum != categories[c].second)
            problems.add(term + "category " + str(categories[c].first) + ": frequency " + str(categories[c].second)
                         + ", its documents' sum " + str(sum));
    }
    if (triples != 0) {
        //per document, every category
        if (categories.size() == 1) {
            for (size_t p = 0; p < postings.size(); p++)
                triples->push_back(Triple(postings[p].ident, ident, postings[p].frequency));
        }
        else {
            map<NindDocId, uint64_t> frequencies;
            for (size_t p = 0; p < postings.size(); p++) frequencies[postings[p].ident] += postings[p].frequency;
            for (map<NindDocId, uint64_t>::const_iterator it = frequencies.begin(); it != frequencies.end(); it++)
                triples->push_back(Triple(it->first, ident, it->second));
        }
    }
    //format v2: the directory, then every block, one by one (each checked against its descriptor)
    if (!termIndex.isFormatV2()) return;
    try {
        NindTermIndex::TermDirectory directory;
        if (!termIndex.getTermDirectory(ident, directory, buffer)) {
            problems.add(term + "no directory");
            return;
        }
        if (directory.categories.size() != categories.size()) {
            problems.add(term + "the directory's categories aren't the definition's");
            return;
        }
        vector<Document> blockPostings;
        begin = 0;
        for (size_t c = 0; c < directory.categories.size(); begin = categoryEnds[c], c++) {
            const NindTermIndex::TermDirectory::Category &category = directory.categories[c];
            const size_t docsNb = categoryEnds[c] - begin;
            const unsigned char cg = categories[c].first;
            bool same = category.cg == cg && category.docsNb == docsNb && category.inlinePostings.size() <= docsNb;
            size_t p = begin;
            for (size_t i = 0; same && i < category.inlinePostings.size(); i++, p++)
                same = category.inlinePostings[i].ident == postings[p].ident
                    && category.inlinePostings[i].frequency == postings[p].frequency;
            for (size_t b = 0; b < category.blocks.size(); b++) {
                blockPostings.clear();
                termIndex.getTermBlocks(ident, directory, c, b, b + 1, blockPostings, buffer);
                //(document, term, the block's shortest length): checked against the local index
                if (lengthClaims != 0 && category.blocks[b].minLength != 0)
                    for (size_t i = 0; i < blockPostings.size(); i++)
                        lengthClaims->push_back(Triple(blockPostings[i].ident, ident, category.blocks[b].minLength));
                if (blockPostings.size() > categoryEnds[c] - p) same = false;
                for (size_t i = 0; same && i < blockPostings.size(); i++, p++)
                    same = blockPostings[i].ident == postings[p].ident && blockPostings[i].frequency == postings[p].frequency;
            }
            if (p != categoryEnds[c]) same = false;
            if (!same) problems.add(term + "category " + str(cg) + ": its blocks aren't its postings");
            if (category.blocks.empty() && category.inlinePostings.size() > NindTermIndex::BLOCK_SIZE)
                problems.add(term + "category " + str(cg) + ": a long list without blocks");
        }
    }
    catch (const FileException &e) { problems.add(term + "a block doesn't match its descriptor: " + e.what()); }
}
}
////////////////////////////////////////////////////////////
NindIndexVerify::Report NindIndexVerify::verify(const string &fileNameExtensionLess,
                                                const bool crossCheck,
                                                const size_t maxProblems)
{
    Report report;
    Problems problems(report, maxProblems);
    //the lexicon
    NindPadFile::Identification identification;
    try {
        const bool withRetrolexicon = fileExists(fileNameExtensionLess + ".nindretrolexicon");
        NindLexiconIndex lexicon(fileNameExtensionLess, false, withRetrolexicon);
        report.formatV2 = lexicon.isFormatV2();
        identification = lexicon.getIdentification();
        report.wordsNb = identification.lexiconWordsNb;
        vector<NindLexiconIndex::Entry> entries;
        lexicon.getEntries(entries);
        //every ident from 1 to the words number, once (getEntries: increasing idents, no duplicate)
        if (entries.size() != report.wordsNb || (!entries.empty() && entries.back().ident != report.wordsNb))
            problems.add("lexicon: " + str(entries.size()) + " words, " + str(report.wordsNb) + " in its identification");
    }
    catch (const FileException &e) {
        problems.add(string("lexicon: ") + e.what());
        return report;
    }
    NindReadBuffer buffer;
    vector<Triple> termTriples, localTriples, lengthClaims;
    map<NindDocId, uint64_t> localLengths;          //(cross check) per document, its localisations
    //the term index
    try {
        NindTermIndex termIndex(fileNameExtensionLess, false, identification, 0);
        if (termIndex.isFormatV2() != report.formatV2) problems.add("term index: not the lexicon's format");
        const unsigned int maxIdent = max(termIndex.getMaxIdentAt(), report.wordsNb + 1);
        for (unsigned int ident = 1; ident < maxIdent; ident++)
            verifyTerm(termIndex, ident, report.wordsNb, buffer, problems, report, crossCheck ? &termTriples : 0,
                       crossCheck ? &lengthClaims : 0);
    }
    catch (const FileException &e) { problems.add(string("term index: ") + e.what()); }
    //the local index
    report.withLocalIndex = fileExists(fileNameExtensionLess + ".nindlocalindex");
    if (report.withLocalIndex) {
        try {
            NindLocalIndex localIndex(fileNameExtensionLess, false, identification);
            if (localIndex.isFormatV2() != report.formatV2) problems.add("local index: not the lexicon's format");
            vector<NindDocId> docIdents;
            localIndex.getDocIdents(docIdents);
            report.documentsNb = docIdents.size();
            for (size_t i = 0; i < docIdents.size(); i++) {
                const string document = "document " + str(docIdents[i]) + ": ";
                list<Term> sequential, positioned;
                try {
                    if (!localIndex.getLocalDef(docIdents[i], sequential)) {
                        problems.add(document + "listed, but not found");
                        continue;
                    }
                }
                catch (const FileException &e) { problems.add(document + "doesn't decode: " + e.what()); continue; }
                try {
                    if (!localIndex.getLocalDef(docIdents[i], positioned, buffer) || !sameLocalDefs(sequential, positioned))
                        problems.add(document + "the two read paths disagree");
                }
                catch (const FileException &e) { problems.add(document + "doesn't decode (const path): " + e.what()); }
                map<unsigned int, uint64_t> frequencies;
                for (list<Term>::const_iterator t = sequential.begin(); t != sequential.end(); t++) {
                    if (t->term == 0 || t->term > report.wordsNb)
                        problems.add(document + "term " + str(t->term) + " not in the lexicon");
                    frequencies[t->term] += t->localisation.size();
                }
                if (crossCheck)
                    for (map<unsigned int, uint64_t>::const_iterator it = frequencies.begin(); it != frequencies.end(); it++) {
                        localTriples.push_back(Triple(docIdents[i], it->first, it->second));
                        localLengths[docIdents[i]] += it->second;
                    }
            }
        }
        catch (const FileException &e) { problems.add(string("local index: ") + e.what()); }
    }
    //the cross check: a block's shortest length is at most each of its documents' length (a
    //document's length: its localisations; above, a top-k search could skip a block it needs)
    if (crossCheck && report.withLocalIndex) {
        for (size_t i = 0; i < lengthClaims.size(); i++) {
            const map<NindDocId, uint64_t>::const_iterator it = localLengths.find(get<0>(lengthClaims[i]));
            if (it != localLengths.end() && get<2>(lengthClaims[i]) > it->second)
                problems.add("term " + str(get<1>(lengthClaims[i])) + ": a block's shortest length " + str(get<2>(lengthClaims[i]))
                             + ", but document " + str(get<0>(lengthClaims[i])) + " has " + str(it->second));
        }
    }
    //the cross check: the same triples, both ways
    if (crossCheck && report.withLocalIndex) {
        sort(termTriples.begin(), termTriples.end());
        sort(localTriples.begin(), localTriples.end());
        size_t i = 0, j = 0;
        while (i < termTriples.size() || j < localTriples.size()) {
            if (j == localTriples.size() || (i < termTriples.size() && termTriples[i] < localTriples[j])) {
                problems.add("document " + str(get<0>(termTriples[i])) + ", term " + str(get<1>(termTriples[i]))
                             + ": frequency " + str(get<2>(termTriples[i])) + " in the term index, not so in the local index");
                i++;
            }
            else if (i == termTriples.size() || localTriples[j] < termTriples[i]) {
                problems.add("document " + str(get<0>(localTriples[j])) + ", term " + str(get<1>(localTriples[j]))
                             + ": frequency " + str(get<2>(localTriples[j])) + " in the local index, not so in the term index");
                j++;
            }
            else { i++; j++; }
        }
    }
    return report;
}
////////////////////////////////////////////////////////////
