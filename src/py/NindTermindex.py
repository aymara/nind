#!/usr/bin/env python
# -*- coding: utf-8 -*-
import sys
from os import getenv, path
from time import ctime
import codecs
from NindPadFile import calculeRejpartition
from NindIndex import NindIndex

def usage():
    if getenv("PY") != None: script = sys.argv[0].replace(getenv("PY"), '$PY')
    else: script = sys.argv[0]
    print """© l'ATEJCON.
Analyse ou dumpe un fichier nindtermindex du système nind et affiche les stats. 
Le format du fichier est défini dans le document LAT2014.JYS.440.

usage   : %s <fichier> [ <analyse> | <dumpe> ]
exemple : %s FRE.termindex dumpe
"""%(script, script)

OFF = "\033[m"
RED = "\033[1;31m"

def main():
    try:
        if len(sys.argv) < 2 : raise Exception()
        termindexFileName = path.abspath(sys.argv[1])
        action = 'analyse' 
        if len(sys.argv) > 2 : action = sys.argv[2]
        
        #la classe
        nindTermindex = NindTermindex(termindexFileName)
        if action.startswith('anal'): nindTermindex.analyseFichierTermindex(True)
        elif action.startswith('dump'):
            outFilename = termindexFileName + '-dump.txt'
            outFile = codecs.open(outFilename, 'w', 'utf-8')
            nbLignes = nindTermindex.dumpeFichier(outFile)
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
# <dejfinition>           ::= <flagDejfinition=17> <identifiantTerme> <longueurDonnejes> <donnejesTerme>
# <flagDejfinition=17>    ::= <Integer1>
# <identifiantTerme>      ::= <Integer3>
# <longueurDonnejes>      ::= <Integer3>
# <donnejesTerme>         ::= { <donnejesCG> }
# <donnejesCG>            ::= <flagCg=61> <catejgorie> <frejquenceTerme> <nbreDocs> <listeDocuments>
# <flagCg=61>             ::= <Integer1>
# <catejgorie>            ::= <Integer1>
# <frejquenceTerme>       ::= <IntegerULat>
# <nbreDocs>              ::= <IntegerULat>
# <listeDocuments>        ::= { <identDocRelatif> <frejquenceDoc> }
# <identDocRelatif>       ::= <IntegerULat>
# <frejquenceDoc>         ::= <IntegerULat>
##############################
# <spejcifique>           ::= { <valeur> }
# <valeur>                ::= <Integer4>
############################################################

FLAG_DEJFINITION = 17
FLAG_CG = 61
#<flagDejfinition=17>(1) <identifiantTerme>(3) <longueurDonnejes>(3) = 7
TAILLE_TESTE_DEJFINITION = 7

