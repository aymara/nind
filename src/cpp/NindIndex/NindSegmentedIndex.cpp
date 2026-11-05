//
// C++ Implementation: NindSegmentedIndex
//
// Description: un index fait de segments immuables, interrogej comme un seul ; voir NindSegmentedIndex.h
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
#include "NindSegmentedIndex.h"
#include "NindDocNames.h"
#include "NindExceptions.h"
#include "NindLexiconIndex.h"
#include "NindLocalIndex.h"
#include "NindTermIndex.h"
#include "NindThreadPool.h"
#include "NindBasics/NindReadBuffer.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <map>
#include <unordered_map>
#include <sys/stat.h>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
namespace {
const char LENGTHS_MAGIC_V1[8] = {'N', 'I', 'N', 'D', 'D', 'L', '0', '1'};
const char LENGTHS_MAGIC_V2[8] = {'N', 'I', 'N', 'D', 'D', 'L', '0', '2'};
const char DELETES_MAGIC[8] = {'N', 'I', 'N', 'D', 'D', 'E', '0', '1'};
const char *MANIFEST_EXTENSION = ".nindsegments";
const char *MANIFEST_HEADER = "nindsegments 1";

//the whole file; false if there is none
bool readWholeFile(const string &fileName, vector<unsigned char> &bytes)
{
    FILE *file = fopen(fileName.c_str(), "rb");
    if (file == 0) return false;
    struct stat fileStat;
    if (fstat(fileno(file), &fileStat) != 0) {
        fclose(file);
        throw ReadFileException(fileName);
    }
    bytes.resize((size_t)fileStat.st_size);
    const bool ok = bytes.empty() || fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
    fclose(file);
    if (!ok) throw ReadFileException(fileName);
    return true;
}
uint32_t get4(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint64_t get8(const unsigned char *p)
{
    return (uint64_t)get4(p) | ((uint64_t)get4(p + 4) << 32);
}
//buffered exclusive writer of little-endian numbers: the file is removed on failure
class Writer {
public:
    explicit Writer(const string &fileName): m_fileName(fileName), m_file(fopen(fileName.c_str(), "wbx")), m_bytes() {
        if (m_file == 0) throw OpenFileException(fileName);
    }
    ~Writer() {
        if (m_file != 0) {
            fclose(m_file);
            remove(m_fileName.c_str());
        }
    }
    void put(const char *bytes, const size_t size) { m_bytes.insert(m_bytes.end(), bytes, bytes + size); flushIfFull(); }
    void put4(const uint32_t value) {
        for (int i = 0; i < 4; i++) m_bytes.push_back((unsigned char)(value >> (8 * i)));
        flushIfFull();
    }
    void put8(const uint64_t value) {
        for (int i = 0; i < 8; i++) m_bytes.push_back((unsigned char)(value >> (8 * i)));
        flushIfFull();
    }
    void close() {
        flush();
        FILE *file = m_file;
        m_file = 0;
        if (fclose(file) != 0) {
            remove(m_fileName.c_str());
            throw WriteFileException(m_fileName);
        }
    }
private:
    void flushIfFull() { if (m_bytes.size() >= (1u << 20)) flush(); }
    void flush() {
        if (!m_bytes.empty() && fwrite(m_bytes.data(), 1, m_bytes.size(), m_file) != m_bytes.size())
            throw WriteFileException(m_fileName);
        m_bytes.clear();
    }
    string m_fileName;
    FILE *m_file;
    vector<unsigned char> m_bytes;
};
}
////////////////////////////////////////////////////////////
bool NindDocLengths::read(const string &fileName,
                          vector<NindDocId> &docIds,
                          vector<uint32_t> &lengths)
{
    vector<unsigned char> bytes;
    if (!readWholeFile(fileName, bytes)) return false;
    if (bytes.size() < 12) throw FormatFileException(fileName);
    size_t idSize;
    if (memcmp(bytes.data(), LENGTHS_MAGIC_V2, 8) == 0) idSize = 8;
    else if (memcmp(bytes.data(), LENGTHS_MAGIC_V1, 8) == 0) idSize = 4;
    else throw FormatFileException(fileName);
    const uint64_t count = get4(bytes.data() + 8);
    if (bytes.size() != 12 + (idSize + 4) * count) throw FormatFileException(fileName);
    docIds.resize((size_t)count);
    lengths.resize((size_t)count);
    const unsigned char *ids = bytes.data() + 12;
    const unsigned char *values = ids + idSize * count;
    for (size_t i = 0; i < count; i++) {
        docIds[i] = (idSize == 8) ? get8(ids + 8 * i) : get4(ids + 4 * i);
        lengths[i] = get4(values + 4 * i);
    }
    return true;
}
////////////////////////////////////////////////////////////
bool NindDocLengths::readLast(const string &fileName,
                              NindDocId &docId)
{
    FILE *file = fopen(fileName.c_str(), "rb");
    if (file == 0) return false;
    unsigned char header[12], last[8];
    bool found = false, ok = fread(header, 1, 12, file) == 12;
    if (ok) {
        const size_t idSize = memcmp(header, LENGTHS_MAGIC_V2, 8) == 0 ? 8 : (memcmp(header, LENGTHS_MAGIC_V1, 8) == 0 ? 4 : 0);
        ok = idSize != 0;
        const uint64_t count = get4(header + 8);
        if (ok && count != 0) {
            const uint64_t place = 12 + idSize * (count - 1);
#ifdef _WIN32
            ok = _fseeki64(file, (__int64)place, SEEK_SET) == 0;
#else
            ok = fseeko(file, (off_t)place, SEEK_SET) == 0;
#endif
            ok = ok && fread(last, 1, idSize, file) == idSize;
            if (ok) {
                docId = (idSize == 8) ? get8(last) : get4(last);
                found = true;
            }
        }
    }
    fclose(file);
    if (!ok) throw FormatFileException(fileName);
    return found;
}
////////////////////////////////////////////////////////////
void NindDocLengths::write(const string &fileName,
                           const vector<NindDocId> &docIds,
                           const vector<uint32_t> &lengths)
{
    Writer writer(fileName);
    writer.put(LENGTHS_MAGIC_V2, 8);
    writer.put4((uint32_t)docIds.size());
    for (size_t i = 0; i < docIds.size(); i++) writer.put8(docIds[i]);
    for (size_t i = 0; i < lengths.size(); i++) writer.put4(lengths[i]);
    writer.close();
}
////////////////////////////////////////////////////////////
bool NindSegmentDeletes::read(const string &fileName)
{
    docIds.clear();
    termCounts.clear();
    vector<unsigned char> bytes;
    if (!readWholeFile(fileName, bytes)) return false;
    if (bytes.size() < 16 || memcmp(bytes.data(), DELETES_MAGIC, 8) != 0) throw FormatFileException(fileName);
    const uint64_t docsNb = get4(bytes.data() + 8), termsNb = get4(bytes.data() + 12);
    if (bytes.size() != 16 + 8 * docsNb + 8 * termsNb) throw FormatFileException(fileName);
    docIds.resize((size_t)docsNb);
    const unsigned char *p = bytes.data() + 16;
    for (size_t i = 0; i < docsNb; i++, p += 8) {
        docIds[i] = get8(p);
        if (i > 0 && docIds[i] <= docIds[i - 1]) throw FormatFileException(fileName);
    }
    termCounts.resize((size_t)termsNb);
    for (size_t i = 0; i < termsNb; i++, p += 8) {
        termCounts[i] = make_pair(get4(p), get4(p + 4));
        if (i > 0 && termCounts[i].first <= termCounts[i - 1].first) throw FormatFileException(fileName);
    }
    return true;
}
////////////////////////////////////////////////////////////
void NindSegmentDeletes::write(const string &fileName) const
{
    Writer writer(fileName);
    writer.put(DELETES_MAGIC, 8);
    writer.put4((uint32_t)docIds.size());
    writer.put4((uint32_t)termCounts.size());
    for (size_t i = 0; i < docIds.size(); i++) writer.put8(docIds[i]);
    for (size_t i = 0; i < termCounts.size(); i++) {
        writer.put4(termCounts[i].first);
        writer.put4(termCounts[i].second);
    }
    writer.close();
}
////////////////////////////////////////////////////////////
vector<string> NindSegmentedIndex::readManifest(const string &base)
{
    vector<string> segments;
    ifstream file((base + MANIFEST_EXTENSION).c_str());
    if (!file) return segments;
    string line;
    if (!getline(file, line) || line != MANIFEST_HEADER) throw FormatFileException(base + MANIFEST_EXTENSION);
    while (getline(file, line)) {
        //a directory next to the manifest, never a path
        if (line.empty() || line == "." || line == ".." || line.find('/') != string::npos || line.find('\\') != string::npos)
            throw FormatFileException(base + MANIFEST_EXTENSION);
        segments.push_back(line);
    }
    return segments;
}
////////////////////////////////////////////////////////////
void NindSegmentedIndex::writeManifest(const string &base,
                                       const vector<string> &segments)
{
    const string fileName = base + MANIFEST_EXTENSION;
    remove(fileName.c_str());
    Writer writer(fileName);
    writer.put(MANIFEST_HEADER, strlen(MANIFEST_HEADER));
    writer.put("\n", 1);
    for (size_t i = 0; i < segments.size(); i++) {
        writer.put(segments[i].data(), segments[i].size());
        writer.put("\n", 1);
    }
    writer.close();
}
////////////////////////////////////////////////////////////
string NindSegmentedIndex::segmentBase(const string &base,
                                       const string &segment)
{
    const size_t slash = base.find_last_of('/');
    if (slash == string::npos) return segment + "/" + base;
    return base.substr(0, slash + 1) + segment + base.substr(slash);
}
////////////////////////////////////////////////////////////
class NindSegmentedIndex::Impl {
public:
    // A segment's files, opened: immutable, so shared by the indexes opened on later generations
    struct Segment {
        string name, base;
        unique_ptr<NindLexiconIndex> lexicon;
        unique_ptr<NindTermIndex> terms;
        unique_ptr<NindBm25Scorer> scorer;
        unique_ptr<NindDocNames> names;
        uint64_t totalLength;
        Segment(const string &segmentName, const string &segmentBase): name(segmentName), base(segmentBase),
            lexicon(), terms(), scorer(), names(), totalLength(0)
        {
            lexicon.reset(new NindLexiconIndex(base, false));
            terms.reset(new NindTermIndex(base, false, lexicon->getIdentification(), 0));
            vector<NindDocId> docIds;
            vector<uint32_t> lengths;
            if (!NindDocLengths::read(base + ".ninddoclengths", docIds, lengths)) {
                //written before document lengths were stored apart: from the local index
                NindLocalIndex local(base, false, lexicon->getIdentification());
                local.getDocIdents(docIds);
                lengths.assign(docIds.size(), 0);
                list<NindLocalIndex::Term> localDef;
                for (size_t d = 0; d < docIds.size(); d++) {
                    if (!local.getLocalDef(docIds[d], localDef)) continue;
                    for (list<NindLocalIndex::Term>::const_iterator it = localDef.begin(); it != localDef.end(); it++)
                        lengths[d] += (uint32_t)(*it).localisation.size();
                }
            }
            for (size_t d = 0; d < lengths.size(); d++) totalLength += lengths[d];
            scorer.reset(new NindBm25Scorer(docIds, lengths, docIds.empty() ? 0.0 : (double)totalLength / docIds.size()));
            FILE *namesFile = fopen((base + ".ninddocnames").c_str(), "rb");
            if (namesFile != 0) {
                fclose(namesFile);
                names.reset(new NindDocNames(base + ".ninddocnames"));
            }
        }
    };
    // A segment in this index: what its deletions are here
    struct Part {
        shared_ptr<const Segment> segment;
        string base;                    //its files in this index's generation (the segment may have been opened in another)
        vector<uint64_t> deleted;       //one bit per position (empty: none)
        uint64_t deletedNb;
        uint64_t liveLength;
        vector<pair<unsigned int, uint32_t> > termCounts;     //postings of deleted documents, by term
        Part(): segment(), base(), deleted(), deletedNb(0), liveLength(0), termCounts() {}
        bool isLive(const size_t position) const {
            return position < segment->scorer->docsNb()
                && (deleted.empty() || (deleted[position / 64] >> (position % 64) & 1) == 0);
        }
        uint64_t deletedPostings(const unsigned int term) const {
            const auto it = lower_bound(termCounts.begin(), termCounts.end(), make_pair(term, (uint32_t)0));
            return (it != termCounts.end() && it->first == term) ? it->second : 0;
        }
        uint64_t liveDocsNb() const { return segment->scorer->docsNb() - deletedNb; }
    };

