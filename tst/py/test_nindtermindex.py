"""Tests for NindTermindex.py (the .nindtermindex reader) against files
produced by NindIndexer._write_term_index (see nind_engine.py).

Term ids are direct array indices here (no hashing): each postings list is
delta-encoded by ascending document id, which is exactly what these tests
pin down.
"""
from nind.nind_engine import NindIndexer
from nind.NindTermindex import NindTermindex


def write_term_index(index_dir, inverted_index, max_term_id):
    indexer = NindIndexer(str(index_dir), prefix="corpus")
    indexer._write_term_index(inverted_index, max_term_id)
    return str(index_dir / "corpus.nindtermindex")


def test_single_term_round_trips(tmp_path):
    inverted_index = {1: [(2, 1), (5, 2)]}
    path = write_term_index(tmp_path, inverted_index, max_term_id=1)

    index = NindTermindex(path)
    result = index.donneListeTermesCG(1)
    assert len(result) == 1
    categorie, frequence_terme, docs = result[0]
    assert categorie == 0
    assert frequence_terme == 3          # sum of the two document frequencies
    assert docs == [(2, 1), (5, 2)]


def test_document_ids_with_gaps_survive_delta_encoding(tmp_path):
    inverted_index = {1: [(1, 1), (4, 2), (10, 3)]}
    path = write_term_index(tmp_path, inverted_index, max_term_id=1)

    index = NindTermindex(path)
    (_, _, docs) = index.donneListeTermesCG(1)[0]
    assert docs == [(1, 1), (4, 2), (10, 3)]


def test_multiple_terms_are_independent(tmp_path):
    inverted_index = {
        1: [(0, 5)],
        2: [(0, 1), (1, 1)],
        3: [(1, 9)],
    }
    path = write_term_index(tmp_path, inverted_index, max_term_id=3)

    index = NindTermindex(path)
    assert index.donneListeTermesCG(1)[0][2] == [(0, 5)]
    assert index.donneListeTermesCG(2)[0][2] == [(0, 1), (1, 1)]
    assert index.donneListeTermesCG(3)[0][2] == [(1, 9)]


def test_unknown_term_id_returns_empty_list(tmp_path):
    path = write_term_index(tmp_path, {1: [(0, 1)]}, max_term_id=1)
    index = NindTermindex(path)
    assert index.donneListeTermesCG(999) == []


def test_structural_indirection_is_consistent(tmp_path):
    path = write_term_index(tmp_path, {1: [(0, 1)], 5: [(2, 3)]}, max_term_id=5)
    index = NindTermindex(path)
    assert index.analyseFichierIndex(False) is True
