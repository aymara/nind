//
// C++ Interface: NindIndex
//
// Description: La gestion d'un fichier (inverse ou d'index locaux)
// voir "nind, indexation post-S2", LAT2014.JYS.440
//
// Cette classe gere la complexite du fichier qui doit rester coherent pour ses lecteurs
// pendant que son ecrivain l'enrichit en fonction des nouvelles indexations.
// Cette classe est dérivable.
//
// Author: jys <jy.sage@orange.fr>, (C) LATEJCON 2017
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
#ifndef NindIndex_H
#define NindIndex_H
////////////////////////////////////////////////////////////
#include "NindBasics/NindPadFile.h"
#include "NindBasics/NindFile.h"
#include "NindBasics/NindDocId.h"
#include "NindCommonExport.h"
#include "NindExceptions.h"
#include <cstdint>
#include <stdio.h>
#include <string>
#include <list>
#include <vector>
////////////////////////////////////////////////////////////
namespace latecon {
    namespace nindex {
////////////////////////////////////////////////////////////
class DLLExportLexicon NindIndex : public NindPadFile {
public:
    /**\brief Full structural analysis of an index file (on top of the pad-file envelope) */
    struct IndexStats {
        unsigned int maxIdent;
        unsigned int usedCount;
        NindPadFile::HoleStats holes;
        NindPadFile::Repartition definitionSizes;
        IndexStats(): maxIdent(0), usedCount(0), holes(), definitionSizes() {}
    };

    /**\brief Validate the file and compute indirection-usage statistics
    *\return the file's index-level structural statistics
    *\throws NindPadFileException if the file is structurally invalid */
    IndexStats analyseIndex();

    /**\brief Enter bulk mode, to write many definitions fast (writer only).
    * Until endBulk(), definitions are appended to the file, indirections and the tail
    * (specifics + identification) stay in memory and writes aren't flushed one by one:
    * the file is invalid for readers until endBulk(), and a crash leaves it unusable.
    * For a file that no reader opens before it is complete (e.g. a new index generation).
    * Definitions are placed exactly as in normal mode (same holes reused), so the file
    * ends up byte-identical to the same writes in normal mode. */
    void beginBulk();

    /**\brief Leave bulk mode: write the indirections and the tail, then flush.
    * Also called by the destructor (errors then ignored): call it explicitly. */
    void endBulk();

    /**\brief Test if bulk mode is on
    *\return true if in bulk mode */
    bool isBulk() const { return m_isBulk; }

    /**\brief true for a file of wide fields: format v2 (10-byte indirections) or v3 (11, a term
    * index), false for v1 (8): a v1 file is read, but a writer refuses to extend it (rebuild it,
    * e.g. with NindIndexMerge) */
    bool isFormatV2() const { return getDataEntrySize() >= 10; }

    /**\brief true for a term index of format v3 (11-byte indirections: their kind, see ENTRY_*) */
    bool isFormatV3() const { return getDataEntrySize() == 11; }

    /**\brief A format v3 indirection's kind (<genreEntreje>): none, a definition, or a term of one
    * posting without definition (its document and frequency in the indirection's fields) */
    static const unsigned int ENTRY_NONE = 0, ENTRY_DEFINITION = 1, ENTRY_SINGLETON = 2;

protected:

    //the fields of an indirection, as wide as the file's format
    uint64_t getIndirectionOffset(NindReadBuffer &in) const {
        return isFormatV2() ? in.getInt6() : in.getInt5(); }
    unsigned int getIndirectionLength(NindReadBuffer &in) const {
        return isFormatV2() ? in.getInt4() : in.getInt3(); }
    //an indirection as a definition's area: a v3 term of one posting has none (offset and length 0)
    void readIndirection(NindReadBuffer &in, uint64_t &offset, unsigned int &length) const {
        unsigned int kind;
        readEntry(in, offset, length, kind);
        if (kind != ENTRY_DEFINITION) { offset = 0; length = 0; }
    }
    //an indirection's fields and kind (v1, v2: ENTRY_DEFINITION, or ENTRY_NONE at offset 0)
    void readEntry(NindReadBuffer &in, uint64_t &offset, unsigned int &length, unsigned int &kind) const {
        offset = getIndirectionOffset(in);
        length = getIndirectionLength(in);
        if (!isFormatV3()) { kind = offset != 0 ? ENTRY_DEFINITION : ENTRY_NONE; return; }
        kind = in.getInt1();
        if (kind > ENTRY_SINGLETON || (kind == ENTRY_DEFINITION && offset == 0))
            throw NindIndexException("NindIndex bad indirection kind : " + m_fileName);
    }
    //an indirection's fields, then (v3) its kind
    void putIndirection(const uint64_t offset, const uint64_t length, const unsigned int kind) {
        putIndirectionOffset(offset);
        putIndirectionLength(length);
        if (isFormatV3()) m_file.putInt1(kind);
    }
    void putIndirectionOffset(const uint64_t offset) {
        if (isFormatV2()) m_file.putInt6(offset); else m_file.putInt5(offset); }
    void putIndirectionLength(const uint64_t length) {
        if (isFormatV2()) m_file.putInt4(length);
        else if (length > 0xFFFFFFu) throw OutOfBoundException("value over 3 bytes " + m_fileName);
        else m_file.putInt3((unsigned int)length); }

