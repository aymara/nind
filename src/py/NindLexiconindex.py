#!/usr/bin/env python
# -*- coding: utf-8 -*-
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
    print """© l'ATEJCON.
Analyse ou dumpe un fichier nindlexiconindex du système nind et affiche les stats. 
Le format du fichier est défini dans le document LAT2014.JYS.440.

usage   : %s <fichier> [ <analyse> | <dumpe> ]
exemple : %s FRE.termindex dumpe
"""%(script, script)

def main():
    try:
        if len(sys.argv) < 2 : raise Exception()
        lexiconindexFileName = path.abspath(sys.argv[1])
        action = 'analyse' 
        if len(sys.argv) > 2 : action = sys.argv[2]
        
        #la classe
        nindLexiconindex = NindLexiconindex(lexiconindexFileName)
        if action.startswith('anal'): nindLexiconindex.analyseFichierLexiconindex(True)
        elif action.startswith('dump'):
            outFilename = lexiconindexFileName + '-dump.txt'
            outFile = codecs.open(outFilename, 'w', 'utf-8')
            nbLignes = nindLexiconindex.dumpeFichier(outFile)
            outFile.close()
            print '%d lignes écrites dans %s'%(nbLignes, outFilename)
        else: raise Exception()
    except Exception as exc:
        if len(exc.args) == 0: usage()
        else:
            print "******************************"
            print exc.args[0]
            print "******************************"
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
# <identifiantS>          ::= <Integer3>
# <nbreComposejs>         ::= <IntegerULat>
# <composejs>             ::= { <composej> } 
# <composej>              ::= <identifiantA> <identifiantRelC>
# <identifiantA>          ::= <Integer3>
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
    def donneIdentifiant(self, mot, sousMotId):
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
                self.litNombre3()      #identifiantS
                nbreComposes = self.litNombreULat()
                for i in range(nbreComposes):
                    self.litNombre3()          #identifiantA
                    self.litNombreSLat()       #identifiantRelC
                continue
            #c'est le mot cherche
            identifiantS = self.litNombre3()
            if sousMotId == 0: return identifiantS        #identifiant simple trouve
            nbreComposes = self.litNombreULat()
            identifiantC = identifiantS
            for i in range(nbreComposes):
                #<identifiantA> <identifiantRelC>
                identifiantA = self.litNombre3()
                identifiantC += self.litNombreSLat()
                if sousMotId == identifiantA: return identifiantC    #identifiant compose trouve
        #pas trouve
        return 0

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
            for identifiant in range(maxIdent):
                #trouve les donnejes 
                trouvej, longueurDonnejes, tailleExtension = self.__donneDonnejes(identifiant)
                if not trouvej: continue      #identifiant pas trouve
                nbDonnejes +=1
                totalDonnejes += longueurDonnejes + TAILLE_TESTE_DEJFINITION
                if tailleExtension > 0: nbExtensions += 1
                totalExtensions += tailleExtension
                #examine les données
                finDonnejes = self.tell() + longueurDonnejes
                while self.tell() < finDonnejes:
                    #<motSimple> <identifiantS> <nbreComposejs> <composejs>
                    motSimple = self.litString()
                    identifiantS = self.litNombre3()
                    nbreComposejs = self.litNombreULat()
                    composejs.append(nbreComposejs)
                    identifiantC = identifiantS
                    for i in range(nbreComposejs):
                        #<identifiantA> <identifiantRelC>
                        identifiantA = self.litNombre3()
                        identifiantC += self.litNombreSLat()
                        
                total = totalDonnejes + totalExtensions
            if trace:
                nbreMotsS, composejsMin, composejsMax, nbreMotsC, moyenne, ejcartType = calculeRejpartition(composejs)
                print "============="
                print "DONNÉES        % 10d (%6.2f %%) % 9d occurrences"%(totalDonnejes, float(100)*totalDonnejes/total, nbDonnejes)
                print "EXTENSIONS     % 10d (%6.2f %%) % 9d occurrences"%(totalExtensions, float(100)*totalExtensions/total, nbExtensions)
                print "TOTAL          % 10d %08X"%(total, total)
                print "============="
                total = nbreMotsS + nbreMotsC
                print "MOTS SIMPLES   % 10d (%6.2f %%)"%(nbreMotsS, float(100)*nbreMotsS/total)
                print "MOTS COMPOSÉS  % 10d (%6.2f %%)"%(nbreMotsC, float(100)*nbreMotsC/total)
                print "TOTAL          % 10d"%(total)
                print "============="
                print "COMPOSÉS MAX   % 10d composés finissant par le même mot simple"%(composejsMax)
                print "COMPOSÉS MIN   % 10d composés finissant par le même mot simple"%(composejsMin)
                print "MOYENNE        % 10d composés finissant par le même mot simple"%(moyenne)
                print "ÉCART-TYPE     % 10d"%(ejcartType)
                print "============="
                print "%0.2f octets / mot"%(float(self.donneTailleFichier())/(nbreMotsS+nbreMotsC))
                print "============="
               
        except Exception as exc: 
            cestBon = False
            if trace: print 'ERREUR :', exc.args[0]  

        try:
            #rejcupehre l'adresse et la longueur des spejcifiques 
            (offsetSpejcifiques, tailleSpejcifiques) = self.donneSpejcifiques()
            self.seek(offsetSpejcifiques, 0)
            spejcifiques = []
            for i in range(tailleSpejcifiques): spejcifiques.append(self.litNombre1())
            if trace:
                print "%d mots de données spécifiques"%(tailleSpejcifiques/4)
                print ', '.join(spejcifiques)
        except Exception as exc: 
            cestBon = False
            if trace: print 'ERREUR :', exc.args[0] 
   
    #######################################################################
    #dumpe le fichier lexique sur un fichier texte
    def dumpeFichier(self, outFile):
        nbLignes = 0
        #trouve le max des identifiants
        maxIdent = self.donneMaxIdentifiant()
        for identifiant in range(maxIdent):
            #trouve les donnejes 
            trouvej, longueurDonnejes, tailleExtension = self.__donneDonnejes(identifiant)
            if not trouvej: continue      #identifiant pas trouve
            #examine les données
            finDonnejes = self.tell() + longueurDonnejes
            while self.tell() < finDonnejes:
                #<motSimple> <identifiantS> <nbreComposes> <composes>
                motSimple = self.litString()
                identifiantS = self.litNombre3()
                nbreComposes = self.litNombreULat()
                outFile.write('[%s] %06d (%d) '%(motSimple, identifiantS, nbreComposes))
                identifiantC = identifiantS
                composes = []
                for i in range(nbreComposes):
                    #<identifiantA> <identifiantRelC>
                    identifiantA = self.litNombre3()
                    identifiantC += self.litNombreSLat()
                    composes.append('%06d %06d'%(identifiantA, identifiantC))
                outFile.write(' <%s>\n'%(', '.join(composes)))
                nbLignes +=1
        return nbLignes
     
        
        
if __name__ == '__main__':
    main()
