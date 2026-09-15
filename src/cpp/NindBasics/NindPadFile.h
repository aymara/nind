//
// C++ Interface: NindPadFile
//
// Description: La gestion basique d'un fichier nind : correspondance ident -> donnejes et identification
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
#ifndef NindPadFile_H
#define NindPadFile_H
////////////////////////////////////////////////////////////
#include "NindFile.h"
#include "NindCommonExport.h"
#include "NindExceptions.h"
#include <cstdint>
#include <stdio.h>
#include <string>
#include <list>
#include <map>
#include <utility>
////////////////////////////////////////////////////////////
namespace latecon {
    namespace nindex {
////////////////////////////////////////////////////////////
class DLLExportLexicon NindPadFile {

public:
    /**\brief Structure to hold files identification */
    struct Identification {
        unsigned int lexiconWordsNb;
        unsigned int lexiconTime;
        Identification(): lexiconWordsNb(0), lexiconTime(0) {}
        Identification(const unsigned int nb, const unsigned int id): lexiconWordsNb(nb), lexiconTime(id) {}
        ~Identification() {}
        bool operator==(const Identification &id2) const {
            return (this->lexiconWordsNb == id2.lexiconWordsNb && this->lexiconTime == id2.lexiconTime); }
        bool operator!=(const Identification &id2) const {
            return (this->lexiconWordsNb != id2.lexiconWordsNb || this->lexiconTime != id2.lexiconTime); }
    };

    /**\brief Basic descriptive statistics (count/min/max/sum/mean/stddev) over a list of numbers */
    struct Repartition {
        unsigned int count;
        int64_t minValue;
        int64_t maxValue;
        int64_t sum;
        double mean;
        double stddev;
        Repartition(): count(0), minValue(0), maxValue(0), sum(0), mean(0.0), stddev(0.0) {}
    };

    /**\brief Statistics about the "holes" (unused gaps) between a file's occupied byte ranges */
    struct HoleStats {
        unsigned int holesCount;
        uint64_t holesSize;
        std::map<uint64_t, unsigned int> holeSizeHistogram;        //{length: count}
        std::map<uint64_t, unsigned int> occupiedSizeHistogram;    //{length: count}
        HoleStats(): holesCount(0), holesSize(0), holeSizeHistogram(), occupiedSizeHistogram() {}
    };

    /**\brief Statistics about one indexed block of the pad-file envelope */
    struct BlockStats {
        uint64_t blockAddr;
        unsigned int blockNum;
        unsigned int entriesUsed;
        unsigned int entriesTotal;
        uint64_t enVracAddr;
        uint64_t enVracSize;
        BlockStats(): blockAddr(0), blockNum(0), entriesUsed(0), entriesTotal(0), enVracAddr(0), enVracSize(0) {}
    };

    /**\brief Full structural analysis of a pad file's envelope */
    struct PadFileStats {
        unsigned int dataEntrySize;
        unsigned int specificsSize;
        std::list<BlockStats> blocks;
        uint64_t indexTotalSize;      //tailleIndex
        uint64_t enVracTotalSize;     //tailleEnVrac
        Identification identification;
        uint64_t fileSize;
        PadFileStats(): dataEntrySize(0), specificsSize(0), blocks(), indexTotalSize(0),
            enVracTotalSize(0), identification(), fileSize(0) {}
    };

    /**\brief Walk and validate the whole pad-file envelope, computing size statistics
    *\return the file's structural statistics
    *\throws NindPadFileException if the file's structure is invalid */
    PadFileStats analysePadFile();

protected:
    /**\brief Compute basic descriptive statistics over a list of numbers
    *\param nombres the numbers to summarize
    *\return the computed statistics (all zero if nombres is empty) */
    static Repartition calculeRejpartition(const std::list<int64_t> &nombres);

    /**\brief Derive the "holes" (unused gaps) between a file's occupied byte ranges
    *\param nonVidesList occupied (address, length) ranges (a local sorted copy is used)
    *\return the computed hole statistics
    *\throws NindPadFileException if two occupied ranges overlap */
    static HoleStats chercheVides(std::list<std::pair<uint64_t, unsigned int> > nonVidesList);

protected:
//<flagIndexej=47>(1) <addrBlocSuivant>(5) <nombreIndex>(3) = 9
#define TAILLE_TETE_INDEX 9

    /**\brief Creates NindPadFile with a specified name associated with.
    *\param fileName absolute path file name
    *\param isWriter true if writer, false if reader
    *\param referenceIdentification unique identification for checking (if 0, no checks)
    *\param specificsSize size in bytes of specific datas
    *\param dataEntrySize size in bytes of one data entry (for neo writer)
    *\param dataEntriesBlocSize number of entries in a single block (for neo writer) */
    NindPadFile(const std::string &fileName,
                const bool isWriter,
                const Identification &referenceIdentification,
                const unsigned int specificsSize,
                const unsigned int dataEntrySize = 0,
                const unsigned int dataEntriesBlocSize = 0);

    virtual ~NindPadFile();

    /**\brief get position of specified entry (with reader synchronization)
    *\param ident ident of specified entry
    *\return offset into file of specified entry (0 if out of bounds)*/
    uint64_t getEntryPos(const unsigned int ident);

    /**\brief add empty entries block
    *\param fileIdentification unique identification of file */
    void addEntriesBlock(const Identification &fileIdentification);

    /**\brief get size of 1rst entries block
    *\return size of 1rst entries block */
    unsigned int getFirstEntriesBlockSize();

    //return l'identifiant maximum possible avec le systehme actuel d'indirection
    unsigned int getMaxIdent() const;

    /**\brief Read from file specific datas and leave result into read buffer */
    void getSpecifics();

    /**\brief get identification of file
    *\param identification where unique identification of file is returned */
    void getFileIdentification(Identification &identification);

    /**\brief get size of specific datas + identification */
    unsigned int getSpecificsAndIdentificationSize() const;

    /**\brief write specifics header into write buffer */
    void writeSpecificsHeader();

    /**\brief write identification into write buffer
    * \param fileIdentification unique identification of file */
    void writeIdentification(const Identification &fileIdentification);

public:
    /**\brief get file name
    * \return file name */
    std::string getFileName();
    NindFile m_file;                //pour l'ecrivain ou le lecteur

protected:
    std::string m_fileName;
    bool m_isWriter;                    //vrai si ejcrivain
    bool m_isExistingWriter;            //vrai si ejcrivain dejjah existant

    std::list<std::pair<uint64_t, unsigned int> > m_entriesBlocksMap;  //gestion des blocs d'entrejes

private:
    //retourne la position d'une entreje sans synchronisation
    uint64_t getJustEntryPos(const unsigned int ident);

    //ejtablit la carte des blocs d'entrejes
    void mapEntriesBlocks();

    //vejrifie la structure des spejcifiques
    void checkSpejcifiques();

    //vejrifie l'apairage avec la rejfejrence
    void checkIdentification(const Identification &referenceIdentification);

    unsigned int m_specificsSize;                //size in bytes of specific datas
    unsigned int m_dataEntrySize;               //size in bytes of one data entry (for neo writer)
    unsigned int m_dataEntriesBlocSize;         //number of entries in a single block (for neo writer)
};
////////////////////////////////////////////////////////////
    } // end namespace
} // end namespace
#endif
////////////////////////////////////////////////////////////