    //a definition header's identifier or length: Entier3 (v1) or Entier4 (v2)
    unsigned int getDefinitionField(NindReadBuffer &in) const {
        return isFormatV2() ? in.getInt4() : in.getInt3(); }
    void putDefinitionField(const uint64_t value) {
        if (isFormatV2()) m_file.putInt4(value);
        else if (value > 0xFFFFFFu) throw OutOfBoundException("value over 3 bytes " + m_fileName);
        else m_file.putInt3((unsigned int)value); }
    void putDefinitionFieldAt(const uint64_t value, const unsigned int offset) {
        if (isFormatV2()) m_file.putInt4(value, offset);
        else if (value > 0xFFFFFFu) throw OutOfBoundException("value over 3 bytes " + m_fileName);
        else m_file.putInt3((unsigned int)value, offset); }
    //a count or a document id delta: EntierULat (v1) or EntierULat64 (v2)
    uint64_t getCountField(NindReadBuffer &in) const {
        return isFormatV2() ? in.getUIntLat64() : in.getUIntLat(); }
    void putCountField(const uint64_t value) {
        if (isFormatV2()) m_file.putUIntLat64(value); else m_file.putUIntLat(value); }
    //an external document identifier: Entier4 (v1) or Entier8 (v2)
    NindDocId getExternalDocId(NindReadBuffer &in) const {
        return isFormatV2() ? in.getInt8() : in.getInt4(); }
    void putExternalDocId(const NindDocId ident) {
        if (isFormatV2()) m_file.putInt8(ident); else m_file.putInt4(ident); }

    /**\brief Creates NindIndex with a specified name associated with.
    *\param fileName absolute path file name
    *\param isWriter true if writer, false if reader  
    *\param lexiconIdentification unique identification of lexicon  (if 0, no checks)
    *\param specificsSize size in bytes of specific datas
    *\param definitionMinimumSize size in bytes of the smallest definition
    *\param indirectionBlocSize number of entries in a single indirection block */
    NindIndex(const std::string &fileName,
              const bool isWriter,
              const Identification &lexiconIdentification,
              const unsigned int specificsSize,
              const unsigned int definitionMinimumSize = 0,
              const unsigned int indirectionBlocSize = 0,
              const unsigned int newEntrySize = 10,
              const unsigned int otherEntrySize = 0);

    virtual ~NindIndex();
    
    /**\brief Thread-safe (const) read of a definition into buffer (see getDefinition)
    *\param ident ident of definition
    *\param buffer read buffer of the caller, receives the definition
    *\param bytesNb bytes to read, 0 for the whole definition
    *\return true if the definition exists */
    bool getDefinitionAt(const unsigned int ident,
                         NindReadBuffer &buffer,
                         const unsigned int bytesNb = 0) const;

    /**\brief Thread-safe (const) place of a definition in the file, without reading it
    *\param ident ident of definition
    *\param buffer read buffer of the caller (for the indirection)
    *\param offset receives the definition's offset in the file
    *\param length receives its length
    *\return true if the definition exists */
    bool getDefinitionRefAt(const unsigned int ident,
                            NindReadBuffer &buffer,
                            uint64_t &offset,
                            unsigned int &length) const;

    /**\brief Read from file datas of specified definition and leave result into read buffer 
    *\param ident ident of definition
    *\param bytesNb optional number of bytes to read, in case of partial read
    *\return true if ident was found, false otherwise */
    bool getDefinition(const unsigned int ident,
                       const unsigned int bytesNb = 0);
        
    /**\brief Write on file datas of specified definition yet constructed into write buffer
    * buffer contains datas of specified definition + specific datas + identification
    *\param ident ident of definition */
    void setDefinition(const unsigned int ident);

