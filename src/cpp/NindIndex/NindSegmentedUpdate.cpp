//
// C++ Implementation: NindSegmentedUpdate
//
// Description: modification d'un index en segments ; voir NindSegmentedUpdate.h
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
#include "NindSegmentedUpdate.h"
#include "NindDocNames.h"
#include "NindExceptions.h"
#include "NindIndexBuilder.h"
#include "NindIndexMerge.h"
#include "NindLexiconIndex.h"
#include "NindLocalIndex.h"
#include "NindSegmentedIndex.h"
#include "NindTermIndex.h"
#include "NindBasics/NindReadBuffer.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <set>
#include <unordered_map>
#include <stdexcept>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
namespace {
const char *SEGMENT_EXTENSIONS[] = {".nindlexiconindex", ".nindretrolexicon", ".nindtermindex", ".nindlocalindex",
                                    ".ninddoclengths", ".ninddocnames", ".ninddeletes"};

bool fileExists(const string &fileName)
{
    FILE *file = fopen(fileName.c_str(), "rb");
    if (file == 0) return false;
    fclose(file);
    return true;
}
string directoryOf(const string &base)
{
    return base.substr(0, base.find_last_of('/'));
}
void makeDirectory(const string &directory)
{
#ifdef _WIN32
    const int error = _mkdir(directory.c_str());
#else
    const int error = mkdir(directory.c_str(), 0777);
#endif
    if (error != 0) throw OpenFileException(directory);
}
void removeSegment(const string &segmentBase)
{
    for (unsigned int e = 0; e < sizeof(SEGMENT_EXTENSIONS) / sizeof(SEGMENT_EXTENSIONS[0]); e++)
        remove((segmentBase + SEGMENT_EXTENSIONS[e]).c_str());
#ifdef _WIN32
    _rmdir(directoryOf(segmentBase).c_str());
#else
    rmdir(directoryOf(segmentBase).c_str());
#endif
}
// a name given once, in this index and in every generation
string newSegmentName(const vector<string> &segments)
{
    unsigned long number = 0;
    for (size_t s = 0; s < segments.size(); s++) {
        unsigned long n = 0;
        if (sscanf(segments[s].c_str(), "seg-%lu-", &n) == 1) number = max(number, n);
    }
    random_device random;
    char name[64];
    snprintf(name, sizeof(name), "seg-%06lu-%08x", number + 1, (unsigned int)random());
    return name;
}
// the postings of documents in a term, per category, without a local index: its directory, then
// only the blocks that may hold them
void countTermPostings(const NindTermIndex &terms, const unsigned int term, const vector<NindDocId> &documents,
                       NindReadBuffer &buffer, map<unsigned int, uint32_t> &counts)
{
    NindTermIndex::TermDirectory directory;
    if (!terms.getTermDirectory(term, directory, buffer)) return;
    vector<NindTermIndex::Document> postings;
    uint32_t count = 0;
    for (size_t c = 0; c < directory.categories.size(); c++) {
        const NindTermIndex::TermDirectory::Category &category = directory.categories[c];
        for (size_t i = 0; i < category.inlinePostings.size(); i++)
            if (binary_search(documents.begin(), documents.end(), category.inlinePostings[i].ident)) count++;
        size_t b = 0;
        for (size_t d = 0; d < documents.size() && b < category.blocks.size(); d++) {
            //the block that may hold document d
            b = lower_bound(category.blocks.begin() + b, category.blocks.end(), documents[d],
                            [](const NindTermIndex::Block &block, const NindDocId id) { return block.last < id; })
                - category.blocks.begin();
            if (b == category.blocks.size() || category.blocks[b].first > documents[d]) continue;
            postings.clear();
            terms.getTermBlocks(term, directory, c, b, b + 1, postings, buffer);
            //(every deleted document of this block)
            for (size_t p = 0; p < postings.size(); p++)
                if (binary_search(documents.begin(), documents.end(), postings[p].ident)) count++;
            while (d + 1 < documents.size() && documents[d + 1] <= category.blocks[b].last) d++;
        }
    }
    if (count != 0) counts[term] += count;
}
// adds documents to the deleted ones of a segment: their file replaced
void deleteDocuments(const NindSegmentedIndex::SegmentInfo &segment, const vector<pair<NindDocId, size_t> > &documents)
{
    const string fileName = segment.base + ".ninddeletes";
    NindSegmentDeletes deletes;
    deletes.read(fileName);
    map<unsigned int, uint32_t> counts(deletes.termCounts.begin(), deletes.termCounts.end());
    NindLexiconIndex lexicon(segment.base, false);
    const NindPadFile::Identification identification = lexicon.getIdentification();
    NindReadBuffer buffer;
    if (fileExists(segment.base + ".nindlocalindex")) {
        NindLocalIndex local(segment.base, false, identification);
        list<NindLocalIndex::Term> localDef;
        set<pair<unsigned int, unsigned char> > terms;
        for (size_t d = 0; d < documents.size(); d++) {
            //(by increasing document in a segment's files: its internal id is its place + 1)
            if (!local.getLocalDefAt(documents[d].first, (unsigned int)documents[d].second + 1, localDef, buffer))
                throw NindIndexException("NindSegmentedUpdate::commit document not in its local index : " + segment.base);
            terms.clear();
            for (list<NindLocalIndex::Term>::const_iterator t = localDef.begin(); t != localDef.end(); t++)
                terms.insert(make_pair((*t).term, (*t).cg));
            for (set<pair<unsigned int, unsigned char> >::const_iterator t = terms.begin(); t != terms.end(); t++)
                counts[t->first]++;
        }
    }
    else {
        //no local index: every term's directory
        vector<NindDocId> ids;
        for (size_t d = 0; d < documents.size(); d++) ids.push_back(documents[d].first);
        sort(ids.begin(), ids.end());
        NindTermIndex terms(segment.base, false, identification, 0);
        const unsigned int wordsNb = identification.lexiconWordsNb;
        for (unsigned int term = 1; term <= wordsNb; term++) countTermPostings(terms, term, ids, buffer, counts);
    }
    for (size_t d = 0; d < documents.size(); d++) deletes.docIds.push_back(documents[d].first);
    sort(deletes.docIds.begin(), deletes.docIds.end());
    deletes.termCounts.assign(counts.begin(), counts.end());
    //(the name may be a hard link to the file of an earlier generation, which stays as it is)
    remove(fileName.c_str());
    deletes.write(fileName);
}
// the new segment of the documents (their words: ids in words), words in the lexicon by increasing
// bytes
void writeSegment(const string &segmentBase, const map<NindDocId, vector<uint32_t> const *> &documents,
                  const vector<string> &words, const vector<string> &names, const bool withLocalIndex)
{
    //the words these documents use (not those of a document replaced or removed since), sorted
    vector<uint32_t> used;
    {
        vector<bool> seen(words.size(), false);
        for (map<NindDocId, vector<uint32_t> const *>::const_iterator d = documents.begin(); d != documents.end(); d++)
            for (size_t i = 0; i < d->second->size(); i++) {
                const uint32_t word = (*d->second)[i];
                if (!seen[word]) {
                    seen[word] = true;
                    used.push_back(word);
                }
            }
        sort(used.begin(), used.end(), [&words](const uint32_t a, const uint32_t b) { return words[a] < words[b]; });
    }
    NindLexiconIndex lexicon(segmentBase, true, false, max((unsigned int)used.size(), 1u));
    vector<list<string> > lexiconWords;
    lexiconWords.reserve(used.size());
    for (size_t w = 0; w < used.size(); w++) lexiconWords.push_back(list<string>(1, words[used[w]]));
    vector<unsigned int> idents;
    lexicon.beginBulk();
    lexicon.addWords(lexiconWords, idents);
    lexicon.endBulk();
    vector<list<string> >().swap(lexiconWords);
    const NindPadFile::Identification identification = lexicon.getIdentification();
    //update's word id -> term id: an array, no hashing per token
    vector<uint32_t> termIds(words.size(), 0);
    for (size_t w = 0; w < used.size(); w++) termIds[used[w]] = idents[w];
    NindIndexBuilder builder;
    vector<NindDocId> docIds;
    vector<uint32_t> lengths, tokens;
    for (map<NindDocId, vector<uint32_t> const *>::const_iterator d = documents.begin(); d != documents.end(); d++) {
        tokens.resize(d->second->size());
        for (size_t i = 0; i < tokens.size(); i++) tokens[i] = termIds[(*d->second)[i]];
        builder.addDocument(d->first, tokens.data(), tokens.size());
        docIds.push_back(d->first);
        lengths.push_back((uint32_t)tokens.size());
    }
    {
        NindTermIndex terms(segmentBase, true, identification, 0, (unsigned int)used.size() + 1);
        terms.beginBulk();
        builder.writeTermIndex(terms, identification);
        terms.endBulk();
    }
    if (withLocalIndex) {
        NindLocalIndex local(segmentBase, true, identification, (unsigned int)documents.size() + 1);
        local.beginBulk();
        builder.writeLocalIndex(local, identification);
        local.endBulk();
    }
    NindDocLengths::write(segmentBase + ".ninddoclengths", docIds, lengths);
    bool named = false;
    for (size_t d = 0; d < names.size(); d++) named = named || !names[d].empty();
    if (named) {
        vector<NindDocId> namedIds;
        vector<string> namedNames;
        for (size_t d = 0; d < names.size(); d++) {
            if (names[d].empty()) continue;
            namedIds.push_back(docIds[d]);
            namedNames.push_back(names[d]);
        }
        NindDocNames::write(segmentBase + ".ninddocnames", namedIds, namedNames);
    }
}
// merges these segments into a new one, without their deleted documents; the segments' list updated
void mergeSegments(const string &base, vector<string> &segments, const vector<size_t> &chosen,
                   NindSegmentedUpdate::Stats &stats)
{
    vector<string> shardBases;
    NindIndexMerge::Options options;
    vector<pair<NindDocId, uint32_t> > documents;
    vector<string> nameFiles;
    vector<vector<NindDocId> > nameDeletes;
    for (size_t c = 0; c < chosen.size(); c++) {
        const string shard = NindSegmentedIndex::segmentBase(base, segments[chosen[c]]);
        NindSegmentDeletes deletes;
        deletes.read(shard + ".ninddeletes");
        vector<NindDocId> docIds;
        vector<uint32_t> lengths;
        if (!NindDocLengths::read(shard + ".ninddoclengths", docIds, lengths)) {
            //an index written before lengths were stored apart: from its local index
            const NindSegmentedIndex old(shard);
            old.getDocuments(docIds, lengths);
        }
        for (size_t d = 0; d < docIds.size(); d++)
            if (!binary_search(deletes.docIds.begin(), deletes.docIds.end(), docIds[d]))
                documents.push_back(make_pair(docIds[d], lengths[d]));
        shardBases.push_back(shard);
        if (fileExists(shard + ".ninddocnames")) {
            nameFiles.push_back(shard + ".ninddocnames");
            nameDeletes.push_back(deletes.docIds);
        }
        options.deletedDocuments.push_back(deletes.docIds);
    }
    sort(documents.begin(), documents.end());
    const string name = newSegmentName(segments);
    if (documents.empty()) {
        //nothing left in them
        vector<string> kept;
        set<size_t> gone(chosen.begin(), chosen.end());
        for (size_t s = 0; s < segments.size(); s++) if (gone.count(s) == 0) kept.push_back(segments[s]);
        segments.swap(kept);
        for (size_t c = 0; c < shardBases.size(); c++) removeSegment(shardBases[c]);
        stats.mergesNb++;
        return;
    }
    const string target = NindSegmentedIndex::segmentBase(base, name);
    makeDirectory(directoryOf(target));
    vector<NindDocId> docIds(documents.size());
    vector<uint32_t> lengths(documents.size());
    options.documentLengths.reserve(documents.size());
    for (size_t d = 0; d < documents.size(); d++) {
        docIds[d] = documents[d].first;
        lengths[d] = documents[d].second;
        options.documentLengths.push_back(make_pair(documents[d].first, (unsigned int)documents[d].second));
    }
    vector<pair<NindDocId, uint32_t> >().swap(documents);
    try {
        NindIndexMerge(shardBases, options).merge(target);
        NindDocLengths::write(target + ".ninddoclengths", docIds, lengths);
        if (!nameFiles.empty()) NindDocNames::merge(nameFiles, nameDeletes, target + ".ninddocnames");
    }
    catch (...) {
        removeSegment(target);
        throw;
    }
    //the new segment takes the place of the first one
    vector<string> kept;
    set<size_t> gone(chosen.begin(), chosen.end());
    for (size_t s = 0; s < segments.size(); s++) {
        if (s == chosen.front()) kept.push_back(name);
        else if (gone.count(s) == 0) kept.push_back(segments[s]);
    }
    segments.swap(kept);
    for (size_t c = 0; c < shardBases.size(); c++) removeSegment(shardBases[c]);
    stats.mergesNb++;
    stats.mergedDocsNb += docIds.size();
}
// the first count of a side file: <magic (8 bytes, its first magicSize given)> <count (Entier4)>;
// false if there is no file
bool headerCount(const string &fileName, const char *magic, const size_t magicSize, uint64_t &count)
{
    FILE *file = fopen(fileName.c_str(), "rb");
    if (file == 0) return false;
    unsigned char header[12];
    const bool ok = fread(header, 1, 12, file) == 12 && memcmp(header, magic, magicSize) == 0;
    fclose(file);
    if (!ok) throw FormatFileException(fileName);
    count = (uint64_t)header[8] | ((uint64_t)header[9] << 8) | ((uint64_t)header[10] << 16) | ((uint64_t)header[11] << 24);
    return true;
}
// the documents and deleted documents of each segment, as NindSegmentedIndex::segments() counts
// them, from their files' headers: opening the index would read every segment's lengths and
// lexicon (a commit's merge policy needs nothing else)
vector<NindSegmentedIndex::SegmentInfo> segmentCounts(const string &base, const vector<string> &segments)
{
    vector<NindSegmentedIndex::SegmentInfo> infos(segments.size());
    for (size_t s = 0; s < segments.size(); s++) {
        const string shard = NindSegmentedIndex::segmentBase(base, segments[s]);
        infos[s].name = segments[s];
        infos[s].base = shard;
        //lengths: their own count (NINDDL02 or NINDDL01); without (an older index): opened
        if (!headerCount(shard + ".ninddoclengths", "NINDDL0", 7, infos[s].docsNb)) {
            infos[s].docsNb = NindSegmentedIndex(shard).segments()[0].docsNb;
        }
        infos[s].deletedNb = 0;
        headerCount(shard + ".ninddeletes", "NINDDE01", 8, infos[s].deletedNb);
    }
    return infos;
}
// what the tiered policy merges now (empty: nothing)
vector<size_t> segmentsToMerge(const vector<NindSegmentedIndex::SegmentInfo> &infos, const unsigned int mergeFactor,
                               const uint64_t maxMergedDocs)
{
    map<unsigned int, vector<size_t> > tiers;
    for (size_t s = 0; s < infos.size(); s++) {
        const uint64_t live = infos[s].docsNb - infos[s].deletedNb;
        //over half deleted: rewritten alone
        if (infos[s].deletedNb * 2 > infos[s].docsNb) return vector<size_t>(1, s);
        //at half the limit, a segment is full: without this, such segments would pile up in a tier
        //that can't be merged, and two others always fit under the limit
        if (maxMergedDocs != 0 && live * 2 >= maxMergedDocs) continue;
        unsigned int tier = 0;
        for (uint64_t limit = mergeFactor; live >= limit; limit *= mergeFactor) tier++;
        tiers[tier].push_back(s);
    }
    for (map<unsigned int, vector<size_t> >::const_iterator t = tiers.begin(); t != tiers.end(); t++) {
        if (t->second.size() < mergeFactor) continue;
        //its smallest segments that fit under the limit (all of them, most of the time)
        vector<size_t> chosen = t->second;
        stable_sort(chosen.begin(), chosen.end(), [&infos](const size_t a, const size_t b) {
            return infos[a].docsNb - infos[a].deletedNb < infos[b].docsNb - infos[b].deletedNb; });
        uint64_t live = 0;
        size_t kept = 0;
        for (; kept < chosen.size(); kept++) {
            live += infos[chosen[kept]].docsNb - infos[chosen[kept]].deletedNb;
            if (maxMergedDocs != 0 && live > maxMergedDocs) break;
        }
        chosen.resize(kept);
        //(mergeSegments puts the new segment at the first one's place, in manifest order)
        sort(chosen.begin(), chosen.end());
        if (chosen.size() >= 2) return chosen;
    }
    return vector<size_t>();
}
}
////////////////////////////////////////////////////////////
NindSegmentedUpdate::NindSegmentedUpdate(const string &base,
                                         const Options &options):
    m_base(base),
    m_options(options),
    m_documents(),
    m_words(),
    m_wordIds()
{
}
////////////////////////////////////////////////////////////
NindSegmentedUpdate::~NindSegmentedUpdate()
{
}
////////////////////////////////////////////////////////////
void NindSegmentedUpdate::addDocument(const NindDocId ident,
                                      const vector<string> &words,
                                      const string &name)
{
    Document &document = m_documents[ident];
    document.words.resize(words.size());
    for (size_t i = 0; i < words.size(); i++) document.words[i] = wordId(words[i]);
    document.name = name;
}
////////////////////////////////////////////////////////////
void NindSegmentedUpdate::addDocuments(const vector<NindDocId> &idents,
                                       const uint32_t *ends,
                                       const uint32_t *localIds,
                                       const size_t wordsNb,
                                       const vector<string> &vocabulary,
                                       const vector<string> &names)
{
    if (names.size() != idents.size()) throw invalid_argument("addDocuments: as many names as documents expected");
    uint32_t previous = 0;
    for (size_t d = 0; d < idents.size(); d++) {
        if (ends[d] < previous || ends[d] > wordsNb)
            throw invalid_argument("addDocuments: ends must be ascending and within the words");
        previous = ends[d];
    }
    if (previous != wordsNb) throw invalid_argument("addDocuments: the last end must be the number of words");
    for (size_t i = 0; i < wordsNb; i++)
        if (localIds[i] >= vocabulary.size()) throw invalid_argument("addDocuments: a local id beyond the vocabulary");
    vector<uint32_t> ids(vocabulary.size());
    for (size_t v = 0; v < vocabulary.size(); v++) ids[v] = wordId(vocabulary[v]);
    for (size_t d = 0, begin = 0; d < idents.size(); begin = ends[d], d++) {
        Document &document = m_documents[idents[d]];
        document.words.resize(ends[d] - begin);
        for (size_t i = begin; i < ends[d]; i++) document.words[i - begin] = ids[localIds[i]];
        document.name = names[d];
    }
}
////////////////////////////////////////////////////////////
void NindSegmentedUpdate::addBatch(const vector<NindDocId> &idents,
                                   const uint32_t *ends,
                                   const uint32_t *localIds,
                                   const size_t tokensNb,
                                   const char *words,
                                   const size_t wordsBytes,
                                   const uint32_t *wordEnds,
                                   const size_t wordsNb,
                                   const vector<string> &names)
{
    vector<string> vocabulary(wordsNb);
    for (size_t w = 0, begin = 0; w < wordsNb; begin = wordEnds[w], w++) {
        if (wordEnds[w] < begin || wordEnds[w] > wordsBytes) throw invalid_argument("addBatch: word ends ascending, within the words");
        vocabulary[w].assign(words + begin, wordEnds[w] - begin);
    }
    addDocuments(idents, ends, localIds, tokensNb, vocabulary, names);
}
////////////////////////////////////////////////////////////
uint32_t NindSegmentedUpdate::wordId(const string &word)
{
    const pair<unordered_map<string, uint32_t>::iterator, bool> found =
        m_wordIds.insert(make_pair(word, (uint32_t)m_words.size()));
    if (found.second) {
        if (m_words.size() == 0xFFFFFFFFu) throw invalid_argument("NindSegmentedUpdate: too many distinct words");
        m_words.push_back(word);
    }
    return found.first->second;
}
////////////////////////////////////////////////////////////
void NindSegmentedUpdate::removeDocument(const NindDocId ident)
{
    Document &document = m_documents[ident];
    document.words.clear();
    document.name.clear();
}
////////////////////////////////////////////////////////////
bool NindSegmentedUpdate::getMaxDocIdent(const string &base,
                                         NindDocId &docId)
{
    const vector<string> segments = NindSegmentedIndex::readManifest(base);
    bool found = false;
    for (size_t s = 0; s < segments.size(); s++) {
        const string shard = NindSegmentedIndex::segmentBase(base, segments[s]);
        NindDocId last = 0;
        bool here = false;
        if (fileExists(shard + ".ninddoclengths")) here = NindDocLengths::readLast(shard + ".ninddoclengths", last);
        else here = NindSegmentedIndex(shard).getMaxDocIdent(last);
        if (here && (!found || last > docId)) docId = last;
        found = found || here;
    }
    return found;
}
////////////////////////////////////////////////////////////
NindSegmentedUpdate::Stats NindSegmentedUpdate::commit()
{
    Stats stats;
    vector<string> segments = NindSegmentedIndex::readManifest(m_base);
    //1) the replaced and removed documents, masked in their segments: only documents the index may
    //have (a new id, above its largest, costs nothing)
    NindDocId maxIdent = 0;
    const bool any = getMaxDocIdent(m_base, maxIdent);
    if (any && !m_documents.empty() && m_documents.begin()->first <= maxIdent) {
        const NindSegmentedIndex index(m_base);
        const vector<NindSegmentedIndex::SegmentInfo> infos = index.segments();
        map<size_t, vector<pair<NindDocId, size_t> > > bySegment;
        for (map<NindDocId, Document>::const_iterator d = m_documents.begin(); d != m_documents.end(); d++) {
            size_t segment = 0, position = 0;
            if (d->first > maxIdent || !index.locate(d->first, segment, position)) continue;
            bySegment[segment].push_back(make_pair(d->first, position));
            stats.removedDocsNb++;
        }
        for (map<size_t, vector<pair<NindDocId, size_t> > >::const_iterator s = bySegment.begin(); s != bySegment.end(); s++)
            deleteDocuments(infos[s->first], s->second);
    }
    //2) the new segment
    map<NindDocId, vector<uint32_t> const *> documents;
    vector<string> names;
    for (map<NindDocId, Document>::const_iterator d = m_documents.begin(); d != m_documents.end(); d++) {
        if (d->second.words.empty()) continue;
        documents[d->first] = &d->second.words;
        names.push_back(d->second.name);
    }
    if (!documents.empty()) {
        const string name = newSegmentName(segments);
        const string segmentBase = NindSegmentedIndex::segmentBase(m_base, name);
        makeDirectory(directoryOf(segmentBase));
        writeSegment(segmentBase, documents, m_words, names, m_options.withLocalIndex);
        segments.push_back(name);
        stats.addedDocsNb = documents.size();
    }
    NindSegmentedIndex::writeManifest(m_base, segments);
    //3) the merges
    while (m_options.mergeFactor >= 2) {
        const vector<size_t> chosen = segmentsToMerge(segmentCounts(m_base, segments), m_options.mergeFactor,
                                                      m_options.maxMergedDocs);
        if (chosen.empty()) break;
        mergeSegments(m_base, segments, chosen, stats);
        NindSegmentedIndex::writeManifest(m_base, segments);
    }
    stats.segmentsNb = (unsigned int)segments.size();
    return stats;
}
////////////////////////////////////////////////////////////
NindSegmentedUpdate::Stats NindSegmentedUpdate::forceMerge(const string &base)
{
    Stats stats;
    vector<string> segments = NindSegmentedIndex::readManifest(base);
    bool deletions = false;
    for (size_t s = 0; s < segments.size(); s++)
        deletions = deletions || fileExists(NindSegmentedIndex::segmentBase(base, segments[s]) + ".ninddeletes");
    if (segments.size() > 1 || deletions) {
        vector<size_t> all;
        for (size_t s = 0; s < segments.size(); s++) all.push_back(s);
        mergeSegments(base, segments, all, stats);
        NindSegmentedIndex::writeManifest(base, segments);
    }
    stats.segmentsNb = (unsigned int)segments.size();
    return stats;
}
////////////////////////////////////////////////////////////
