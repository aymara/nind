//
// C++ Interface: NindMappedFile
//
// Description: a whole file mapped read-only into memory
//
// This file is part of NIND (as "nouvelle indexation").
// NIND is free software: you can redistribute it and/or modify it under the terms of the
// GNU Lesser General Public License (see <http://www.gnu.org/licenses/>), either version 3 of
// the License, or any later version.
////////////////////////////////////////////////////////////
#ifndef NindMappedFile_H
#define NindMappedFile_H
////////////////////////////////////////////////////////////
#include "NindCommonExport.h"
#include "NindExceptions.h"
#include <cstddef>
#include <string>
////////////////////////////////////////////////////////////
namespace latecon {
    namespace nindex {
////////////////////////////////////////////////////////////
/**\brief A whole file mapped read-only (mmap; MapViewOfFile on Windows): its pages are read on
* first access and belong to the system's page cache, shared by every process reading the file
* and given back under memory pressure, instead of the process's own memory. For files written
* once and never modified (a later write to the file would show through). The mapping stays
* valid after the file is removed (POSIX; on Windows a mapped file can't be removed). The
* bytes are untrusted, as any read: their reader checks every offset against size(). Not
* copyable; thread-safe (read-only). */
class DLLExportLexicon NindMappedFile {
public:
    /**\brief Map a file
    *\throws OpenFileException if it can't be opened, ReadFileException if it can't be mapped */
    explicit NindMappedFile(const std::string &fileName);
    ~NindMappedFile();

    /**\brief the file's bytes (null for an empty file) */
    const unsigned char *data() const { return m_data; }
    /**\brief their number */
    size_t size() const { return m_size; }

private:
    NindMappedFile(const NindMappedFile &);
    NindMappedFile &operator=(const NindMappedFile &);
    const unsigned char *m_data;
    size_t m_size;
#ifdef _WIN32
    void *m_mapping;            //the file mapping object's handle (its view keeps the file open)
#endif
};
////////////////////////////////////////////////////////////
    } // end namespace
} // end namespace
#endif
////////////////////////////////////////////////////////////
