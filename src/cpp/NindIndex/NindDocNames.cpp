//
// C++ Implementation: NindDocNames
//
// Description: the names of the documents of an index (see NindDocNames.h).
//
// Copyright: 2026 LATEJCON. See LICENCE.md file that comes with this distribution
// This file is part of NIND (as "nouvelle indexation").
// NIND is free software: you can redistribute it and/or modify it under the terms of the
// GNU Less General Public License (LGPL) as published by the Free Software Foundation,
// (see <http://www.gnu.org/licenses/>), either version 3 of the License, or any later version.
// NIND is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
// even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Less General Public License for more details.
////////////////////////////////////////////////////////////
#include "NindDocNames.h"
#include "NindExceptions.h"
#include "NindBasics/NindMappedFile.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <utility>
#include <sys/stat.h>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
namespace {
//NINDDN01: 4-byte document idents (read only); NINDDN02: 8-byte (64-bit NindDocId), written
const char MAGIC_V1[8] = {'N', 'I', 'N', 'D', 'D', 'N', '0', '1'};
const char MAGIC_V2[8] = {'N', 'I', 'N', 'D', 'D', 'N', '0', '2'};
//NINDDN03: NINDDN02 with 8-byte offsets, written only when the names take over 4 GiB
const char MAGIC_V3[8] = {'N', 'I', 'N', 'D', 'D', 'N', '0', '3'};
//<magic> <flags> <nombre> <premierIdent>
const size_t HEADER_SIZE_V1 = 20;
const size_t HEADER_SIZE_V2 = 24;
const uint32_t FLAG_IDENTS = 1;

void put4(vector<unsigned char> &bytes, const uint32_t value)
{
    for (int i = 0; i < 4; i++) bytes.push_back((unsigned char)(value >> (8 * i)));
}
void put8(vector<unsigned char> &bytes, const uint64_t value)
{
    for (int i = 0; i < 8; i++) bytes.push_back((unsigned char)(value >> (8 * i)));
}
void putOffset(vector<unsigned char> &bytes, const uint64_t value, const bool wide)
{
    if (wide) put8(bytes, value);
    else put4(bytes, (uint32_t)value);
}

struct NameLess {
    const vector<string> &names;
    explicit NameLess(const vector<string> &n): names(n) {}
    bool operator()(const uint32_t a, const uint32_t b) const { return names[a] < names[b]; }
};
}
////////////////////////////////////////////////////////////
//brief Write the names of documents into a new file
void NindDocNames::write(const string &fileName,
                         const vector<NindDocId> &docIdents,
                         const vector<string> &names)
{
    if (docIdents.size() != names.size())
        throw invalid_argument("NindDocNames::write: as many document idents as names expected");
    if (docIdents.size() > 0xFFFFFFFFu)
        throw invalid_argument("NindDocNames::write: too many documents");
    const uint32_t count = (uint32_t)docIdents.size();
    //documents in increasing ident order
    vector<uint32_t> order(count);
    for (uint32_t i = 0; i < count; i++) order[i] = i;
    stable_sort(order.begin(), order.end(), [&docIdents](const uint32_t a, const uint32_t b) {
        return docIdents[a] < docIdents[b]; });
    vector<string> sortedNames(count);
    vector<NindDocId> sortedIdents(count);
    for (uint32_t i = 0; i < count; i++) {
        sortedIdents[i] = docIdents[order[i]];
        sortedNames[i] = names[order[i]];
        if (i > 0 && sortedIdents[i] == sortedIdents[i - 1])
            throw invalid_argument("NindDocNames::write: document " + to_string(sortedIdents[i]) + " named twice");
    }
    //ranks in name order: names are unique
    vector<uint32_t> byName(count);
    for (uint32_t i = 0; i < count; i++) byName[i] = i;
    sort(byName.begin(), byName.end(), NameLess(sortedNames));
    for (uint32_t i = 1; i < count; i++) {
        if (sortedNames[byName[i]] == sortedNames[byName[i - 1]])
            throw invalid_argument("NindDocNames::write: duplicate document name: " + sortedNames[byName[i]]);
    }
    const bool dense = count == 0 || sortedIdents.back() - sortedIdents.front() == (uint64_t)count - 1;
    //the file (NINDDN02: 8-byte idents)
    uint64_t namesBytes = 0;
    for (uint32_t i = 0; i < count; i++) namesBytes += sortedNames[i].size();
    const bool wide = namesBytes > 0xFFFFFFFFu;
    const char *magic = wide ? MAGIC_V3 : MAGIC_V2;
    vector<unsigned char> bytes(magic, magic + sizeof(MAGIC_V2));
    put4(bytes, dense ? 0 : FLAG_IDENTS);
    put4(bytes, count);
    put8(bytes, count ? sortedIdents.front() : 0);
    if (!dense) for (uint32_t i = 0; i < count; i++) put8(bytes, sortedIdents[i]);
    uint64_t offset = 0;
    putOffset(bytes, 0, wide);
    for (uint32_t i = 0; i < count; i++) {
        offset += sortedNames[i].size();
        putOffset(bytes, offset, wide);
    }
    for (uint32_t i = 0; i < count; i++) put4(bytes, byName[i]);
    for (uint32_t i = 0; i < count; i++) bytes.insert(bytes.end(), sortedNames[i].begin(), sortedNames[i].end());
    //crejation exclusive : jamais d'ejcrasement d'un fichier existant
    FILE *file = fopen(fileName.c_str(), "wbx");
    if (file == 0) throw OpenFileException(fileName);
    const bool ok = fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    if (fclose(file) != 0 || !ok) {
        remove(fileName.c_str());
        throw WriteFileException(fileName);
    }
}
////////////////////////////////////////////////////////////
//brief Merge the names files of shards into a new file
void NindDocNames::merge(const vector<string> &shardFiles,
                         const string &fileName)
{
    merge(shardFiles, vector<vector<NindDocId> >(), fileName);
}
////////////////////////////////////////////////////////////
//brief Merge the names files of shards into a new file, without some of their documents
void NindDocNames::merge(const vector<string> &shardFiles,
                         const vector<vector<NindDocId> > &deleted,
                         const string &fileName)
{
    if (!deleted.empty() && deleted.size() != shardFiles.size())
        throw invalid_argument("NindDocNames::merge: as many lists of deleted documents as shards expected");
    vector<unique_ptr<NindDocNames> > shards;
    //each shard checked whole: the merge reads it all, and its names must be in order and unique
    for (size_t s = 0; s < shardFiles.size(); s++) {
        shards.push_back(unique_ptr<NindDocNames>(new NindDocNames(shardFiles[s])));
        shards.back()->check();
    }
    //the live documents' (ident, shard, rank in the shard), by increasing ident
    struct Item { NindDocId ident; uint32_t shard, rank; };
    vector<Item> items;
    size_t total = 0;
    for (size_t s = 0; s < shards.size(); s++) total += shards[s]->size();
    items.reserve(total);
    for (size_t s = 0; s < shards.size(); s++) {
        for (size_t r = 0; r < shards[s]->size(); r++) {
            const NindDocId ident = shards[s]->docIdentAt(r);
            if (!deleted.empty() && binary_search(deleted[s].begin(), deleted[s].end(), ident)) continue;
            Item item;
            item.ident = ident;
            item.shard = (uint32_t)s;
            item.rank = (uint32_t)r;
            items.push_back(item);
        }
    }
    const auto byIdent = [](const Item &a, const Item &b) { return a.ident < b.ident; };
    if (!is_sorted(items.begin(), items.end(), byIdent)) sort(items.begin(), items.end(), byIdent);
    if (items.size() > 0xFFFFFFFFu) throw invalid_argument("NindDocNames::merge: too many documents");
    uint64_t namesBytes = 0;
    //rank in a shard -> rank in the merged file (a deleted document: NO_RANK)
    const uint32_t NO_RANK = 0xFFFFFFFFu;
    vector<vector<uint32_t> > mergedRanks(shards.size());
    for (size_t s = 0; s < shards.size(); s++) mergedRanks[s].assign(shards[s]->size(), NO_RANK);
    for (size_t i = 0; i < items.size(); i++) {
        if (i > 0 && items[i].ident == items[i - 1].ident)
            throw invalid_argument("NindDocNames::merge: document " + to_string(items[i].ident) + " in several shards");
        mergedRanks[items[i].shard][items[i].rank] = (uint32_t)i;
        namesBytes += shards[items[i].shard]->nameAt(items[i].rank).second;
    }
    const uint64_t count = items.size();
    const NindDocId firstIdent = items.empty() ? 0 : items.front().ident;
    const bool dense = items.empty() || items.back().ident - firstIdent == count - 1;
    const bool wide = namesBytes > 0xFFFFFFFFu;

    FILE *file = fopen(fileName.c_str(), "wbx");
    if (file == 0) throw OpenFileException(fileName);
    try {
        vector<unsigned char> bytes;
        const size_t FLUSH_BYTES = 1 << 20;
        auto write = [&]() {
            if (!bytes.empty() && fwrite(bytes.data(), 1, bytes.size(), file) != bytes.size()) throw WriteFileException(fileName);
            bytes.clear();
        };
        const char *magic = wide ? MAGIC_V3 : MAGIC_V2;
        bytes.assign(magic, magic + sizeof(MAGIC_V2));
        put4(bytes, dense ? 0 : FLAG_IDENTS);
        put4(bytes, (uint32_t)count);
        put8(bytes, firstIdent);
        //<idents>
        for (size_t i = 0; i < items.size() && !dense; i++) {
            put8(bytes, items[i].ident);
            if (bytes.size() >= FLUSH_BYTES) write();
        }
        //<offsets>
        putOffset(bytes, 0, wide);
        uint64_t offset = 0;
        for (size_t i = 0; i < items.size(); i++) {
            offset += shards[items[i].shard]->nameAt(items[i].rank).second;
            putOffset(bytes, offset, wide);
            if (bytes.size() >= FLUSH_BYTES) write();
        }
        //<parNom>: the shards' name orders merged, a heap of (shard, place in its name order), each
        //at a live document
        const auto advance = [&](pair<uint32_t, uint32_t> &cursor) {
            const NindDocNames &shard = *shards[cursor.first];
            while (cursor.second < shard.size()
                   && mergedRanks[cursor.first][shard.rankByName(cursor.second)] == NO_RANK) cursor.second++;
            return cursor.second < shard.size();
        };
        const auto nameOf = [&shards](const pair<uint32_t, uint32_t> &cursor) {
            const NindDocNames &shard = *shards[cursor.first];
            return shard.nameAt(shard.rankByName(cursor.second));
        };
        //name of a > name of b: the smallest name at the heap's top
        const auto greaterName = [&shards, &nameOf](const pair<uint32_t, uint32_t> &a, const pair<uint32_t, uint32_t> &b) {
            const pair<size_t, size_t> nameA = nameOf(a), nameB = nameOf(b);
            const int cmp = memcmp(shards[a.first]->m_bytes + nameA.first, shards[b.first]->m_bytes + nameB.first,
                                   min(nameA.second, nameB.second));
            return cmp != 0 ? cmp > 0 : nameA.second > nameB.second;
        };
        vector<pair<uint32_t, uint32_t> > heap;
        for (uint32_t s = 0; s < shards.size(); s++) {
            pair<uint32_t, uint32_t> cursor(s, 0);
            if (advance(cursor)) heap.push_back(cursor);
        }
        make_heap(heap.begin(), heap.end(), greaterName);
        bool hasPrevious = false;
        pair<uint32_t, uint32_t> previous(0, 0);
        while (!heap.empty()) {
            pop_heap(heap.begin(), heap.end(), greaterName);
            pair<uint32_t, uint32_t> cursor = heap.back();
            //(within a shard, names are unique)
            if (hasPrevious && previous.first != cursor.first && !greaterName(cursor, previous)) {
                const pair<size_t, size_t> name = nameOf(cursor);
                throw invalid_argument("NindDocNames::merge: duplicate document name: "
                                       + string((const char *)shards[cursor.first]->m_bytes + name.first, name.second));
            }
            const NindDocNames &shard = *shards[cursor.first];
            put4(bytes, mergedRanks[cursor.first][shard.rankByName(cursor.second)]);
            if (bytes.size() >= FLUSH_BYTES) write();
            previous = cursor;
            hasPrevious = true;
            cursor.second++;
            if (advance(cursor)) {
                heap.back() = cursor;
                push_heap(heap.begin(), heap.end(), greaterName);
            }
            else heap.pop_back();
        }
        //<noms>: runs of consecutive names of a shard written at once
        write();
        for (size_t i = 0; i < items.size(); ) {
            const NindDocNames &shard = *shards[items[i].shard];
            const size_t begin = shard.nameAt(items[i].rank).first;
            size_t j = i + 1;
            while (j < items.size() && items[j].shard == items[i].shard && items[j].rank == items[j - 1].rank + 1) j++;
            const pair<size_t, size_t> lastName = shard.nameAt(items[j - 1].rank);
            const size_t length = lastName.first + lastName.second - begin;
            if (length != 0 && fwrite(shard.m_bytes + begin, 1, length, file) != length)
                throw WriteFileException(fileName);
            i = j;
        }
    }
    catch (...) {
        fclose(file);
        remove(fileName.c_str());
        throw;
    }
    if (fclose(file) != 0) {
        remove(fileName.c_str());
        throw WriteFileException(fileName);
    }
}
////////////////////////////////////////////////////////////
//brief Read the names of an index
NindDocNames::NindDocNames(const string &fileName):
    m_fileName(fileName),
    m_file(new NindMappedFile(fileName)),
    m_bytes(m_file->data()),
    m_fileSize(m_file->size()),
    m_size(0),
    m_hasIdents(false),
    m_firstIdent(0),
    m_identWidth(8),
    m_offsetWidth(4),
    m_identsPos(0), m_offsetsPos(0), m_byNamePos(0), m_namesPos(0)
{
    //nothing read from the file is trusted: here, the header and the arrays' sizes against the
    //file's (constant time); each lookup checks what it reads; check() reads everything
    if (m_fileSize < HEADER_SIZE_V1) throw FormatFileException(fileName);
    size_t headerSize;
    if (memcmp(m_bytes, MAGIC_V3, sizeof(MAGIC_V3)) == 0) {
        m_identWidth = 8;
        m_offsetWidth = 8;
        headerSize = HEADER_SIZE_V2;
    }
    else if (memcmp(m_bytes, MAGIC_V2, sizeof(MAGIC_V2)) == 0) {
        m_identWidth = 8;
        headerSize = HEADER_SIZE_V2;
    }
    else if (memcmp(m_bytes, MAGIC_V1, sizeof(MAGIC_V1)) == 0) {
        m_identWidth = 4;
        headerSize = HEADER_SIZE_V1;
    }
    else throw FormatFileException(fileName);
    if (m_fileSize < headerSize) throw FormatFileException(fileName);
    const uint32_t flags = read4(8);
    if ((flags & ~FLAG_IDENTS) != 0) throw FormatFileException(fileName);
    m_hasIdents = (flags & FLAG_IDENTS) != 0;
    const uint64_t count = read4(12);
    m_firstIdent = readIdent(16);
    //idents (if any): m_identWidth bytes each; offsets: m_offsetWidth; parNom: 4 bytes each
    const uint64_t arraysBytes = (m_hasIdents ? m_identWidth * count : 0) + m_offsetWidth * (count + 1) + 4 * count;
    if (headerSize + arraysBytes > m_fileSize) throw FormatFileException(fileName);
    m_size = (size_t)count;
    m_identsPos = headerSize;
    m_offsetsPos = m_identsPos + (m_hasIdents ? m_identWidth * m_size : 0);
    m_byNamePos = m_offsetsPos + m_offsetWidth * (m_size + 1);
    m_namesPos = m_byNamePos + 4 * m_size;
    const uint64_t maxIdent = (m_identWidth == 4) ? 0xFFFFFFFFull : ~(uint64_t)0;
    if (!m_hasIdents && m_size != 0 && (uint64_t)(m_size - 1) > maxIdent - m_firstIdent)
        throw FormatFileException(fileName);
    //offsets: the first 0, the last one the end of the file
    if (readOffset(m_offsetsPos) != 0) throw FormatFileException(fileName);
    if (readOffset(m_offsetsPos + m_offsetWidth * m_size) != m_fileSize - m_namesPos) throw FormatFileException(fileName);
}
////////////////////////////////////////////////////////////
NindDocNames::~NindDocNames()
{
}
////////////////////////////////////////////////////////////
//brief Check the whole file
void NindDocNames::check() const
{
    //offsets increasing
    for (size_t i = 1; i <= m_size; i++) {
        if (readOffset(m_offsetsPos + m_offsetWidth * i) < readOffset(m_offsetsPos + m_offsetWidth * (i - 1)))
            throw FormatFileException(m_fileName);
    }
    //idents strictly increasing
    for (size_t i = 1; i < m_size && m_hasIdents; i++) {
        if (readIdent(m_identsPos + m_identWidth * i) <= readIdent(m_identsPos + m_identWidth * (i - 1)))
            throw FormatFileException(m_fileName);
    }
    //name order: ranks within bounds, names strictly increasing (so unique, and a permutation)
    for (size_t i = 1; i < m_size; i++) {
        const pair<size_t, size_t> name = nameAt(rankByName(i));
        if (compareName(rankByName(i - 1), (const char *)m_bytes + name.first, name.second) >= 0)
            throw FormatFileException(m_fileName);
    }
    if (m_size != 0) rankByName(0);
}
////////////////////////////////////////////////////////////
//brief get the name of a document
bool NindDocNames::getName(const NindDocId docIdent,
                           string &name) const
{
    size_t rank;
    if (!m_hasIdents) {
        if (docIdent < m_firstIdent || docIdent - m_firstIdent >= m_size) return false;
        rank = (size_t)(docIdent - m_firstIdent);
    }
    else {
        size_t low = 0, high = m_size;
        while (low < high) {
            const size_t middle = low + (high - low) / 2;
            if (readIdent(m_identsPos + m_identWidth * middle) < docIdent) low = middle + 1;
            else high = middle;
        }
        if (low == m_size || readIdent(m_identsPos + m_identWidth * low) != docIdent) return false;
        rank = low;
    }
    const pair<size_t, size_t> span = nameAt(rank);
    name.assign((const char *)m_bytes + span.first, span.second);
    return true;
}
////////////////////////////////////////////////////////////
//brief get the document of a name
bool NindDocNames::getDocIdent(const string &name,
                               NindDocId &docIdent) const
{
    size_t low = 0, high = m_size;
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (compareName(rankByName(middle), name.data(), name.size()) < 0) low = middle + 1;
        else high = middle;
    }
    if (low == m_size) return false;
    const size_t rank = rankByName(low);
    if (compareName(rank, name.data(), name.size()) != 0) return false;
    docIdent = docIdentAt(rank);
    return true;
}
////////////////////////////////////////////////////////////
//brief every (document, name), in increasing document ident order
void NindDocNames::getEntries(vector<NindDocId> &docIdents,
                              vector<string> &names) const
{
    docIdents.resize(m_size);
    names.resize(m_size);
    for (size_t i = 0; i < m_size; i++) {
        docIdents[i] = docIdentAt(i);
        const pair<size_t, size_t> span = nameAt(i);
        names[i].assign((const char *)m_bytes + span.first, span.second);
    }
}
////////////////////////////////////////////////////////////
uint32_t NindDocNames::read4(const size_t offset) const
{
    const unsigned char *p = m_bytes + offset;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
////////////////////////////////////////////////////////////
uint64_t NindDocNames::readOffset(const size_t offset) const
{
    if (m_offsetWidth == 4) return read4(offset);
    const unsigned char *p = m_bytes + offset;
    uint64_t value = 0;
    for (int i = 7; i >= 0; i--) value = (value << 8) | p[i];
    return value;
}
////////////////////////////////////////////////////////////
NindDocId NindDocNames::readIdent(const size_t offset) const
{
    if (m_identWidth == 4) return read4(offset);
    const unsigned char *p = m_bytes + offset;
    NindDocId value = 0;
    for (int i = 7; i >= 0; i--) value = (value << 8) | p[i];
    return value;
}
////////////////////////////////////////////////////////////
NindDocId NindDocNames::docIdentAt(const size_t rank) const
{
    return m_hasIdents ? readIdent(m_identsPos + m_identWidth * rank) : m_firstIdent + (NindDocId)rank;
}
////////////////////////////////////////////////////////////
pair<size_t, size_t> NindDocNames::nameAt(const size_t rank) const
{
    //(rank < m_size: its two offsets are in the file; what they say is checked here)
    const uint64_t begin = readOffset(m_offsetsPos + m_offsetWidth * rank);
    const uint64_t end = readOffset(m_offsetsPos + m_offsetWidth * (rank + 1));
    if (begin > end || end > m_fileSize - m_namesPos) throw FormatFileException(m_fileName);
    return pair<size_t, size_t>(m_namesPos + (size_t)begin, (size_t)(end - begin));
}
////////////////////////////////////////////////////////////
size_t NindDocNames::rankByName(const size_t place) const
{
    const uint32_t rank = read4(m_byNamePos + 4 * place);
    if (rank >= m_size) throw FormatFileException(m_fileName);
    return rank;
}
////////////////////////////////////////////////////////////
int NindDocNames::compareName(const size_t rank,
                              const char *name,
                              const size_t length) const
{
    const pair<size_t, size_t> span = nameAt(rank);
    const int cmp = memcmp(m_bytes + span.first, name, min(span.second, length));
    if (cmp != 0) return cmp;
    return (span.second < length) ? -1 : (span.second > length ? 1 : 0);
}
////////////////////////////////////////////////////////////
