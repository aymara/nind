#!/usr/bin/env python3
# -*- coding: utf-8 -*-
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
from codecs import open
from NindRetrolexicon import NindRetrolexicon
from NindLocalindex import NindLocalindex
from NindLexiconindex import NindLexiconindex
import NindFile

def usage():
    if getenv("PY") != None: script = sys.argv[0].replace(getenv("PY"), '$PY')
    else: script = sys.argv[0]
    print (f"""© l'ATEJCON.
Dans une base nind spécifiée par un de ses fichiers, dumpe en clair un 
document préalablement indexé.
Le document est spécifié par son identifiant externe ou "last" pour le
dernier document indexé.
Le résultat est sauvegardé dans le fichier <base>-dump-<noExterne>.txt.
Les termes apparaissent dans l'ordre par paquets de 4 sur une ligne.
"#" est le séparateur des mots simples dans les mots composés. 
Les localisations ne sont pas affichées.
Le système nind est expliqué dans le document LAT2017.JYS.470.

usage   : {script} <fichier nind> <n° doc>
exemple : {script} FRE.lexiconindex 9546
exemple : {script} FRE.lexiconindex last
""")

def main():
    try:
        if len(sys.argv) < 3 : raise Exception()
        nindFileName = path.abspath(sys.argv[1])
        noDoc = sys.argv[2]
        dumpeDocument(nindFileName, noDoc)
        
    except Exception as exc:
        if len(exc.args) == 0: usage()
        else:
            print ("******************************")
            print (exc.args[0])
            print ("******************************")
            raise
        sys.exit()

################################
def dumpeDocument(nindFileName, noDoc):
    #calcul des noms de fichiers (remplace l'extension)
    nn = nindFileName.split('.')
    nindlexiconindexName = '.'.join(nn[:-1]) + '.nindlexiconindex'
    nindlocalindexName = '.'.join(nn[:-1]) + '.nindlocalindex'
    nindretrolexiconName = '.'.join(nn[:-1]) + '.nindretrolexicon'
    #les classes
    nindLexiconindex = NindLexiconindex(nindlexiconindexName)
    lexiconIdentification = nindLexiconindex.donneIdentification()
    nindLocalindex = NindLocalindex(nindlocalindexName, lexiconIdentification)
    nindRetrolexicon = NindRetrolexicon(nindretrolexiconName, lexiconIdentification)
    #trouve l'identifiant interne
    if noDoc.startswith('las'):
        (noInterne, noExterne) = nindLocalindex.donneMaxIdentifiants()
        last = 'last-'
    else:
        noExterne = int(noDoc)
        last = ''
    if noExterne not in nindLocalindex.docIdTradExtInt: 
        print ("doc inconnu")
        sys.exit()
    identInterne = nindLocalindex.docIdTradExtInt[noExterne]
    print (f'noExterne -> identInterne : {noExterne} -> {identInterne}')
    #trouve les termes dans le fichier des index locaux
    termList = nindLocalindex.donneListeTermes(noExterne)
    resultat = []
    cmptTermes = 0
    # ouvre le fichier de dump
    fichierDumpName = '.'.join(nn[:-1]) + f'-dump-{last}{noExterne}.txt'
    with open(fichierDumpName, 'w', 'utf8') as dump:
        for termeLocal in termList:
            terme = '#'.join(nindRetrolexicon.donneMot(termeLocal.term))
            if termeLocal.cg == 0:
                resultat.append(terme)
            else:
                resultat.append(f'{terme} [{NindFile.catNb2Str(termeLocal.cg)}]')
            cmptTermes +=1
            if cmptTermes %4 == 0: 
                dump.write(', '.join(resultat) + ',\n')
                resultat.clear()
        dump.write(', '.join(resultat) + '\n')
    print (f'{cmptTermes} occurrences de termes trouvées -> {fichierDumpName}')
   

if __name__ == '__main__':
        main()
