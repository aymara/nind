#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""The per-document local index: document identifier -> term occurrences and positions.

A ``.nindlocalindex`` file is a :class:`~nind.NindIndex.NindIndex` keyed by
*internal* document id (a dense ``1..nombreDocuments`` sequence), plus an
external <-> internal id translation table built at open time from each
document's stored ``identifiantExterne``. Each document's definition lists
every term that occurs in it (delta-encoded relative term ids) together
with the positions ("localisations") it occurs at - the data
:class:`~nind.nind_engine.NindEngine` uses for term-frequency and
document-length calculations.
"""
__author__ = "jys"
__copyright__ = "Copyright (C) 2017 LATEJCON"
__license__ = "GNU LGPL"
__version__ = "2.1.1"
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
from io import StringIO
import codecs
try:
    from .NindPadFile import calculeRejpartition
    from .NindIndex import NindIndex
except ImportError:
    # run directly (not as part of the installed package): see docs/cli.md
    from NindPadFile import calculeRejpartition
    from NindIndex import NindIndex
try:
    from nind import _native as native
except ImportError:
    # nind._native is a compiled extension: not available when introspecting
    # the pure-Python source without building it (e.g. Sphinx autodoc, see
    # docs/conf.py). Constructing NindLocalindex with lexicon_identification
    # still requires it - only module import is tolerant.
    native = None

NINDLOCALINDEX_EXT = '.nindlocalindex'

def usage():
    if getenv("PY") != None: script = sys.argv[0].replace(getenv("PY"), '$PY')
    else: script = sys.argv[0]
    print (f"""© l'ATEJCON.
o Analyse un fichier nindlocalindex du système nind et affiche les statistiques 
o Peut donner l'identifiant interne et externe du dernier document indexé 
o Peut dumper nindlocalindex sur <fichier>-dump.txt
o Peut donner la liste des identifiants externes de tous les documents indexés
o Peut afficher les données correspondant à un document spécifié par son 
  identifiant externe
Le format du fichier est défini dans le document LAT2017.JYS.470.

