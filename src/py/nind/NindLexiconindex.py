#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Lexicon-as-index: word (text) -> identifier, hash-bucketed.

A ``.nindlexiconindex`` file is a :class:`~nind.NindIndex.NindIndex` whose
identifiers are hash buckets (``clefB(mot) % nombreIndirection``, see
:func:`~nind.NindFile.clefB`) rather than direct word ids: each bucket's
definition packs every word that hashed into it, alongside its identifier
and, for compound words, the identifiers of its components. This is the
format :class:`~nind.nind_engine.NindIndexer` writes as ``.nindlexiconindex``
and :class:`~nind.nind_engine.NindEngine` reads to turn a query term into
the identifier used to look it up in :class:`~nind.NindTermindex.NindTermindex`.
"""
__author__ = "jys"
__copyright__ = "Copyright (C) 2017 LATEJCON"
__license__ = "GNU LGPL"
__version__ = "2.1.0"
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
try:
    from . import NindFile
    from .NindIndex import NindIndex
    from .NindPadFile import calculeRejpartition
except ImportError:
    # run directly (not as part of the installed package): see docs/cli.md
    import NindFile
    from NindIndex import NindIndex
    from NindPadFile import calculeRejpartition
from nind import _native as native

NINDLEXICONINDEX_EXT = '.nindlexiconindex'

def usage():
    if getenv("PY") != None: script = sys.argv[0].replace(getenv("PY"), '$PY')
    else: script = sys.argv[0]
    print ("""© l'ATEJCON.
o Analyse un fichier nindlexiconindex du système nind et donne les statistiques
o Peut dumper nindlexiconindex sur <fichier>-dump.txt
  Il s'agit de la structure interne du lexique
o Peut donner les identifiants de mots composés avec le maximum de composants
  et les clefs (les index) avec le maximum de collisions de haschage.
o Peut donner l'identifiant du mot spécifié
o Peut donner la clef du mot simple spécifié
o Peut donner les mots simples qui "collisionnent" sur la même indirection
Le format du fichier est défini dans le document LAT2017.JYS.470.

usage   : %s <fichier> [ <analyse> | <dumpe> | 
          <debogue> <index> | <max> | <ident> <mot> | <clef> <mot> | 
          <collision> <index> ]
