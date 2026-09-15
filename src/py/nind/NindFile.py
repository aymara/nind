#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Low-level binary codec for nind's "Latecon" number/string encodings.

:class:`NindFile` is the base of every other class in this package: it wraps
a plain file handle and adds readers/writers for the primitive types used
throughout nind's binary formats (fixed-width big/little-endian integers,
variable-length "ULat"/"SLat" integers, and length-prefixed UTF-8 strings).
Every other reader (:class:`~nind.NindPadFile.NindPadFile`,
:class:`~nind.NindIndex.NindIndex`, ...) builds on top of it.

This module is read/write-oriented but does no structural interpretation of
the files it reads or writes - that is the job of :mod:`nind.NindPadFile`
and the classes built on it. See the ``<fichier>`` grammar comment below for
the primitive types it implements.
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
################################################################
# IMPORTANT : des essais ont ejtej faits avec une bufferisation des lectures comme pour NindLateconFile.cpp
# Rejsultats trehs dejcevants en temps de traitement.
# Nous gardons la lecture directe depuis le fichier et remercions la gestion du cache !
################################################################
import sys
import os

def usage():
    #print(sys.version)
    print ("""© l'ATEJCON.
Programme de test de la classe NindFile.
Cette classe permet l'accès aux fichiers binaires avec codages Latejcon.
Nind_testLateconNumber a écrit dans /tmp/Nind_testLateconNumber.lat la
taille des données sur un entier 3 bits puis un nombre N sous 4 formes : 
1) N en non signé, 2) N en signé, 3) -N en signé, 4) -N en non signé.
Le programme relit ce fichier (ou un autre) et relit les 4 nombres
latecon sous 2 interprétations : 1) non signée, 2) signée"

usage   : %s <fichier codage latecon>
exemple : %s /tmp/Nind_testLateconNumber.lat
"""%(sys.argv[0], sys.argv[0]))

def main():
    if len(sys.argv) < 2 :
        usage()
        sys.exit()
    latFileName = os.path.abspath(sys.argv[1])
    
    latFile = NindFile(latFileName)
    #taille du fichier
    latFile.seek(0, 2)
    taille = latFile.tell()
    print ("taille du fichier : %d"%(taille))
    #lit la taille
    latFile.seek(0, 0)
    taille = latFile.litNombre3()
    print ("taille=%d"%(taille))
    nonSignes = []
    signes = []
    #lit les nombres en non signé 
    for i in range(4): nonSignes.append(latFile.litNombreULat())
    #lit les nombres en non signé 
    latFile.seek(3, 0)
    for i in range(4): signes.append(latFile.litNombreSLat())
    #affiche
    for i in range(4): print ("U: %d S: %d"%(nonSignes[i], signes[i]))
    
def clefA(mot):
    """Compute the "A" hash key of a word (French: *clefA* = "key A").

    A simple rolling XOR/shift hash over the word's UTF-8 bytes. Used
    where nind needs a second, differently-shifted hash from the same word
    (`clefB` is the other one) - see the C++ ``NindLexicon`` header comments
    for the rationale.

    :param mot: the word to hash.
    :return: the hash as an unsigned integer.
    """
    mBytes = mot.encode('utf-8')
    clef = 0x55555555
    shifts = 0
    for octet in mBytes:
        clef ^= (octet << shifts%24)
        shifts += 7
    return clef

def clefB(mot):
    """Compute the "B" hash key of a word (French: *clefB* = "key B").

    Same idea as :func:`clefA` with different shift constants. This is the
    hash :class:`~nind.NindLexiconindex.NindLexiconindex` actually uses to
    bucket words: a word's entry lives at index ``clefB(mot) %
    nombreIndirection`` in the ``.nindlexiconindex`` file.

    :param mot: the word to hash.
    :return: the hash as an unsigned integer.
    """
    mBytes = mot.encode('utf-8')
    clef = 0x55555555
    shifts = 0
    for octet in mBytes: 
        clef ^= (octet << shifts%23)
        shifts += 5
    return clef

