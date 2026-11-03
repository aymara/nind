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
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <sys/stat.h>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
namespace {
const char MAGIC[8] = {'N', 'I', 'N', 'D', 'D', 'N', '0', '1'};
//<magic> <flags> <nombre> <premierIdent>
const size_t HEADER_SIZE = 20;
const uint32_t FLAG_IDENTS = 1;

void put4(vector<unsigned char> &bytes, const uint32_t value)
{
    for (int i = 0; i < 4; i++) bytes.push_back((unsigned char)(value >> (8 * i)));
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
                         const vector<uint32_t> &docIdents,
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
    vector<uint32_t> sortedIdents(count);
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
    const bool dense = count == 0 ||
        (uint64_t)sortedIdents.back() - sortedIdents.front() + 1 == (uint64_t)count;
    //the file
    vector<unsigned char> bytes(MAGIC, MAGIC + sizeof(MAGIC));
    put4(bytes, dense ? 0 : FLAG_IDENTS);
    put4(bytes, count);
    put4(bytes, count ? sortedIdents.front() : 0);
    if (!dense) for (uint32_t i = 0; i < count; i++) put4(bytes, sortedIdents[i]);
    uint64_t offset = 0;
    put4(bytes, 0);
    for (uint32_t i = 0; i < count; i++) {
        offset += sortedNames[i].size();
        if (offset > 0xFFFFFFFFu) throw invalid_argument("NindDocNames::write: names too long together");
        put4(bytes, (uint32_t)offset);
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
//brief Read the names of an index
NindDocNames::NindDocNames(const string &fileName):
    m_fileName(fileName),
    m_bytes(),
    m_size(0),
    m_hasIdents(false),
    m_firstIdent(0),
    m_identsPos(0), m_offsetsPos(0), m_byNamePos(0), m_namesPos(0)
{
    FILE *file = fopen(fileName.c_str(), "rb");
    if (file == 0) throw OpenFileException(fileName);
    struct stat fileStat;
    if (fstat(fileno(file), &fileStat) != 0) {
        fclose(file);
        throw ReadFileException(fileName);
    }
    m_bytes.resize((size_t)fileStat.st_size);
    const bool ok = m_bytes.empty() || fread(m_bytes.data(), 1, m_bytes.size(), file) == m_bytes.size();
    fclose(file);
    if (!ok) throw ReadFileException(fileName);
    //nothing read from the file is trusted: every size is checked against the file's size
    if (m_bytes.size() < HEADER_SIZE || memcmp(m_bytes.data(), MAGIC, sizeof(MAGIC)) != 0)
        throw FormatFileException(fileName);
    const uint32_t flags = read4(8);
    if ((flags & ~FLAG_IDENTS) != 0) throw FormatFileException(fileName);
    m_hasIdents = (flags & FLAG_IDENTS) != 0;
    const uint64_t count = read4(12);
    m_firstIdent = read4(16);
    //idents (if any), offsets, parNom: 4 bytes each
    const uint64_t arrays = (m_hasIdents ? count : 0) + (count + 1) + count;
    if (HEADER_SIZE + 4 * arrays > m_bytes.size()) throw FormatFileException(fileName);
    m_size = (size_t)count;
    m_identsPos = HEADER_SIZE;
    m_offsetsPos = m_identsPos + (m_hasIdents ? 4 * m_size : 0);
    m_byNamePos = m_offsetsPos + 4 * (m_size + 1);
    m_namesPos = m_byNamePos + 4 * m_size;
    if (!m_hasIdents && m_size != 0 && (uint64_t)m_firstIdent + m_size - 1 > 0xFFFFFFFFu)
        throw FormatFileException(fileName);
    //offsets: from 0, increasing, the last one the end of the file
    if (read4(m_offsetsPos) != 0) throw FormatFileException(fileName);
    for (size_t i = 1; i <= m_size; i++) {
        if (read4(m_offsetsPos + 4 * i) < read4(m_offsetsPos + 4 * (i - 1))) throw FormatFileException(fileName);
    }
    if (m_namesPos + (uint64_t)read4(m_offsetsPos + 4 * m_size) != m_bytes.size()) throw FormatFileException(fileName);
    //idents strictly increasing
    for (size_t i = 1; i < m_size && m_hasIdents; i++) {
        if (read4(m_identsPos + 4 * i) <= read4(m_identsPos + 4 * (i - 1))) throw FormatFileException(fileName);
    }
    //name order: ranks within bounds, names strictly increasing (so unique, and a permutation)
    for (size_t i = 0; i < m_size; i++) {
        if (read4(m_byNamePos + 4 * i) >= m_size) throw FormatFileException(fileName);
        if (i == 0) continue;
        const pair<size_t, size_t> name = nameAt(read4(m_byNamePos + 4 * i));
        if (compareName(read4(m_byNamePos + 4 * (i - 1)), (const char *)m_bytes.data() + name.first, name.second) >= 0)
            throw FormatFileException(fileName);
    }
}
////////////////////////////////////////////////////////////
//brief get the name of a document
bool NindDocNames::getName(const uint32_t docIdent,
                           string &name) const
{
    size_t rank;
    if (!m_hasIdents) {
        if (docIdent < m_firstIdent || (uint64_t)docIdent - m_firstIdent >= m_size) return false;
        rank = docIdent - m_firstIdent;
    }
    else {
        size_t low = 0, high = m_size;
        while (low < high) {
            const size_t middle = low + (high - low) / 2;
            if (read4(m_identsPos + 4 * middle) < docIdent) low = middle + 1;
            else high = middle;
        }
        if (low == m_size || read4(m_identsPos + 4 * low) != docIdent) return false;
        rank = low;
    }
    const pair<size_t, size_t> span = nameAt(rank);
    name.assign((const char *)m_bytes.data() + span.first, span.second);
    return true;
}
////////////////////////////////////////////////////////////
//brief get the document of a name
bool NindDocNames::getDocIdent(const string &name,
                               uint32_t &docIdent) const
{
    size_t low = 0, high = m_size;
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (compareName(read4(m_byNamePos + 4 * middle), name.data(), name.size()) < 0) low = middle + 1;
        else high = middle;
    }
    if (low == m_size) return false;
    const size_t rank = read4(m_byNamePos + 4 * low);
    if (compareName(rank, name.data(), name.size()) != 0) return false;
    docIdent = docIdentAt(rank);
    return true;
}
////////////////////////////////////////////////////////////
//brief every (document, name), in increasing document ident order
void NindDocNames::getEntries(vector<uint32_t> &docIdents,
                              vector<string> &names) const
{
    docIdents.resize(m_size);
    names.resize(m_size);
    for (size_t i = 0; i < m_size; i++) {
        docIdents[i] = docIdentAt(i);
        const pair<size_t, size_t> span = nameAt(i);
        names[i].assign((const char *)m_bytes.data() + span.first, span.second);
    }
}
////////////////////////////////////////////////////////////
uint32_t NindDocNames::read4(const size_t offset) const
{
    const unsigned char *p = m_bytes.data() + offset;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
////////////////////////////////////////////////////////////
uint32_t NindDocNames::docIdentAt(const size_t rank) const
{
    return m_hasIdents ? read4(m_identsPos + 4 * rank) : m_firstIdent + (uint32_t)rank;
}
////////////////////////////////////////////////////////////
pair<size_t, size_t> NindDocNames::nameAt(const size_t rank) const
{
    const size_t begin = read4(m_offsetsPos + 4 * rank);
    const size_t end = read4(m_offsetsPos + 4 * (rank + 1));
    return pair<size_t, size_t>(m_namesPos + begin, end - begin);
}
////////////////////////////////////////////////////////////
int NindDocNames::compareName(const size_t rank,
                              const char *name,
                              const size_t length) const
{
    const pair<size_t, size_t> span = nameAt(rank);
    const int cmp = memcmp(m_bytes.data() + span.first, name, min(span.second, length));
    if (cmp != 0) return cmp;
    return (span.second < length) ? -1 : (span.second > length ? 1 : 0);
}
////////////////////////////////////////////////////////////
