#!/usr/bin/env python
# -*- coding: utf-8 -*-
__author__ = "jys"
__copyright__ = "Copyright (C) 2017 LATEJCON"
__license__ = "GNU LGPL"
__version__ = "2.0.1"
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
from time import ctime
import codecs
import NindLateconFile
from NindPadFile import calculeRejpartition
from NindIndex import NindIndex

def usage():
    if getenv("PY") != None: script = sys.argv[0].replace(getenv("PY"), '$PY')
    else: script = sys.argv[0]
    print ("""© l'ATEJCON.
Analyse un fichier nindlexiconindex du système nind et affiche les stats. 
Peut dumper nindlexiconindex sur <fichier>-dump.txt
Peut donner les identifiants de mots composés avec le maximum de composants
Peut donner l'identifiant du mot spécifié
Peut donner les mots simples qui "collisionnent" sur la même indirection
Le format du fichier est défini dans le document LAT2017.JYS.470.

usage   : %s <fichier> [ <analyse> | <dumpe> | <max> | <ident> <mot> | <collision> <index> ]
exemple : %s FRE.nindlexiconindex
exemple : %s FRE.nindlexiconindex dumpe
exemple : %s FRE.nindlexiconindex max
exemple : %s FRE.nindlexiconindex ident "épistémologie_compulsive"
exemple : %s FRE.nindlexiconindex coll 1268512
"""%(script, script, script, script, script, script))

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
            nbLignes = nindLexiconindex.dumpeFichier(outFile)
            outFile.close()
            print ('%d lignes écrites dans %s'%(nbLignes, outFilename))
        elif action.startswith('max'):
            maximums = nindLexiconindex.donneMax(5)
            for (nbreComposejs, identifiantS, index) in maximums:
                print ('%8d  %8d  (%d)'%(identifiantS, index, nbreComposejs))
        elif action.startswith('id'):
            motsSimples = mot.split('_')
            print ('identifiant : ', nindLexiconindex.donneIdentifiant(motsSimples))
            print ('clef        : ', nindLexiconindex.donneClef(motsSimples[-1]))
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
# <flagDejfinition=13>    ::= <Integer1>
# <identifiantHash>       ::= <Integer3>
# <longueurDonnejes>      ::= <Integer3>
# <donnejesHash>          ::= { <mot> }
# <mot>                   ::= <motSimple> <identifiantS> <nbreComposejs> <composejs>
# <motSimple>             ::= <longueurMot> <motUtf8>
# <longueurMot>           ::= <Integer1>
# <motUtf8>               ::= { <Octet> }
# <identifiantS>          ::= <Integer4>
# <nbreComposejs>         ::= <IntegerULat>
# <composejs>             ::= { <composej> } 
# <composej>              ::= <identifiantA> <identifiantRelC>
# <identifiantA>          ::= <Integer4>
# <identifiantRelC>       ::= <IntegerSLat>
##############################
# <spejcifique>           ::= <vide>
############################################################

FLAG_DEJFINITION = 13
#<flagDejfinition=13>(1) <identifiantHash>(3) <longueurDonnejes>(3) = 7
TAILLE_TESTE_DEJFINITION = 7

