//
// NindDocId.h: the type of external document identifiers
//
// This file is part of NIND (as "nouvelle indexation").
// NIND is free software: you can redistribute it and/or modify it under the terms of the
// GNU Lesser General Public License (see <http://www.gnu.org/licenses/>), either version 3 of
// the License, or any later version.
////////////////////////////////////////////////////////////
#ifndef NindDocId_H
#define NindDocId_H
#include <cstdint>
namespace latecon {
    namespace nindex {
/**\brief An external document identifier: the application's numbering of its documents, as
* the term index's postings, the local index and the documents' names hold it. 64 bits (format
* v2, docs/proposals/format-v2-blocks.md); the v1 formats store 32 bits and refuse larger ones.
* (The internal numbering of documents, in the local index, stays 32 bits: at most 4.29G
* documents in one index.) */
typedef uint64_t NindDocId;
/**\brief the largest document identifier the v1 formats store */
const NindDocId NIND_DOC_ID_MAX_V1 = 0xFFFFFFFFull;
    } // end namespace
} // end namespace
#endif
