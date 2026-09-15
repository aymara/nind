#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Reverse lexicon: word identifier -> clear-text word(s).

A ``.nindretrolexicon`` file is the mirror image of
:mod:`~nind.NindLexiconindex` (which maps a word to its identifier): given
an identifier, :class:`NindRetrolexicon` returns the word it stands for,
including reconstructing compound words from their component identifiers.
It is a plain :class:`~nind.NindPadFile.NindPadFile` (not a
:class:`~nind.NindIndex.NindIndex`): entries here are either a UTF-8 word
directly, or a pointer pair to two other entries (see the grammar below).

Not written by :class:`~nind.nind_engine.NindIndexer` (``NindEngine`` only
supports simple, non-compound words and has no use for it) - this class
exists to read files produced by the C++ side or by legacy nind tooling.
"""
__author__ = "jys"
__copyright__ = "Copyright (C) 2017 LATEJCON"
__license__ = "GNU LGPL"
__version__ = "2.0.7"
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
from os import getenv, path
import codecs
import math
try:
    from .NindPadFile import NindPadFile
    from .NindPadFile import chercheVides
except ImportError:
    # run directly (not as part of the installed package): see docs/cli.md
    from NindPadFile import NindPadFile
    from NindPadFile import chercheVides
from nind import _native as native

NINDRETROLEXICON_EXT = '.nindretrolexicon'

def usage():
    if getenv("PY") != None: script = sys.argv[0].replace(getenv("PY"), '$PY')
    else: script = sys.argv[0]
    print ("""© l'ATEJCON.
o Analyse un fichier nindretrolexicon du système nind et donne les statistiques
o Peut dumper nindretrolexicon sur <fichier>-lexique.txt et donner les
  statistiques sur les longueurs de mots. Il s'agit du lexique en clair
o Peut donner le mot spécifié par son identifiant.
Le format du fichier est défini dans le document LAT2017.JYS.470.

