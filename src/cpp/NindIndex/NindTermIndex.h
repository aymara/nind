//
// C++ Interface: NindTermIndex
//
// Description: La gestion du fichier inverse en fichier
// voir "nind, indexation post-S2", LAT2014.JYS.440
//
// Cette classe gere la complexite du fichier inverse qui doit rester coherent pour ses lecteurs
// pendant que son ecrivain l'enrichit en fonction des nouvelles indexations.
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
#ifndef NindTermIndex_H
#define NindTermIndex_H
////////////////////////////////////////////////////////////
#include "NindIndex.h"
#include "NindBasics/NindDocId.h"
#include "NindCommonExport.h"
#include "NindExceptions.h"
#include <stdio.h>
#include <string>
#include <list>
#include <vector>
////////////////////////////////////////////////////////////
namespace latecon {
    namespace nindex {
////////////////////////////////////////////////////////////
class DLLExportLexicon NindTermIndex : public NindIndex {
public:

    /**\brief Creates NindTermIndex with a specified name associated with.
    *\param fileNameExtensionLess absolute path file name without extension
    *\param isTermIndexWriter true if termIndex writer, false if termIndex reader
    *\param specificsNumber number of specific unsigned int
    *\param lexiconIdentification unique identification of lexicon
    *\param indirectionBlocSize number of entries in a single indirection block */
    NindTermIndex(const std::string &fileNameExtensionLess,
                  const bool isTermIndexWriter,
                  const Identification &lexiconIdentification,
                  const unsigned int specificsNumber,
                  const unsigned int indirectionBlocSize = 0);

    virtual ~NindTermIndex();

    /**\brief Structures to hold datas of a term */
    struct Document {
        NindDocId ident;                //external document identifier (64 bits; the v1 format stores 32)
        unsigned int frequency;
        Document(): ident(0), frequency(0) {}
        Document(const NindDocId id, const unsigned int freq): ident(id), frequency(freq) {}
        ~Document() {}
        bool operator< (const Document &doc2) { return (this->ident < doc2.ident); }
    };
    struct TermCG {
        unsigned char cg;                       // categorie gramaticale
        unsigned int frequency;
        std::list<Document> documents;          //identifiants ordonnes par ordre ascendant
        TermCG(): cg(0), frequency(0), documents() {}
        TermCG(const unsigned char cat, const unsigned int freq): cg(cat), frequency(freq), documents() {}
        ~TermCG() {}
    };

    /**\brief A block of a category's postings (format v2), as its descriptor gives it */
    struct Block {
        NindDocId first, last;          //its first and last documents
        uint64_t docsNb;
        unsigned int maxFrequency;      //the largest frequency in it
        unsigned int minLength;         //the shortest document length in it, 0 = unknown
        uint64_t offset, length;        //in the definition
    };
    /**\brief A term's directory: per category, its inline postings or its blocks' descriptors */
    struct TermDirectory {
        struct Category {
            unsigned char cg;
            unsigned int frequency;
            uint64_t docsNb;
            std::vector<Document> inlinePostings;   //a short list (or every list of a v1 file)
            std::vector<Block> blocks;              //a long list (format v2)
        };
        uint64_t offset;                //the definition's place in the file
        unsigned int length;
        std::vector<Category> categories;
    };

    /**\brief Thread-safe (const) read of a term's directory only (format v2: the blocks aren't
    * read; format v1: everything is inline)
    *\return true if the term was found */
    bool getTermDirectory(const unsigned int ident,
                          TermDirectory &directory,
                          NindReadBuffer &buffer) const;

    /**\brief Thread-safe (const) read of blocks [firstBlock, endBlock) of a category of a
    * directory (contiguous in the file: one read), postings appended to postings */
    void getTermBlocks(const unsigned int ident,
                       const TermDirectory &directory,
                       const size_t category,
                       const size_t firstBlock,
                       const size_t endBlock,
                       std::vector<Document> &postings,
                       NindReadBuffer &buffer) const;

    /**\brief Read a full term definition as a list of structures
    *\param ident ident of term
    *\param termDef structure to receive all datas of the specified term
    *\return true if term was found, false otherwise */
    bool getTermDef(const unsigned int ident,
                    std::list<struct TermCG> &termDef);

    /**\brief Read the postings of a term into flat, reusable arrays (no allocation per posting)
    *\param ident ident of term
    *\param postings receives (document, frequency) of every category of the term, one category
    * after the other, each in ascending document order (cleared first; its capacity is kept)
    *\param categoryEnds receives the end, in postings, of each category, in file order (cleared first)
    *\return true if term was found, false otherwise (both left empty) */
    bool getTermPostings(const unsigned int ident,
                         std::vector<Document> &postings,
                         std::vector<size_t> &categoryEnds);

    /**\brief Thread-safe (const) getTermDef: reads into the caller's buffer (see NindFile::readAt);
    * several threads can read one term index at once, each with its own buffer */
    bool getTermDef(const unsigned int ident,
                    std::list<struct TermCG> &termDef,
                    NindReadBuffer &buffer) const;

    /**\brief Thread-safe (const) getTermPostings: reads into the caller's buffer */
    bool getTermPostings(const unsigned int ident,
                         std::vector<Document> &postings,
                         std::vector<size_t> &categoryEnds,
                         NindReadBuffer &buffer) const;

    /**\brief Read specifics as a list of words
    *\param specifics list to receive all specifics */
    void getSpecificWords(std::list<unsigned int> &specifics);

