//
// C++ Interface: NindDocIdMap
//
// Description: Table compacte des identifiants externes de documents vers les identifiants internes.
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
#ifndef NindDocIdMap_H
#define NindDocIdMap_H
////////////////////////////////////////////////////////////
#include "NindBasics/NindDocId.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>
////////////////////////////////////////////////////////////
namespace latecon {
    namespace nindex {
////////////////////////////////////////////////////////////
/**\brief External -> internal document ids, for hundreds of millions of documents (a std::map takes
* ~48 bytes per document). Ids given in increasing order go to flat arrays: 4 bytes per document
* while they are consecutive (found by position), 12 otherwise (found by binary search); the others
* go to a map. Internal id 0 is not a valid id: find() returns it for an unknown document. Not
* thread-safe (NindLocalIndex holds its m_docIdMutex). */
class NindDocIdMap {
public:
    NindDocIdMap(): m_consecutive(true), m_first(0), m_size(0) {}

    /**\brief the internal id of a document, 0 if unknown */
    unsigned int find(const NindDocId ident) const {
        const size_t position = flatPosition(ident);
        if (position != NOT_FLAT) return m_internals[position];
        if (m_others.empty()) return 0;
        const std::map<NindDocId, unsigned int>::const_iterator it = m_others.find(ident);
        return it == m_others.end() ? 0 : it->second;
    }

    /**\brief add a document, or change its internal id (not 0) */
    void set(const NindDocId ident, const unsigned int internal) {
        const size_t position = flatPosition(ident);
        if (position != NOT_FLAT) {
            if (m_internals[position] == 0) m_size++;
            m_internals[position] = internal;
            return;
        }
        if (!m_others.empty()) {
            const std::map<NindDocId, unsigned int>::iterator it = m_others.find(ident);
            if (it != m_others.end()) {
                it->second = internal;
                return;
            }
        }
        m_size++;
        if (m_internals.empty() || ident > lastFlat()) appendFlat(ident, internal);
        else m_others[ident] = internal;
    }

    /**\brief remove a document (nothing if unknown) */
    void erase(const NindDocId ident) {
        const size_t position = flatPosition(ident);
        if (position != NOT_FLAT) {
            if (m_internals[position] != 0) m_size--;
            m_internals[position] = 0;
            return;
        }
        m_size -= m_others.erase(ident);
    }

    /**\brief number of documents */
    size_t size() const { return m_size; }

    /**\brief (external, internal) of every document, by increasing external id, to visitor */
    template <typename Visitor>
    void forEach(Visitor visitor) const {
        std::map<NindDocId, unsigned int>::const_iterator other = m_others.begin();
        for (size_t i = 0; i < m_internals.size(); i++) {
            const NindDocId ident = m_consecutive ? m_first + i : m_idents[i];
            for (; other != m_others.end() && other->first < ident; ++other) visitor(other->first, other->second);
            if (m_internals[i] != 0) visitor(ident, m_internals[i]);
        }
        for (; other != m_others.end(); ++other) visitor(other->first, other->second);
    }

private:
    static const size_t NOT_FLAT = ~(size_t)0;

    NindDocId lastFlat() const {
        return m_consecutive ? m_first + (m_internals.size() - 1) : m_idents.back();
    }
    //its place in the flat arrays (with internal id 0 if erased), NOT_FLAT if it has none
    size_t flatPosition(const NindDocId ident) const {
        if (m_internals.empty()) return NOT_FLAT;
        if (m_consecutive) {
            if (ident < m_first || ident - m_first >= m_internals.size()) return NOT_FLAT;
            return (size_t)(ident - m_first);
        }
        const std::vector<NindDocId>::const_iterator it = std::lower_bound(m_idents.begin(), m_idents.end(), ident);
        return (it == m_idents.end() || *it != ident) ? NOT_FLAT : (size_t)(it - m_idents.begin());
    }
    //ident is after the last flat one
    void appendFlat(const NindDocId ident, const unsigned int internal) {
        if (m_internals.empty()) m_first = ident;
        else if (m_consecutive && ident != lastFlat() + 1) {
            //a gap: the ids are stored from now on
            m_idents.resize(m_internals.size());
            for (size_t i = 0; i < m_idents.size(); i++) m_idents[i] = m_first + i;
            m_consecutive = false;
        }
        if (!m_consecutive) m_idents.push_back(ident);
        m_internals.push_back(internal);
    }

    bool m_consecutive;                     //the flat ids are m_first, m_first + 1... (m_idents unused)
    NindDocId m_first;
    size_t m_size;
    std::vector<NindDocId> m_idents;        //the flat ids, in increasing order
    std::vector<uint32_t> m_internals;      //their internal ids, 0 = erased
    std::map<NindDocId, unsigned int> m_others;     //ids that came out of order
};
////////////////////////////////////////////////////////////
    } // end namespace
} // end namespace
#endif