    /**\brief Rewrite parts of a definition in place, its place and length unchanged: the write
    * buffer holds the parts, in order, then the specifics and identification (bulk mode or not;
    * like any rewrite in place, a reader of another process may see it half-written)
    *\param ident ident of definition
    *\param parts (offset in the definition, length) of each part */
    void setDefinitionParts(const unsigned int ident,
                            const std::vector<std::pair<unsigned int, unsigned int> > &parts);

    /**\brief The place of a definition (in bulk mode: where the bulk session has it)
    *\return false if there is none */
    bool getDefinitionArea(const unsigned int ident,
                           uint64_t &offset,
                           unsigned int &length);

    /**\brief (format v3) a term of one posting, in its indirection, without definition: the
    * write buffer holds the specifics and identification (as an erasure's); a definition it had is
    * freed. document below 2^48 */
    void setSingleton(const unsigned int ident,
                      const uint64_t document,
                      const unsigned int frequency);

    /**\brief (format v3) a term of one posting, sequential read (in bulk mode too)
    *\return false if its indirection isn't one */
    bool getSingleton(const unsigned int ident,
                      uint64_t &document,
                      unsigned int &frequency);

    /**\brief Thread-safe (const) read of an indirection: its kind (ENTRY_*) and fields (a
    * definition's offset and length, or a term of one posting's document and frequency)
    *\return false if the identifier has no indirection */
    bool getEntryAt(const unsigned int ident,
                    NindReadBuffer &buffer,
                    uint64_t &offset,
                    unsigned int &length,
                    unsigned int &kind) const;

    /**\brief The smallest area a definition takes (see the constructor), for a file whose format
    * allows smaller ones */
    void setDefinitionMinimumSize(const unsigned int size) { m_definitionMinimumSize = size; }

    /**\brief (format v3) a term of one posting, thread-safe (const) read
    *\return false if its indirection isn't one */
    bool getSingletonAt(const unsigned int ident,
                        NindReadBuffer &buffer,
                        uint64_t &document,
                        unsigned int &frequency) const;
        
    /**\brief check if indirection exists and create indirection block if necessary
    *\param ident ident of definition
    *\param fileIdentification unique identification of file */
    void checkExtendIndirection(const unsigned int ident,
                                const Identification &fileIdentification);
    
private:
    //ejcrit une nouvelle indirection dans l'index (v3: of kind ENTRY_DEFINITION, ENTRY_NONE at 0)
    void setIndirection(const uint64_t indirection,
                        const uint64_t offsetDejfinition,
                        const unsigned int longueurDejfinition,
                        const unsigned int kind = ENTRY_DEFINITION);

    //ejtablit la carte des vides
    void mapEmptySpaces();

    //remplit la carte des vides ah partir de la liste des zones occupejes (blocs d'indirection en plus)
    void fillEmptyAreas(std::list<std::pair<uint64_t, unsigned int> > &nonVidesList);

    //ejcriture d'une dejfinition en mode massif
    void setDefinitionBulk(const unsigned int ident,
                           const unsigned int tailleQueue);

    //indirections en mejmoire en mode massif ; format v3, their kinds (ENTRY_SINGLETON: the pair
    //holds the document and frequency, not an area)
    std::vector<std::pair<uint64_t, unsigned int> > m_bulkIndirections;
    std::vector<unsigned char> m_bulkKinds;
    bool bulkSingleton(const unsigned int ident) const {
        return ident < m_bulkKinds.size() && m_bulkKinds[ident] == ENTRY_SINGLETON; }

    //trouve une nouvelle zone pour les nouvelles donnejs
    //retourne true si l'identification est dejjah ejcrite
    bool findNewArea(const unsigned int dataSize,
                     uint64_t &offsetDejfinition,
                     unsigned int &longueurDejfinition);

    //brief Place l'ancienne zone de donnejes dans la gestion du vide
    void vacateOldArea(const uint64_t oldOffsetEntry,
                       const unsigned int oldLengthEntry);

    //dumpe la map des indirections (uniquement pour debogue)
    void dumpIndirection();

    unsigned int m_definitionMinimumSize;       //taille minimum admissible pour une definition
    std::list<std::pair<uint64_t, unsigned int> > m_emptyAreas;         //gestion des zones libres
};
////////////////////////////////////////////////////////////
    } // end namespace
} // end namespace
#endif
////////////////////////////////////////////////////////////