    /**\brief Write a full term definition as a list of structures
    *\param ident ident of term
    *\param termDef structure containing all datas of the specified term
    *\param specifics list of specific unsigned int
    *\param fileIdentification unique identification of lexicon */
    void setTermDef(const unsigned int ident,
                    const std::list<struct TermCG> &termDef,
                    const Identification &fileIdentification,
                    const std::list<unsigned int> &specifics);

    /**\brief Rewrite only the specifics (and identification), leaving every term definition unchanged
    *\param specifics list of specific unsigned int
    *\param fileIdentification unique identification of lexicon */
    void setSpecificWords(const std::list<unsigned int> &specifics,
                          const Identification &fileIdentification);

    /**\brief (writer, format v2) The documents' lengths, for each block's <longueurMin>: the
    * shortest length of its documents, 0 (unknown) if one of them has no length here (or no
    * lengths were given). They must be the lengths a search scores with: a top-k search bounds
    * the block's contributions with it.
    *\param lengths (document, length) pairs, in any order (a document given twice: its last one) */
    void setDocumentLengths(const std::vector<std::pair<NindDocId, unsigned int> > &lengths);

    /**\brief (writer) Change a term's postings: the removed documents leave every category,
    * then the added postings go into category cg (a document already there: its frequency
    * replaced); a term left without documents is erased. The definition is rewritten whole, with
    * room for the next additions (format v2). Fast path, in bulk mode (format v2): when nothing
    * is removed, the term's only category is cg and every added document comes after its last one,
    * only its last block is rewritten, followed by the new blocks, and its directory, in place in
    * the definition's room (left by an earlier rewrite), when there is enough. Each term at most
    * once between beginBulk and endBulk.
    *\param ident ident of term
    *\param removed documents to remove, in increasing order
    *\param cg category of the added postings
    *\param added postings to add, by increasing document
    *\param fileIdentification unique identification of lexicon
    *\param specifics list of specific unsigned int
    *\return true if the definition was rewritten in place (fast path) */
    bool updateTermDef(const unsigned int ident,
                       const std::vector<NindDocId> &removed,
                       const unsigned char cg,
                       const std::vector<Document> &added,
                       const Identification &fileIdentification,
                       const std::list<unsigned int> &specifics);

private:
    void decodeTermDef(const unsigned int ident,
                       NindReadBuffer &in,
                       std::list<struct TermCG> &termDef) const;
    void decodeTermPostings(const unsigned int ident,
                            NindReadBuffer &in,
                            std::vector<Document> &postings,
                            std::vector<size_t> &categoryEnds) const;
    //format v2: the definition's header and directory (in at the definition's first byte)
    void parseDirectoryV2(const unsigned int ident,
                          NindReadBuffer &in,
                          TermDirectory &directory) const;
    //format v2: one block, in positioned at its first byte, checked against its descriptor
    void decodeBlock(const unsigned int ident,
                     const Block &block,
                     NindReadBuffer &in,
                     std::vector<Document> &postings) const;
    //format v2: the whole definition (inline postings and blocks) into flat postings, per category
    //(category, term frequency) if categories isn't null
    void decodeTermDefV2(const unsigned int ident,
                         NindReadBuffer &in,
                         std::vector<Document> &postings,
                         std::vector<size_t> &categoryEnds,
                         std::vector<std::pair<unsigned char, unsigned int> > *categories) const;
    //format v2: forms the definition (directory, then blocks) in the write buffer; with room: a
    //single category with blocks gets room after its directory and after its blocks, for
    //additions written in place (updateTermDef)
    void writeTermDefV2(const unsigned int ident,
                        const std::list<struct TermCG> &termDef,
                        const bool withRoom = false);
    //a definition, from the write buffer's start, then the specifics and identification
    void writeTermDef(const unsigned int ident,
                      const std::list<struct TermCG> &termDef,
                      const Identification &fileIdentification,
                      const std::list<unsigned int> &specifics,
                      const bool withRoom);
    //format v2: a block's descriptor (<offsetBloc> <longueurBloc> as given); returns where they are
    unsigned int putBlockDescriptor(const Block &block,
                                    const NindDocId previousLast);
    //format v2: a block (<flagBloc=71> ...) of documents [begin, end)
    void putBlock(const unsigned int ident,
                  const unsigned char cg,
                  const Document *documents,
                  const size_t docsNb);
    //format v2: a block's descriptor fields from its documents (offset and length: 0)
    Block describeBlock(const Document *documents,
                        const size_t docsNb) const;
    //the fast path of updateTermDef: false if it doesn't apply or there isn't room
    bool appendInPlace(const unsigned int ident,
                       const unsigned char cg,
                       const std::vector<Document> &added,
                       const Identification &fileIdentification,
                       const std::list<unsigned int> &specifics);
public:
    /**\brief number of postings per block of a long list (format v2; a list of at most this many
    * documents is written inline) */
    static const unsigned int BLOCK_SIZE = 128;
private:

    unsigned int m_specificsNumber;

    //the documents' lengths (setDocumentLengths): by id (dense) or (id, length) by id (sparse)
    std::vector<unsigned int> m_denseLengths;
    std::vector<std::pair<NindDocId, unsigned int> > m_sparseLengths;
    //a compact block's packed bytes (reused)
    std::vector<unsigned char> m_packed;
    //a document's length, 0 if unknown
    unsigned int documentLength(const NindDocId ident) const;

    //brief write specifics footer and identification into write buffer
    //param specifics list of specific unsigned int
    //param fileIdentification unique identification of file */
    void writeSpecificsAndIdentification(const std::list<unsigned int> &specifics,
                                         const Identification &fileIdentification);

};
////////////////////////////////////////////////////////////
    } // end namespace
} // end namespace
#endif
////////////////////////////////////////////////////////////