exemple : %s FRE.nindlexiconindex
exemple : %s FRE.nindlexiconindex dump
exemple : %s FRE.nindlexiconindex max
exemple : %s FRE.nindlexiconindex id "épistémologie_compulsive"
exemple : %s FRE.nindlexiconindex cle "compulsive"
exemple : %s FRE.nindlexiconindex col 1268512
"""%(script, script, script, script, script, script, script))

def main():
    try:
        if len(sys.argv) < 2 : raise Exception()
        lexiconindexFileName = path.abspath(sys.argv[1])
        action = 'analyse' 
        if len(sys.argv) > 2 : action = sys.argv[2]
        mot = ''
        if len(sys.argv) > 3 : mot = sys.argv[3]
        
        #la classe
        nindLexiconindex = NindLexiconindex(lexiconindexFileName)
        if action.startswith('anal'): nindLexiconindex.analyseFichierLexiconindex(True)
        elif action.startswith('dump'):
            outFilename = lexiconindexFileName + '-dump.txt'
            outFile = codecs.open(outFilename, 'w', 'utf-8')
            nbLignes, nbErreurs = nindLexiconindex.dumpeFichier(outFile)
            outFile.close()
            print ('%d lignes (dont %d en erreur) écrites dans %s'%(nbLignes, nbErreurs, outFilename))
        elif action.startswith('deb'):
            nindLexiconindex.debogueIndex(int(mot))
        elif action.startswith('max'):
            composejs, collisions = nindLexiconindex.donneMax(5)
            print ("   ident     index  nombre")
            for (nbreComposejs, identifiantS, index) in composejs:
                print ('%8d  %8d  (%d)'%(identifiantS, index, nbreComposejs))
            for (nbreCollisions, index) in collisions:
                print ("          %8d : %d"  %(index, nbreCollisions))
        elif action.startswith('id'):
            motsSimples = mot.split('_')
            print ('identifiant : ', nindLexiconindex.donneIdentifiant(motsSimples))
            print ('clef        : ', nindLexiconindex.donneClef(motsSimples[-1]))
        elif action.startswith('cle'):
            print ('clef        : ', nindLexiconindex.donneClef(mot))
        elif action.startswith('col'):
            collisions = nindLexiconindex.donneCollisions(int(mot))
            for (motSimple, identifiantS, nbreComposes) in collisions:
                print ('%8d %s (%d)'%(identifiantS, motSimple, nbreComposes))
        else: raise Exception()
    except Exception as exc:
        if len(exc.args) == 0: usage()
        else:
            print ("******************************")
            print (exc.args[0])
            print ("******************************")
            raise
        sys.exit()

############################################################
# <dejfinition>           ::= <flagDejfinition=13> <identifiantHash> <longueurDonnejes> <donnejesHash>
# <flagDejfinition=13>    ::= <Entier1>
# <identifiantHash>       ::= <Entier3>
# <longueurDonnejes>      ::= <Entier3>
# <donnejesHash>          ::= { <mot> }
# <mot>                   ::= <motSimple> <identifiantS> <nbreComposejs> <composejs>
# <motSimple>             ::= <MotUtf8>
# <identifiantS>          ::= <Entier3>
# <nbreComposejs>         ::= <EntierULat>
# <composejs>             ::= { <composej> } 
# <composej>              ::= <identifiantA> <identifiantRelC>
# <identifiantA>          ::= <Entier3>
# <identifiantRelC>       ::= <EntierSLat>
##############################
# <spejcifique>           ::= <vide>
############################################################

FLAG_DEJFINITION = 13
#<flagDejfinition=13>(1) <identifiantHash>(3) <longueurDonnejes>(3) = 7
TAILLE_TESTE_DEJFINITION = 7

class NindLexiconindex(NindIndex):
    """Read-only word-to-identifier lexicon, hash-bucketed by :func:`~nind.NindFile.clefB`.

    :meth:`donneIdentifiant` and :meth:`donneIdentification` delegate to the
    ``nind._native`` bindings; the rest (``analyseFichierLexiconindex``,
    ``dumpeFichier``, ``debogueIndex``, ``donneCollisions``, ``donneMax``,
    ``donneClef``) are diagnostics used by ``Nind_*`` command-line tools to
    inspect hash-bucket distribution and collisions, and remain hand-rolled
    pure-Python parsing since ``nind._native`` doesn't expose that
    introspection.
    """

    def __init__(self, lexiconindexFileName):
        """Open ``lexiconindexFileName`` read-only.

        :param lexiconindexFileName: path to the ``.nindlexiconindex`` file.
        :raises Exception: if the file's pad-file envelope is invalid.
        """
        NindIndex.__init__(self, lexiconindexFileName)
        #trouve le modulo = nombreIndirection
        self.nombreIndirection = self.donneMaxIdentifiant()
        base = lexiconindexFileName
        if base.endswith(NINDLEXICONINDEX_EXT): base = base[:-len(NINDLEXICONINDEX_EXT)]
        self._native = native.NindLexiconIndex(base, is_writer=False)

    #trouve les donnejes 
    def __donneDonnejes(self, identifiant):
        #lit la définition du mot
        (offsetDejfinition, longueurDejfinition) = self.donneAdresseDejfinition(identifiant)
        if offsetDejfinition == 0: return False, 0, 0      #identifiant pas trouve
        self.seek(offsetDejfinition, 0)
        #<flagDejfinition=13> <identifiantHash> <longueurDonnejes> 
        if self.litNombre1() != FLAG_DEJFINITION: 
            raise Exception('%s : pas FLAG_DEJFINITION à %08X'%(self.latFileName, offsetDejfinition))
        if self.litNombre3() != identifiant: 
            raise Exception('%s : %d pas trouvé à %08X'%(self.latFileName, index, offsetDejfinition+1))
        longueurDonnejes = self.litNombre3()
        tailleExtension = longueurDejfinition - longueurDonnejes - TAILLE_TESTE_DEJFINITION
        if tailleExtension < 0:
            raise Exception('%s : %d incohérent à %08X'%(self.latFileName, identifiant, offsetDejfinition+5))
        return True, longueurDonnejes, tailleExtension
        
    #trouve l'identifiant du mot fourni sous forme d'une liste de mots simples
    def donneIdentifiant(self, motsSimples):
        """Look up the identifier of a word, given as a list of simple words.

        French *donneIdentifiant* = "gives identifier". A single-element
        list looks up a simple word; a multi-element list looks up the
        compound word formed by those simple words in order.

        :param motsSimples: list of one or more simple-word strings, e.g.
            ``["compulsive"]`` or ``["épistémologie", "compulsive"]``.
        :return: the word's identifier, or ``0`` if it is not in the
            lexicon.
        """
        return self._native.get_word_id(motsSimples)

    def donneIdentification(self):
        """Return this lexicon's :class:`nind._native.Identification`.

        Needed to open the corresponding ``.nindtermindex``/
        ``.nindlocalindex``/``.nindretrolexicon`` files, which cross-check
        against it.
        """
        return self._native.get_identification()

    #######################################################################"
    #analyse du fichier
    def analyseFichierLexiconindex(self, trace):
        """Validate the file and report simple/compound word statistics.

        French *analyseFichierLexiconindex* = "analyzes lexicon-index
        file". Extends :meth:`~nind.NindIndex.NindIndex.analyseFichierIndex`
        with a breakdown of simple vs. compound words per bucket and the
        distribution of compound-word component counts.

        :param trace: if ``True``, print a human-readable report to stdout.
        :return: ``False`` if the underlying index is invalid; otherwise
            prints the report when ``trace`` is set (no explicit return
            value on the success path).
        """
        cestbon = self.analyseFichierIndex(trace)
        if not cestbon: return False
        if trace: print ("======LEXICON=======")
        try:
            #trouve le max des identifiants
            maxIdent = self.donneMaxIdentifiant()
            totalDonnejes = totalExtensions = 0
            nbDonnejes = nbExtensions = 0
            composejs = []
            for index in range(maxIdent):
                try:
                    #trouve les donnejes 
                    trouvej, longueurDonnejes, tailleExtension = self.__donneDonnejes(index)
                    if not trouvej: continue      #index pas trouve
                    nbDonnejes +=1
                    totalDonnejes += longueurDonnejes + TAILLE_TESTE_DEJFINITION
                    if tailleExtension > 0: nbExtensions += 1
                    totalExtensions += tailleExtension
                    #examine les données
                    finDonnejes = self.tell() + longueurDonnejes
                    while self.tell() < finDonnejes:
                        #<motSimple> <identifiantS> <nbreComposejs> <composejs>
                        motSimple = self.litString()
                        #longueur = self.litNombre1()
                        #motSimple = self.litOctets(longueur)
                        identifiantS = self.litNombre4()
                        nbreComposejs = self.litNombreULat()
                        composejs.append(nbreComposejs)
                        identifiantC = identifiantS
                        for i in range(nbreComposejs):
                            #<identifiantA> <identifiantRelC>
                            identifiantA = self.litNombre4()
                            identifiantC += self.litNombreSLat()
                except:
                    if trace: print ('*******ERREUR SUR INDEX :', index)
                    raise
            total = totalDonnejes + totalExtensions
            if trace:
                nbreMotsS, composejsMin, composejsMax, nbreMotsC, moyenne, ejcartType = calculeRejpartition(composejs)
                print ("DONNÉES        % 10d (%6.2f %%) % 9d occurrences"%(totalDonnejes, float(100)*totalDonnejes/total, nbDonnejes))
                print ("EXTENSIONS     % 10d (%6.2f %%) % 9d occurrences"%(totalExtensions, float(100)*totalExtensions/total, nbExtensions))
                print ("TOTAL          % 10d %08X"%(total, total))
                print ("=============")
                total = nbreMotsS + nbreMotsC
                print ("MOTS SIMPLES   % 10d (%6.2f %%)"%(nbreMotsS, float(100)*nbreMotsS/total))
                print ("MOTS COMPOSÉS  % 10d (%6.2f %%)"%(nbreMotsC, float(100)*nbreMotsC/total))
                print ("TOTAL          % 10d"%(total))
                print ("=============")
                print ("COMPOSÉS MAX   % 10d composés finissant par le même mot simple"%(composejsMax))
                print ("COMPOSÉS MIN   % 10d composés finissant par le même mot simple"%(composejsMin))
                print ("MOYENNE        % 10d composés finissant par le même mot simple"%(moyenne))
                print ("ÉCART-TYPE     % 10d"%(ejcartType))
                print ("=============")
                print ("%0.2f octets / mot"%(float(self.donneTailleFichier())/(nbreMotsS+nbreMotsC)))
                print ("=============")
               
        except Exception as exc: 
            cestBon = False
            if trace: print ('*******ERREUR :', exc.args[0])
            raise

        try:
            #rejcupehre l'adresse et la longueur des spejcifiques 
            (offsetSpejcifiques, tailleSpejcifiques) = self.donneSpejcifiques()
            self.seek(offsetSpejcifiques, 0)
            spejcifiques = []
            for i in range(tailleSpejcifiques): spejcifiques.append(self.litNombre1())
            if trace:
                print ("%d mots de données spécifiques"%(tailleSpejcifiques/4))
                print (', '.join(spejcifiques))
        except Exception as exc: 
            cestBon = False
            if trace: print ('ERREUR :', exc.args[0])
            raise
   
    #######################################################################
    #dumpe le fichier lexique sur un fichier texte
    def dumpeFichier(self, outFile):
        """Dump every bucket's entries to a text file, for inspection.

        French *dumpeFichier* = "dumps file". One line per word: its
        identifier, the word itself, and (for compound words) its
        component identifier pairs.

        :param outFile: a writable text file object (UTF-8).
        :return: a ``(nbLignes, nbErreurs)`` tuple: number of lines
            written, and number of those that hit a decoding error.
        """
        nbLignes = nbErreurs = 0
        #trouve le max des identifiants
        maxIdent = self.donneMaxIdentifiant()
        for index in range(maxIdent):
            try:
                #trouve les donnejes
                trouvej, longueurDonnejes, tailleExtension = self.__donneDonnejes(index)
                if not trouvej: continue      #index pas trouve
                #examine les données
                finDonnejes = self.tell() + longueurDonnejes
                while self.tell() < finDonnejes:
                    #<motSimple> <identifiantS> <nbreComposes> <composes>
                    motSimple = self.litString()
                    #longueur = self.litNombre1()
                    #motSimple = self.litOctets(longueur)
                    identifiantS = self.litNombre4()
                    nbreComposes = self.litNombreULat()
                    outFile.write('%06d [%s] (%d) '%(identifiantS, motSimple, nbreComposes))
                    identifiantC = identifiantS
                    composes = []
                    for i in range(nbreComposes):
                        #<identifiantA> <identifiantRelC>
                        identifiantA = self.litNombre4()
                        identifiantC += self.litNombreSLat()
                        composes.append('%06d %06d'%(identifiantA, identifiantC))
                    outFile.write(' <%s>\n'%(', '.join(composes)))
            except Exception as exc: 
                outFile.write('*******ERREUR: %s, INDEX: %d\n'%(exc.args[0], index))
                nbErreurs +=1
            nbLignes +=1
        return nbLignes, nbErreurs

    #######################################################################
    #parcourt le fichier en mode debogue pour trouver une dejfinition
    def debogueIndex(self, index):
        """Print a step-by-step trace of decoding one bucket's definition.

        French *debogueIndex* = "debugs index". Diagnostic helper for the
        ``Nind_checkRejtroAndLexicon.py`` CLI: prints every field as it is
        read, to help pinpoint where a corrupted bucket goes wrong.

        :param index: the bucket (hash) index to inspect.
        """
        print('index=', index)
        (offsetDejfinition, longueurDejfinition) = self.donneAdresseDejfinition(index)
        print('offsetDejfinition=', offsetDejfinition)
        print('longueurDejfinition=', longueurDejfinition)
        if offsetDejfinition == 0: 
            print('index pas trouvé')
            return
        self.seek(offsetDejfinition, 0)
        print('<flagDejfinition=13> <identifiantHash> <longueurDonnejes>')
        flagDejfinition = self.litNombre1()
        print('flagDejfinition=', flagDejfinition, '  (FLAG_DEJFINITION=', FLAG_DEJFINITION, ')')
        identifiantHash = self.litNombre3()
        print('identifiantHash=', identifiantHash)
        longueurDonnejes = self.litNombre3()
        print('longueurDonnejes=', longueurDonnejes)
        tailleExtension = longueurDejfinition - longueurDonnejes - TAILLE_TESTE_DEJFINITION
        print('tailleExtension=', tailleExtension, '  (TAILLE_TESTE_DEJFINITION=', TAILLE_TESTE_DEJFINITION, ')')
        if flagDejfinition != FLAG_DEJFINITION: 
            print('flagDejfinition != ',FLAG_DEJFINITION)
            return
        if identifiantHash != index: 
            print('identifiantHash != ',index)
            return
        if tailleExtension < 0:
            print('tailleExtension < 0')
            return
        #examine les données
        finDonnejes = self.tell() + longueurDonnejes
        print('tell()=', self.tell())
        print('finDonnejes=', finDonnejes)
        mem = self.tell()
        donnejes = self.litOctets(longueurDonnejes)
        print('donnejes=', donnejes)
        self.seek(mem, 0)
        while self.tell() < finDonnejes:
            print('<motSimple> <identifiantS> <nbreComposejs> <composejs>')
            longueur = self.litNombre1()
            motSimple = self.litOctets(longueur)
            print('motSimple=', motSimple)
            identifiantS = self.litNombre4()
            print('identifiantS=', identifiantS)
            nbreComposes = self.litNombreULat()
            print('nbreComposes=', nbreComposes)
            identifiantC = identifiantS
            #composes = []
            for i in range(nbreComposes):
                identifiantA = self.litNombre4()
                identifiantC += self.litNombreSLat()
                #composes.append('%06d %06d'%(identifiantA, identifiantC))
                
    #######################################################################
    #donne les mots simples enregistrejs sur l'indirection spejcifieje et le nombre de composants
    def donneCollisions(self, index):
        """List every word hash-bucketed onto ``index`` (a "hash collision" report).

        French *donneCollisions* = "gives collisions".

        :param index: the bucket (hash) index to inspect.
        :return: a list of ``(motSimple, identifiantS, nbreComposes)``
            tuples, one per word sharing that bucket.
        """
        rejsultat = []
        #trouve les donnejes 
        trouvej, longueurDonnejes, tailleExtension = self.__donneDonnejes(index)
        if not trouvej: return rejsultat      #index pas trouve
        #examine les données
        finDonnejes = self.tell() + longueurDonnejes
        while self.tell() < finDonnejes:
            #<motSimple> <identifiantS> <nbreComposes> <composes>
            motSimple = self.litString()
            identifiantS = self.litNombre4()
            nbreComposes = self.litNombreULat()
            rejsultat.append((motSimple, identifiantS, nbreComposes))
            for i in range(nbreComposes):
                #<identifiantA> <identifiantRelC>
                self.litNombre4()
                self.litNombreSLat()
        return rejsultat

    #######################################################################
    #donne les identifiants des mots composejs qui ont le plus de composants
    def donneMax(self, taille):
        """Find the words with the most components and the buckets with the most collisions.

        French *donneMax* = "gives max[ima]". Scans the whole file to find
        the top ``taille`` compound words by component count, and the top
        ``taille`` buckets by number of colliding words.

        :param taille: how many top entries to keep for each ranking.
        :return: a ``(composejs, collisions)`` tuple: ``composejs`` is a
            list of ``(nbreComposejs, identifiantS, index)`` tuples sorted
            by descending component count; ``collisions`` is a list of
            ``(nbreCollisions, index)`` tuples sorted by descending
            collision count.
        """
        #trouve le max des identifiants
        maxIdent = self.donneMaxIdentifiant()
        composejs = []
        collisions = []
        for index in range(maxIdent):
            #trouve les donnejes 
            trouvej, longueurDonnejes, tailleExtension = self.__donneDonnejes(index)
            if not trouvej: continue      #index pas trouve
            #examine les données
            finDonnejes = self.tell() + longueurDonnejes
            nbreCollisions = 0
            while self.tell() < finDonnejes:
                nbreCollisions +=1
                #<motSimple> <identifiantS> <nbreComposejs> <composejs>
                motSimple = self.litString()
                identifiantS = self.litNombre4()
                nbreComposejs = self.litNombreULat()
                composejs.append((nbreComposejs, identifiantS, index))
                composejs.sort()
                composejs.reverse()
                if len(composejs) > taille: composejs.pop()
                for i in range(nbreComposejs):
                    #<identifiantA> <identifiantRelC>
                    self.litNombre4()
                    self.litNombreSLat()
            collisions.append((nbreCollisions, index))
            collisions.sort()
            collisions.reverse()
            if len(collisions) > taille: collisions.pop()
        return composejs, collisions
    
    #######################################################################
    #donne la clef d'accehs (pour deboguer)
    def donneClef(self, mot):
        """Return the bucket index a given simple word hashes to.

        French *donneClef* = "gives key". Debugging helper: same
        computation :meth:`donneIdentifiant` performs internally, exposed
        so a caller can e.g. inspect a specific bucket with
        :meth:`donneCollisions` or :meth:`debogueIndex`.

        :param mot: the simple word to hash.
        :return: its bucket index (``clefB(mot) % nombreIndirection``).
        """
        clefB = NindFile.clefB(mot)
        return clefB % self.nombreIndirection
        
    #######################################################################
        
if __name__ == '__main__':
    main()