usage   : {script} <fichier> [ <analyse> | <dernier> | <dumpe> | <ident> | <affiche> <ident> ]
exemple : {script} FRE.nindlocalindex
exemple : {script} FRE.nindlocalindex dern
exemple : {script} FRE.nindlocalindex dump
exemple : {script} FRE.nindlocalindex iden
exemple : {script} FRE.nindlocalindex affi 3456
""")


def main():
    try:
        if len(sys.argv) < 2 : raise Exception()
        localindexFileName = path.abspath(sys.argv[1])
        action = 'analyse' 
        if len(sys.argv) > 2 : action = sys.argv[2]
        identExterne = 0
        if len(sys.argv) > 3 : identExterne = int(sys.argv[3])
        
        #la classe
        nindLocalindex = NindLocalindex(localindexFileName)
        if action.startswith('anal'): nindLocalindex.analyseFichierLocalindex(True)
        elif action.startswith('der'):
            (noInterne, noExterne) = nindLocalindex.donneMaxIdentifiants()
            print (f'dernier document indexé : n° interne {noInterne}, n° externe : {noExterne}')
        elif action.startswith('dump'):
            outFilename = localindexFileName + '-dump.txt'
            outFile = codecs.open(outFilename, 'w', 'utf-8')
            nbLignes = nindLocalindex.dumpeFichier(outFile)
            outFile.close()
            print (f'{nbLignes} lignes écrites dans {outFilename}')
        elif action.startswith('id'):
            listeIdentifiants = nindLocalindex.donneidentifiantsExternes()
            listeIdentifiants.sort()
            #les affiche, un par ligne
            for (externe, interne) in listeIdentifiants: print (externe, ' <-> ', interne)
        elif action.startswith('affi'):
            print (nindLocalindex.afficheDocument(identExterne))
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
# <dejfinition>           ::= <flagDejfinition=19> <identifiantDoc> <identifiantExterne> <longueurDonnejes> <donnejesDoc>
# <flagDejfinition=19>    ::= <Entier1>
# <identifiantDoc>        ::= <Entier3>
# <identifiantExterne>    ::= <Entier4>
# <longueurDonnejes>      ::= <Entier3>
# <donnejesDoc>           ::= { <donnejesTerme> }
# <donnejesTerme>         ::= <identTermeRelatif> <catejgorie> <nbreLocalisations> <localisations>
# <identTermeRelatif>     ::= <EntierSLat>
# <catejgorie>            ::= <Entier1>
# <nbreLocalisations>     ::= <Entier1>
# <localisations>         ::= { <localisationRelatif> <longueur> }
# <localisationRelatif>   ::= <EntierSLat>
# <longueur>              ::= <Entier1>
##############################
# <spejcifique>           ::= <maxIdentifiantInterne> <nombreDocuments>
# <maxIdentifiantInterne> ::= <Entier4>
# <nombreDocuments>       ::= <Entier4>
############################################################

FLAG_DEJFINITION = 19
#<maxIdentifiantInterne>(4) <nombreDocuments>(4) = 8
TAILLE_SPEJCIFIQUES = 8
#<flagDejfinition=19>(1) <identifiantDoc>(3) <identifiantExterne>(4) <longueurDonnejes>(3) = 11
TAILLE_TESTE_DEJFINITION = 11

class NindLocalindex(NindIndex):
    """Read-only per-document local index: term occurrences and positions, keyed by document id.

    Documents are addressed by their *external* id everywhere in the
    public API (:meth:`donneListeTermes`, :meth:`afficheDocument`); the
    class transparently translates to/from the internal id used inside the
    file, via a table built once at open time. :meth:`donneListeTermes`
    delegates to the ``nind._native`` bindings and requires
    ``lexicon_identification``; the diagnostic methods
    (``analyseFichierLocalindex``, ``dumpeFichier``, ``afficheDocument``)
    work without it, remaining hand-rolled pure-Python parsing since
    ``nind._native`` doesn't expose that introspection.
    """

    def __init__(self, localindexFileName, lexicon_identification=None):
        """Open ``localindexFileName`` read-only and build the external<->internal id table.

        :param localindexFileName: path to the ``.nindlocalindex`` file.
        :param lexicon_identification: the ``nind._native.Identification``
            of the :class:`~nind.NindLexiconindex.NindLexiconindex` this
            local index was built against (see its ``donneIdentification``).
            Required only for :meth:`donneListeTermes`; the diagnostic
            methods work without it.
        :raises Exception: if the file's pad-file envelope is invalid, or
            its specifics block has the wrong size.
        """
        NindIndex.__init__(self, localindexFileName)
        self._native = None
        if lexicon_identification is not None:
            if native is None:
                raise ImportError('nind._native compiled extension is not available (build it via "uv sync")')
            base = localindexFileName
            if base.endswith(NINDLOCALINDEX_EXT): base = base[:-len(NINDLOCALINDEX_EXT)]
            self._native = native.NindLocalIndex(base, is_writer=False,
                                                  lexicon_identification=lexicon_identification)
        #rejcupehre l'adresse et la longueur des spejcifiques
        (offsetSpejcifiques, tailleSpejcifiques) = self.donneSpejcifiques()
        if tailleSpejcifiques != TAILLE_SPEJCIFIQUES: 
            raise Exception('%s : taille incompatible des spécifiques'%(self.latFileName))
        self.seek(offsetSpejcifiques, 0)
        #<maxIdentifiantInterne> <nombreDocuments>
        self.maxIdentifiantInterne = self.litNombre4()
        nombreDocuments = self.litNombre4()
        #initialisation traduction identitiant externe -> identifiant interne
        self.docIdTradExtInt = {}
        for noDocInterne in range(1, self.maxIdentifiantInterne +1):
            (offsetDejfinition, longueurDejfinition) = self.donneAdresseDejfinition(noDocInterne)
            if offsetDejfinition == 0: continue
            self.seek(offsetDejfinition, 0)
            #<flagDejfinition=19> <identifiantDoc> <identifiantExterne> 
            if self.litNombre1() != FLAG_DEJFINITION: 
                raise Exception('%s : pas FLAG_DEJFINITION à %08X'%(self.latFileName, offsetDejfinition))
            if self.litNombre3() != noDocInterne: 
                raise Exception('%s : %d pas trouvé à %08X'%(self.latFileName, ident, offsetDejfinition+1))
            noDocExterne = self.litNombre4()
            self.docIdTradExtInt[noDocExterne] = noDocInterne       
  
    #######################################################################
    #trouve les donnejes 
    def __donneDonnejes(self, identifiant):
        #lit la définition du mot
        (offsetDejfinition, longueurDejfinition) = self.donneAdresseDejfinition(identifiant)
        if offsetDejfinition == 0: return False, 0, 0, 0      #identifiant pas trouve
        self.seek(offsetDejfinition, 0)
        #<flagDejfinition=19> <identifiantDoc> <identifiantExterne> <longueurDonnejes> 
        if self.litNombre1() != FLAG_DEJFINITION: 
            raise Exception('%s : pas FLAG_DEJFINITION à %08X'%(self.latFileName, offsetDejfinition))
        if self.litNombre3() != identifiant: 
            raise Exception('%s : %d pas trouvé à %08X'%(self.latFileName, index, offsetDejfinition+1))
        identifiantExterne = self.litNombre4()
        longueurDonnejes = self.litNombre3()
        tailleExtension = longueurDejfinition - longueurDonnejes - TAILLE_TESTE_DEJFINITION
        if tailleExtension < 0:
            raise Exception('%s : %d incohérent à %08X'%(self.latFileName, identifiant, offsetDejfinition+5))
        return True, longueurDonnejes, tailleExtension, identifiantExterne

    #######################################################################
    #retourne la structure dejcrivant les localisations de termes pour le document spejcifiej
    def donneListeTermes(self, noDocExterne):
        """Return every term occurrence and its positions for a given document.

        French *donneListeTermes* = "gives term list".

        :param noDocExterne: the document's external identifier.
        :return: a list of ``nind._native.Term`` (``.term``, ``.cg``,
            ``.localisation``: list of ``Localisation`` with
            ``.position``/``.length``), one per term occurring in the
            document. Empty list if ``noDocExterne`` is unknown.
        :raises Exception: if opened without ``lexicon_identification``.
        """
        if self._native is None:
            raise Exception('%s : donneListeTermes nécessite lexicon_identification à l\'ouverture'%(self.latFileName))
        #nind._native.NindLocalIndex.get_local_def prend directement l'identifiant externe
        #(la traduction externe -> interne est faite en interne par le C++)
        local_def = self._native.get_local_def(noDocExterne)
        return local_def if local_def is not None else []
              
    #######################################################################
    #retourne les identifiants interne et externe du dernier document indexej
    def donneMaxIdentifiants(self):
        """Return the internal and external identifiers of the last-indexed document.

        French *donneMaxIdentifiants* = "gives max identifiers".

        :return: an ``(internalId, externalId)`` tuple, or ``(0, 0)`` if
            that document has since been erased.
        """
        #trouve les donnejes du dernier doc indexej
        trouvej, dummy, dummy, identifiantExterne = self.__donneDonnejes(self.maxIdentifiantInterne)
        if not trouvej: return (0, 0)   # il a ejtej effacej
        return (self.maxIdentifiantInterne, identifiantExterne)

    #######################################################################
    #analyse du fichier
    def analyseFichierLocalindex(self, trace):
        """Validate the file and report corpus-wide document/occurrence statistics.

        French *analyseFichierLocalindex* = "analyzes local-index file".
        Extends :meth:`~nind.NindIndex.NindIndex.analyseFichierIndex` with
        totals (document count, term-document and term-position occurrence
        counts) and a per-document occurrence-count distribution.

        :param trace: if ``True``, print a human-readable report to stdout.
        :return: ``False`` if the underlying index is invalid; otherwise
            prints the report when ``trace`` is set (no explicit return
            value on the success path).
        """
        cestbon = self.analyseFichierIndex(trace)
        if not cestbon: return False
        if trace: print ("======LOCALINDEX=======")
        try:
            #trouve le max des identifiants
            maxIdent = self.donneMaxIdentifiant()
            totalExtensions = nbExtensions = 0
            totalDonnejes = 0
            totalLocalisations = 0
            totalTermDoc = 0
            occurrences = []
            for identifiant in range(maxIdent):
                try:
                    trouvej, longueurDonnejes, tailleExtension, identifiantExterne = self.__donneDonnejes(identifiant)
                    if not trouvej: continue
                    if tailleExtension > 0: nbExtensions += 1
                    totalExtensions += tailleExtension
                    totalDonnejes += longueurDonnejes + TAILLE_TESTE_DEJFINITION
                    #examine les données
                    noTerme = localisationAbsolue = 0
                    nbOccurrences = 0
                    noTermSet = set()
                    finDonnejes = self.tell() + longueurDonnejes
                    while self.tell() < finDonnejes:
                        #<identTermeRelatif> <catejgorie> <nbreLocalisations> <localisations>
                        nbOccurrences +=1
                        noTerme += self.litNombreSLat()
                        noTermSet.add(noTerme)
                        catejgorie = self.litNombre1()
                        nbreLocalisations = self.litNombre1()
                        totalLocalisations += nbreLocalisations
                        for i in range (nbreLocalisations):
                            #<localisationRelatif> <longueur>
                            localisationAbsolue += self.litNombreSLat()
                            longueur = self.litNombre1()
                    occurrences.append(nbOccurrences)
                    totalTermDoc += len(noTermSet)
                except:
                    if trace: print ('*******ERREUR SUR IDENTIFIANT :', identifiant)
                    raise                                   
            if trace:
                nbDonnejes, occurrencesMin, occurrencesMax, totalOccurrences, moyenne, ejcartType = calculeRejpartition(occurrences)
                total = totalDonnejes + totalExtensions
                print ("DONNÉES        % 10d (%6.2f %%) % 9d occurrences"%(totalDonnejes, float(100)*totalDonnejes/total, nbDonnejes))
                print ("EXTENSIONS     % 10d (%6.2f %%) % 9d occurrences"%(totalExtensions, float(100)*totalExtensions/total, nbExtensions))
                print ("TOTAL          % 10d %08X"%(total, total))
                print ("=============")
                print ("DOCUMENTS      % 10d "%(nbDonnejes))
                print ("TERMES-DOCS    % 10d occurrences"%(totalTermDoc))
                print ("TERMES         % 10d occurrences"%(totalOccurrences))
                print ("LOCALISATIONS  % 10d occurrences"%(totalLocalisations))
                print ("=============")
                print ("DOCUMENT MAX   % 10d occurrences de termes"%(occurrencesMax))
                print ("DOCUMENT MIN   % 10d occurrences de termes"%(occurrencesMin))
                print ("MOYENNE        % 10d occurrences de termes"%(moyenne))
                print ("ÉCART-TYPE     % 10d occurrences de termes"%(ejcartType))
                print ("=============")
                print ("%0.2f octets / occurrence de terme"%(float(self.donneTailleFichier())/totalOccurrences))
                
        except Exception as exc: 
            cestBon = False
            if trace: print ('ERREUR :', exc.args[0])
            
        if trace: print ("=============")
        try:
            #rejcupehre l'adresse et la longueur des spejcifiques 
            (offsetSpejcifiques, tailleSpejcifiques) = self.donneSpejcifiques()
            if tailleSpejcifiques != TAILLE_SPEJCIFIQUES: 
                raise Exception('%s : taille incompatible des spécifiques'%(self.latFileName))
            self.seek(offsetSpejcifiques, 0)
            #<maxIdentifiantInterne> <nombreDocuments>
            maxIdentifiantInterne = self.litNombre4()
            nombreDocuments = self.litNombre4()
            if trace:
                print ("Max identifiant interne utilisé: %d"%(maxIdentifiantInterne))
                print ("Nombre de documents indexés    : %d"%(nombreDocuments))
        except Exception as exc: 
            cestBon = False
            if trace: print ('ERREUR :', exc.args[0])

    #######################################################################
    #dumpe le fichier lexique sur un fichier texte
    def dumpeFichier(self, outFile):
        """Dump every document's full term/position data to a text file, for inspection.

        French *dumpeFichier* = "dumps file".

        :param outFile: a writable text file object (UTF-8).
        :return: the number of documents written.
        """
        nbLignes = 0
        #trouve le max des identifiants
        maxIdent = self.donneMaxIdentifiant()
        for identifiant in range(maxIdent):
            #trouve les donnejes 
            trouvej, longueurDonnejes, tailleExtension, identifiantExterne = self.__donneDonnejes(identifiant)
            if not trouvej: continue      #identifiant pas trouve
            nbLignes +=1
            outFile.write('%07d/%07d:: '%(identifiant, identifiantExterne))
            #examine les données
            noTerme = localisationAbsolue = 0
            finDonnejes = self.tell() + longueurDonnejes
            while self.tell() < finDonnejes:
                #<identTermeRelatif> <catejgorie> <nbreLocalisations> <localisations>
                noTerme += self.litNombreSLat()
                catejgorie = self.litNombre1()
                nbreLocalisations = self.litNombre1()
                outFile.write('[%d](%d)'%(noTerme, catejgorie))
                localisationsList = []
                for i in range (nbreLocalisations):
                    #<localisationRelatif> <longueur>
                    localisationAbsolue += self.litNombreSLat()
                    longueur = self.litNombre1()
                    localisationsList.append('%d(%d)'%(localisationAbsolue, longueur))
                outFile.write('<%s> '%(','.join(localisationsList)))
            outFile.write('\n')
        return nbLignes

    #######################################################################
    def donneidentifiantsExternes(self):
        """Return every ``(externalId, internalId)`` pair known to this file.

        French *donneIdentifiantsExternes* = "gives external identifiers".
        Used by :class:`~nind.nind_engine.NindEngine` to enumerate the
        whole corpus (e.g. to compute the total document count and average
        document length).

        :return: a list of ``(noDocExterne, noDocInterne)`` tuples.
        """
        return list(self.docIdTradExtInt.items())

    #######################################################################
    #dejcode les donnejes associejes ah un terme
    def afficheDocument(self, noDocExterne):
        """Format one document's full term/position data as a human-readable string.

        French *afficheDocument* = "displays document". Same content as
        one :meth:`dumpeFichier` line, computed for a single document.

        :param noDocExterne: the document's external identifier.
        :return: a formatted string, or ``"<id> : inconnu"`` if
            ``noDocExterne`` is unknown.
        """
        rejsultat = StringIO()
        #trouve l'identifiant interne
        if noDocExterne not in self.docIdTradExtInt: return '%d : inconnu'%(noDocExterne)
        noDocInterne = self.docIdTradExtInt[noDocExterne]
        rejsultat.write('%07d/%07d:: '%(noDocInterne, noDocExterne))
        #trouve les donnejes 
        trouvej, longueurDonnejes, tailleExtension, identifiantExterne = self.__donneDonnejes(noDocInterne)
        if not trouvej: longueurDonnejes = 0                #identifiant pas trouvej, document effacej
        #examine les données
        noTerme = localisationAbsolue = 0
        finDonnejes = self.tell() + longueurDonnejes
        while self.tell() < finDonnejes:
            #<identTermeRelatif> <catejgorie> <nbreLocalisations> <localisations>
            noTerme += self.litNombreSLat()
            catejgorie = self.litNombre1()
            nbreLocalisations = self.litNombre1()
            rejsultat.write('[%d](%d)'%(noTerme, catejgorie))
            localisationsList = []
            for i in range (nbreLocalisations):
                #<localisationRelatif> <longueur>
                localisationAbsolue += self.litNombreSLat()
                longueur = self.litNombre1()
                localisationsList.append('%d(%d)'%(localisationAbsolue, longueur))
            rejsultat.write('<%s> '%(','.join(localisationsList)))
        return rejsultat.getvalue()
    #######################################################################
        
if __name__ == '__main__':
    main()
