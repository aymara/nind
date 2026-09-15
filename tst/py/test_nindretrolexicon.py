"""Tests for NindRetrolexicon.py.

Unlike the other formats, nind_engine.py has no writer for .nindretrolexicon
(it isn't needed by NindEngine's simple flat lexicon), so there is no
existing Python writer to reuse here. The fixture below builds a minimal,
valid file directly from NindFile's binary primitives, following the format
documented at the top of NindRetrolexicon.py:

    <fichier>       ::= <tailleEntreje> <tailleSpejcifiques>
                        <flagIndexej> <addrBlocSuivant> <nombreIndex> { <dejfinitionMot> }
                        { <Utf8> }                                    (blocEnVrac)
                        <flagSpecifique> <flagIdentification> <maxIdentifiant> <identifieurUnique>
    <dejfinitionMot> ::= <flagComposej=31> <identifiantA> <identifiantS>   (9 bytes)
                       | <flagSimple=37> <longueurMotUtf8> <adresseMotUtf8> (7 of 9 bytes, zero-padded)

Idents are direct array indices (fixed 9-byte slots), like NindTermindex /
NindLocalindex, not hashed like NindLexiconindex.
"""
import time

from nind.NindFile import NindFile
from nind.NindRetrolexicon import NindRetrolexicon

FLAG_INDEXEJ = 47
FLAG_SPEJCIFIQUE = 57
FLAG_IDENTIFICATION = 53
FLAG_COMPOSEJ = 31
FLAG_SIMPLE = 37
TAILLE_ENTREJE = 9


def write_retrolexicon(path, words_by_ident):
    """words_by_ident: {ident: ("simple", word) | ("compound", identA, identS)}"""
    max_ident = max(words_by_ident) if words_by_ident else 0
    nombre_index = max_ident + 1

    nf = NindFile(path, enEjcriture=True)
    nf.ejcritNombre1(TAILLE_ENTREJE)
    nf.ejcritNombre3(0)                     # tailleSpejcifiques : vide
    nf.ejcritNombre1(FLAG_INDEXEJ)
    nf.ejcritNombre5(0)                     # addrBlocSuivant : un seul bloc
    nf.ejcritNombre3(nombre_index)
    slot_table_offset = nf.tell()
    nf.ejcritZejros(nombre_index * TAILLE_ENTREJE)   # slots vides par defaut

    utf8_offset_by_ident = {}
    for ident, entry in sorted(words_by_ident.items()):
        if entry[0] == "simple":
            utf8_offset_by_ident[ident] = nf.tell()
            nf.ejcritChaine(entry[1])

    nf.ejcritNombre1(FLAG_SPEJCIFIQUE)
    nf.ejcritNombre1(FLAG_IDENTIFICATION)
    nf.ejcritNombre4(max_ident)
    nf.ejcritNombre4(int(time.time()))

    for ident, entry in words_by_ident.items():
        nf.seek(slot_table_offset + ident * TAILLE_ENTREJE, 0)
        if entry[0] == "simple":
            encoded = entry[1].encode("utf-8")
            nf.ejcritNombre1(FLAG_SIMPLE)
            nf.ejcritNombre1(len(encoded))
            nf.ejcritNombre5(utf8_offset_by_ident[ident])
        else:
            _, identA, identS = entry
            nf.ejcritNombre1(FLAG_COMPOSEJ)
            nf.ejcritNombre4(identA)
            nf.ejcritNombre4(identS)
    nf.close()


def test_simple_word_round_trips(tmp_path):
    path = str(tmp_path / "simple.nindretrolexicon")
    write_retrolexicon(path, {1: ("simple", "alpha")})

    retro = NindRetrolexicon(path)
    assert retro.donneMot(1) == ["alpha"]


def test_two_component_compound_round_trips(tmp_path):
    path = str(tmp_path / "compound2.nindretrolexicon")
    # ident 3 = "alpha_beta": identA points at the "alpha" prefix, identS at
    # the newly-appended simple word "beta".
    write_retrolexicon(path, {
        1: ("simple", "alpha"),
        2: ("simple", "beta"),
        3: ("compound", 1, 2),
    })

    retro = NindRetrolexicon(path)
    assert retro.donneMot(3) == ["alpha", "beta"]


def test_three_component_compound_round_trips(tmp_path):
    path = str(tmp_path / "compound3.nindretrolexicon")
    write_retrolexicon(path, {
        1: ("simple", "alpha"),
        2: ("simple", "beta"),
        3: ("simple", "gamma"),
        4: ("compound", 1, 2),   # alpha_beta
        5: ("compound", 4, 3),   # alpha_beta_gamma
    })

    retro = NindRetrolexicon(path)
    assert retro.donneMot(5) == ["alpha", "beta", "gamma"]


def test_unknown_ident_returns_empty_word(tmp_path):
    path = str(tmp_path / "unknown.nindretrolexicon")
    write_retrolexicon(path, {1: ("simple", "alpha")})
    retro = NindRetrolexicon(path)
    assert retro.donneMot(999) == []


def test_empty_slot_within_range_returns_empty_word(tmp_path):
    # ident 2 is inside the allocated range (0..2) but was never written to,
    # so its slot stays all-zero.
    path = str(tmp_path / "gap.nindretrolexicon")
    write_retrolexicon(path, {0: ("simple", "alpha"), 2: ("simple", "gamma")})
    retro = NindRetrolexicon(path)
    assert retro.donneMot(1) == []
    assert retro.donneMot(0) == ["alpha"]
    assert retro.donneMot(2) == ["gamma"]


def test_structural_analysis_works_without_lexicon_identification(tmp_path):
    # analyseFichierPadFile (inherited, now nind._native-backed) is a
    # diagnostic that must keep working even without a lexicon on hand.
    path = str(tmp_path / "analyse.nindretrolexicon")
    write_retrolexicon(path, {1: ("simple", "alpha"), 2: ("simple", "beta")})
    retro = NindRetrolexicon(path)
    assert retro.analyseFichierPadFile(False) is True