class NindLexiconindex(NindIndex):
    def __init__(self, lexiconindexFileName):
        NindIndex.__init__(self, lexiconindexFileName)
        #trouve le modulo = nombreIndirection
        self.nombreIndirection = self.donneMaxIdentifiant()
        
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
        
    #trouve l'identifiant du mot
    def __donneIdentifiantIntermejdiaire(self, mot, sousMotId):
        clefB = NindLateconFile.clefB(mot)
        index = clefB % self.nombreIndirection
        #trouve les donnejes 
        trouvej, longueurDonnejes, tailleExtension = self.__donneDonnejes(index)
        if not trouvej: return 0      #identifiant pas trouve
        finDonnejes = self.tell() + longueurDonnejes
        while self.tell() < finDonnejes:
            #<motSimple> <identifiantS> <nbreComposes> <composes>
            motSimple = self.litString()
            if motSimple != mot: 
                #pas le mot cherche, on continue
                self.litNombre4()      #identifiantS
                nbreComposes = self.litNombreULat()
                for i in range(nbreComposes):
                    self.litNombre4()          #identifiantA
                    self.litNombreSLat()       #identifiantRelC
                continue
            #c'est le mot cherche
            identifiantS = self.litNombre4()
            if sousMotId == 0: return identifiantS        #identifiant simple trouve
            nbreComposes = self.litNombreULat()
            identifiantC = identifiantS
            for i in range(nbreComposes):
                #<identifiantA> <identifiantRelC>
                identifiantA = self.litNombre4()
                identifiantC += self.litNombreSLat()
                if sousMotId == identifiantA: return identifiantC    #identifiant compose trouve
        #pas trouve
        return 0
    
    #trouve l'identifiant du mot fourni sous forme d'une liste de mots simples
    def donneIdentifiant(self, motsSimples):
        sousMotId = 0
        for mot in motsSimples:
            sousMotId = self.__donneIdentifiantIntermejdiaire(mot, sousMotId)
            if sousMotId == 0: break
        return sousMotId

    #######################################################################"
    #analyse du fichier
    def analyseFichierLexiconindex(self, trace):
        cestbon = self.analyseFichierIndex(trace)
        try:
            #trouve le max des identifiants
            maxIdent = self.donneMaxIdentifiant()
            totalDonnejes = totalExtensions = 0
            nbDonnejes = nbExtensions = 0
            composejs = []
            for index in range(maxIdent):
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
                    identifiantS = self.litNombre4()
                    nbreComposejs = self.litNombreULat()
                    composejs.append(nbreComposejs)
                    identifiantC = identifiantS
                    for i in range(nbreComposejs):
                        #<identifiantA> <identifiantRelC>
                        identifiantA = self.litNombre4()
                        identifiantC += self.litNombreSLat()
                        
                total = totalDonnejes + totalExtensions
            if trace:
                nbreMotsS, composejsMin, composejsMax, nbreMotsC, moyenne, ejcartType = calculeRejpartition(composejs)
                print ("=============")
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
            if trace: print ('ERREUR :', exc.args[0])

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
   
    #######################################################################
    #dumpe le fichier lexique sur un fichier texte
    def dumpeFichier(self, outFile):
        nbLignes = 0
        #trouve le max des identifiants
        maxIdent = self.donneMaxIdentifiant()
        for index in range(maxIdent):
            #trouve les donnejes 
            trouvej, longueurDonnejes, tailleExtension = self.__donneDonnejes(index)
            if not trouvej: continue      #index pas trouve
            #examine les données
            finDonnejes = self.tell() + longueurDonnejes
            while self.tell() < finDonnejes:
                #<motSimple> <identifiantS> <nbreComposes> <composes>
                motSimple = self.litString()
                identifiantS = self.litNombre4()
                nbreComposes = self.litNombreULat()
                outFile.write('[%s] %06d (%d) '%(motSimple, identifiantS, nbreComposes))
                identifiantC = identifiantS
                composes = []
                for i in range(nbreComposes):
                    #<identifiantA> <identifiantRelC>
                    identifiantA = self.litNombre4()
                    identifiantC += self.litNombreSLat()
                    composes.append('%06d %06d'%(identifiantA, identifiantC))
                outFile.write(' <%s>\n'%(', '.join(composes)))
                nbLignes +=1
        return nbLignes

    #######################################################################
    #donne les mots simples enregistrejs sur l'indirection spejcifieje et le nombre de composants
    def donneCollisions(self, index):
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
        #trouve le max des identifiants
        maxIdent = self.donneMaxIdentifiant()
        composejs = []
        for index in range(maxIdent):
            #trouve les donnejes 
            trouvej, longueurDonnejes, tailleExtension = self.__donneDonnejes(index)
            if not trouvej: continue      #index pas trouve
            #examine les données
            finDonnejes = self.tell() + longueurDonnejes
            while self.tell() < finDonnejes:
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
        return composejs
    
    #######################################################################
    #donne la clef d'accehs (pour deboguer)
    def donneClef(self, mot):
        clefB = NindLateconFile.clefB(mot)
        return clefB % self.nombreIndirection
        
    #######################################################################
        
if __name__ == '__main__':
    main()
