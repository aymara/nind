#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""The shared "pad file" envelope used by every nind index/lexicon file.

A "pad file" (the name reflects that entries are padded to a fixed size so
they can be located by simple arithmetic) is nind's common on-disk
container: a small fixed header, one or more *indexed blocks* (fixed-size
indirection tables mapping an integer identifier to an offset+length
elsewhere in the file), the variable-length definitions those indirections
point to ("en vrac", i.e. loosely packed), and a trailer holding
format-specific data plus a max-identifier/timestamp identification stamp.
See the ``<fichier>`` grammar comment below for the exact layout.

:class:`NindPadFile` implements this envelope read-only (mirroring
``NindBasics::NindPadFile`` in C++, which also has no writer - every
concrete writer, in C++ or in
:class:`~nind.nind_engine.NindIndexer` here, hand-rolls the envelope
itself). Concrete formats (:class:`~nind.NindIndex.NindIndex` and its
``.nindlexiconindex``/``.nindtermindex``/``.nindlocalindex`` subclasses,
:class:`~nind.NindRetrolexicon.NindRetrolexicon`) subclass it and only add
the meaning of the "en vrac" definitions and the specifics block.
"""
__author__ = "jys"
__copyright__ = "Copyright (C) 2017 LATEJCON"
__license__ = "GNU LGPL"
__version__ = "2.1.2"
# Author: jys <jy.sage@orange.fr>, (C) LATEJCON 2017
# Copyright: 2014-2017 LATEJCON. See LICENCE.md file that comes with this distribution
# This file is part of NIND (as "nouvelle indexation").
# NIND is free software: you can redistribute it and/or modify it under the terms of the 
# GNU Less General Public License (LGPL) as published by the Free Software Foundation, 
# (see <http://www.gnu.org/licenses/>), either version 3 of the License, or any later version.
# NIND is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without 
# even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU Less General Public License for more details.
import sys
from os import path, getenv
import codecs
import datetime
import time
import math
try:
    from .NindFile import NindFile
except ImportError:
    # run directly (not as part of the installed package): see docs/cli.md
    from NindFile import NindFile

def usage():
    if getenv("PY") != None: script = sys.argv[0].replace(getenv("PY"), '$PY')
    else: script = sys.argv[0]
    print ("""© l'ATEJCON.
o Analyse un fichier plat du système nind et affiche les stats. 
Les types de fichiers : nindlexiconindex, nindtermindex, nindlocalindex,
nindretrolexicon
Le format du fichier est défini dans le document LAT2017.JYS.470.