class NindTermindex(NindIndex):
    def __init__(self, termindexFileName):
        NindIndex.__init__(self, termindexFileName)

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

    #retourne la structure dejcrivant le fichier inversej pour ce terme
    def donneListeTermesCG(self, ident):
        #trouve les donnejes 
        trouvej, longueurDonnejes, tailleExtension = self.__donneDonnejes(ident)
        if not trouvej: return []          #terme inconnu
        finDonnejes = self.tell() + longueurDonnejes
        #lit les donnes
        resultat = []
        while self.tell() < finDonnejes:
            #<flagCg=61> <catejgorie> <frejquenceTerme> <nbreDocs> <listeDocuments>
            if self.litNombre1() != FLAG_CG: raise Exception('%s : pas FLAG_CG à %d'%(self.latFileName, self.tell() -1))
            catejgorie = self.litNombre1()
            frejquenceTerme = self.litNombreULat()
            nbreDocs = self.litNombreULat()
            noDoc = 0
            docs = []
            for i in range(nbreDocs):
                #<identDocRelatif> <frejquenceDoc>
                identDocRelatif = self.litNombreULat()
                frejquenceDoc = self.litNombreULat()
                noDoc += identDocRelatif
                docs.append((noDoc, frejquenceDoc))
            resultat.append((catejgorie, frejquenceTerme, docs))
        return resultat
 
    #######################################################################"
    #analyse du fichier
    def analyseFichierTermindex(self, trace):
        cestbon = self.analyseFichierIndex(trace)
        try:
            #trouve le max des identifiants
            maxIdent = self.donneMaxIdentifiant()
            totalDonnejes = totalExtensions = 0
            occurrencesDoc = 0
            nbDonnejes = nbExtensions = 0
            nbHapax = 0
            frejquences = []
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
                    #<flagCg=61> <catejgorie> <frejquenceTerme> <nbreDocs> <listeDocuments>
                    if self.litNombre1() != FLAG_CG: 
                        raise Exception('%s : pas FLAG_CG à %08X'%(self.latFileName, self.tell() -1))
                    catejgorie = self.litNombre1()
                    frejquenceTerme = self.litNombreULat()
                    nbreDocs = self.litNombreULat()
                    totalFrejquences = 0
                    noDoc = 0
                    for i in range(nbreDocs):
                        #<identDocRelatif> <frejquenceDoc>
                        identDocRelatif = self.litNombreULat()
                        frejquenceDoc = self.litNombreULat()
                        totalFrejquences += frejquenceDoc
                        noDoc += identDocRelatif
                    if totalFrejquences != frejquenceTerme: 
                        raise Exception('%s : fréquences incompatibles sur terme %d'%(self.latFileName, identifiant))
                    frejquences.append(frejquenceTerme)
                    occurrencesDoc += nbreDocs 
                    if totalFrejquences == 1: nbHapax +=1
                    
            if trace:
                nbTermesCG, frejquenceMin, frejquenceMax, totalFrejquences, moyenne, ejcartType = calculeRejpartition(frejquences)
                print "============="
                total = totalDonnejes + totalExtensions
                print "DONNÉES        % 10d (%6.2f %%) % 9d occurrences"%(totalDonnejes, float(100)*totalDonnejes/total, nbDonnejes)
                print "EXTENSIONS     % 10d (%6.2f %%) % 9d occurrences"%(totalExtensions, float(100)*totalExtensions/total, nbExtensions)
                print "TOTAL          % 10d %08X"%(total, total)
                print "============="
                print "TERMES-CG      % 10d"%(nbTermesCG)
                print "FRÉQUENCE MIN  % 10d"%(frejquenceMin)
                print "FRÉQUENCE MAX  % 10d"%(frejquenceMax)
                print "TOTAL FRÉQUENCE% 10d"%(totalFrejquences)
                print "MOYENNE        % 10d"%(moyenne)
                print "ÉCART TYPE     % 10d"%(ejcartType)
                print "============="
                print "TERMES-DOCS    % 10d occurrences"%(occurrencesDoc)
                print "TERMES         % 10d occurrences"%(totalFrejquences)
                print "HAPAX          % 10d (%6.2f %%)"%(nbHapax, float(100)*nbHapax/nbDonnejes)
                print "============="
                print "%0.2f octets / occurrence de terme-doc"%(float(self.donneTailleFichier())/occurrencesDoc)
                print "%0.2f octets / occurrence de terme"%(float(self.donneTailleFichier())/totalFrejquences)
        except Exception as exc: 
            cestBon = False
            if trace: print 'ERREUR :', exc.args[0]  
            
        try:
            #rejcupehre l'adresse et la longueur des spejcifiques 
            (offsetSpejcifiques, tailleSpejcifiques) = self.donneSpejcifiques()
            #doit estre un multiple de 4
            if tailleSpejcifiques/4*4 != tailleSpejcifiques:
                raise Exception('%s : taille incompatible des spécifiques (doit être multiple de 4)'%(self.latFileName))
            self.seek(offsetSpejcifiques, 0)
            spejcifiques = []
            for i in range(tailleSpejcifiques/4): spejcifiques.append('%d'%(self.litNombre4()))
            if trace:
                print "============="
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
            outFile.write('%06d:\n'%(identifiant))
            frejquenceGlobale = 0
            nbLignes +=1
            #examine les données
            finDonnejes = self.tell() + longueurDonnejes
            while self.tell() < finDonnejes:
                #<flagCg=61> <catejgorie> <frejquenceTerme> <nbreDocs> <listeDocuments>
                if self.litNombre1() != FLAG_CG: raise Exception('pas FLAG_CG à %d'%(self.tell() -1))
                catejgorie = self.litNombre1()
                frejquenceTerme = self.litNombreULat()
                nbreDocs = self.litNombreULat()
                outFile.write('[%d] (%d) <%d>'%(catejgorie, frejquenceTerme, nbreDocs))
                frejquenceGlobale += frejquenceTerme
                totalFrequences = 0
                noDoc = 0
                docList = []
                for i in range(nbreDocs):
                    #<identDocRelatif> <frejquenceDoc>
                    incrementIdentDoc = self.litNombreULat()
                    frejquenceDoc = self.litNombreULat()
                    totalFrequences += frejquenceDoc
                    noDoc += incrementIdentDoc
                    docList.append('%05d (%d)'%(noDoc, frejquenceDoc))
                outFile.write(' :: %s\n'%(', '.join(docList)))
                if totalFrequences != frejquenceTerme: raise Exception('fréquences incompatibles sur terme %d'%(noTerm))
            outFile.write('frequence totale de %06d : %d\n'%(identifiant, frejquenceGlobale))
            outFile.write('\n')
        return nbLignes
       
            
if __name__ == '__main__':
    main()
