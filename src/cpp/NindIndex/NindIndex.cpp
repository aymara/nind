//
// C++ Implementation: NindIndex
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
#include "NindIndex.h"
//#include <iostream>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
// <donnejesIndexejes>     ::= <indirection>
// <blocEnVrac>            ::= <blocDejfinition>
//
// <indirection>           ::= <offsetDejfinition> <longueurDejfinition> 
// <offsetDejfinition>     ::= <Entier5>
// <longueurDejfinition>   ::= <Entier3>
//
// <blocDejfinition>       ::= { <dejfinition> | <vide> }
// <dejfinition>           ::= { <Octet> }
// <vide>                  ::= { <Octet> }
////////////////////////////////////////////////////////////
//<offsetDejfinition> <longueurDejfinition> = 8 (v1 : Entier5 Entier3) or 10 (v2 : Entier6 Entier4)
#define TAILLE_INDIRECTION_V1 8
#define TAILLE_INDIRECTION_V2 10
#define TAILLE_INDIRECTION getDataEntrySize()
//<tailleEntreje>(1) <tailleSpejcifiques>(3) = 4
#define TAILLE_ENTETE_FIXE 4
////////////////////////////////////////////////////////////
//brief Creates NindIndex with a specified name associated with.
//param fileName absolute path file name
//param isWriter true if writer, false if reader  
//param lexiconIdentification unique identification of lexicon  (if 0, no checks)
//param specificsSize size in bytes of specific datas
//param definitionMinimumSize size in bytes of the smallest definition
//param indirectionBlocSize number of entries in a single indirection block */
NindIndex::NindIndex(const std::string &fileName,
                             const bool isWriter,
                             const Identification &lexiconIdentification,
                             const unsigned int specificsSize,
                             const unsigned int definitionMinimumSize,
                             const unsigned int indirectionBlocSize):
    NindPadFile(fileName, 
                isWriter, 
                lexiconIdentification, 
                specificsSize,
                TAILLE_INDIRECTION_V2,          //new files: format v2
                indirectionBlocSize,
                6,                              //v2: 6-byte <addrBlocSuivant>
                TAILLE_INDIRECTION_V1),         //v1 files: read (a writer refuses them)
    m_definitionMinimumSize(definitionMinimumSize),
    m_emptyAreas()
{
    if (m_isExistingWriter) mapEmptySpaces();
}
////////////////////////////////////////////////////////////
NindIndex::~NindIndex()
{
    //le fichier doit estre complet : ne pas oublier les indirections et la queue
    if (m_isBulk) {
        try { endBulk(); }
        catch (...) {}
    }
    m_file.close();
}
////////////////////////////////////////////////////////////
//brief Enter bulk mode
void NindIndex::beginBulk()
{
    if (!m_isWriter) throw NindIndexException("NindIndex::beginBulk not a writer : " + m_fileName);
    if (m_isBulk) return;
    m_file.flush();
    //les indirections actuelles, bloc par bloc
    m_bulkIndirections.clear();
    m_bulkIndirections.reserve(getMaxIdent());
    for (list<pair<uint64_t, unsigned int> >::const_iterator it = m_entriesBlocksMap.begin();
        it != m_entriesBlocksMap.end(); it++) {
        m_file.setPos((*it).first, SEEK_SET);
        m_file.readBuffer((*it).second * TAILLE_INDIRECTION);
        for (unsigned int i = 0; i < (*it).second; i++) {
            //<offsetDejfinition> <longueurDejfinition>
            const uint64_t offsetDejfinition = getIndirectionOffset(m_file.inBuffer());
            const unsigned int longueurDejfinition = getIndirectionLength(m_file.inBuffer());
            m_bulkIndirections.push_back(pair<uint64_t, unsigned int>(offsetDejfinition, longueurDejfinition));
        }
    }
    //la queue actuelle (spejcifiques et identification)
    const unsigned int tailleQueue = getSpecificsAndIdentificationSize();
    m_file.setPos(-(long int)tailleQueue, SEEK_END);
    m_bulkEnd = m_file.getPos();
    m_file.readBuffer(tailleQueue);
    m_bulkTail.resize(tailleQueue);
    for (unsigned int i = 0; i < tailleQueue; i++) m_bulkTail[i] = m_file.getInt1();
    //les vides (m_emptyAreas) sont gejrejs comme en mode normal
    m_bulkPos = UINT64_MAX;
    m_isBulk = true;
    m_file.setBulk(true);
}
////////////////////////////////////////////////////////////
//brief Leave bulk mode
void NindIndex::endBulk()
{
    if (!m_isBulk) return;
    //1) les indirections, bloc par bloc, par tranches
    const unsigned int TRANCHE = 65536;
    size_t ident = 0;
    for (list<pair<uint64_t, unsigned int> >::const_iterator it = m_entriesBlocksMap.begin();
        it != m_entriesBlocksMap.end(); it++) {
        for (unsigned int first = 0; first < (*it).second; first += TRANCHE) {
            const unsigned int nb = min(TRANCHE, (*it).second - first);
            m_file.createBuffer(nb * TAILLE_INDIRECTION);
            for (unsigned int i = 0; i < nb; i++, ident++) {
                const pair<uint64_t, unsigned int> entry = (ident < m_bulkIndirections.size()) ?
                    m_bulkIndirections[ident] : pair<uint64_t, unsigned int>(0, 0);
                putIndirectionOffset(entry.first);
                putIndirectionLength(entry.second);
            }
            bulkWrite((*it).first + (uint64_t)first * TAILLE_INDIRECTION, 0, nb * TAILLE_INDIRECTION);
        }
    }
    //2) la queue, aprehs les donnejes
    m_file.createBuffer(m_bulkTail.size());
    m_file.putBytes(&m_bulkTail[0], m_bulkTail.size());
    bulkWrite(m_bulkEnd, 0, m_bulkTail.size());
    m_isBulk = false;
    m_file.setBulk(false);         //vidange, taille du fichier
    vector<pair<uint64_t, unsigned int> >().swap(m_bulkIndirections);
}
////////////////////////////////////////////////////////////
//brief Thread-safe (const) place of a definition in the file, without reading it
bool NindIndex::getDefinitionRefAt(const unsigned int ident,
                                   NindReadBuffer &buffer,
                                   uint64_t &offset,
                                   unsigned int &length) const
{
    if (m_isBulk) throw NindIndexException("NindIndex::getDefinitionRefAt in bulk mode : " + m_fileName);
    const uint64_t indirection = getEntryPosAt(ident, buffer);
    if (indirection == 0) return false;
    m_file.readAt(indirection, TAILLE_INDIRECTION, buffer);
    offset = getIndirectionOffset(buffer);
    length = getIndirectionLength(buffer);
    return offset != 0;
}
////////////////////////////////////////////////////////////
//brief Thread-safe (const) read of a definition into buffer (see getDefinition)
bool NindIndex::getDefinitionAt(const unsigned int ident,
                                NindReadBuffer &buffer,
                                const unsigned int bytesNb) const
{
    if (m_isBulk) throw NindIndexException("NindIndex::getDefinitionAt in bulk mode : " + m_fileName);
    const uint64_t indirection = getEntryPosAt(ident, buffer);
    if (indirection == 0) return false;
    //<offsetDejfinition> <longueurDejfinition>
    m_file.readAt(indirection, TAILLE_INDIRECTION, buffer);
    const uint64_t offsetDejfinition = getIndirectionOffset(buffer);
    const unsigned int longueurDejfinition = (bytesNb == 0) ? getIndirectionLength(buffer) : bytesNb;
    //si la dejfinition n'a pas encore ete indexe, retourne false
    if (offsetDejfinition == 0) return false;
    m_file.readAt(offsetDejfinition, longueurDejfinition, buffer);
    return true;
}
////////////////////////////////////////////////////////////
//brief Read from file datas of a specified definition and leave result into read buffer 
//param ident ident of definition
//param bytesNb optional number of bytes to read, in case of partial read
//return true if ident was found, false otherwise */
bool NindIndex::getDefinition(const unsigned int ident,
                              const unsigned int bytesNb)
{
    uint64_t indirection = getEntryPos(ident);
    if (indirection == 0) return false;
    if (m_isBulk) {
        //les indirections sont en mejmoire
        if (ident >= m_bulkIndirections.size()) return false;
        const pair<uint64_t, unsigned int> entry = m_bulkIndirections[ident];
        if (entry.first == 0) return false;
        m_file.flush();
        bulkForgetPosition();
        m_file.setPos(entry.first, SEEK_SET);
        m_file.readBuffer((bytesNb == 0) ? entry.second : bytesNb);
        return true;
    }
    m_file.flush();
    m_file.setPos(indirection, SEEK_SET);    //se positionne sur l'indirection de la dejfinition
    m_file.readBuffer(TAILLE_INDIRECTION);
    //<offsetDejfinition> <longueurDejfinition>
    const uint64_t offsetDejfinition = getIndirectionOffset(m_file.inBuffer());
    const unsigned int longueurDejfinition = (bytesNb == 0) ? getIndirectionLength(m_file.inBuffer()) : bytesNb;
    //si la dejfinition n'a pas encore ete indexe, retourne false
    if (offsetDejfinition == 0) return false;
    //et lit tout ce qui concerne la dejfinition
    m_file.setPos(offsetDejfinition, SEEK_SET);    //se positionne sur la dejfinition
    m_file.readBuffer(longueurDejfinition);
    return true;
}
////////////////////////////////////////////////////////////
//brief Write on file datas of a specified definition yet constructed into write buffer
//buffer contains datas of specified definition + specific datas + identification
//param ident ident of definition
void NindIndex::setDefinition(const unsigned int ident)
{
    //taille des spejcifiques et de l'identification qui sont en queue de buffer
    const int tailleQueue = getSpecificsAndIdentificationSize();
    //1) trouve l'ancienne indirection si elle existe
    uint64_t indirection = getEntryPos(ident);
    if (indirection == 0) throw NindIndexException("NindIndex::setDefinition : " + m_fileName);
    if (m_isBulk) {
        setDefinitionBulk(ident, tailleQueue);
        return;
    }
    m_file.setPos(indirection, SEEK_SET);    //se positionne sur l'indirection de la dejfinition
    //<offsetDejfinition> <longueurDejfinition> 
    m_file.readBuffer(TAILLE_INDIRECTION);
    const uint64_t oldOffsetEntry = getIndirectionOffset(m_file.inBuffer());
    const unsigned int oldLengthEntry = getIndirectionLength(m_file.inBuffer());
    //le buffer d'ejcriture contient <dejfinition> puis les spejcifiques et l'identification
    if (m_file.getOutBufferSize() < (unsigned int)tailleQueue)
        throw NindIndexException("NindIndex::setDefinition write buffer smaller than specifics : " + m_fileName);
    //taille de la dejfinition
    const unsigned int dataSize = m_file.getOutBufferSize() - tailleQueue;
    //si la taille est ah 0, c'est un effacement
    if (dataSize == 0) {
        //EFFACEMENT
        //met a jour la liste des emplacements vides s'il y a des anciennes donnees
        if (oldLengthEntry != 0) vacateOldArea(oldOffsetEntry, oldLengthEntry);
        //ejcrit sur le fichier les spejcifiques et l'identification qui constituent le buffer
        m_file.setPos(-tailleQueue, SEEK_END);      //se positionne sur les spejcifiques
        m_file.writeBuffer();                       //ejcriture effective sur le fichier  
        //met ah 0 l'indirection sur le fichier
        setIndirection(indirection, 0, 0);
        return;
    }
    //INSERTION ou REMPLACEMENT
    //on est susr que l'entreje a une taille supejrieure ou ejgale ah m_definitionMinimumSize
    //deux cas, soit l'entree est assez grande, soit non
    //si la taille est compatible avec l'ancienne, ejcrit au meme endroit
    if (dataSize <= oldLengthEntry) {
        //REMPLACEMENT AU MESME ENDROIT
        //3a) ecrit les nouvelles donnees
        m_file.setPos(oldOffsetEntry, SEEK_SET);        //se positionne sur la dejfinition
        m_file.writeBuffer(0, dataSize);                //ejcrit la nouvelle dejfinition, juste elle
        //ejcrit sur le fichier les spejcifiques et l'identification qui constituent le buffer
        m_file.setPos(-tailleQueue, SEEK_END);          //se positionne sur les spejcifiques
        m_file.writeBuffer(dataSize, tailleQueue);      //ejcriture effective sur le fichier             
        //l'indirection reste inchangee
        return;
    }
    //INSERTION ou REMPLACEMENT NOUVEL ENDROIT
    //Sinon ecrit sur un endroit libre et efface virtuellement
    uint64_t offsetDejfinition = 0;
    unsigned int longueurDejfinition = 0;
    //2) trouve une place libre
    //cherche d'abord une place compatible (>= dataSize et < dataSize + m_definitionMinimumSize)
    const bool AlaFin = findNewArea(dataSize, offsetDejfinition, longueurDejfinition);
    //3b) ecrit les nouvelles donnees
    m_file.setPos(offsetDejfinition, SEEK_SET);    //se positionne sur la dejfinition
    //si en fin, ejcrit donnejes + spejcifiques + identification
    if (AlaFin) {
        m_file.writeBuffer();                     //ecrit le buffer entier
    }
    else {
        m_file.writeBuffer(0, dataSize);                //ejcrit la nouvelle dejfinition, juste elle
        //ejcrit sur le fichier les spejcifiques et l'identification qui constituent le buffer
        m_file.setPos(-tailleQueue, SEEK_END);          //se positionne sur les spejcifiques
        m_file.writeBuffer(dataSize, tailleQueue);      //ejcriture effective sur le fichier             
    }
    //4) met a jour la liste des emplacements vides s'il y a des anciennes donnees
    if (oldLengthEntry != 0) vacateOldArea(oldOffsetEntry, oldLengthEntry);
    //5) ecrit la nouvelle indirection
    setIndirection(indirection, offsetDejfinition, longueurDejfinition);
}
////////////////////////////////////////////////////////////
//ejcriture d'une dejfinition en mode massif : toujours ah la fin, sauf si elle tient ah son ancienne place
void NindIndex::setDefinitionBulk(const unsigned int ident,
                                  const unsigned int tailleQueue)
{
    //des blocs d'indirection ont pu estre ajoutejs depuis
    if (m_bulkIndirections.size() < getMaxIdent())
        m_bulkIndirections.resize(getMaxIdent(), pair<uint64_t, unsigned int>(0, 0));
    if (ident >= m_bulkIndirections.size()) throw NindIndexException("NindIndex::setDefinition : " + m_fileName);
    pair<uint64_t, unsigned int> &entry = m_bulkIndirections[ident];
    if (m_file.getOutBufferSize() < tailleQueue)
        throw NindIndexException("NindIndex::setDefinition write buffer smaller than specifics : " + m_fileName);
    const unsigned int dataSize = m_file.getOutBufferSize() - tailleQueue;
    //la queue est toujours en fin de buffer
    m_file.copyOutBuffer(dataSize, tailleQueue, m_bulkTail);
    //les places choisies sont celles du mode normal (mesmes vides, mesme fin)
    if (dataSize == 0) {
        //EFFACEMENT
        if (entry.second != 0) vacateOldArea(entry.first, entry.second);
        entry = pair<uint64_t, unsigned int>(0, 0);
        return;
    }
    if (dataSize <= entry.second) {
        //REMPLACEMENT AU MESME ENDROIT, longueur inchangeje
        bulkWrite(entry.first, 0, dataSize);
        return;
    }
    //INSERTION ou REMPLACEMENT NOUVEL ENDROIT : un vide ou la fin
    uint64_t offsetDejfinition = 0;
    unsigned int longueurDejfinition = 0;
    const bool aLaFin = findNewArea(dataSize, offsetDejfinition, longueurDejfinition);
    bulkWrite(offsetDejfinition, 0, dataSize);
    if (aLaFin) m_bulkEnd += dataSize;
    if (entry.second != 0) vacateOldArea(entry.first, entry.second);
    entry = pair<uint64_t, unsigned int>(offsetDejfinition, longueurDejfinition);
}
////////////////////////////////////////////////////////////
//brief rewrite parts of a definition in place, its place and length unchanged
void NindIndex::setDefinitionParts(const unsigned int ident,
                                   const vector<pair<unsigned int, unsigned int> > &parts)
{
    uint64_t offset = 0;
    unsigned int length = 0;
    if (!getDefinitionArea(ident, offset, length))
        throw NindIndexException("NindIndex::setDefinitionParts no definition : " + m_fileName);
    const unsigned int tailleQueue = getSpecificsAndIdentificationSize();
    unsigned int position = 0;
    for (size_t i = 0; i < parts.size(); i++) {
        if ((uint64_t)parts[i].first + parts[i].second > length)
            throw NindIndexException("NindIndex::setDefinitionParts outside the definition : " + m_fileName);
        position += parts[i].second;
    }
    if (m_file.getOutBufferSize() != position + tailleQueue)
        throw NindIndexException("NindIndex::setDefinitionParts write buffer : " + m_fileName);
    if (m_isBulk) {
        //la queue est toujours en fin de buffer (copieje avant les ejcritures, qui vident le buffer)
        m_file.copyOutBuffer(position, tailleQueue, m_bulkTail);
        position = 0;
        for (size_t i = 0; i < parts.size(); i++) {
            bulkWrite(offset + parts[i].first, position, parts[i].second);
            position += parts[i].second;
        }
        return;
    }
    position = 0;
    for (size_t i = 0; i < parts.size(); i++) {
        m_file.setPos(offset + parts[i].first, SEEK_SET);
        m_file.writeBuffer(position, parts[i].second);
        position += parts[i].second;
    }
    //ejcrit sur le fichier les spejcifiques et l'identification qui constituent la fin du buffer
    m_file.setPos(-(long int)tailleQueue, SEEK_END);
    m_file.writeBuffer(position, tailleQueue);
}
////////////////////////////////////////////////////////////
//brief the place of a definition
bool NindIndex::getDefinitionArea(const unsigned int ident,
                                  uint64_t &offset,
                                  unsigned int &length)
{
    if (m_isBulk) {
        if (ident >= m_bulkIndirections.size()) return false;
        offset = m_bulkIndirections[ident].first;
        length = m_bulkIndirections[ident].second;
        return offset != 0;
    }
    const uint64_t indirection = getEntryPos(ident);
    if (indirection == 0) return false;
    m_file.flush();
    m_file.setPos(indirection, SEEK_SET);
    m_file.readBuffer(TAILLE_INDIRECTION);
    offset = getIndirectionOffset(m_file.inBuffer());
    length = getIndirectionLength(m_file.inBuffer());
    return offset != 0;
}
////////////////////////////////////////////////////////////
//brief verifie que l'indirection existe et cree un bloc supplementaire si c'est pertinent
//param ident ident of definition
//param fileIdentification unique identification of file 
void NindIndex::checkExtendIndirection(const unsigned int ident,
                                       const Identification &fileIdentification)
{
    //si la dejfinition n'a pas d'indirection, ajoute un bloc d'indirection
    uint64_t indirection = getEntryPos(ident);
    if (indirection == 0) {
        //la dejfinition est hors des blocs d'indexation actuels, cree un nouveau bloc d'indirection
        addEntriesBlock(fileIdentification);
        //redemande l'indirection pour la dejfinition, erreur si hors limite
        indirection = getEntryPos(ident);
        if (indirection == 0) throw NindIndexException("NindIndex::checkExtendIndirection : " + m_fileName);
    }
}
////////////////////////////////////////////////////////////
//brief Validate the file and compute indirection-usage statistics
//return the file's index-level structural statistics
//throws NindPadFileException/NindIndexException if the file is structurally invalid */
NindIndex::IndexStats NindIndex::analyseIndex()
{
    if (m_isBulk) throw NindIndexException("NindIndex::analyseIndex : file in bulk mode " + m_fileName);
    IndexStats stats;
    stats.maxIdent = getMaxIdent();

    //trouve la carte des non vides : d'abord les fixes, puis les blocs d'indirection
    list<pair<uint64_t, unsigned int> > nonVidesList;
    nonVidesList.push_back(pair<uint64_t, unsigned int>(0, TAILLE_ENTETE_FIXE));
    for (list<pair<uint64_t, unsigned int> >::const_iterator it = m_entriesBlocksMap.begin();
        it != m_entriesBlocksMap.end(); it++) {
        const pair<uint64_t, unsigned int> nonVide((*it).first - TAILLE_TETE_INDEX,
            (*it).second * TAILLE_INDIRECTION + TAILLE_TETE_INDEX);
        nonVidesList.push_back(nonVide);
    }
    //puis chaque dejfinition indexeje
    list<int64_t> definitionSizes;
    for (unsigned int identifiant = 0; identifiant < stats.maxIdent; identifiant++) {
        const uint64_t indirection = getEntryPos(identifiant);
        if (indirection == 0)
            throw NindIndexException("NindIndex::analyseIndex : identifiant sans indirection " + m_fileName);
        m_file.setPos(indirection, SEEK_SET);
        //<offsetDejfinition> <longueurDejfinition>
        m_file.readBuffer(TAILLE_INDIRECTION);
        const uint64_t offsetDejfinition = getIndirectionOffset(m_file.inBuffer());
        const unsigned int longueurDejfinition = getIndirectionLength(m_file.inBuffer());
        //si la dejfinition n'a pas encore ete indexe, n'en tient pas compte
        if (offsetDejfinition != 0) {
            nonVidesList.push_back(pair<uint64_t, unsigned int>(offsetDejfinition, longueurDejfinition));
            definitionSizes.push_back((int64_t)longueurDejfinition);
            stats.usedCount++;
        }
    }
    //enfin les spejcifiques et l'identification
    const int tailleQueue = getSpecificsAndIdentificationSize();
    m_file.setPos(-tailleQueue, SEEK_END);
    nonVidesList.push_back(pair<uint64_t, unsigned int>((uint64_t)m_file.getPos(), tailleQueue));

    stats.holes = chercheVides(nonVidesList);
    stats.definitionSizes = calculeRejpartition(definitionSizes);
    return stats;
}
////////////////////////////////////////////////////////////
//ejcrit une nouvelle indirection dans l'index
void NindIndex::setIndirection(const uint64_t indirection,
                               const uint64_t offsetDejfinition,
                               const unsigned int longueurDejfinition)
{
    //se positionne sur l'indirection de la dejfinition
    m_file.setPos(indirection, SEEK_SET);
    //<offsetDejfinition> <longueurDejfinition> 
    m_file.createBuffer(TAILLE_INDIRECTION);
    putIndirectionOffset(offsetDejfinition);               // <offsetDejfinition> 
    putIndirectionLength(longueurDejfinition);             //<longueurDejfinition>
    m_file.writeBuffer();                           //ecriture effective sur le fichier      
}
////////////////////////////////////////////////////////////
//ejtablit la carte des vides
void NindIndex::mapEmptySpaces()
{
    //trouve la carte des non vides (les blocs d'indirection sont ajoutejs par fillEmptyAreas)
    list<pair<uint64_t, unsigned int> > nonVidesList;
    //prend toutes les entrees d'indirection
    unsigned int ident = 0;
    uint64_t indirection = getEntryPos(ident);
    while (indirection != 0) {
        m_file.setPos(indirection, SEEK_SET);    //se positionne sur l'indirection de la dejfinition
        m_file.readBuffer(TAILLE_INDIRECTION);
        //<offsetDejfinition> <longueurDejfinition> 
        const uint64_t offsetDejfinition = getIndirectionOffset(m_file.inBuffer());
        const unsigned int longueurDejfinition = getIndirectionLength(m_file.inBuffer());
        //si la dejfinition n'a pas encore ete indexe, n'en tient pas compte
        if (offsetDejfinition != 0) {
            const pair<uint64_t, unsigned int> nonVide(offsetDejfinition, longueurDejfinition);
            nonVidesList.push_back(nonVide);
        }
        ident++;
        indirection = getEntryPos(ident);
    } 
    //ajoute les spejcifiques et l'identification dans les non vides
    //taille des spejcifiques et de l'identification qui sont en queue de buffer
    const int tailleQueue = getSpecificsAndIdentificationSize();
    m_file.setPos(-tailleQueue, SEEK_END);       //se positionne sur les spejcifiques
    const pair<uint64_t, unsigned int> nonVide(m_file.getPos(), tailleQueue);
    nonVidesList.push_back(nonVide);
    fillEmptyAreas(nonVidesList);
}
////////////////////////////////////////////////////////////
//remplit la carte des vides ah partir de la liste des zones occupejes
void NindIndex::fillEmptyAreas(list<pair<uint64_t, unsigned int> > &nonVidesList)
{
    //les blocs d'indirection sont mis dans les non-vides
    for (list<pair<uint64_t, unsigned int> >::const_iterator it = m_entriesBlocksMap.begin();
        it != m_entriesBlocksMap.end(); it++) {
        const pair<uint64_t, unsigned int> nonVide((*it).first - TAILLE_TETE_INDEX, (*it).second * TAILLE_INDIRECTION + TAILLE_TETE_INDEX);
        nonVidesList.push_back(nonVide);
    }
    m_emptyAreas.clear();
    //ordonne les non vides
    nonVidesList.sort();
    //remplit la carte des vides
    uint64_t addressePrec = 0;
    unsigned int longueurPrec = 0;
    for (list<pair<uint64_t, unsigned int> >::const_iterator it = nonVidesList.begin();
        it != nonVidesList.end(); it++) {
        const int64_t longueurVide = (int64_t)(*it).first - (int64_t)addressePrec - (int64_t)longueurPrec;
        if (longueurVide < 0) throw NindIndexException("NindIndex::mapEmptySpaces " + m_fileName);
        if (longueurVide > 0) {
            const pair<uint64_t, unsigned int> emptyArea(addressePrec + longueurPrec, longueurVide);
            m_emptyAreas.push_back(emptyArea); 
        }
        addressePrec = (*it).first;
        longueurPrec = (*it).second;           
    }
    //dumpEmptyAreas();
}
////////////////////////////////////////////////////////////
//trouve une nouvelle zone pour les nouvelles donnejs
bool NindIndex::findNewArea(const unsigned int dataSize,
                            uint64_t &offsetDejfinition,
                            unsigned int &longueurDejfinition)
{
    list<pair<uint64_t, unsigned int> >::iterator memit = m_emptyAreas.end();
    list<pair<uint64_t, unsigned int> >::iterator it = m_emptyAreas.begin(); 
    while (it != m_emptyAreas.end()) {
        if ((*it).second >= dataSize) {
            //taille suffisante
            if ((*it).second < dataSize + m_definitionMinimumSize) {
                //mais pas trop grande
                offsetDejfinition = (*it).first;       //c'est la qu'il faut ecrire
                longueurDejfinition = (*it).second;    //on conserve la meme longueur pour des raisons de perfs 
                m_emptyAreas.erase(it);               //efface l'entree
                return false;                         //pas ah la fin
            }
            //memorise le plus petit parmi les trop grands
            if (memit != m_emptyAreas.end()) {                  //premiere comparaison ?
                if ((*it).second < (*memit).second) memit = it; //non, mejmorise le plus petit
            }
            else memit = it;                                    //oui mejmorise le premier 
        }
        it++;
    }
    //pas trouvej, cherche une place plus grande
    //si une place plus grande a ete trouve, en prend une partie
    if (memit != m_emptyAreas.end()) {
        offsetDejfinition = (*memit).first;       //c'est la qu'il faut ecrire
        longueurDejfinition = dataSize;     //avec la taille juste
        (*memit).first += dataSize;        //retaille l'entree (minimum m_definitionMinimumSize)
        (*memit).second -= dataSize;
        return false;                            //pas ah la fin
    }
    //ejcrit a la fin 
    offsetDejfinition = m_isBulk ? m_bulkEnd : m_file.getFileSize() - getSpecificsAndIdentificationSize();       //ecrit a la fin 
    longueurDejfinition = (dataSize > m_definitionMinimumSize)? dataSize : m_definitionMinimumSize;  //avec la taille 
    return true;                                //ah la fin
}
////////////////////////////////////////////////////////////
//brief Place l'ancienne zone de donnejes dans la gestion du vide
void NindIndex::vacateOldArea(const uint64_t oldOffsetEntry,
                              const unsigned int oldLengthEntry)
{
    list<pair<uint64_t, unsigned int> >::iterator it = m_emptyAreas.begin(); 
    while (it != m_emptyAreas.end()) {
        if ((*it).first + (*it).second == oldOffsetEntry) {
            (*it).second += oldLengthEntry;             //globalisation avec l'entree precedente
            //ca peut arriver que la globalisation avec la suivante soit aussi possible
            list<pair<uint64_t, unsigned int> >::iterator nextit = it; 
            nextit++;
            if (nextit != m_emptyAreas.end()) {
                if ((*it).first + (*it).second == (*nextit).first) {
                    (*it).second += (*nextit).second;
                    m_emptyAreas.erase(nextit);            //efface l'entree suivante                           
                }
            }
            break;
        }
        if ((*it).first == oldOffsetEntry + oldLengthEntry) {
            (*it).second += oldLengthEntry;             //globalisation avec l'entree suivante
            (*it).first = oldOffsetEntry;
            break;
        }
        if ((*it).first > oldOffsetEntry) {
            const pair<uint64_t, unsigned int> emptyArea(oldOffsetEntry, oldLengthEntry);
            m_emptyAreas.insert(it, emptyArea);                 //nouvelle entree au bon endroit
            break;
        }
        it++;                    
    }
    if (it == m_emptyAreas.end()) {
        const pair<uint64_t, unsigned int> emptyArea(oldOffsetEntry, oldLengthEntry);
        m_emptyAreas.push_back(emptyArea);                 //nouvelle entree a la fin
    }
}
////////////////////////////////////////////////////////////
// void NindIndex::dumpEmptyAreas()
// {
//     cout<<"VIDES: "<<m_emptyAreas.size()<<" (";
// //      for (list<pair<unsigned int, uint64_t> >::iterator it = m_emptyAreas.begin(); it != m_emptyAreas.end(); it++) 
// //          cout<<(*it).first<<" - "<<(*it).second<<", ";
//     cout<<")"<<endl;
// }
////////////////////////////////////////////////////////////