usage   : %s <fichier>
exemple : %s fre.nindtermindex
"""%(script, script))


def main():
    if len(sys.argv) < 2 :
        usage()
        sys.exit()
    padFileName = path.abspath(sys.argv[1])
    #la classe
    nindPadFile = NindPadFile(padFileName)
    nindPadFile.analyseFichierPadFile(True)

#############################################################
# <fichier>               ::= <tailleEntreje> <tailleSpejcifiques> { <blocIndexej> <blocEnVrac> } 
#                             <blocSpejcifique> <blocIdentification> 
#
# <tailleEntreje>         ::= <Entier1>
# <tailleSpejcifiques>    ::= <Entier3>
#
# <blocIndexej>           ::= <flagIndexej=47> <addrBlocSuivant> <nombreIndex> { <donnejesIndexejes> }
# <flagIndexej=47>        ::= <Entier1>
# <addrBlocSuivant>       ::= <Entier5>
# <nombreIndex>           ::= <Entier3>
#
# <donnejesIndexejes>     ::= { <Octet> }
# <blocEnVrac>            ::= { <Octet> }
#
# <blocSpejcifique>       ::= <flagSpecifique=57> <spejcifiques> 
# <flagSpecifique=57>     ::= <Entier1>
# <spejcifique>           ::= { <Octet> }
#
# <blocIdentification>    ::= <flagIdentification=53> <maxIdentifiant> <identifieurUnique> 
# <flagIdentification=53> ::= <Entier1>
# <maxIdentifiant>        ::= <Entier3>
# <identifieurUnique>     ::= <dateHeure>
# <dateHeure >            ::= <Entier4>
#############################################################""    
FLAG_INDEXEJ = 47
FLAG_SPEJCIFIQUE = 57
FLAG_IDENTIFICATION = 53
#<flagIndexej=47>(1) <addrBlocSuivant>(5) <nombreIndex>(3) = 9
TAILLE_TETE_INDEXEJ = 9
#<flagIdentification=53>(1) <maxIdentifiant>(4) <identifieurUnique>(4) = 9
TAILLE_IDENTIFICATION = 9
#<flagSpecifique=57>(1)
TAILLE_ENTETE_SPEJCIFIQUE = 1
#<tailleEntreje>(1) <tailleSpejcifiques>(3) = 4
TAILLE_FIXES = 4
    
#############################################################
class NindPadFile(NindFile):
    """Read-only base class implementing the pad-file envelope over :class:`~nind.NindFile.NindFile`.

    Subclasses get, for free, the machinery to locate an entry by
    identifier (:meth:`donnePositionEntreje`), find the file's specifics
    and identification trailer (:meth:`donneSpejcifiques`,
    :meth:`donneIdentificationFichier`), and walk/validate the whole file
    (:meth:`analyseFichierPadFile`).
    """

    def __init__(self, padFileName, enEjcriture = False, enModification = False, tailleEntreje = 0, tailleSpejcifiques = 0):
        """Open (or create) a pad file.

        :param padFileName: path to the file.
        :param enEjcriture: if ``True``, create the file for writing and
            write its fixed header (``tailleEntreje``/``tailleSpejcifiques``)
            plus zeroed specifics+identification; if ``False`` (default),
            open read-only and read that same fixed header back.
        :param enModification: passed through to
            :class:`~nind.NindFile.NindFile` (in-place modification mode).
        :param tailleEntreje: size in bytes of one indirection-table entry
            (French *tailleEntrée* = "entry size"); only used when writing.
        :param tailleSpejcifiques: size in bytes of the format-specific
            trailer block (French *tailleSpécifiques* = "specifics size");
            only used when writing.
        """
        #en lecture uniquement
        NindFile.__init__(self, padFileName, enEjcriture, enModification)
        self.entriesBlocksMap = []
        if enEjcriture and not enModification:
            self.tailleEntreje = tailleEntreje
            self.tailleSpejcifiques = tailleSpejcifiques
            #ejcrit la partie fixe
            self.seek(0, 0)
            self.ejcritNombre1(tailleEntreje)
            self.ejcritNombre3(tailleSpejcifiques)
            #ejcrit les spejcifiques et l'identification ah 0
            self.ejcritZejros(TAILLE_IDENTIFICATION + TAILLE_ENTETE_SPEJCIFIQUE + tailleSpejcifiques)
        else:   
            #lit la partie fixe
            #<tailleEntreje> <tailleSpejcifiques>
            self.seek(0, 0)
            self.tailleEntreje = self.litNombre1()
            self.tailleSpejcifiques = self.litNombre3()
        
    #############################################################
    # vejrifie l'intejgritej du fichier (pour pouvoir analyser un fichier dejconnant)
    def vejrifieFichier(self):
        """Check the file's structural integrity (French *vérifieFichier* = "checks file").

        Builds the indirection-blocks map (raising if the ``FLAG_INDEXEJ``
        markers are missing/misplaced) and confirms the specifics block's
        ``FLAG_SPEJCIFIQUE`` marker is where expected. Subclasses'
        ``__init__`` call this right after opening, so that a malformed
        file fails fast rather than on first lookup.

        :raises Exception: if the file's structure is invalid.
        """
        #ejtablit la carte des index sur les diffejrents blocs
        self.__ejtablitCarteIndex()
        # vejrifie les spejcifiques
        self.donneSpejcifiques()

    #############################################################
    #retourne l'identification du fichier
    def donneIdentificationFichier(self):
        """Return the file's identification trailer (French *donneIdentificationFichier* = "gives file identification").

        :return: a ``(maxIdentifiant, identifieurUnique)`` tuple: the
            highest identifier ever assigned in this file, and a unique
            stamp (in practice a Unix timestamp) written when the file was
            finalized.
        :raises Exception: if the trailer's ``FLAG_IDENTIFICATION`` marker
            is missing.
        """
        #<flagIdentification=53> <maxIdentifiant> <identifieurUnique>
        self.seek(-TAILLE_IDENTIFICATION, 2)
        if self.litNombre1() != FLAG_IDENTIFICATION:
            raise Exception('%s : pas FLAG_IDENTIFICATION à %08X'%(self.latFileName, self.tell() -1))
        maxIdentifiant = self.litNombre4()
        identifieurUnique = self.litNombre4()
        return (maxIdentifiant, identifieurUnique)

    #############################################################
    #retourne l'adresse et la longueur des spejcifiques
    def donneSpejcifiques(self):
        """Locate the format-specific trailer block (French *donneSpécifiques* = "gives specifics").

        The "specifics" block holds whatever extra fixed-size data a
        concrete format needs beyond the generic envelope (e.g.
        :class:`~nind.NindLocalindex.NindLocalindex` stores the document
        count there).

        :return: a ``(offsetSpejcifiques, tailleSpejcifiques)`` tuple: the
            byte offset of the block and its size (as declared in the
            file's fixed header).
        :raises Exception: if the block's ``FLAG_SPEJCIFIQUE`` marker is
            missing.
        """
        #<flagSpecifique=57> <spejcifiques>
        self.seek(-TAILLE_IDENTIFICATION -TAILLE_ENTETE_SPEJCIFIQUE - self.tailleSpejcifiques , 2)
        if self.litNombre1() != FLAG_SPEJCIFIQUE:
            raise Exception('%s : pas FLAG_SPEJCIFIQUE à %08X'%(self.latFileName, self.tell() -1))
        offsetSpejcifiques = self.tell()
        return (offsetSpejcifiques, self.tailleSpejcifiques)

    #############################################################
    #donne l'adresse de l'index correspondant ah l'ident spejcifiej, 0 si hors limite
    def donnePositionEntreje(self, ident):
        """Return the file offset of the indirection-table entry for ``ident``.

        French *donnePositionEntrée* = "gives entry position". Looks up
        ``ident`` across the (possibly chained) indexed blocks built by
        :meth:`vejrifieFichier`; if not found, rebuilds the blocks map once
        (in case the file grew since it was opened) before giving up.

        :param ident: the identifier to locate.
        :return: the byte offset of that identifier's indirection entry, or
            ``0`` if ``ident`` is out of range.
        """
        position = self.__donneJustePositionEntreje(ident)
        #si pas trouvej, recharge la map une fois (au cas ouh le fichier aurait changej)
        if position == 0:
            self.__ejtablitCarteIndex()
            position = self.__donneJustePositionEntreje(ident)
        return position

    #############################################################
    #donne la taille du fichier
    def donneTailleFichier(self):
        """Return the total size of the file in bytes (French *donneTailleFichier* = "gives file size")."""
        self.seek(0, 2)
        return self.tell()
    
    #############################################################   
    #donne l'adresse de l'index correspondant ah l'ident spejcifiej, 0 si hors limite
    def __donneJustePositionEntreje(self, ident):
        firstIdent = 0
        for (addrPremierIndex, nombreIndex) in self.entriesBlocksMap:
            if ident < firstIdent + nombreIndex : 
                return (ident - firstIdent) * self.tailleEntreje + addrPremierIndex
            firstIdent += nombreIndex
        return 0

    #############################################################   
    #ejtablit la carte des index sur les diffejrents blocs
    def __ejtablitCarteIndex(self):
        self.entriesBlocksMap = []
        self.seek(TAILLE_FIXES, 0)
        while True:
            #<flagIndexej=47> <addrBlocSuivant> <nombreIndex>
            flagIndexej = self.litNombre1()
            if flagIndexej != FLAG_INDEXEJ: 
                raise Exception('%s : pas FLAG_INDEXEJ à %08X'%(self.latFileName, self.tell()))
            addrBlocSuivant = self.litNombre5()
            nombreIndex = self.litNombre3()
            addrPremierIndex = self.tell()
            self.entriesBlocksMap.append((addrPremierIndex, nombreIndex))
            if addrBlocSuivant == 0: break
            #saute au bloc d'indirection suivant
            self.seek(addrBlocSuivant, 0)
            
    #############################################################
    #retourne l'identifiant maximum possible avec le systehme d'index du fichier
    def donneMaxIdentifiant(self):
        """Return the highest identifier addressable by this file's index blocks.

        French *donneMaxIdentifiant* = "gives max identifier". This is the
        *capacity* of the indirection system (sum of ``nombreIndex`` across
        all chained indexed blocks), not necessarily the highest identifier
        actually in use - compare with :meth:`donneIdentificationFichier`'s
        ``maxIdentifiant``, which is.

        :raises Exception: if a block's ``FLAG_INDEXEJ`` marker is missing.
        """
        self.seek(TAILLE_FIXES, 0)
        maxIdent = 0
        while True:
            addrBloc = self.tell()
            #<flagIndexej=47> <addrBlocSuivant> <nombreIndex>
            if self.litNombre1() != FLAG_INDEXEJ: 
                raise Exception('%s : pas FLAG_INDEXEJ à %08X'%(self.latFileName, self.tell()))
            addrBlocSuivant = self.litNombre5()
            maxIdent += self.litNombre3()
            if addrBlocSuivant == 0: break
            #saute au bloc d'indirection suivant
            self.seek(addrBlocSuivant, 0)
        return maxIdent
    
    #############################################################
    #retourne la liste des occupations des fixes, des blocs indexejs, des spejcifiques et de l'identification
    def donneCarteNonVides(self):
        """Return the file's known "non-empty" (occupied) byte ranges.

        French *donneCarteNonVides* = "gives non-empty map". Lists the
        ranges occupied by the fixed header and every indexed block, as a
        starting point for callers that then add the ranges occupied by
        the definitions themselves, so :func:`chercheVides` can find what's
        left over ("holes" - freed or never-written space).

        :return: a ``(maxIdent, nonVidesList)`` tuple: the addressable
            identifier capacity (same as :meth:`donneMaxIdentifiant`), and
            a list of ``(address, length)`` occupied ranges.
        :raises Exception: if a block's ``FLAG_INDEXEJ`` marker is missing.
        """
        #les fixes
        nonVidesList = [ (0, TAILLE_FIXES) ]
        self.seek(TAILLE_FIXES, 0)
        #les blocs indexejs
        maxIdent = 0
        while True:
            addrBloc = self.tell()
            #<flagIndexej=47> <addrBlocSuivant> <nombreIndex>
            if self.litNombre1() != FLAG_INDEXEJ: 
                raise Exception('%s : pas FLAG_INDEXEJ à %08X'%(self.latFileName, self.tell()))
            addrBlocSuivant = self.litNombre5()
            nombreIndex = self.litNombre3()
            maxIdent += nombreIndex
            tailleBloc = TAILLE_TETE_INDEXEJ + nombreIndex * self.tailleEntreje
            nonVidesList.append((addrBloc, tailleBloc))
            if addrBlocSuivant == 0: break
            #saute au bloc d'indirection suivant
            self.seek(addrBlocSuivant, 0)
        #les spejcifiques et l'identification
        tailleSpejcifIdent = TAILLE_ENTETE_SPEJCIFIQUE + TAILLE_IDENTIFICATION + self.tailleSpejcifiques
        self.seek(-tailleSpejcifIdent, 2)
        nonVidesList.append((self.tell(), tailleSpejcifIdent))
        return maxIdent, nonVidesList
    
    #############################################################
    #############################################################
    # change l'identification ah la fin du fichier
    def changeIdentificationFichier(self, maxIdentifiant, identifieurUnique):
        """Overwrite the file's identification trailer in place.

        French *changeIdentificationFichier* = "changes file
        identification". The file must have been opened in a writable mode
        (``enEjcriture``/``enModification``); this seeks to the trailer and
        rewrites its two fields.

        :param maxIdentifiant: new highest-identifier value to record.
        :param identifieurUnique: new unique stamp to record.
        :raises Exception: if the trailer's ``FLAG_IDENTIFICATION`` marker
            is missing.
        """
        # <flagIdentification=53> <maxIdentifiant> <identifieurUnique>
        self.seek(-TAILLE_IDENTIFICATION, 2)
        if self.litNombre1() != FLAG_IDENTIFICATION: 
            raise Exception('%s : pas FLAG_IDENTIFICATION à %08X'%(self.latFileName, self.tell() -1))
        self.ejcritNombre4(maxIdentifiant)
        self.ejcritNombre4(identifieurUnique)
    
    #############################################################""
    #analyse complehtement le fichier et retourne True si ok
    def analyseFichierPadFile(self, trace):
        """Walk and validate the whole pad-file envelope, optionally printing a report.

        French *analyseFichierPadFile* = "analyzes pad file". Delegates the
        binary traversal to ``nind._native``'s ``analyse_pad_file()`` (see
        ``NindPadFile::analysePadFile`` in the C++), then reports the same
        size breakdowns as before (fixed header / indexed blocks / "en
        vrac" data / specifics / identification). Used both as a diagnostic
        (``Nind_dumpDocument.py`` and friends) and as the first step of
        every subclass's own ``analyseFichierXxx`` method.

        :param trace: if ``True``, print a human-readable report to stdout.
        :return: ``True`` if the envelope is structurally valid, ``False``
            otherwise (errors are reported via ``trace`` rather than
            raised).
        :note: unlike the previous hand-rolled implementation, a structural
            error stops the report at that point rather than continuing to
            print whatever the remaining sections can still compute.
        """
        if trace: print ("======PADFILE=======")
        try:
            stats = self._native.analyse_pad_file()
        except Exception as exc:
            if trace: print ('ERREUR :', exc.args[0])
            return False

        if trace:
            print('TAILLE ENTRÉES             : ', stats.data_entry_size)
            print('TAILLE DONNÉES SPÉCIFIQUES : ', stats.specifics_size)
            print ("=============")
            for block in stats.blocks:
                print ("%08X: Bloc indexé  n° % 2d :    % 10d / %d entrées utilisées"%(
                    block.block_addr, block.block_num, block.entries_used, block.entries_total))
                print ("%08X: Bloc en vrac n° % 2d :    % 10d octets"%(
                    block.en_vrac_addr, block.block_num, block.en_vrac_size))
            print ("%d blocs indexés  de taille totale % 10d octets"%(len(stats.blocks), stats.index_total_size))
            print ("%d blocs en vrac  de taille totale % 10d octets"%(len(stats.blocks), stats.en_vrac_total_size))
            print ("=============")

        maxIdentifiant = stats.identification.lexicon_words_nb
        dateHeure = stats.identification.lexicon_time
        print ("max=%d dateheure=%d (%s)"%(maxIdentifiant, dateHeure, time.ctime(int(dateHeure))))

        tailleSpejcifique = stats.specifics_size + TAILLE_ENTETE_SPEJCIFIQUE
        total = stats.index_total_size + stats.en_vrac_total_size + tailleSpejcifique + TAILLE_IDENTIFICATION + TAILLE_FIXES
        if trace:
            print ("=============")
            print ("FIXES          % 10d (%6.2f %%)"%(TAILLE_FIXES, float(100)*TAILLE_FIXES/total))
            print ("INDEXÉS        % 10d (%6.2f %%)"%(stats.index_total_size, float(100)*stats.index_total_size/total))
            print ("EN VRAC        % 10d (%6.2f %%)"%(stats.en_vrac_total_size, float(100)*stats.en_vrac_total_size/total))
            print ("SPÉCIFIQUES    % 10d (%6.2f %%)"%(tailleSpejcifique, float(100)*tailleSpejcifique/total))
            print ("IDENTIFICATION % 10d (%6.2f %%)"%(TAILLE_IDENTIFICATION, float(100)*TAILLE_IDENTIFICATION/total))
            print ("TOTAL          % 10d %08X"%(total, total))
            print ("taille fichier % 10d %08X"%(stats.file_size, stats.file_size))
        return True
    
#############################################################""
#ah partir de la carte des non-vides, trouve le nombre et la taille des vides
def chercheVides(nonVidesList):
    """Derive the "holes" (unused gaps) between a file's occupied byte ranges.

    French *chercheVides* = "looks for empties". Sorts ``nonVidesList`` by
    address and walks it, reporting any gap between consecutive occupied
    ranges as a "vide" (hole) - typically caused by entries that were
    deleted or replaced by a bigger definition without compacting the file.
    Used by every ``analyseFichierXxx``/statistics method to report on file
    fragmentation.

    :param nonVidesList: list of ``(address, length)`` occupied ranges
        (as produced by e.g. :meth:`NindPadFile.donneCarteNonVides`); sorted
        in place.
    :return: a ``(nbreVides, tailleVides, typesVides, typesNonVides)``
        tuple: the number and total size of holes found, and two
        ``{length: count}`` histograms (of hole sizes and of occupied-range
        sizes, respectively).
    :raises Exception: if two occupied ranges overlap.
    """
    #ordonne les indirections
    nonVidesList.sort()
    addressePrec = longueurPrec = 0
    nbreVides = tailleVides = 0
    typesVides = {}
    typesNonVides = {}
    for (addresse, longueur) in nonVidesList:
        if longueur not in typesNonVides: typesNonVides[longueur] = 0
        typesNonVides[longueur] +=1
        longueurVide = addresse - addressePrec - longueurPrec
        if longueurVide < 0: 
            raise Exception('%s : chevauchement %08X-%d et %08X-%d'%(self.latFileName, addressePrec, longueurPrec, addresse, longueur))
        if longueurVide > 0:
            #print 'addresse=%d, addressePrec=%d, longueurPrec=%d'%(addresse, addressePrec, longueurPrec)
            nbreVides +=1
            tailleVides += longueurVide
            if longueurVide not in typesVides: typesVides[longueurVide] = 0
            typesVides[longueurVide] +=1
        addressePrec = addresse
        longueurPrec = longueur 
    return nbreVides, tailleVides, typesVides, typesNonVides

#############################################################
#ah partir d'une liste calcule le max, le min, la moyenne et l'ejcart-type
def calculeRejpartition(nombres):
    """Compute basic descriptive statistics over a list of numbers.

    French *calculeRépartition* = "computes distribution". Small helper
    shared by the various ``analyseFichierXxx`` methods to summarize things
    like definition sizes or term frequencies.

    :param nombres: list of numbers (int or float).
    :return: a ``(count, min, max, sum, mean, stddev)`` tuple; all zeros if
        ``nombres`` is empty.
    """
    if len(nombres) == 0: return 0, 0, 0, 0, 0.0, 0.0
    somme = somme2 = 0
    min = max = nombres[0]
    for nombre in nombres :
        somme += nombre
        somme2 += nombre**2
        if min > nombre: min = nombre
        if max < nombre: max = nombre
    moyenne = float(somme) / len(nombres)
    variance = float(somme2) / len(nombres) - moyenne**2
    ecartType = math.sqrt(variance)
    return len(nombres), min, max, somme, moyenne, ecartType


if __name__ == '__main__':
        main()
    
        
