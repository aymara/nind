//
// C++ Implementation: NindTermIndex
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
#include "NindTermIndex.h"
#include <algorithm>
//#include <iostream>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
//(definition of the static member: it may be bound by reference)
const unsigned int NindTermIndex::BLOCK_SIZE;
////////////////////////////////////////////////////////////
// <dejfinition>           ::= <flagDejfinition=17> <identifiantTerme>
//                             <longueurDonnejes> <donnejesTerme>
// <flagDejfinition=17>    ::= <Entier1>
// <identifiantTerme>      ::= <Entier4>
// <longueurDonnejes>      ::= <Entier3>
// <donnejesTerme>         ::= { <donnejesCG> }
// <donnejesCG>            ::= <flagCg=61> <catejgorie> <frejquenceTerme>
//                             <nbreDocs> <listeDocuments>
// <flagCg=61>             ::= <Entier1>
// <catejgorie>            ::= <Entier1>
// <frejquenceTerme>       ::= <EntierULat>
// <nbreDocs>              ::= <EntierULat>
// <listeDocuments>        ::= { <identDocRelatif> <frejquenceDoc> }
// <identDocRelatif>       ::= <EntierULat>
// <frejquenceDoc>         ::= <EntierULat>
////////////////////////////////////////////////////////////
// <spejcifique>           ::= { <valeur> }
// <valeur>                ::= <Entier4>
////////////////////////////////////////////////////////////
#define FLAG_DEJFINITION (isFormatV2() ? 23 : 17)
#define FLAG_CG (isFormatV2() ? 67 : 61)
//<flagDejfinition=17>(1) <identifiantTerme>(4) = 5
#define OFFSET_LONGUEUR 5
//<flagDejfinition=17>(1) <identifiantTerme>(4) <longueurDonnejes>(3) = 8
#define TAILLE_TESTE_DEJFINITION (isFormatV2() ? 9 : 8)   //v2 : 1 + 4 + 4
//<flagDejfinition=17>(1) <identifiantTerme>(4) <longueurDonnejes>(3) <flagCg>(1) <catejgorie>(1) <frejquenceTerme>(1) 
//<nbreDocs>(1) <identDocRelatif>(5) <frejquenceDoc>(1) = 18
//le minimum doit prendre en compte le maximum dans la numejrotation !
#define TAILLE_DEJFINITION_MINIMUM 18
//<flagCg>(1) <catejgorie>(1) <frejquenceTerme>(3) <nbreDocs>(3) = 8
#define TAILLE_TESTE_DEJFINITION_MAXIMUM (isFormatV2() ? 25 : 8)   //v2 : 1 + 1 + 9 + 9 + 5
//<identDocRelatif>(3) <frejquenceDoc>(2) = 5
#define TAILLE_DOC_MAXIMUM (isFormatV2() ? 14 : 5)   //v2 : 9 + 5
//taille minimum du buffer d'ecriture
#define TAILLE_BUFFER_MINIMUM 128
//format v2 blocks: <flagBloc=71> <identifiantTerme> <catejgorie> <nbreDocsBloc>
#define FLAG_BLOC 71
//<flagBloc>(1) <identifiantTerme>(4) <catejgorie>(1) <nbreDocsBloc>(5) = 11
#define TAILLE_TESTE_BLOC_MAXIMUM 11
//<premierDocRelatif>(9) <dernierDocRelatif>(9) <nbreDocsBloc>(5) <frejquenceMax>(5) <longueurMin>(5)
//<offsetBloc>(4) <longueurBloc>(4) = 41
#define TAILLE_DESCRIPTEUR_MAXIMUM 41
////////////////////////////////////////////////////////////
//brief Creates NindTermIndex with a specified name associated with.
//param fileNameExtensionLess absolute path file name without extension
//param isTermIndexWriter true if termIndex writer, false if termIndex reader  */
//param lexiconIdentification unique identification of lexicon */
//param specificsNumber number of specific unsigned int
//param indirectionBlocSize number of entries in a single indirection block */
NindTermIndex::NindTermIndex(const std::string &fileNameExtensionLess,
                             const bool isTermIndexWriter,
                             const Identification &lexiconIdentification,
                             const unsigned int specificsNumber,
                             const unsigned int indirectionBlocSize):
    NindIndex(fileNameExtensionLess + ".nindtermindex", 
              isTermIndexWriter, 
              lexiconIdentification, 
              specificsNumber * 4,
              TAILLE_DEJFINITION_MINIMUM, 
              indirectionBlocSize),
    m_specificsNumber(specificsNumber),
    m_denseLengths(),
    m_sparseLengths()
{
}
////////////////////////////////////////////////////////////
NindTermIndex::~NindTermIndex()
{
}
////////////////////////////////////////////////////////////
//brief Read a full term definition as a list of structures
//param ident ident of term
//param termDef structure to receive all datas of the specified term
//return true if term was found, false otherwise */
bool NindTermIndex::getTermDef(const unsigned int ident,
                               list<struct TermCG> &termDef)
{
    if (!getDefinition(ident)) return false;
    decodeTermDef(ident, m_file.inBuffer(), termDef);
    return true;
}
////////////////////////////////////////////////////////////
//brief Thread-safe (const) read of a full term definition (see getTermDef)
bool NindTermIndex::getTermDef(const unsigned int ident,
                               list<struct TermCG> &termDef,
                               NindReadBuffer &buffer) const
{
    if (!getDefinitionAt(ident, buffer)) return false;
    decodeTermDef(ident, buffer, termDef);
    return true;
}
////////////////////////////////////////////////////////////
//dejcode la dejfinition d'un terme lue dans in
void NindTermIndex::decodeTermDef(const unsigned int ident,
                                  NindReadBuffer &in,
                                  list<struct TermCG> &termDef) const
{
    if (isFormatV2()) {
        vector<Document> postings;
        vector<size_t> ends;
        vector<pair<unsigned char, unsigned int> > categories;
        decodeTermDefV2(ident, in, postings, ends, &categories);
        size_t begin = 0;
        for (size_t c = 0; c < categories.size(); c++) {
            termDef.push_back(TermCG(categories[c].first, categories[c].second));
            termDef.back().documents.assign(postings.begin() + begin, postings.begin() + ends[c]);
            begin = ends[c];
        }
        return;
    }
    //<flagDejfinition=17> <identifiantTerme> <longueurDonnejes> <donnees>
    if (in.getInt1() != FLAG_DEJFINITION) 
        throw NindTermIndexException("NindTermIndex::getTermDef A : " + m_fileName);
    const unsigned int identTerme = in.getInt4();
    if (identTerme != ident) 
        throw NindTermIndexException("NindTermIndex::getTermDef B : " + m_fileName);
    const unsigned int longueurDonnejes = getDefinitionField(in);
    //positionne la fin de buffer en fonction de la longueur effective des donnejesTerme
    in.setEndInBuffer(longueurDonnejes);
    //<flagCg> <catejgorie> <frejquenceTerme> <nbreDocs> <listeDocuments>
    while (!in.endOfInBuffer()) {
        if (in.getInt1() != FLAG_CG) 
            throw NindTermIndexException("NindTermIndex::getTermDef C : " + m_fileName);
        const unsigned char catejgorie = in.getInt1();
        const uint64_t frejquenceTerme = getCountField(in);
        const uint64_t nbreDocs = getCountField(in);
        if (frejquenceTerme > 0xFFFFFFFFull)
            throw NindTermIndexException("NindTermIndex::getTermDef term frequency over 32 bits : " + m_fileName);
        //v2 : <nbreBlocs> (0 : postings inline)
        if (isFormatV2() && in.getUIntLat() != 0)
            throw NindTermIndexException("NindTermIndex::getTermDef blocks not supported yet : " + m_fileName);
        termDef.push_back(TermCG(catejgorie, (unsigned int)frejquenceTerme));
        struct TermCG &termCG = termDef.back();
        list<Document> &documents = termCG.documents;
        NindDocId identDocument = 0;         //pour calculer le no de doc absolu avec la numerotation relative
        for (uint64_t it = 0; it != nbreDocs; it++) {
            //<identDocRelatif> <frejquenceDoc>
            const uint64_t delta = getCountField(in);
            if (delta > ~(NindDocId)0 - identDocument)
                throw NindTermIndexException("NindTermIndex::getTermDef D : " + m_fileName);
            identDocument += delta;
            if (!isFormatV2() && identDocument > NIND_DOC_ID_MAX_V1)
                throw NindTermIndexException("NindTermIndex::getTermDef D : " + m_fileName);
            const unsigned int frejquenceDoc = in.getUIntLat();
            documents.push_back(Document(identDocument, frejquenceDoc));
        }
    }
}
////////////////////////////////////////////////////////////
//brief Read the postings of a term into flat, reusable arrays (no allocation per posting)
//param ident ident of term
//param postings receives (document, frequency) of every category, one after the other
//param categoryEnds receives the end, in postings, of each category, in file order
//return true if term was found, false otherwise */
bool NindTermIndex::getTermPostings(const unsigned int ident,
                                    vector<Document> &postings,
                                    vector<size_t> &categoryEnds)
{
    postings.clear();
    categoryEnds.clear();
    if (!getDefinition(ident)) return false;
    decodeTermPostings(ident, m_file.inBuffer(), postings, categoryEnds);
    return true;
}
////////////////////////////////////////////////////////////
//brief Thread-safe (const) read of the postings of a term (see getTermPostings)
bool NindTermIndex::getTermPostings(const unsigned int ident,
                                    vector<Document> &postings,
                                    vector<size_t> &categoryEnds,
                                    NindReadBuffer &buffer) const
{
    postings.clear();
    categoryEnds.clear();
    if (!getDefinitionAt(ident, buffer)) return false;
    decodeTermPostings(ident, buffer, postings, categoryEnds);
    return true;
}
////////////////////////////////////////////////////////////
//dejcode les postings d'un terme lus dans in
void NindTermIndex::decodeTermPostings(const unsigned int ident,
                                       NindReadBuffer &in,
                                       vector<Document> &postings,
                                       vector<size_t> &categoryEnds) const
{
    if (isFormatV2()) {
        decodeTermDefV2(ident, in, postings, categoryEnds, 0);
        return;
    }
    //same format and checks as getTermDef
    //<flagDejfinition=17> <identifiantTerme> <longueurDonnejes> <donnees>
    if (in.getInt1() != FLAG_DEJFINITION)
        throw NindTermIndexException("NindTermIndex::getTermPostings A : " + m_fileName);
    const unsigned int identTerme = in.getInt4();
    if (identTerme != ident)
        throw NindTermIndexException("NindTermIndex::getTermPostings B : " + m_fileName);
    const unsigned int longueurDonnejes = getDefinitionField(in);
    in.setEndInBuffer(longueurDonnejes);
    //<flagCg> <catejgorie> <frejquenceTerme> <nbreDocs> <listeDocuments>
    while (!in.endOfInBuffer()) {
        if (in.getInt1() != FLAG_CG)
            throw NindTermIndexException("NindTermIndex::getTermPostings C : " + m_fileName);
        in.getInt1();                       //catejgorie
        getCountField(in);                  //frejquenceTerme
        const uint64_t nbreDocs = getCountField(in);
        if (isFormatV2() && in.getUIntLat() != 0)
            throw NindTermIndexException("NindTermIndex::getTermPostings blocks not supported yet : " + m_fileName);
        //nbreDocs comes from the file: no reserve on it, every read is bounded by the buffer
        NindDocId identDocument = 0;         //pour calculer le no de doc absolu avec la numerotation relative
        for (uint64_t it = 0; it != nbreDocs; it++) {
            //<identDocRelatif> <frejquenceDoc>
            const uint64_t delta = getCountField(in);
            if (delta > ~(NindDocId)0 - identDocument)
                throw NindTermIndexException("NindTermIndex::getTermPostings D : " + m_fileName);
            identDocument += delta;
            if (!isFormatV2() && identDocument > NIND_DOC_ID_MAX_V1)
                throw NindTermIndexException("NindTermIndex::getTermPostings D : " + m_fileName);
            const unsigned int frejquenceDoc = in.getUIntLat();
            postings.push_back(Document(identDocument, frejquenceDoc));
        }
        categoryEnds.push_back(postings.size());
    }
}
////////////////////////////////////////////////////////////
//brief Read specifics as a list of words
//param specifics list to receive all specifics */
void NindTermIndex::getSpecificWords(list<unsigned int> &specifics)
{
    //raz
    specifics.clear();
    //lit les spejcifiques sur le fichier
    getSpecifics();
    for (unsigned int count = 0; count != m_specificsNumber; count++)
        specifics.push_back(m_file.getInt4());  
}
////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////
//format v2: l'en-teste et le rejpertoire de la dejfinition (in : au premier octet de la dejfinition)
void NindTermIndex::parseDirectoryV2(const unsigned int ident,
                                     NindReadBuffer &in,
                                     TermDirectory &directory) const
{
    directory.categories.clear();
    //<flagDejfinition=23> <identifiantTerme> <longueurDonnejes> { <catejgorieV2> }
    if (in.getInt1() != FLAG_DEJFINITION)
        throw NindTermIndexException("NindTermIndex::getTermDef A : " + m_fileName);
    if (in.getInt4() != ident)
        throw NindTermIndexException("NindTermIndex::getTermDef B : " + m_fileName);
    const unsigned int longueurDonnejes = getDefinitionField(in);
    in.setEndInBuffer(longueurDonnejes);
    while (!in.endOfInBuffer()) {
        //<flagCg=67> <catejgorie> <frejquenceTerme> <nbreDocs> <nbreBlocs>
        if (in.getInt1() != FLAG_CG)
            throw NindTermIndexException("NindTermIndex::getTermDef C : " + m_fileName);
        directory.categories.push_back(TermDirectory::Category());
        TermDirectory::Category &category = directory.categories.back();
        category.cg = in.getInt1();
        const uint64_t frejquenceTerme = in.getUIntLat64();
        category.docsNb = in.getUIntLat64();
        const unsigned int nbreBlocs = in.getUIntLat();
        if (frejquenceTerme > 0xFFFFFFFFull)
            throw NindTermIndexException("NindTermIndex::getTermDef term frequency over 32 bits : " + m_fileName);
        category.frequency = (unsigned int)frejquenceTerme;
        if (nbreBlocs == 0) {
            //<postingsEnLigne>: docsNb comes from the file: no reserve, every read is bounded
            NindDocId identDocument = 0;
            for (uint64_t it = 0; it != category.docsNb; it++) {
                const uint64_t delta = in.getUIntLat64();
                if (delta > ~(NindDocId)0 - identDocument || (it > 0 && delta == 0))
                    throw NindTermIndexException("NindTermIndex::getTermDef D : " + m_fileName);
                identDocument += delta;
                category.inlinePostings.push_back(Document(identDocument, in.getUIntLat()));
            }
            continue;
        }
        //{ <descripteurBloc> }: nbreBlocs comes from the file: no reserve either
        NindDocId previousLast = 0;
        uint64_t total = 0;
        for (unsigned int b = 0; b != nbreBlocs; b++) {
            Block block;
            const uint64_t firstRelative = in.getUIntLat64();
            const uint64_t lastRelative = in.getUIntLat64();
            if (firstRelative > ~(NindDocId)0 - previousLast)
                throw NindTermIndexException("NindTermIndex::getTermDef E : " + m_fileName);
            block.first = previousLast + firstRelative;
            if (lastRelative > ~(NindDocId)0 - block.first)
                throw NindTermIndexException("NindTermIndex::getTermDef E : " + m_fileName);
            block.last = block.first + lastRelative;
            block.docsNb = in.getUIntLat();
            block.maxFrequency = in.getUIntLat();
            block.minLength = in.getUIntLat();
            block.offset = in.getInt4();
            block.length = in.getInt4();
            //a block after the first starts after the previous one's last document
            if (block.docsNb == 0 || (b > 0 && firstRelative == 0))
                throw NindTermIndexException("NindTermIndex::getTermDef E : " + m_fileName);
            total += block.docsNb;
            previousLast = block.last;
            category.blocks.push_back(block);
        }
        if (total != category.docsNb)
            throw NindTermIndexException("NindTermIndex::getTermDef F : " + m_fileName);
    }
}
////////////////////////////////////////////////////////////
//format v2: un bloc (in : ah son premier octet, fin de buffer ah sa fin), vejrifiej par son descripteur
void NindTermIndex::decodeBlock(const unsigned int ident,
                                const Block &block,
                                NindReadBuffer &in,
                                vector<Document> &postings) const
{
    //<flagBloc=71> <identifiantTerme> <catejgorie> <nbreDocsBloc> { <identDocRelatif> <frejquenceDoc> }
    if (in.getInt1() != FLAG_BLOC || in.getInt4() != ident)
        throw NindTermIndexException("NindTermIndex::getTermDef H : " + m_fileName);
    in.getInt1();                   //catejgorie (checked by the descriptor's place)
    if (in.getUIntLat() != block.docsNb)
        throw NindTermIndexException("NindTermIndex::getTermDef H : " + m_fileName);
    NindDocId identDocument = block.first;
    unsigned int maxFrequency = 0;
    for (uint64_t it = 0; it != block.docsNb; it++) {
        const uint64_t delta = in.getUIntLat64();
        if (delta > ~(NindDocId)0 - identDocument || (it == 0 && delta != 0) || (it > 0 && delta == 0))
            throw NindTermIndexException("NindTermIndex::getTermDef I : " + m_fileName);
        identDocument += delta;
        const unsigned int frequency = in.getUIntLat();
        if (frequency > maxFrequency) maxFrequency = frequency;
        postings.push_back(Document(identDocument, frequency));
    }
    if (identDocument != block.last || maxFrequency != block.maxFrequency || !in.endOfInBuffer())
        throw NindTermIndexException("NindTermIndex::getTermDef J : " + m_fileName);
}
////////////////////////////////////////////////////////////
//format v2: dejcode la dejfinition entiehre (postings en ligne et blocs) dans in, lue en entier
void NindTermIndex::decodeTermDefV2(const unsigned int ident,
                                    NindReadBuffer &in,
                                    vector<Document> &postings,
                                    vector<size_t> &categoryEnds,
                                    vector<pair<unsigned char, unsigned int> > *categories) const
{
    TermDirectory directory;
    parseDirectoryV2(ident, in, directory);
    const unsigned int directoryEnd = in.getInBufferSize();
    for (size_t c = 0; c < directory.categories.size(); c++) {
        const TermDirectory::Category &category = directory.categories[c];
        if (categories != 0) categories->push_back(pair<unsigned char, unsigned int>(category.cg, category.frequency));
        postings.insert(postings.end(), category.inlinePostings.begin(), category.inlinePostings.end());
        for (size_t b = 0; b < category.blocks.size(); b++) {
            const Block &block = category.blocks[b];
            //blocks lie after the directory, within the definition
            if (block.offset < directoryEnd)
                throw NindTermIndexException("NindTermIndex::getTermDef G : " + m_fileName);
            in.setInBufferPtr((unsigned int)block.offset);
            in.setEndInBuffer((unsigned int)block.length);
            decodeBlock(ident, block, in, postings);
        }
        categoryEnds.push_back(postings.size());
    }
}
////////////////////////////////////////////////////////////
//brief Thread-safe (const) read of a term's directory only
bool NindTermIndex::getTermDirectory(const unsigned int ident,
                                     TermDirectory &directory,
                                     NindReadBuffer &buffer) const
{
    directory.categories.clear();
    if (!isFormatV2()) {
        //format v1: no blocks, everything inline
        list<TermCG> termDef;
        if (!getTermDef(ident, termDef, buffer)) return false;
        directory.offset = 0;
        directory.length = 0;
        for (list<TermCG>::const_iterator it = termDef.begin(); it != termDef.end(); it++) {
            directory.categories.push_back(TermDirectory::Category());
            TermDirectory::Category &category = directory.categories.back();
            category.cg = (*it).cg;
            category.frequency = (*it).frequency;
            category.docsNb = (*it).documents.size();
            category.inlinePostings.assign((*it).documents.begin(), (*it).documents.end());
        }
        return true;
    }
    if (!getDefinitionRefAt(ident, buffer, directory.offset, directory.length)) return false;
    //the header, for the directory's length, then the directory
    if (directory.length < TAILLE_TESTE_DEJFINITION)
        throw NindTermIndexException("NindTermIndex::getTermDirectory : " + m_fileName);
    m_file.readAt(directory.offset, TAILLE_TESTE_DEJFINITION, buffer);
    buffer.setInBufferPtr(OFFSET_LONGUEUR);
    const uint64_t directorySize = (uint64_t)TAILLE_TESTE_DEJFINITION + getDefinitionField(buffer);
    if (directorySize > directory.length)
        throw NindTermIndexException("NindTermIndex::getTermDirectory : " + m_fileName);
    m_file.readAt(directory.offset, (unsigned int)directorySize, buffer);
    parseDirectoryV2(ident, buffer, directory);
    //every block within the definition, after the directory
    for (size_t c = 0; c < directory.categories.size(); c++) {
        const vector<Block> &blocks = directory.categories[c].blocks;
        for (size_t b = 0; b < blocks.size(); b++) {
            if (blocks[b].offset < directorySize || blocks[b].offset + blocks[b].length > directory.length)
                throw NindTermIndexException("NindTermIndex::getTermDef G : " + m_fileName);
        }
    }
    return true;
}
////////////////////////////////////////////////////////////
//brief Thread-safe (const) read of blocks [firstBlock, endBlock) of a category
void NindTermIndex::getTermBlocks(const unsigned int ident,
                                  const TermDirectory &directory,
                                  const size_t category,
                                  const size_t firstBlock,
                                  const size_t endBlock,
                                  vector<Document> &postings,
                                  NindReadBuffer &buffer) const
{
    if (category >= directory.categories.size() || firstBlock >= endBlock
        || endBlock > directory.categories[category].blocks.size())
        throw NindTermIndexException("NindTermIndex::getTermBlocks bad block range : " + m_fileName);
    const vector<Block> &blocks = directory.categories[category].blocks;
    //contiguous (as written): one read; checked by getTermDirectory to be within the definition
    const uint64_t begin = blocks[firstBlock].offset;
    uint64_t end = begin;
    for (size_t b = firstBlock; b < endBlock; b++) {
        if (blocks[b].offset != end)
            throw NindTermIndexException("NindTermIndex::getTermBlocks blocks not contiguous : " + m_fileName);
        end += blocks[b].length;
    }
    m_file.readAt(directory.offset + begin, (unsigned int)(end - begin), buffer);
    for (size_t b = firstBlock; b < endBlock; b++) {
        buffer.setInBufferPtr((unsigned int)(blocks[b].offset - begin));
        buffer.setEndInBuffer((unsigned int)blocks[b].length);
        decodeBlock(ident, blocks[b], buffer, postings);
    }
}
////////////////////////////////////////////////////////////
//format v2: forme la dejfinition (rejpertoire, puis blocs) dans le buffer d'ejcriture
void NindTermIndex::writeTermDefV2(const unsigned int ident,
                                   const list<struct TermCG> &termDef,
                                   const bool withRoom)
{
    //the room for additions in place (one category with blocks only): extraBlocks more
    //descriptors after the directory, as many blocks (and the last one's growth) after the blocks
    const size_t singleDocsNb = termDef.size() == 1 ? termDef.front().documents.size() : 0;
    const size_t extraBlocks = (withRoom && singleDocsNb > BLOCK_SIZE) ?
        max((size_t)2, (singleDocsNb + BLOCK_SIZE - 1) / BLOCK_SIZE / 16) : 0;
    //2) la taille maximum du buffer d'ecriture
    uint64_t tailleMaximum = TAILLE_TESTE_DEJFINITION + getSpecificsAndIdentificationSize() + TAILLE_BUFFER_MINIMUM;
    for (list<struct TermCG>::const_iterator it1 = termDef.begin(); it1 != termDef.end(); it1++) {
        const uint64_t docsNb = (*it1).documents.size();
        const uint64_t blocksNb = (docsNb <= BLOCK_SIZE) ? 0 : (docsNb + BLOCK_SIZE - 1) / BLOCK_SIZE;
        tailleMaximum += TAILLE_TESTE_DEJFINITION_MAXIMUM + docsNb * TAILLE_DOC_MAXIMUM
                         + blocksNb * (TAILLE_DESCRIPTEUR_MAXIMUM + TAILLE_TESTE_BLOC_MAXIMUM);
    }
    if (extraBlocks != 0)
        tailleMaximum += (uint64_t)extraBlocks * TAILLE_DESCRIPTEUR_MAXIMUM + TAILLE_TESTE_DEJFINITION_MAXIMUM
                         + (uint64_t)(extraBlocks + 1) * (TAILLE_TESTE_BLOC_MAXIMUM + BLOCK_SIZE * TAILLE_DOC_MAXIMUM);
    if (tailleMaximum > 0xFFFFFFFFull)
        throw OutOfBoundException("NindTermIndex::setTermDef definition over 4 GiB " + m_fileName);
    m_file.createBuffer((unsigned int)tailleMaximum);
    //3) <flagDejfinition=23> <identifiantTerme> <longueurDonnejes> { <catejgorieV2> }
    m_file.putInt1(FLAG_DEJFINITION);
    m_file.putInt4(ident);
    putDefinitionField(0);     //la taille du rejpertoire sera ejcrite plus tard, quand elle sera connue
    //the descriptors' <offsetBloc> and <longueurBloc>, patched once the blocks are written
    //(each category's documents copied once into a vector, the blocks referring to it)
    struct Pending { size_t category; unsigned char cg; size_t begin, end; unsigned int patch; };
    vector<Pending> pending;
    vector<vector<Document> > categoryDocuments;
    for (list<struct TermCG>::const_iterator it1 = termDef.begin(); it1 != termDef.end(); it1++) {
        categoryDocuments.push_back(vector<Document>((*it1).documents.begin(), (*it1).documents.end()));
        const vector<Document> &documents = categoryDocuments.back();
        for (size_t d = 1; d < documents.size(); d++) {
            if (documents[d].ident <= documents[d - 1].ident)
                throw NindTermIndexException("NindTermIndex::setTermDef documents not in increasing order : " + m_fileName);
        }
        const size_t docsNb = documents.size();
        const size_t blocksNb = (docsNb <= BLOCK_SIZE) ? 0 : (docsNb + BLOCK_SIZE - 1) / BLOCK_SIZE;
        //<flagCg=67> <catejgorie> <frejquenceTerme> <nbreDocs> <nbreBlocs>
        m_file.putInt1(FLAG_CG);
        m_file.putInt1((*it1).cg);
        m_file.putUIntLat64((*it1).frequency);
        m_file.putUIntLat64(docsNb);
        m_file.putUIntLat(blocksNb);
        if (blocksNb == 0) {
            //<postingsEnLigne>
            NindDocId identPrec = 0;
            for (size_t d = 0; d < docsNb; d++) {
                m_file.putUIntLat64(documents[d].ident - identPrec);
                m_file.putUIntLat(documents[d].frequency);
                identPrec = documents[d].ident;
            }
            continue;
        }
        //{ <descripteurBloc> }: blocks of BLOCK_SIZE documents, the last one possibly smaller
        NindDocId previousLast = 0;
        for (size_t b = 0; b < blocksNb; b++) {
            const size_t begin = b * BLOCK_SIZE;
            const size_t end = (begin + BLOCK_SIZE < docsNb) ? begin + BLOCK_SIZE : docsNb;
            const Block block = describeBlock(&documents[begin], end - begin);
            const unsigned int patch = putBlockDescriptor(block, previousLast);
            pending.push_back(Pending{categoryDocuments.size() - 1, (*it1).cg, begin, end, patch});
            previousLast = block.last;
        }
    }
    //la taille rejelle du rejpertoire
    putDefinitionFieldAt(m_file.getOutBufferSize() - TAILLE_TESTE_DEJFINITION, OFFSET_LONGUEUR);
    //room for more descriptors (and a longer category header), then the blocks
    if (extraBlocks != 0) {
        const unsigned int room = extraBlocks * TAILLE_DESCRIPTEUR_MAXIMUM + TAILLE_TESTE_DEJFINITION_MAXIMUM;
        for (unsigned int i = 0; i < room; i++) m_file.putInt1(0);
    }
    //4) the blocks, after the directory, and their offsets and lengths in the descriptors
    const unsigned int blocksBegin = m_file.getOutBufferSize();
    for (size_t p = 0; p < pending.size(); p++) {
        const vector<Document> &documents = categoryDocuments[pending[p].category];
        const unsigned int offset = m_file.getOutBufferSize();
        putBlock(ident, pending[p].cg, &documents[pending[p].begin], pending[p].end - pending[p].begin);
        m_file.putInt4(offset, pending[p].patch);
        m_file.putInt4(m_file.getOutBufferSize() - offset, pending[p].patch + 4);
    }
    //room for extraBlocks more blocks, and the last one's growth
    if (extraBlocks != 0 && !pending.empty()) {
        const uint64_t averageBlock = (m_file.getOutBufferSize() - blocksBegin) / pending.size() + 1;
        const uint64_t room = (extraBlocks + 1) * averageBlock;
        for (uint64_t i = 0; i < room; i++) m_file.putInt1(0);
    }
}
////////////////////////////////////////////////////////////
//format v2: a block's descriptor fields from its documents
NindTermIndex::Block NindTermIndex::describeBlock(const Document *documents,
                                                  const size_t docsNb) const
{
    Block block;
    block.first = documents[0].ident;
    block.last = documents[docsNb - 1].ident;
    block.docsNb = docsNb;
    block.maxFrequency = 0;
    //the shortest document, 0 if one is unknown (or no lengths)
    block.minLength = (m_denseLengths.empty() && m_sparseLengths.empty()) ? 0 : ~0u;
    for (size_t d = 0; d < docsNb; d++) {
        if (documents[d].frequency > block.maxFrequency) block.maxFrequency = documents[d].frequency;
        if (block.minLength != 0) block.minLength = min(block.minLength, documentLength(documents[d].ident));
    }
    block.offset = 0;
    block.length = 0;
    return block;
}
////////////////////////////////////////////////////////////
//format v2: a block's descriptor; returns where its <offsetBloc> <longueurBloc> are
unsigned int NindTermIndex::putBlockDescriptor(const Block &block,
                                               const NindDocId previousLast)
{
    //<premierDocRelatif> <dernierDocRelatif> <nbreDocsBloc> <frejquenceMax> <longueurMin> <offsetBloc> <longueurBloc>
    m_file.putUIntLat64(block.first - previousLast);
    m_file.putUIntLat64(block.last - block.first);
    m_file.putUIntLat(block.docsNb);
    m_file.putUIntLat(block.maxFrequency);
    m_file.putUIntLat(block.minLength);
    const unsigned int patch = m_file.getOutBufferSize();
    m_file.putInt4(block.offset);
    m_file.putInt4(block.length);
    return patch;
}
////////////////////////////////////////////////////////////
//format v2: a block of docsNb documents
void NindTermIndex::putBlock(const unsigned int ident,
                             const unsigned char cg,
                             const Document *documents,
                             const size_t docsNb)
{
    //<flagBloc=71> <identifiantTerme> <catejgorie> <nbreDocsBloc> { <identDocRelatif> <frejquenceDoc> }
    m_file.putInt1(FLAG_BLOC);
    m_file.putInt4(ident);
    m_file.putInt1(cg);
    m_file.putUIntLat(docsNb);
    NindDocId identPrec = documents[0].ident;
    for (size_t d = 0; d < docsNb; d++) {
        m_file.putUIntLat64(documents[d].ident - identPrec);
        m_file.putUIntLat(documents[d].frequency);
        identPrec = documents[d].ident;
    }
}
////////////////////////////////////////////////////////////
//brief Write a full term definition as as a list of structures
//param ident ident of term
//param termDef structure containing all datas of the specified term */
//param specifics list of specific unsigned int
//param fileIdentification unique identification of lexicon */
void NindTermIndex::setTermDef(const unsigned int ident,
                               const list<struct TermCG> &termDef,
                               const Identification &fileIdentification,
                               const list<unsigned int> &specifics)
{
    //section critique ah protejger des control-C (refermeje sur tout chemin de sortie, exceptions comprises)
    NindCriticalSection criticalSection(m_file);
    //1) verifie que le terme n'est pas en dehors du dernier bloc d'indirection
    //il faut le faire maintenant parce que le buffer d'ecriture est unique
    checkExtendIndirection(ident, fileIdentification);
    
    //effacement ?
    if (termDef.size() == 0) {
        //efface dans le fichier en l'ejcrivant vide
        m_file.createBuffer(getSpecificsAndIdentificationSize()); 
        //ejcrit les spejcifiques et l'identification
        writeSpecificsAndIdentification(specifics, fileIdentification);
        setDefinition(ident);
        return;
    }       
    if (isFormatV2()) {
        writeTermDef(ident, termDef, fileIdentification, specifics, false);
        return;
    }
    //2) calcule la taille maximum du buffer d'ecriture
    //<flagDejfinition=17> <identifiantTerme> <longueurDonnejes> <donnejesTerme>
    unsigned int tailleMaximum = TAILLE_TESTE_DEJFINITION + getSpecificsAndIdentificationSize();    
    for (list<struct TermCG>::const_iterator it1 = termDef.begin(); it1 != termDef.end(); it1++) {
        //<flagCg> <catejgorie> <frejquenceTerme> <nbreDocs> <listeDocuments>
        //<identDocRelatif> <frejquenceDoc>
        //le buffer est maximise pour écrire l'identification a la fin
        tailleMaximum += TAILLE_TESTE_DEJFINITION_MAXIMUM + 
                            (*it1).documents.size()*TAILLE_DOC_MAXIMUM;
        //il ne doit pas etre plus petit que le minimum 
        if (tailleMaximum < TAILLE_BUFFER_MINIMUM) tailleMaximum = TAILLE_BUFFER_MINIMUM;
    }
    //3) forme le buffer a ecrire sur le fichier
    m_file.createBuffer(tailleMaximum); 
    //<flagDejfinition=17> <identifiantTerme> <longueurDonnejes> <donnejesTerme>
    m_file.putInt1(FLAG_DEJFINITION);
    m_file.putInt4(ident);
    putDefinitionField(0);     //la taille des donnees sera ecrite plus tard, quand elle sera connue
    for (list<struct TermCG>::const_iterator it1 = termDef.begin(); it1 != termDef.end(); it1++) {
        //<flagCg> <catejgorie> <frejquenceTerme> <nbreDocs> <listeDocuments>
        m_file.putInt1(FLAG_CG);
        m_file.putInt1((*it1).cg);
        putCountField((*it1).frequency);
        const list<struct Document> &documents = (*it1).documents;
        putCountField(documents.size());
        if (isFormatV2()) m_file.putUIntLat(0);       //<nbreBlocs> : postings inline
        NindDocId identPrec = 0;                 //pour mettre les identifiants de documents en relatif
        for (list<struct Document>::const_iterator it2 = documents.begin(); it2 != documents.end(); it2++) {
            //<identDocRelatif> <frejquenceDoc> (documents in increasing order)
            if ((*it2).ident < identPrec)
                throw NindTermIndexException("NindTermIndex::setTermDef documents not in increasing order : " + m_fileName);
            putCountField((*it2).ident - identPrec);
            m_file.putUIntLat((*it2).frequency);
            identPrec = (*it2).ident;
        }
    }
    //ecrit la taille reelle du buffer
    const unsigned int longueurDonnejes = m_file.getOutBufferSize() - TAILLE_TESTE_DEJFINITION;
    putDefinitionFieldAt(longueurDonnejes, OFFSET_LONGUEUR);  //la taille dans la trame
    //calcule le nombre d'extra octets ah mettre dans le buffer pour atteindre la taille minimum
    int extra = m_file.getOutBufferSize() - TAILLE_DEJFINITION_MINIMUM;
    while (extra++ < 0) m_file.putInt1(0);
    //4) ejcrit les spejcifiques et l'identification
    writeSpecificsAndIdentification(specifics, fileIdentification);
    setDefinition(ident);
}
////////////////////////////////////////////////////////////
//brief Rewrite only the specifics (and identification), leaving every term definition unchanged
//param specifics list of specific unsigned int
//param fileIdentification unique identification of lexicon */
void NindTermIndex::setSpecificWords(const list<unsigned int> &specifics,
                                     const Identification &fileIdentification)
{
    if (!m_isWriter)
        throw NindTermIndexException("NindTermIndex::setSpecificWords termindex is not writable " + m_fileName);
    //section critique ah protejger des control-C (refermeje sur tout chemin de sortie, exceptions comprises)
    NindCriticalSection criticalSection(m_file);
    m_file.createBuffer(getSpecificsAndIdentificationSize());
    writeSpecificsAndIdentification(specifics, fileIdentification);
    //les spejcifiques et l'identification sont toujours en queue de fichier
    writeTail(0, getSpecificsAndIdentificationSize());
}
////////////////////////////////////////////////////////////
//brief write specifics footer and identification into write buffer
//param specifics list of specific unsigned int
//param fileIdentification unique identification of file */
void NindTermIndex::writeSpecificsAndIdentification(const list<unsigned int> &specifics,
                                                    const Identification &fileIdentification)
{
    //controsle de la taille des spejcifiques
    if (specifics.size() != m_specificsNumber) 
        throw NindTermIndexException("NindTermIndex::writeSpecificsAndIdentification" + m_fileName);
    writeSpecificsHeader();
    //{ <valeur> }
    for(list<unsigned int>::const_iterator specIt = specifics.begin(); specIt != specifics.end(); specIt++)
        m_file.putInt4(*specIt);
    writeIdentification(fileIdentification);
}
////////////////////////////////////////////////////////////
//brief (writer) the documents' lengths, for the blocks' <longueurMin>
void NindTermIndex::setDocumentLengths(const vector<pair<NindDocId, unsigned int> > &lengths)
{
    m_denseLengths.clear();
    m_sparseLengths.clear();
    if (lengths.empty()) return;
    NindDocId maxIdent = 0;
    for (size_t i = 0; i < lengths.size(); i++) maxIdent = max(maxIdent, lengths[i].first);
    //dense when the ids are (about) consecutive: a table by id
    if (maxIdent < 2 * (NindDocId)lengths.size() + 1024) {
        m_denseLengths.assign((size_t)maxIdent + 1, 0);
        for (size_t i = 0; i < lengths.size(); i++) m_denseLengths[(size_t)lengths[i].first] = lengths[i].second;
        return;
    }
    m_sparseLengths = lengths;
    stable_sort(m_sparseLengths.begin(), m_sparseLengths.end(),
                [](const pair<NindDocId, unsigned int> &a, const pair<NindDocId, unsigned int> &b) { return a.first < b.first; });
    //a document given twice: its last length
    vector<pair<NindDocId, unsigned int> > unique;
    for (size_t i = 0; i < m_sparseLengths.size(); i++) {
        if (!unique.empty() && unique.back().first == m_sparseLengths[i].first) unique.back() = m_sparseLengths[i];
        else unique.push_back(m_sparseLengths[i]);
    }
    m_sparseLengths.swap(unique);
}
////////////////////////////////////////////////////////////
//a document's length, 0 if unknown
unsigned int NindTermIndex::documentLength(const NindDocId ident) const
{
    if (!m_denseLengths.empty()) return ident < m_denseLengths.size() ? m_denseLengths[(size_t)ident] : 0;
    const vector<pair<NindDocId, unsigned int> >::const_iterator it = lower_bound(m_sparseLengths.begin(), m_sparseLengths.end(),
        pair<NindDocId, unsigned int>(ident, 0),
        [](const pair<NindDocId, unsigned int> &a, const pair<NindDocId, unsigned int> &b) { return a.first < b.first; });
    return (it != m_sparseLengths.end() && it->first == ident) ? it->second : 0;
}
////////////////////////////////////////////////////////////
//a definition (format v2), then the specifics and identification, written
void NindTermIndex::writeTermDef(const unsigned int ident,
                                 const list<struct TermCG> &termDef,
                                 const Identification &fileIdentification,
                                 const list<unsigned int> &specifics,
                                 const bool withRoom)
{
    writeTermDefV2(ident, termDef, withRoom);
    //calcule le nombre d'extra octets ah mettre dans le buffer pour atteindre la taille minimum
    int extra = m_file.getOutBufferSize() - TAILLE_DEJFINITION_MINIMUM;
    while (extra++ < 0) m_file.putInt1(0);
    writeSpecificsAndIdentification(specifics, fileIdentification);
    setDefinition(ident);
}
////////////////////////////////////////////////////////////
//brief (writer) change a term's postings (see the header)
bool NindTermIndex::updateTermDef(const unsigned int ident,
                                  const vector<NindDocId> &removed,
                                  const unsigned char cg,
                                  const vector<Document> &added,
                                  const Identification &fileIdentification,
                                  const list<unsigned int> &specifics)
{
    for (size_t i = 1; i < removed.size(); i++)
        if (removed[i] <= removed[i - 1])
            throw NindTermIndexException("NindTermIndex::updateTermDef removed documents not in increasing order : " + m_fileName);
    for (size_t i = 1; i < added.size(); i++)
        if (added[i].ident <= added[i - 1].ident)
            throw NindTermIndexException("NindTermIndex::updateTermDef added documents not in increasing order : " + m_fileName);
    if (!isFormatV2())
        throw NindPadFileException("NindTermIndex::updateTermDef format v1 file, written by an older nind: rebuild it " + m_fileName);
    //section critique ah protejger des control-C (refermeje sur tout chemin de sortie, exceptions comprises)
    NindCriticalSection criticalSection(m_file);
    checkExtendIndirection(ident, fileIdentification);
    //the fast path: the last block, the new ones and the directory, in place
    if (removed.empty() && !added.empty() && isBulk() && appendInPlace(ident, cg, added, fileIdentification, specifics))
        return true;
    //the whole definition (sequential read: also in bulk mode, where the definitions are where
    //the bulk session put them)
    list<TermCG> termDef;
    getTermDef(ident, termDef);
    //the removed documents leave every category
    list<TermCG>::iterator target = termDef.end();
    for (list<TermCG>::iterator it = termDef.begin(); it != termDef.end(); it++) {
        if (!removed.empty()) {
            for (list<Document>::iterator d = it->documents.begin(); d != it->documents.end(); ) {
                if (binary_search(removed.begin(), removed.end(), d->ident)) d = it->documents.erase(d);
                else d++;
            }
        }
        if (it->cg == cg) target = it;
    }
    //the added postings into category cg (a new category goes after the others)
    if (!added.empty()) {
        if (target == termDef.end()) target = termDef.insert(termDef.end(), TermCG(cg, 0));
        list<Document> merged;
        list<Document>::const_iterator d = target->documents.begin();
        for (size_t a = 0; a < added.size(); a++) {
            while (d != target->documents.end() && d->ident < added[a].ident) merged.push_back(*d++);
            if (d != target->documents.end() && d->ident == added[a].ident) d++;      //replaced
            merged.push_back(added[a]);
        }
        while (d != target->documents.end()) merged.push_back(*d++);
        target->documents.swap(merged);
    }
    //frequencies again, empty categories out
    for (list<TermCG>::iterator it = termDef.begin(); it != termDef.end(); ) {
        if (it->documents.empty()) { it = termDef.erase(it); continue; }
        uint64_t total = 0;
        for (list<Document>::const_iterator d = it->documents.begin(); d != it->documents.end(); d++) total += d->frequency;
        if (total > 0xFFFFFFFFull)
            throw OutOfBoundException("NindTermIndex::updateTermDef term frequency over 32 bits " + m_fileName);
        it->frequency = (unsigned int)total;
        it++;
    }
    if (termDef.empty()) {
        //efface dans le fichier en l'ejcrivant vide
        m_file.createBuffer(getSpecificsAndIdentificationSize());
        writeSpecificsAndIdentification(specifics, fileIdentification);
        setDefinition(ident);
        return false;
    }
    writeTermDef(ident, termDef, fileIdentification, specifics, true);
    return false;
}
////////////////////////////////////////////////////////////
//the fast path of updateTermDef
bool NindTermIndex::appendInPlace(const unsigned int ident,
                                  const unsigned char cg,
                                  const vector<Document> &added,
                                  const Identification &fileIdentification,
                                  const list<unsigned int> &specifics)
{
    //the definition's place as the bulk session leaves it (each term updated once: its file one)
    uint64_t areaOffset = 0;
    unsigned int areaLength = 0;
    if (!getDefinitionAreaBulk(ident, areaOffset, areaLength)) return false;
    //the definition read (sequentially, where the bulk session has it), its directory parsed
    if (!getDefinition(ident)) return false;
    NindReadBuffer &in = m_file.inBuffer();
    TermDirectory directory;
    parseDirectoryV2(ident, in, directory);
    const unsigned int directoryEnd = in.getInBufferSize();
    if (directory.categories.size() != 1 || directory.categories[0].cg != cg) return false;
    const TermDirectory::Category &category = directory.categories[0];
    if (category.blocks.empty() || added.front().ident <= category.blocks.back().last) return false;
    for (size_t b = 0; b < category.blocks.size(); b++)
        if (category.blocks[b].offset < directoryEnd || category.blocks[b].offset + category.blocks[b].length > areaLength)
            throw NindTermIndexException("NindTermIndex::updateTermDef G : " + m_fileName);
    //the last block's documents, then the added ones, cut into blocks
    const size_t last = category.blocks.size() - 1;
    vector<Document> documents;
    in.setInBufferPtr((unsigned int)category.blocks[last].offset);
    in.setEndInBuffer((unsigned int)category.blocks[last].length);
    decodeBlock(ident, category.blocks[last], in, documents);
    documents.insert(documents.end(), added.begin(), added.end());
    uint64_t addedFrequency = 0;
    for (size_t a = 0; a < added.size(); a++) addedFrequency += added[a].frequency;
    if (category.frequency + addedFrequency > 0xFFFFFFFFull) return false;
    vector<Block> blocks(category.blocks.begin(), category.blocks.begin() + last);
    for (size_t begin = 0; begin < documents.size(); begin += BLOCK_SIZE) {
        const size_t end = min(begin + (size_t)BLOCK_SIZE, documents.size());
        blocks.push_back(describeBlock(&documents[begin], end - begin));
    }
    //the buffer: the directory, the blocks from the last one on, the specifics and identification
    m_file.createBuffer(TAILLE_TESTE_DEJFINITION + TAILLE_TESTE_DEJFINITION_MAXIMUM
                        + blocks.size() * TAILLE_DESCRIPTEUR_MAXIMUM
                        + (blocks.size() - last) * (TAILLE_TESTE_BLOC_MAXIMUM + BLOCK_SIZE * TAILLE_DOC_MAXIMUM)
                        + getSpecificsAndIdentificationSize() + TAILLE_BUFFER_MINIMUM);
    m_file.putInt1(FLAG_DEJFINITION);
    m_file.putInt4(ident);
    putDefinitionField(0);
    m_file.putInt1(FLAG_CG);
    m_file.putInt1(cg);
    m_file.putUIntLat64(category.frequency + addedFrequency);
    m_file.putUIntLat64(category.docsNb + added.size());
    m_file.putUIntLat(blocks.size());
    vector<unsigned int> patches;
    NindDocId previousLast = 0;
    for (size_t b = 0; b < blocks.size(); b++) {
        patches.push_back(putBlockDescriptor(blocks[b], previousLast));
        previousLast = blocks[b].last;
    }
    const unsigned int directorySize = m_file.getOutBufferSize();
    putDefinitionFieldAt(directorySize - TAILLE_TESTE_DEJFINITION, OFFSET_LONGUEUR);
    //the directory must end before the first block, the blocks within the definition
    if (directorySize > category.blocks[0].offset) return false;
    const uint64_t firstOffset = category.blocks[last].offset;
    for (size_t b = last; b < blocks.size(); b++) {
        const unsigned int start = m_file.getOutBufferSize();
        putBlock(ident, cg, &documents[(b - last) * BLOCK_SIZE], blocks[b].docsNb);
        const uint64_t offset = firstOffset + (start - directorySize);
        if (offset > 0xFFFFFFFFull) return false;
        m_file.putInt4(offset, patches[b]);
        m_file.putInt4(m_file.getOutBufferSize() - start, patches[b] + 4);
    }
    const unsigned int blocksSize = m_file.getOutBufferSize() - directorySize;
    if (firstOffset + blocksSize > areaLength) return false;
    writeSpecificsAndIdentification(specifics, fileIdentification);
    vector<pair<unsigned int, unsigned int> > parts;
    parts.push_back(pair<unsigned int, unsigned int>(0, directorySize));
    parts.push_back(pair<unsigned int, unsigned int>((unsigned int)firstOffset, blocksSize));
    setDefinitionPartsBulk(ident, parts);
    return true;
}
////////////////////////////////////////////////////////////
