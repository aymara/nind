//
// C++ Interface: NindDocNames
//
// Description: the names of the documents of an index (their identifiers in the
// corpus, e.g. "d49309"), for both lookups: document id -> name and name -> document id.
// Written once (a new index generation is never modified), mapped for reading.
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
#ifndef NindDocNames_H
#define NindDocNames_H
////////////////////////////////////////////////////////////
#include "NindCommonExport.h"
#include "NindBasics/NindDocId.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
////////////////////////////////////////////////////////////
namespace latecon {
    namespace nindex {
class NindMappedFile;
////////////////////////////////////////////////////////////
/**\brief The names of the documents of an index (file <name>.ninddocnames).
* <fichier>        ::= <magic="NINDDN02"> <flags> <nombre> <premierIdent>
*                      [ <idents> ] <offsets> <parNom> <noms>
* <flags>          ::= <Entier4>    bit 0 : idents given (otherwise premierIdent, premierIdent + 1...)
* <nombre>         ::= <Entier4>    number of documents
* <premierIdent>   ::= <Entier8>    (64-bit NindDocId; <Entier4> in NINDDN01, still read)
* <idents>         ::= { <Entier8> }    nombre idents, strictly increasing (<Entier4> in NINDDN01)
* <offsets>        ::= { <Entier4> }    nombre + 1 offsets into <noms>, the first 0, increasing
*                                       (magic NINDDN03: <Entier8>, only for names over 4 GiB)
* <parNom>         ::= { <Entier4> }    the documents' ranks in increasing name order (byte order)
* <noms>           ::= { <Octet> }      the names (UTF-8), in document order
* Entier4: 4 bytes, little-endian. Names are unique; they may be empty.
* Read through a mapping of the file (NindMappedFile): the names stay in the page cache, shared by
* the processes reading them, not in the process's memory (4.6 GiB for MS MARCO v2's 138M
* passages). Opening checks the header and the arrays' sizes; each lookup checks the bounds of
* what it reads (FormatFileException otherwise); check() checks the whole file (offsets
* increasing, idents and names in order: a file that passes gives every lookup right). */
class DLLExportLexicon NindDocNames {
public:
    /**\brief Write the names of documents into a new file
    *\param fileName file name (must not exist)
    *\param docIdents document idents
    *\param names their names, in the same order (unique)
    *\throws std::invalid_argument if the sizes differ or an ident or a name is repeated
    *\throws OpenFileException or WriteFileException on I/O errors (the file is then removed) */
    static void write(const std::string &fileName,
                      const std::vector<NindDocId> &docIdents,
                      const std::vector<std::string> &names);

    /**\brief Merge the names files of shards into a new file, the same as write() with all their
    * documents, without building the names in memory (shards' files are loaded, the result streamed)
    *\param shardFiles the shards' files, in any order
    *\param fileName the new file (must not exist; removed on failure)
    *\throws std::invalid_argument if a document or a name is in several shards
    *\throws OpenFileException, ReadFileException, FormatFileException, WriteFileException on I/O errors */
    static void merge(const std::vector<std::string> &shardFiles,
                      const std::string &fileName);

    /**\brief merge() without some of the shards' documents
    *\param deleted per shard, its documents to leave out, in increasing order (empty: none) */
    static void merge(const std::vector<std::string> &shardFiles,
                      const std::vector<std::vector<NindDocId> > &deleted,
                      const std::string &fileName);

    /**\brief Read the names of an index
    *\param fileName file name
    *\throws OpenFileException, ReadFileException, FormatFileException if invalid */
    explicit NindDocNames(const std::string &fileName);
    ~NindDocNames();

    /**\brief Check the whole file: offsets increasing, idents strictly increasing, names in strictly
    * increasing order (unique)
    *\throws FormatFileException if invalid */
    void check() const;

    /**\brief number of documents */
    size_t size() const { return m_size; }

    /**\brief get the name of a document
    *\param docIdent document ident
    *\param name receives its name
    *\return true if the document has a name */
    bool getName(const NindDocId docIdent,
                 std::string &name) const;

    /**\brief get the document of a name
    *\param name the name
    *\param docIdent receives the document ident
    *\return true if a document has that name */
    bool getDocIdent(const std::string &name,
                     NindDocId &docIdent) const;

    /**\brief every (document, name), in increasing document ident order
    *\param docIdents receives the documents
    *\param names receives their names */
    void getEntries(std::vector<NindDocId> &docIdents,
                    std::vector<std::string> &names) const;

private:
    uint32_t read4(const size_t offset) const;
    uint64_t readOffset(const size_t offset) const;
    NindDocId readIdent(const size_t offset) const;
    NindDocId docIdentAt(const size_t rank) const;
    //the name of the document of rank rank: (offset into m_bytes, length)
    std::pair<size_t, size_t> nameAt(const size_t rank) const;
    //compares the name of rank rank with name (like std::string::compare)
    int compareName(const size_t rank, const char *name, const size_t length) const;
    //the rank of the document at a place in name order (checked)
    size_t rankByName(const size_t place) const;

    NindDocNames(const NindDocNames &);
    NindDocNames &operator=(const NindDocNames &);
    std::string m_fileName;
    std::unique_ptr<NindMappedFile> m_file;
    const unsigned char *m_bytes;           //the whole file, mapped
    size_t m_fileSize;
    size_t m_size;
    bool m_hasIdents;
    NindDocId m_firstIdent;
    size_t m_identWidth;            //4 (NINDDN01) or 8 (NINDDN02, 03) bytes per ident
    size_t m_offsetWidth;           //4 (NINDDN01, 02) or 8 (NINDDN03) bytes per offset
    size_t m_identsPos, m_offsetsPos, m_byNamePos, m_namesPos;
};
////////////////////////////////////////////////////////////
    } // end namespace
} // end namespace
#endif
////////////////////////////////////////////////////////////
