#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generic "definitions indexed by integer identifier" pad file.

:class:`NindIndex` specializes :class:`~nind.NindPadFile.NindPadFile` for
the common case where the indirection table maps a simple integer
identifier directly to a variable-length "definition" record: each
indirection entry is just an ``(offset, length)`` pair (see the
``<indirection>`` grammar rule below). This is the shared shape behind all
three concrete index formats:
:class:`~nind.NindLexiconindex.NindLexiconindex` (``.nindlexiconindex``,
keyed by hash bucket), :class:`~nind.NindTermindex.NindTermindex`
(``.nindtermindex``, keyed by term id) and
:class:`~nind.NindLocalindex.NindLocalindex` (``.nindlocalindex``, keyed by
internal document id) - each of those only adds the meaning of the bytes
inside a definition.
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
from time import ctime
try:
    from .NindPadFile import NindPadFile
except ImportError:
    # run directly (not as part of the installed package): see docs/cli.md
    from NindPadFile import NindPadFile

def usage():
    #print(sys.version)
    if getenv("PY") != None: script = sys.argv[0].replace(getenv("PY"), '$PY')
    else: script = sys.argv[0]
    print ("""© l'ATEJCON.
o Analyse un fichier NindIndex du système nind et affiche les statistiques. 
o Peut donner l'offset dans le fichier et la longueur des données 
  correspondant à un identifiant.
Les types de fichiers : nindlexiconindex, nindtermindex, nindlocalindex
Le format du fichier est défini dans le document LAT2017.JYS.470.

usage   : %s <fichier> [ <analyse> | <indirection> <index> ]
exemple : %s FRE.termindex
exemple : %s FRE.termindex indir 398892
"""%(script, script, script))

def main():
    try:
        if len(sys.argv) < 2 : raise Exception()
        indexFileName = path.abspath(sys.argv[1])
        action = 'analyse' 
        if len(sys.argv) > 2 : action = sys.argv[2]
        index = 0
        if len(sys.argv) > 3 : index = int(sys.argv[3])
        
        #la classe
        nindIndex = NindIndex(indexFileName)
        if action.startswith('anal'): nindIndex.analyseFichierIndex(True)
        elif action.startswith('ind'):
            (offsetDejfinition, longueurDejfinition) = nindIndex.donneAdresseDejfinition(index)
            print ('offset   : ', offsetDejfinition)
            print ('longueur : ', longueurDejfinition)
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
# <donnejesIndexejes>     ::= <indirection>
# <blocEnVrac>            ::= <blocDejfinition>
#
# <indirection>           ::= <offsetDejfinition> <longueurDejfinition> 
# <offsetDejfinition>     ::= <Entier5>
# <longueurDejfinition>   ::= <Entier3>
#
# <blocDejfinition>       ::= { <dejfinition> | <vide> }
# <dejfinition>           ::= { <Octet> }
# <vide>                  ::= { <Octet> }
############################################################
#<offsetDejfinition>(5) <longueurDejfinition>(3) = 8
TAILLE_INDIRECTION = 8
############################################################

class NindIndex(NindPadFile):
    """Read-only pad file whose indirection entries are plain ``(offset, length)`` pairs.

    Adds :meth:`donneAdresseDejfinition` on top of
    :class:`~nind.NindPadFile.NindPadFile`, resolving an identifier straight
    to where its variable-length definition lives; format-specific
    subclasses use it and then decode the definition's bytes themselves.
    """

    def __init__(self, indexFileName):
        """Open ``indexFileName`` read-only and verify its structure.

        :param indexFileName: path to the ``.nind*index`` file.
        :raises Exception: if the file's pad-file envelope is invalid.
        """
        #en lecture uniquement
        NindPadFile.__init__(self, indexFileName)
        self.vejrifieFichier()

    #donne l'adresse et la longueur de la dejfinition
    def donneAdresseDejfinition(self, identifiant):
        """Return the offset and length of ``identifiant``'s definition.

        French *donneAdresseDéfinition* = "gives definition address".

        :param identifiant: the integer identifier to look up.
        :return: an ``(offsetDejfinition, longueurDejfinition)`` tuple, or
            ``(0, 0)`` if ``identifiant`` is out of range.
        """
        position = self.donnePositionEntreje(identifiant)
        if position == 0: return (0, 0)          #identifiant hors limite
        self.seek(position, 0)
        #<offsetDejfinition> <longueurDejfinition>
        offsetDejfinition = self.litNombre5()
        longueurDejfinition = self.litNombre3()
        return (offsetDejfinition, longueurDejfinition)

    #analyse complehtement le fichier et retourne True si ok
    def analyseFichierIndex(self, trace):
        """Validate the file (via :meth:`~nind.NindPadFile.NindPadFile.analyseFichierPadFile`) and report indirection-usage statistics.

        French *analyseFichierIndex* = "analyzes index file". Delegates the
        indirection walk to ``nind._native``'s ``analyse_index()`` (see
        ``NindIndex::analyseIndex`` in the C++), which checks that every
        identifier resolves to a valid indirection entry and reports the
        size distribution of definitions plus the holes between them.

        :param trace: if ``True``, print a human-readable report to stdout.
        :return: ``True`` if the file is structurally valid, ``False``
            otherwise.
        :note: unlike the previous hand-rolled implementation, a structural
            error stops the report at that point rather than continuing to
            print whatever the remaining sections can still compute.
        """
        cestbon = self.analyseFichierPadFile(trace)
        if not cestbon: return False
        if trace: print ("======INDEX=======")
        try:
            stats = self._native.analyse_index()
        except Exception as exc:
            if trace: print ('*******ERREUR :', exc.args[0])
            return False

        if trace:
            print ("%d / %d indirections utilisées"%(stats.used_count, stats.max_ident))
            print ("=============")

        holes = stats.holes
        defs = stats.definition_sizes
        if trace:
            total = holes.holes_size + defs.sum
            print ("DÉFINITIONS    % 10d (%6.2f %%) % 9d occurrences"%(defs.sum, float(100)*defs.sum/total, stats.used_count))
            print ("VIDES          % 10d (%6.2f %%) % 9d occurrences"%(holes.holes_size, float(100)*holes.holes_size/total, holes.holes_count))
            print ("TOTAL          % 10d %08X"%(total, total))
            print ("=============")
            typesVidesList = list(holes.hole_size_histogram.items())
            typesNonVidesList = list(holes.occupied_size_histogram.items())
            typesVidesList.sort()
            typesNonVidesList.sort()
            print ("VIDES     de ", typesVidesList[:3], " à ", typesVidesList[-1:])
            print ("NON VIDES de ", typesNonVidesList[:3], " à ", typesNonVidesList[-1:])
            print ("=============")
            print ("DÉFINITIONS MAX% 10d octets"%(defs.max_value))
            print ("DÉFINITIONS MIN% 10d octets"%(defs.min_value))
            print ("MOYENNE        % 10d octets"%(defs.mean))
            print ("ÉCART-TYPE     % 10d octets"%(defs.stddev))
        return True
        
if __name__ == '__main__':
    main()
