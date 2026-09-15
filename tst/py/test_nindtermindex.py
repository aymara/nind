"""Tests for NindTermindex.py (the .nindtermindex reader) against files
produced by NindIndexer._write_term_index (see nind_engine.py).

Term ids are direct array indices here (no hashing): each postings list is
delta-encoded by ascending document id, which is exactly what these tests
pin down. These are structural tests of the term-index format alone, so an
arbitrary (but consistent between writer and reader) lexicon identification
stamp is used - it doesn't need to correspond to a real lexicon file.
"""
from nind import _native as native
from nind.nind_engine import NindIndexer
from nind.NindTermindex import NindTermindex

IDENTIFICATION = native.Identification(lexicon_words_nb=1, lexicon_time=1)


def write_term_index(index_dir, inverted_index):
    indexer = NindIndexer(str(index_dir), prefix="corpus")
    indexer._write_term_index(inverted_index, IDENTIFICATION)
    return str(index_dir / "corpus.nindtermindex")


def test_single_term_round_trips(tmp_path):
    inverted_index = {1: [(2, 1), (5, 2)]}
    path = write_term_index(tmp_path, inverted_index)

    index = NindTermindex(path, IDENTIFICATION)
    result = index.donneListeTermesCG(1)
    assert len(result) == 1
    term_cg = result[0]
    assert term_cg.cg == 0
    assert term_cg.frequency == 3          # sum of the two document frequencies
    assert [(d.ident, d.frequency) for d in term_cg.documents] == [(2, 1), (5, 2)]


def test_document_ids_with_gaps_survive_delta_encoding(tmp_path):
    inverted_index = {1: [(1, 1), (4, 2), (10, 3)]}
    path = write_term_index(tmp_path, inverted_index)

    index = NindTermindex(path, IDENTIFICATION)
    term_cg = index.donneListeTermesCG(1)[0]
    assert [(d.ident, d.frequency) for d in term_cg.documents] == [(1, 1), (4, 2), (10, 3)]


def test_multiple_terms_are_independent(tmp_path):
    inverted_index = {
        1: [(0, 5)],
        2: [(0, 1), (1, 1)],
        3: [(1, 9)],
    }
    path = write_term_index(tmp_path, inverted_index)

    index = NindTermindex(path, IDENTIFICATION)
    assert [(d.ident, d.frequency) for d in index.donneListeTermesCG(1)[0].documents] == [(0, 5)]
    assert [(d.ident, d.frequency) for d in index.donneListeTermesCG(2)[0].documents] == [(0, 1), (1, 1)]
    assert [(d.ident, d.frequency) for d in index.donneListeTermesCG(3)[0].documents] == [(1, 9)]


def test_unknown_term_id_returns_empty_list(tmp_path):
    path = write_term_index(tmp_path, {1: [(0, 1)]})
    index = NindTermindex(path, IDENTIFICATION)
    assert index.donneListeTermesCG(999) == []


def test_structural_indirection_is_consistent(tmp_path):
    path = write_term_index(tmp_path, {1: [(0, 1)], 5: [(2, 3)]})
    index = NindTermindex(path, IDENTIFICATION)
    assert index.analyseFichierIndex(False) is True


def test_structural_analysis_works_without_lexicon_identification(tmp_path):
    # analyseFichierPadFile/analyseFichierIndex are diagnostics that must
    # keep working even when the caller has no lexicon on hand (unlike
    # donneListeTermesCG, which requires lexicon_identification).
    path = write_term_index(tmp_path, {1: [(0, 1)], 5: [(2, 3)]})
    index = NindTermindex(path)
    assert index.analyseFichierIndex(False) is True