usage   : %s <fichier> [ <analyse> | <lexique> | <mot> <ident> ]
exemple : %s FRE.nindretrolexicon
exemple : %s FRE.nindretrolexicon lexi
exemple : %s FRE.nindretrolexicon mot 19321
"""%(script, script, script, script))

def main():
    try:
        if len(sys.argv) < 2 : raise Exception()
        retrolexiconFileName = path.abspath(sys.argv[1])
        action = 'analyse' 
        if len(sys.argv) > 2 : action = sys.argv[2]
        ident = 0
        if len(sys.argv) > 3 : ident = int(sys.argv[3])
        
        #la classe
        nindRetrolexicon = NindRetrolexicon(retrolexiconFileName)
        if action.startswith('anal'): nindRetrolexicon.analyseFichierRetrolexicon(True)
        elif action.startswith('lexi'):
            outFilename = retrolexiconFileName + '-lexique.txt'
            outFile = codecs.open(outFilename, 'w', 'utf-8')
            (nbLignes, nbErreurs, rejpartition) = nindRetrolexicon.dumpeFichier(outFile)
            outFile.close()
            rejpartition.sort()
            somme = somme2 = nbMots = 0
            for (tailleMot, nombre) in rejpartition: 
                print ('% 3d : % 9d'%(tailleMot, nombre))
                somme += tailleMot * nombre
                somme2 += (tailleMot**2) * nombre
                nbMots += nombre
            moyenne = float(somme) / nbMots
            variance = float(somme2) / nbMots - moyenne**2
            ejcartType = math.sqrt(variance)
            print ("=============")
            print ("COMPOSÉS MAX   {:3d} composants".format(rejpartition[-1][0]))
            print ("COMPOSÉS MIN   {:3d} composant".format(rejpartition[0][0]))
            print ("MOYENNE        {:6.2f} composants".format(moyenne))
            print ("ÉCART-TYPE     {:6.2f}".format(ejcartType))
            print ("=============")
            print('      % 9d lignes en erreur'%(nbErreurs))
            print('        -------')
            print('      % 9d lignes écrites dans %s'%(nbLignes, path.basename(outFilename)))
        elif action.startswith('mot'): 
            print ('_'.join(nindRetrolexicon.donneMot(ident)))
        else: raise Exception()
    except Exception as exc:
        if len(exc.args) == 0: usage()
        else:
            print ("******************************")
            print (exc.args[0])
            print ("******************************")
            raise
        sys.exit()
        
#####################################################
# <donnejesIndexejes>     ::= <dejfinitionMot>
# <blocEnVrac>            ::= <blocUtf8>
#
# <dejfinitionMot>        ::= <motComposej> | <motSimple>
# <motComposej>           ::= <flagComposej=31> <identifiantA> <identifiantS>
# <flagComposej=31>       ::= <Entier1>
# <identifiantA>          ::= <Entier3>
# <identifiantS>          ::= <Entier3>
# <motSimple>             ::= <flagSimple=37> <longueurMotUtf8> <adresseMotUtf8>
# <flagSimple=37>         ::= <Entier1>
# <longueurMotUtf8>       ::= <Entier1>
# <adresseMotUtf8>        ::= <Entier5>
#
# <blocUtf8>              ::= { <Utf8> }
##############################
# <spejcifique>           ::= <vide>
#####################################################

FLAG_COMPOSEJ = 31
FLAG_SIMPLE = 37
TAILLE_COMPOSEJ_MAXIMUM = 100

class NindRetrolexicon(NindPadFile):
    """Read-only reverse lexicon: resolves a word identifier back to text.

    Each entry is either a "simple word" (a UTF-8 string stored directly in
    the file's "en vrac" area) or a "compound word" (a pair of identifiers
    ``identifiantA``/``identifiantS`` pointing at the word's two parts,
    themselves resolved recursively by :meth:`donneMot`).

    :meth:`donneMot` delegates to the ``nind._native`` bindings when opened
    with ``lexicon_identification``; otherwise (including when called from
    the diagnostics below, which don't have a lexicon on hand) it falls
    back to the original hand-rolled chain-walk, since ``dumpeFichier``/
    ``analyseFichierRetrolexicon`` themselves call :meth:`donneMot` and must
    keep working standalone.
    """

    def __init__(self, retrolexiconFileName, lexicon_identification=None):
        """Open ``retrolexiconFileName`` read-only and verify its structure.

        :param retrolexiconFileName: path to the ``.nindretrolexicon`` file.
        :param lexicon_identification: the ``nind._native.Identification``
            of the :class:`~nind.NindLexiconindex.NindLexiconindex` this
            retrolexicon was built against (see its ``donneIdentification``).
            Optional: enables the ``nind._native``-backed fast path in
            :meth:`donneMot`; omit it to use the pure-Python fallback.
        :raises Exception: if the file does not exist, or its pad-file
            envelope is invalid.
        """
        #si le lexique inverse n'existe pas, on ne fait rien
        if not path.isfile(retrolexiconFileName): raise Exception("%s n'existe pas"%(retrolexiconFileName))
        #on initialise la classe mehre en lecture uniquement
        NindPadFile.__init__(self, retrolexiconFileName)
        self.vejrifieFichier()
        self._native = None
        if lexicon_identification is not None:
            base = retrolexiconFileName
            if base.endswith(NINDRETROLEXICON_EXT): base = base[:-len(NINDRETROLEXICON_EXT)]
            self._native = native.NindRetrolexicon(base, is_writer=False,
                                                     lexicon_identification=lexicon_identification)

    def createFile(self):
        """No-op: this class is read-only and never creates a file.

        Kept as a stub because sibling writer classes (in C++, and
        conceptually here) expose a ``createFile``; there is no Python
        writer for this format (see the module docstring).
        """
        return

    def donneMot (self, ident):
        """Resolve a word identifier to its component simple word(s).

        French *donneMot* = "gives word". Follows the ``identifiantA`` /
        ``identifiantS`` chain for as long as the entry is a compound word,
        collecting each simple word it bottoms out on, in order.

        :param ident: the word identifier to resolve.
        :return: a list of simple-word strings (one element for a simple
            word, several for a compound word, in left-to-right order), or
            ``[]`` if ``ident`` is unknown.
        :raises Exception: if the file is inconsistent (a dangling
            identifier, a non-terminal entry where a terminal one was
            expected, or a self-referential loop).
        """
        if self._native is not None:
            components = self._native.get_components(ident)
            return components if components is not None else []
        motsSimples = []
        (trouvej, motSimple, identifiantA, identifiantS) = self.__donneDefMot(ident)
        #si pas trouvej, retourne chaisne vide
        if not trouvej: return []
        #tailleMot = 0
        while True:
            #print(mot)
            #tailleMot +=1
            #si c'est un mot simple, c'est la fin 
            if identifiantA == 0:
                motsSimples.insert(0, motSimple)
                break
            #si c'est un mot composej, recupehre le mot simple du couple
            (trouvej, motSimple, identifiantA2, identifiantS2) = self.__donneDefMot(identifiantS)
            if not trouvej: raise Exception("%d pas trouvé A dans %s"%(identifiantS, self.latFileName))
            if identifiantA2 != 0: raise Exception("%d pas terminal %s"%(identifiantS, self.latFileName))
            motsSimples.insert(0, motSimple)
            #recupere l'autre mot du couple
            (trouvej, motSimple, identifiantA, identifiantS) = self.__donneDefMot(identifiantA)
            if not trouvej: raise Exception("%d pas trouvé B dans %s"%(identifiantA, self.latFileName))
            #pour detecter les bouclages induits par un fichier bouclant
            if len(motsSimples) == TAILLE_COMPOSEJ_MAXIMUM: 
                raise Exception("%d bouclage dans %s"%(ident, self.latFileName))
        #retourne la liste de mots simples
        return (motsSimples)

    def __donneDefMot(self, ident):
        #trouve l'adresse des donnees dans le fichier
        offsetDejfinition = self.donnePositionEntreje(ident)
        #si pas trouve, le mot est inconnu
        if offsetDejfinition == 0: return (False, "", 0, 0)          #mot inconnu
        #lit la dejfinition
        self.seek(offsetDejfinition, 0)
        flag = self.litNombre1()
        if flag == 0: return (False, "", 0, 0)          #mot inconnu
        if flag == FLAG_SIMPLE:
            #<flagSimple=37> <longueurMotUtf8> <adresseMotUtf8>
            longueurMotUtf8 = self.litNombre1()
            adresseMotUtf8 = self.litNombre5()
            #lit la chaisne
            self.seek(adresseMotUtf8, 0)
            return (True, self.litChaine(longueurMotUtf8), 0, 0)
            #return (True, self.litOctets(longueurMotUtf8), 0, 0)
        if flag == FLAG_COMPOSEJ:
            #<flagComposej=31> <identifiantA> <identifiantS>
            identifiantA = self.litNombre4()
            identifiantS = self.litNombre4()
            return (True, '', identifiantA, identifiantS)
            #return (True, '', self.litNombre4(), self.litNombre4())
        raise Exception("%d mauvaise définition %s"%(ident, self.latFileName))
    
    ##################################################################
    #analyse complehtement le fichier et retourne True si ok
    def analyseFichierRetrolexicon(self, trace):
        """Validate the file and report simple/compound word statistics.

        French *analyseFichierRetrolexicon* = "analyzes retrolexicon file".
        Validates the pad-file envelope, then walks every identifier and
        tallies how many are simple vs. compound words, plus a byte-level
        breakdown (UTF-8 word bytes vs. holes) via
        :func:`~nind.NindPadFile.chercheVides`.

        :param trace: if ``True``, print a human-readable report to stdout.
        :raises Exception: if the file is structurally invalid (also
            reported via ``trace`` before being re-raised).
        """
        cestbon = self.analyseFichierPadFile(trace)
        try:
            totalUtf8 = 0
            nbreSimples = nbreComposejs = 0
            #met en place la carte des non-vides
            maxIdent, nonVidesList = self.donneCarteNonVides()
            for identifiant in range(maxIdent):
                position = self.donnePositionEntreje(identifiant)
                if position == 0: 
                    raise Exception('%s : identifiant %d sans indirection '%(self.latFileName, identifiant))
                self.seek(position, 0)
                flag = self.litNombre1()
                if flag == 0: continue
                if flag == FLAG_SIMPLE:
                    #<flagSimple=37> <longueurMotUtf8> <adresseMotUtf8>
                    longueurMotUtf8 = self.litNombre1()
                    adresseMotUtf8 = self.litNombre5()
                    nonVidesList.append((adresseMotUtf8, longueurMotUtf8))
                    totalUtf8 += longueurMotUtf8
                    nbreSimples += 1
                elif flag == FLAG_COMPOSEJ:
                    #<flagComposej=31> <identifiantA> <identifiantS>
                    identifiantA = self.litNombre4()
                    identifiantS = self.litNombre4()
                    nbreComposejs += 1
        except Exception as exc: 
            cestBon = False
            if trace: print ('ERREUR :', exc.args[0])
            raise
        if trace:
            print ("=============")
            print ("%d / %d index utilisés"%(nbreSimples + nbreComposejs, maxIdent))
            print ("=============")
            
        try:
            (nbreVides, tailleVides, typesVides, typesNonVides) = chercheVides(nonVidesList)
            total = tailleVides + totalUtf8
            if trace:
                print ("CHAÎNES UTF-8  % 10d (%6.2f %%) % 9d occurrences"%(totalUtf8, float(100)*totalUtf8/total, nbreSimples))
                print ("VIDES          % 10d (%6.2f %%) % 9d occurrences"%(tailleVides, float(100)*tailleVides/total, nbreVides))
                print ("TOTAL          % 10d %08X"%(total, total))
                print ("=============")
                total = nbreSimples + nbreComposejs
                print ("MOTS SIMPLES   % 10d (%6.2f %%)"%(nbreSimples, float(100)*nbreSimples/total))
                print ("MOTS COMPOSÉS  % 10d (%6.2f %%)"%(nbreComposejs, float(100)*nbreComposejs/total))
                print ("TOTAL          % 10d"%(total))
                print ("=============")
                print ("%0.2f octets / mot"%(float(self.donneTailleFichier())/total))
                print ("=============")
        except Exception as exc: 
            cestBon = False
            if trace: print ('ERREUR :', exc.args[0])
            raise

    #######################################################################
    #dumpe le fichier lexique sur un fichier texte
    def dumpeFichier(self, outFile):
        """Dump every resolvable word to a text file, one ``id  word`` line each.

        French *dumpeFichier* = "dumps file". Compound words are written
        with their components joined by ``_``. Used by the
        ``Nind_checkRejtroAndLexicon.py``-style diagnostics to produce a
        human-readable lexicon.

        :param outFile: a writable text file object (UTF-8).
        :return: a ``(nbLignes, nbErreurs, rejpartition)`` tuple: number of
            lines written, number of those that hit an error, and a list of
            ``(wordLengthInComponents, count)`` pairs describing the
            distribution of compound-word lengths.
        """
        nbLignes = nbErreurs = 0
        rejpartition = {}
        #trouve le max des identifiants
        maxIdent = self.donneMaxIdentifiant()
        for index in range(maxIdent):
            try:
                motsSimples = self.donneMot(index)
                tailleMot = len(motsSimples)
                if tailleMot == 0: continue
                mot = '_'.join(motsSimples)
                outFile.write('%06d  %s\n'%(index, mot))
                if tailleMot not in rejpartition: rejpartition[tailleMot] = 0
                rejpartition[tailleMot] +=1
            except Exception as exc: 
                outFile.write('%06d  *******ERREUR: %s\n'%(index, exc.args[0]))
                nbErreurs +=1
                raise
            nbLignes +=1
        return (nbLignes, nbErreurs, list(rejpartition.items()))
    #######################################################################


if __name__ == '__main__':
    main()