    vector<Part> m_parts;               //manifest order
    vector<size_t> m_bySize;            //the parts, by decreasing live documents: search order
    mutable NindThreadPool m_pool;

    Impl(): m_parts(), m_bySize(), m_pool() {}

    const vector<Part> &parts() const { return m_parts; }

    const Part *partOf(const NindDocId docId, size_t &position) const {
        for (size_t p = 0; p < m_parts.size(); p++) {
            position = m_parts[p].segment->scorer->position(docId);
            if (m_parts[p].isLive(position)) return &m_parts[p];
        }
        return 0;
    }
    uint64_t documentFrequency(const list<string> &word, NindReadBuffer &buffer) const {
        uint64_t df = 0;
        for (size_t p = 0; p < m_parts.size(); p++) {
            const Part &part = m_parts[p];
            const unsigned int term = part.segment->lexicon->getWordId(word, buffer);
            if (term == 0) continue;
            const uint64_t postings = part.segment->scorer->documentFrequency(*part.segment->terms, term, buffer);
            const uint64_t deleted = part.deletedPostings(term);
            df += postings > deleted ? postings - deleted : 0;
        }
        return df;
    }
};
////////////////////////////////////////////////////////////
NindSegmentedIndex::NindSegmentedIndex(const string &base,
                                       const NindSegmentedIndex *previous):
    m_impl(new Impl()),
    m_docsNb(0),
    m_avgDocLength(0)
{
    vector<string> names = readManifest(base);
    const bool withManifest = ifstream((base + MANIFEST_EXTENSION).c_str()).good();
    if (!withManifest) names.push_back("");
    uint64_t totalLength = 0;
    for (size_t s = 0; s < names.size(); s++) {
        Impl::Part part;
        //a name is given once: the same name, the same files
        if (previous != 0 && !names[s].empty()) {
            const vector<Impl::Part> &parts = previous->m_impl->parts();
            for (size_t p = 0; p < parts.size(); p++)
                if (parts[p].segment->name == names[s]) part.segment = parts[p].segment;
        }
        const string segmentFiles = withManifest ? segmentBase(base, names[s]) : base;
        if (!part.segment) part.segment.reset(new Impl::Segment(names[s], segmentFiles));
        part.base = segmentFiles;
        const NindBm25Scorer &scorer = *part.segment->scorer;
        part.liveLength = part.segment->totalLength;
        NindSegmentDeletes deletes;
        if (deletes.read(segmentFiles + ".ninddeletes") && !deletes.docIds.empty()) {
            part.deleted.assign(scorer.docsNb() / 64 + 1, 0);
            for (size_t d = 0; d < deletes.docIds.size(); d++) {
                const size_t position = scorer.position(deletes.docIds[d]);
                //(a corrupt file: never a document twice, never out of the table)
                if (position >= scorer.docsNb()) throw FormatFileException(segmentFiles + ".ninddeletes");
                part.deleted[position / 64] |= (uint64_t)1 << (position % 64);
                part.liveLength -= scorer.lengthAt(position);
                part.deletedNb++;
            }
            part.termCounts.swap(deletes.termCounts);
        }
        m_docsNb += part.liveDocsNb();
        totalLength += part.liveLength;
        m_impl->m_parts.push_back(std::move(part));
        m_impl->m_bySize.push_back(s);
    }
    m_avgDocLength = m_docsNb != 0 ? (double)totalLength / (double)m_docsNb : 0.0;
    const vector<Impl::Part> &parts = m_impl->m_parts;
    stable_sort(m_impl->m_bySize.begin(), m_impl->m_bySize.end(), [&parts](const size_t a, const size_t b) {
        return parts[a].liveDocsNb() > parts[b].liveDocsNb(); });
}
////////////////////////////////////////////////////////////
NindSegmentedIndex::~NindSegmentedIndex()
{
}
////////////////////////////////////////////////////////////
vector<pair<NindDocId, double> > NindSegmentedIndex::search(const vector<list<string> > &words,
                                                            const double k1,
                                                            const double b,
                                                            const long topK,
                                                            const bool prune,
                                                            const unsigned int threads) const
{
    const vector<Impl::Part> &parts = m_impl->m_parts;
    const size_t partsNb = parts.size();
    const size_t wordsNb = words.size();
    //per segment, each word's term there (0: not there); each word's df in the whole index
    NindReadBuffer buffer;
    vector<unsigned int> termIds(partsNb * wordsNb, 0);
    vector<uint64_t> dfs(wordsNb, 0);
    for (size_t w = 0; w < wordsNb; w++) {
        //(a word repeated: its first occurrence's)
        size_t first = w;
        for (size_t x = 0; x < w; x++) if (words[x] == words[w]) { first = x; break; }
        if (first != w) {
            dfs[w] = dfs[first];
            for (size_t p = 0; p < partsNb; p++) termIds[p * wordsNb + w] = termIds[p * wordsNb + first];
            continue;
        }
        for (size_t p = 0; p < partsNb; p++) {
            const Impl::Part &part = parts[p];
            const unsigned int term = part.segment->lexicon->getWordId(words[w], buffer);
            termIds[p * wordsNb + w] = term;
            if (term == 0) continue;
            const uint64_t postings = part.segment->scorer->documentFrequency(*part.segment->terms, term, buffer);
            const uint64_t deleted = part.deletedPostings(term);
            dfs[w] += postings > deleted ? postings - deleted : 0;
        }
    }
    atomic<double> threshold(0.0);
    vector<vector<pair<NindDocId, double> > > found(partsNb);
    const auto searchPart = [&](const size_t p, const unsigned int partThreads) {
        const Impl::Part &part = parts[p];
        NindBm25Scorer::SearchContext context;
        context.docsNb = m_docsNb;
        context.avgDocLength = m_avgDocLength;
        context.documentFrequencies = &dfs;
        context.deleted = part.deleted.empty() ? 0 : &part.deleted;
        context.threshold = &threshold;
        const vector<unsigned int> partTerms(termIds.begin() + p * wordsNb, termIds.begin() + (p + 1) * wordsNb);
        found[p] = part.segment->scorer->search(*part.segment->terms, partTerms, k1, b, topK, prune, partThreads, context);
    };
    //the segments, largest first (theta rises sooner), taken one by one by the threads; more threads
    //than segments: the rest within the segments
    const size_t workers = max((size_t)1, min((size_t)threads, partsNb));
    const unsigned int inner = max(1u, threads / (unsigned int)max((size_t)1, partsNb));
    if (workers == 1) {
        for (size_t i = 0; i < partsNb; i++) searchPart(m_impl->m_bySize[i], inner);
    }
    else {
        atomic<size_t> next(0);
        const auto work = [&]() {
            for (size_t i = next++; i < partsNb; i = next++) searchPart(m_impl->m_bySize[i], inner);
        };
        vector<future<void> > done;
        for (size_t w = 1; w < workers; w++) done.push_back(m_impl->m_pool.run(work, workers - 1));
        exception_ptr error;
        try { work(); }
        catch (...) { error = current_exception(); }
        //(every thread finished before anything they use goes)
        for (size_t d = 0; d < done.size(); d++) {
            try { done[d].get(); }
            catch (...) { if (!error) error = current_exception(); }
        }
        if (error) rethrow_exception(error);
    }
    //decreasing score, then increasing document: as one index
    vector<pair<NindDocId, double> > result;
    if (partsNb == 1) result.swap(found[0]);
    else {
        size_t all = 0;
        for (size_t p = 0; p < partsNb; p++) all += found[p].size();
        result.reserve(all);
        for (size_t p = 0; p < partsNb; p++) result.insert(result.end(), found[p].begin(), found[p].end());
        const auto better = [](const pair<NindDocId, double> &x, const pair<NindDocId, double> &y) {
            return x.second != y.second ? x.second > y.second : x.first < y.first; };
        if (topK >= 0 && (size_t)topK < result.size()) {
            partial_sort(result.begin(), result.begin() + topK, result.end(), better);
            result.resize((size_t)topK);
        }
        else sort(result.begin(), result.end(), better);
    }
    return result;
}
////////////////////////////////////////////////////////////
uint64_t NindSegmentedIndex::documentFrequency(const list<string> &word) const
{
    NindReadBuffer buffer;
    return m_impl->documentFrequency(word, buffer);
}
////////////////////////////////////////////////////////////
uint64_t NindSegmentedIndex::termFrequency(const list<string> &word,
                                           const NindDocId docId) const
{
    size_t position = 0;
    const Impl::Part *part = m_impl->partOf(docId, position);
    if (part == 0) return 0;
    NindReadBuffer buffer;
    const unsigned int term = part->segment->lexicon->getWordId(word, buffer);
    if (term == 0) return 0;
    vector<NindTermIndex::Document> postings;
    vector<size_t> categoryEnds;
    part->segment->terms->getTermPostings(term, postings, categoryEnds, buffer);
    //each category's documents are in increasing order; as searches score it: its first category
    size_t begin = 0;
    for (size_t c = 0; c < categoryEnds.size(); begin = categoryEnds[c], c++) {
        const auto it = lower_bound(postings.begin() + begin, postings.begin() + categoryEnds[c], docId,
                                    [](const NindTermIndex::Document &d, const NindDocId id) { return d.ident < id; });
        if (it != postings.begin() + categoryEnds[c] && it->ident == docId) return it->frequency;
    }
    return 0;
}
////////////////////////////////////////////////////////////
uint32_t NindSegmentedIndex::documentLength(const NindDocId docId) const
{
    size_t position = 0;
    const Impl::Part *part = m_impl->partOf(docId, position);
    return part == 0 ? 0 : part->segment->scorer->lengthAt(position);
}
////////////////////////////////////////////////////////////
bool NindSegmentedIndex::contains(const NindDocId docId) const
{
    size_t position = 0;
    return m_impl->partOf(docId, position) != 0;
}
////////////////////////////////////////////////////////////
bool NindSegmentedIndex::locate(const NindDocId docId,
                                size_t &segment,
                                size_t &position) const
{
    const Impl::Part *part = m_impl->partOf(docId, position);
    if (part == 0) return false;
    segment = (size_t)(part - m_impl->m_parts.data());
    return true;
}
////////////////////////////////////////////////////////////
bool NindSegmentedIndex::getName(const NindDocId docId,
                                 string &name) const
{
    size_t position = 0;
    const Impl::Part *part = m_impl->partOf(docId, position);
    return part != 0 && part->segment->names && part->segment->names->getName(docId, name);
}
////////////////////////////////////////////////////////////
bool NindSegmentedIndex::getDocIdent(const string &name,
                                     NindDocId &docId) const
{
    const vector<Impl::Part> &parts = m_impl->m_parts;
    for (size_t p = 0; p < parts.size(); p++) {
        NindDocId found = 0;
        if (!parts[p].segment->names || !parts[p].segment->names->getDocIdent(name, found)) continue;
        //(a replaced or removed document's name stays in its segment's file)
        if (!parts[p].isLive(parts[p].segment->scorer->position(found))) continue;
        docId = found;
        return true;
    }
    return false;
}
////////////////////////////////////////////////////////////
bool NindSegmentedIndex::hasNames() const
{
    const vector<Impl::Part> &parts = m_impl->m_parts;
    for (size_t p = 0; p < parts.size(); p++) if (parts[p].segment->names) return true;
    return false;
}
////////////////////////////////////////////////////////////
bool NindSegmentedIndex::getMaxDocIdent(NindDocId &docId) const
{
    bool found = false;
    const vector<Impl::Part> &parts = m_impl->m_parts;
    for (size_t p = 0; p < parts.size(); p++) {
        const NindBm25Scorer &scorer = *parts[p].segment->scorer;
        for (size_t position = scorer.docsNb(); position-- > 0; ) {
            const NindDocId id = scorer.docIdAt(position);
            if (!found || id > docId) docId = id;
            found = true;
            //(positions in increasing document order, as the lengths files hold them)
            break;
        }
    }
    return found;
}
////////////////////////////////////////////////////////////
void NindSegmentedIndex::getDocuments(vector<NindDocId> &docIds,
                                      vector<uint32_t> &lengths) const
{
    vector<pair<NindDocId, uint32_t> > documents;
    documents.reserve((size_t)m_docsNb);
    const vector<Impl::Part> &parts = m_impl->m_parts;
    for (size_t p = 0; p < parts.size(); p++) {
        const NindBm25Scorer &scorer = *parts[p].segment->scorer;
        for (size_t position = 0; position < scorer.docsNb(); position++)
            if (parts[p].isLive(position)) documents.push_back(make_pair(scorer.docIdAt(position), scorer.lengthAt(position)));
    }
    sort(documents.begin(), documents.end());
    docIds.resize(documents.size());
    lengths.resize(documents.size());
    for (size_t d = 0; d < documents.size(); d++) {
        docIds[d] = documents[d].first;
        lengths[d] = documents[d].second;
    }
}
////////////////////////////////////////////////////////////
vector<NindSegmentedIndex::SegmentInfo> NindSegmentedIndex::segments() const
{
    vector<SegmentInfo> result;
    const vector<Impl::Part> &parts = m_impl->m_parts;
    for (size_t p = 0; p < parts.size(); p++) {
        SegmentInfo info;
        info.name = parts[p].segment->name;
        info.base = parts[p].base;
        info.docsNb = parts[p].segment->scorer->docsNb();
        info.deletedNb = parts[p].deletedNb;
        result.push_back(info);
    }
    return result;
}
////////////////////////////////////////////////////////////