def catNb2Str(cat):
    """Translate a grammatical-category code into its short label.

    French: *donne la catégorie grammaticale en clair* = "give the
    grammatical category in clear [text]". The numeric codes are the
    ``<catégorie>`` values stored in ``.nindtermindex``/``.nindlocalindex``
    entries (Amose's term-type model, see ``LAT2015.JYS.448``).

    :param cat: the numeric category code.
    :return: the category's short label (e.g. ``"ADJ"``, ``"NC"``, ``"V"``),
        or ``''`` if ``cat`` is out of range.
    """
    catList = ["", "ADJ", "ADV", "CONJ", "DET", "DETERMINEUR", "DIVERS", "DIVERS_DATE", "EXCLAMATION", "INTERJ", "NC", "NOMBRE", "NP", "PART", "PONCTU", "PREP", "PRON", "V", "DIVERS_PARTICULE", "CLASS", "AFFIX"]
    if cat >= len(catList): return ''
    return catList[cat]
######################################################################################
# <fichier>               ::= { <Entier1> | <Entier2> | <Entier3> | <Entier4> | <Entier5> | <EntierULat> | <EntierULat> |
#                              <MotUtf8> | <Utf8> | <Octet> }
# <MotUtf8>               ::= <longueur> <Utf8>
# <longueur>              ::= <Entier1>
# <Utf8>                  ::= { <Octet> }
# <Entier1>               ::= <Octet>
# <Entier2>               ::= <Octet> <Octet>
# <Entier3>               ::= <Octet> <Octet> <Octet>
# <Entier4>               ::= <Octet> <Octet> <Octet> <Octet>
# <Entier5>               ::= <Octet> <Octet> <Octet> <Octet> <Octet>
# <EntierULat>            ::= { <Octet> }
# <EntierSLat>            ::= { <Octet> }
######################################################################################
class NindFile:
    """Binary file wrapper implementing nind's "Latecon" primitive codecs.

    Wraps a single OS file handle opened in one of three modes (read-only,
    write-from-scratch, or read/modify-in-place) and exposes matching pairs
    of ``litXxx`` (French *lit* = "reads") / ``ejcritXxx`` (French *écrit* =
    "writes", spelled with the project's "ej" transliteration of ``é``)
    methods for each primitive type in the grammar above: fixed-width
    1/3/4/5-byte integers (some little-endian/"petit-boutiste", some
    big-endian/"gros-boutiste" - see each method), the ``ULat``/``SLat``
    variable-length integer encodings, and length-prefixed or raw UTF-8
    byte strings.

    Every higher-level nind reader (:class:`~nind.NindPadFile.NindPadFile`
    and its subclasses) subclasses this and only adds *structure* on top -
    none of them re-implement byte-level (de)serialization.

    Usable as a context manager (``with NindFile(...) as f:``), which
    guarantees :meth:`close` is called.
    """

    def __init__(self, latFileName, enEjcriture = False, enModification = False):
        """Open ``latFileName`` in one of three modes.

        :param latFileName: path to the file to open.
        :param enEjcriture: if ``True``, open for writing (French
            *enÉcriture* = "in writing [mode]"); the file is truncated and
            written from scratch unless ``enModification`` is also set. If
            ``False`` (the default), the file is opened read-only.
        :param enModification: if ``True`` (and ``enEjcriture`` is also
            ``True``), open an *existing* file for in-place read/write
            modification (French *enModification* = "in modification
            [mode]") instead of truncating it.
        """
        self.latFileName = latFileName
        if not enEjcriture:
            #ouvre le fichier en lecture
            self.latFile = open(self.latFileName, 'rb')
            self.latFile.seek(0, 0)
        elif not enModification:
            #ejcrit le fichier completement
            self.latFile = open(self.latFileName, 'wb')
        else:
            #ouvre pour modifications
            self.latFile = open(self.latFileName, 'r+b')
            self.latFile.seek(0, 0)
        
    def seek(self, offset, from_what):
        """Move the file position, exactly like :meth:`io.IOBase.seek`.

        :param offset: byte offset, interpreted relative to ``from_what``.
        :param from_what: ``0`` = from the start, ``1`` = from the current
            position, ``2`` = from the end (same convention as the stdlib).
        """
        self.latFile.seek(offset, from_what)

    def tell(self):
        """Return the current byte position in the file."""
        return self.latFile.tell()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()
        return False

    def litNombre1(self):
        """Read and return an unsigned 1-byte integer (0..255)."""
        return ord(self.latFile.read(1))

    def litNombre2(self):
        """Read and return an unsigned 2-byte big-endian integer."""
        #gros-boutiste
        ba = bytes(self.latFile.read(2))
        return int( ba[0]*0x100 + ba[1] )
        #return (ba[0] <<8) + ba[1]

    def litNombre3(self):
        """Read and return an unsigned 3-byte little-endian integer."""
        #petit-boutiste
        ba = bytes(self.latFile.read(3))
        return int( (ba[2]*0x100 + ba[1])*0x100 + ba[0] )
        #return (((ba[2] <<8) + ba[1]) <<8) + ba[0]
        #return (ba[2] <<16) + (ba[1] <<8) + ba[0]

    def litNombreS3(self):
        """Read and return a signed (two's-complement) 3-byte little-endian integer."""
        #petit-boutiste
        ba = bytes(self.latFile.read(3))
        res = (ba[2]*0x100 + ba[1])*0x100 + ba[0]
        #res = (((ba[2] <<8) + ba[1]) <<8) + ba[0]
        #res = (ba[2] <<16) + (ba[1] <<8) + ba[0]
        if res < 0x800000: return int(res)
        return int( res - 0x1000000 )

    def litNombre4(self):
        """Read and return an unsigned 4-byte little-endian integer."""
        #petit-boutiste
        ba = bytes(self.latFile.read(4))
        return int( ((ba[3]*0x100 + ba[2])*0x100 + ba[1])*0x100 + ba[0] )
        #return (((((ba[3] <<8) + ba[2]) <<8) + ba[1]) <<8) + ba[0]
        #return (ba[3] <<24) + (ba[2] <<16) + (ba[1] <<8) + ba[0]

    def litNombreS4(self):
        """Read and return a signed (two's-complement) 4-byte little-endian integer."""
        #petit-boutiste
        ba = bytes(self.latFile.read(4))
        res = ((ba[3]*0x100 + ba[2])*0x100 + ba[1])*0x100 + ba[0]
        if res < 0x80000000: return int(res)
        return int(res - 0x100000000)

    def litNombre5(self):
        """Read and return an unsigned 5-byte big-endian integer.

        Used for file offsets (e.g. ``<offsetDéfinition>`` in index files),
        which need more than 4 bytes of range.
        """
        #gros-boutiste
        ba = bytes(self.latFile.read(5))
        return int( (((ba[0]*0x100 + ba[1])*0x100 + ba[2])*0x100 + ba[3])*0x100 + ba[4] )
        #return (((((((ba[0] <<8) + ba[1]) <<8) + ba[2]) <<8) + ba[3]) <<8) + ba[4]
        #return (ba[0] <<32) + (ba[1] <<24) + (ba[2] <<16) + (ba[3] <<8) + ba[4]

    def litNombreULat(self):
        """Read and return an unsigned "ULat" variable-length integer.

        This is nind's own variable-length encoding (French *Latecon*, the
        project's original code-name): the top bits of the first byte say
        how many extra continuation bytes follow (0 to 4), so small values
        take 1 byte and the encoding scales up to a full unsigned 32-bit
        value in 5 bytes. Used wherever a count or a delta is expected to
        usually be small (term/document frequencies, relative doc ids, ...).

        :raises Exception: if the encoding is malformed.
        """
        octet = ord(self.latFile.read(1))
        if not octet&0x80: return octet
        result = ord(self.latFile.read(1))
        if not octet&0x40: return (octet&0x3F) * 0x100 + result
        result = result * 0x100 + ord(self.latFile.read(1))
        if not octet&0x20: return (octet&0x1F) * 0x10000 + result
        result = result * 0x100 + ord(self.latFile.read(1))
        if not octet&0x10: return (octet&0x0F) * 0x1000000 + result
        result = result * 0x100 + ord(self.latFile.read(1))
        if not octet&0x08: return result
        raise Exception('entier Ulatecon invalide à %08X'%(self.latFile.tell()))

    def litNombreSLat(self):
        """Read and return a signed "SLat" variable-length integer.

        The signed counterpart of :meth:`litNombreULat`: same
        self-describing variable-length scheme, but reserving one bit per
        tier for the sign so it can encode negative deltas (e.g. relative
        term-id or position deltas that can go backwards).

        :raises Exception: if the encoding is malformed.
        """
        octet = ord(self.latFile.read(1))
        if octet&0xC0 == 0x00: return octet
        if octet&0xC0 == 0x40: return octet - 0x80
        result = ord(self.latFile.read(1))
        if octet&0x60 == 0x00: return (octet&0x3F) * 0x100 + result
        if octet&0x60 == 0x20: return (octet&0x3F) * 0x100 + result - 0x4000
        result = result * 0x100 + ord(self.latFile.read(1))
        if octet&0x30 == 0x00: return (octet&0x1F) * 0x10000 + result
        if octet&0x30 == 0x10: return (octet&0x1F) * 0x10000 + result - 0x200000
        result = result * 0x100 + ord(self.latFile.read(1))
        if octet&0x18 == 0x00: return (octet&0x0F) * 0x1000000 + result
        if octet&0x18 == 0x08: return (octet&0x0F) * 0x1000000 + result - 0x10000000
        result = result * 0x100 + ord(self.latFile.read(1))
        if (octet&0x08 == 0x00) and (result&0x80000000 == 0x00000000): return result
        if (octet&0x08 == 0x00) and (result&0x80000000 == 0x80000000): return result - 0x0100000000
        raise Exception('entier Slatecon invalide à %08X'%(self.latFile.tell()))
    
    def litString(self):
        """Read a length-prefixed UTF-8 string (French *litString*, mixed EN/FR name).

        Reads one length byte (0..255) followed by that many bytes,
        decoded as UTF-8 - i.e. the ``<MotUtf8>`` grammar rule.

        :return: the decoded string.
        """
        longueur = ord(self.latFile.read(1))
        #return self.latFile.read(longueur).decode('utf-8')
        return bytes(self.latFile.read(longueur)).decode()

    def litChaine(self, longueur):
        """Read ``longueur`` bytes and decode them as UTF-8 (French *litChaine* = "reads string").

        Like :meth:`litString` but the caller supplies the length (already
        read separately), rather than a leading length byte.

        :param longueur: number of bytes to read.
        :return: the decoded string.
        """
        #return self.latFile.read(longueur).decode('utf-8')
        return bytes(self.latFile.read(longueur)).decode()

    def litOctets(self, longueur):
        """Read and return ``longueur`` raw bytes, undecoded (French *litOctets* = "reads bytes")."""
        return bytes(self.latFile.read(longueur))

    def ejcritNombre1(self, entier):
        """Write ``entier`` as an unsigned 1-byte integer.

        :param entier: value in 0..255.
        """
        ba = bytearray(1)
        ba[0] = entier&0xFF
        self.latFile.write(ba)

    def ejcritNombre3(self, entier):
        """Write ``entier`` as an unsigned 3-byte little-endian integer (see :meth:`litNombre3`).

        :param entier: value in 0..0xFFFFFF.
        """
        ba = bytearray(3)
        #petit-boutiste
        ba[0] = entier&0xFF
        ba[1] = (entier//0x100)&0xFF
        ba[2] = (entier//0x10000)&0xFF
        self.latFile.write(ba)

    def ejcritNombre4(self, entier):
        """Write ``entier`` as an unsigned 4-byte little-endian integer (see :meth:`litNombre4`).

        :param entier: value in 0..0xFFFFFFFF.
        """
        ba = bytearray(4)
        #petit-boutiste
        ba[0] = entier&0xFF
        ba[1] = (entier//0x100)&0xFF
        ba[2] = (entier//0x10000)&0xFF
        ba[3] = (entier//0x1000000)&0xFF
        self.latFile.write(ba)

    def ejcritNombre5(self, entier):
        """Write ``entier`` as an unsigned 5-byte big-endian integer (see :meth:`litNombre5`).

        :param entier: value in 0..0xFFFFFFFFFF.
        """
        ba = bytearray(5)
        #gros-boutiste
        ba[0] = (entier//0x100000000)&0xFF
        ba[1] = (entier//0x1000000)&0xFF
        ba[2] = (entier//0x10000)&0xFF
        ba[3] = (entier//0x100)&0xFF
        ba[4] = entier&0xFF
        self.latFile.write(ba)

    def ejcritNombreULat(self, entier):
        """Write ``entier`` using the unsigned "ULat" variable-length encoding (see :meth:`litNombreULat`).

        :param entier: non-negative value; must fit in 32 bits.
        :raises ValueError: if ``entier`` is too large to encode.
        """
        if entier <= 0x7F:
            self.latFile.write(bytes([entier]))
        elif entier <= 0x3FFF:
            self.latFile.write(bytes([0x80 | ((entier>>8)&0x3F), entier&0xFF]))
        elif entier <= 0x1FFFFF:
            self.latFile.write(bytes([0xC0 | ((entier>>16)&0x1F), (entier>>8)&0xFF, entier&0xFF]))
        elif entier <= 0xFFFFFFF:
            self.latFile.write(bytes([0xE0 | ((entier>>24)&0x0F), (entier>>16)&0xFF, (entier>>8)&0xFF, entier&0xFF]))
        elif entier <= 0xFFFFFFFF:
            self.latFile.write(bytes([0xF0, (entier>>24)&0xFF, (entier>>16)&0xFF, (entier>>8)&0xFF, entier&0xFF]))
        else:
            raise ValueError('entier trop grand pour un codage ULat: %d'%(entier))

    def ejcritNombreSLat(self, entier):
        """Write ``entier`` using the signed "SLat" variable-length encoding (see :meth:`litNombreSLat`).

        :param entier: signed value; must fit in 32 bits.
        :raises ValueError: if ``entier`` is too large to encode.
        """
        if -64 <= entier <= 63:
            octet = entier if entier >= 0 else entier + 0x80
            self.latFile.write(bytes([octet]))
        elif -8192 <= entier <= 8191:
            if entier >= 0:
                top, b2 = (entier>>8)&0x1F, entier&0xFF
            else:
                combined = entier + 0x4000
                top, b2 = (combined>>8)&0x3F, combined&0xFF
            self.latFile.write(bytes([0x80|top, b2]))
        elif -1048576 <= entier <= 1048575:
            if entier >= 0:
                top, b2, b3 = (entier>>16)&0x0F, (entier>>8)&0xFF, entier&0xFF
            else:
                combined = entier + 0x200000
                top, b2, b3 = (combined>>16)&0x1F, (combined>>8)&0xFF, combined&0xFF
            self.latFile.write(bytes([0xC0|top, b2, b3]))
        elif -134217728 <= entier <= 134217727:
            if entier >= 0:
                top, b2, b3, b4 = (entier>>24)&0x07, (entier>>16)&0xFF, (entier>>8)&0xFF, entier&0xFF
            else:
                combined = entier + 0x10000000
                top, b2, b3, b4 = (combined>>24)&0x0F, (combined>>16)&0xFF, (combined>>8)&0xFF, combined&0xFF
            self.latFile.write(bytes([0xE0|top, b2, b3, b4]))
        elif -2147483648 <= entier <= 2147483647:
            combined = entier & 0xFFFFFFFF
            self.latFile.write(bytes([0xF0, (combined>>24)&0xFF, (combined>>16)&0xFF, (combined>>8)&0xFF, combined&0xFF]))
        else:
            raise ValueError('entier trop grand pour un codage SLat: %d'%(entier))

    def ejcritChaine(self, chaine):
        """Write ``chaine`` as raw UTF-8 bytes, with no length prefix (French *ejcritChaine* = "writes string").

        Callers that need a length prefix write it separately (see how
        :class:`~nind.nind_engine.NindIndexer` pairs this with
        :meth:`ejcritNombre1` of ``len(encoded)`` to build a ``<MotUtf8>``).

        :param chaine: the string to write.
        """
        self.latFile.write(chaine.encode('utf-8'))

    def ejcritZejros(self, taille):
        """Write ``taille`` zero bytes (French *ejcritZéros* = "writes zeros").

        Used to reserve/pad a region (e.g. an indirection table or the
        specifics+identification trailer) before patching it with real
        values once their final offsets are known.

        :param taille: number of zero bytes to write.
        """
        self.latFile.write(bytearray(taille))

    def close(self):
        """Close the underlying file handle."""
        self.latFile.close()

       
