"""Tests for NindLocalindex.py (the .nindlocalindex reader) against files
produced by NindIndexer._write_local_index (see nind_engine.py).

Documents are addressed by their caller-facing external id (here, the
0-based position of the document in the corpus) - nind._native handles the
external <-> internal id translation internally. Term ids inside one
document are delta-encoded in ascending order, independent of the order
tokens appeared in the text. These are structural tests of the local-index
format alone, so an arbitrary (but consistent between writer and reader)
lexicon identification stamp is used - it doesn't need to correspond to a
real lexicon file.
"""
from nind import _native as native
from nind.nind_engine import NindIndexer
from nind.NindLocalindex import NindLocalindex

IDENTIFICATION = native.Identification(lexicon_words_nb=1, lexicon_time=1)


def write_local_index(index_dir, corpus_tokens, term_to_id):
    indexer = NindIndexer(str(index_dir), prefix="corpus")
    indexer._write_local_index(corpus_tokens, term_to_id, IDENTIFICATION)
    return str(index_dir / "corpus.nindlocalindex")


def test_single_document_round_trips(tmp_path):
    term_to_id = {"alpha": 1, "beta": 2}
    path = write_local_index(tmp_path, [["alpha", "beta", "alpha"]], term_to_id)

    index = NindLocalindex(path, IDENTIFICATION)
    terms = index.donneListeTermes(0)
    assert [t.term for t in terms] == [1, 2]          # ascending term id order
    alpha_entry, beta_entry = terms
    assert [(loc.position, loc.length) for loc in alpha_entry.localisation] == [(0, 1), (2, 1)]
    assert [(loc.position, loc.length) for loc in beta_entry.localisation] == [(1, 1)]


def test_donneidentifiantsExternes_maps_external_to_internal_ids(tmp_path):
    term_to_id = {"a": 1, "b": 2, "c": 3}
    corpus_tokens = [["a", "b"], ["b", "c"]]
    path = write_local_index(tmp_path, corpus_tokens, term_to_id)

    index = NindLocalindex(path, IDENTIFICATION)
    assert sorted(index.donneidentifiantsExternes()) == [(0, 1), (1, 2)]


def test_multiple_documents_are_independent(tmp_path):
    term_to_id = {"a": 1, "b": 2, "c": 3}
    corpus_tokens = [["a", "b"], ["b", "c", "c"]]
    path = write_local_index(tmp_path, corpus_tokens, term_to_id)

    index = NindLocalindex(path, IDENTIFICATION)
    doc0_terms = {t.term: [(loc.position, loc.length) for loc in t.localisation] for t in index.donneListeTermes(0)}
    doc1_terms = {t.term: [(loc.position, loc.length) for loc in t.localisation] for t in index.donneListeTermes(1)}
    assert doc0_terms == {1: [(0, 1)], 2: [(1, 1)]}
    assert doc1_terms == {2: [(0, 1)], 3: [(1, 1), (2, 1)]}


def test_term_ids_are_returned_in_ascending_order_regardless_of_token_order(tmp_path):
    # Term idents are stored as signed deltas from the previous one; this
    # only round-trips correctly if a non-trivial, non-monotonic assignment
    # of ids to tokens still comes back sorted.
    term_to_id = {"z": 50, "a": 3, "m": 3000}
    path = write_local_index(tmp_path, [["z", "a", "m"]], term_to_id)

    index = NindLocalindex(path, IDENTIFICATION)
    terms = index.donneListeTermes(0)
    assert [t.term for t in terms] == [3, 50, 3000]
    positions = {t.term: t.localisation[0].position for t in terms}
    assert positions == {3: 1, 50: 0, 3000: 2}       # original token positions


def test_unknown_external_id_returns_empty_list(tmp_path):
    path = write_local_index(tmp_path, [["alpha"]], {"alpha": 1})
    index = NindLocalindex(path, IDENTIFICATION)
    assert index.donneListeTermes(999) == []


def test_document_count_matches_number_of_documents_indexed(tmp_path):
    term_to_id = {"a": 1}
    path = write_local_index(tmp_path, [["a"], ["a"], ["a"]], term_to_id)
    index = NindLocalindex(path, IDENTIFICATION)
    assert len(index.donneidentifiantsExternes()) == 3


def test_structural_analysis_is_consistent(tmp_path):
    term_to_id = {"a": 1, "b": 2}
    path = write_local_index(tmp_path, [["a", "b"], ["b"]], term_to_id)
    index = NindLocalindex(path, IDENTIFICATION)
    assert index.analyseFichierIndex(False) is True


def test_structural_analysis_works_without_lexicon_identification(tmp_path):
    # analyseFichierPadFile/analyseFichierIndex are diagnostics that must
    # keep working even when the caller has no lexicon on hand (unlike
    # donneListeTermes, which requires lexicon_identification).
    term_to_id = {"a": 1, "b": 2}
    path = write_local_index(tmp_path, [["a", "b"], ["b"]], term_to_id)
    index = NindLocalindex(path)
    assert index.analyseFichierIndex(False) is True
