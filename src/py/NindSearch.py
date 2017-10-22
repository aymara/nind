#!/usr/bin/env python3.5
# -*- coding: utf-8 -*-
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
from builtins import input
import NindLateconFile
from NindLexiconindex import NindLexiconindex
from NindTermindex import NindTermindex
from NindLocalindex import NindLocalindex

def usage():
    if getenv("PY") != None: script = sys.argv[0].replace(getenv("PY"), '$PY')
    else: script = sys.argv[0]
    print ("""© l'ATEJCON.
Programme d'interrogation d'une base nind préalablement indexée
L'utilisateur indique le terme cherché, simple ou composé.
Un terme composé a la forme blanc-souligné (ex kyrielle_outil_informatique).
L'utilisateur indique ensuite le document examiné
et la localisation du terme cherché est affichée.
Le format des fichiers est défini dans le document LAT2014.JYS.440.

usage   : %s <fichier lexiconindex> <terme cherché>
exemple : %s FRE.lexiconindex syntagme_nominal
"""%(script, script))

OFF = "\033[m"
BLUE = "\033[0;34m"
RED = "\033[1;31m"

def main():
    if len(sys.argv) < 3 :
        usage()
        sys.exit()
    lexiconindexFileName = path.abspath(sys.argv[1])
    terme = sys.argv[2].strip()
    
    #calcul des noms de fichiers (remplace l'extension)
    nn = lexiconindexFileName.split('.')
    termindexFileName = '.'.join(nn[:-1])+'.nindtermindex'
    localindexFileName = '.'.join(nn[:-1])+'.nindlocalindex'
    
    #ouvre les classes
    nindLexiconindex = NindLexiconindex(lexiconindexFileName)
    nindTermindex = NindTermindex(termindexFileName)
    nindLocalindex = NindLocalindex(localindexFileName)
    
    #1) verifie l'identification des fichiers
    (maxIdentifiant, dateHeure) = nindLexiconindex.donneIdentificationFichier()
    print ("max=%d dateheure=%d (%s)"%(maxIdentifiant, dateHeure, ctime(int(dateHeure))))
    (maxIdentifiant2, dateHeure2) = nindTermindex.donneIdentificationFichier()
    if maxIdentifiant2 != maxIdentifiant or dateHeure2 != dateHeure:
        print ("%s NON APAIRÉ : max=%d dateheure=%d (%s)"%(termindexFileName, maxIdentifiant2, dateHeure2, ctime(int(dateHeure2))))
    (maxIdentifiant2, dateHeure2) = nindLocalindex.donneIdentificationFichier()
    if maxIdentifiant2 != maxIdentifiant or dateHeure2 != dateHeure:
        print ("%s NON APAIRÉ : max=%d dateheure=%d (%s)"%(localindexFileName, maxIdentifiant2, dateHeure2, ctime(int(dateHeure2))))  
    
    #2) trouve l'identifiant du terme
    #trouve la clef des termes simples
    termesSimples = terme.split('_')
    sousMotId = 0
    for mot in termesSimples:
        sousMotId = nindLexiconindex.donneIdentifiant(mot, sousMotId)
        if sousMotId == 0: break
    if sousMotId == 0:
        print ('INCONNU')
        sys.exit()
        
    #3) trouve les utilisations du terme dans le fichier inverse et les affiche
    termesCGList = nindTermindex.donneListeTermesCG(sousMotId)
    for (categorie, frequenceTerme, docs) in termesCGList:
        docsListe = []
        for (noDoc, frequenceDoc) in docs: docsListe.append('%d(%d)'%(noDoc, frequenceDoc))
        print ('%s[%s] %s%s %d fois dans %s'%(RED, sousMotId, NindLateconFile.catNb2Str(categorie), OFF, frequenceTerme, ' '.join(docsListe)))

    #4) choisit un doc et affiche ce qui concerne le terme
    noDocStr = input("%sno doc : %s"%(BLUE, OFF))
    if not noDocStr.isdigit(): sys.exit()
    noDoc = int(noDocStr)
    termList = nindLocalindex.donneListeTermes(noDoc)
    resultat = []
    for (noTerme, categorie, localisationsList) in termList:
        if noTerme != sousMotId: continue
        locListe = []
        for (localisationAbsolue, longueur) in localisationsList: locListe.append('%d(%d)'%(localisationAbsolue, longueur))
        resultat.append('%s<%s>'%(NindLateconFile.catNb2Str(categorie), ' '.join(locListe)))
    print (' '.join(resultat))
                                                                                  

if __name__ == '__main__':
        main()
