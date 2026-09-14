"""Tests for NindLocalindex.py (the .nindlocalindex reader) against files
produced by NindIndexer._write_local_index (see nind_engine.py).

Internally, documents are numbered 1..N (slot 0 unused) and translated
to/from the caller-facing external id (here, the 0-based position of the
document in the corpus); term ids inside one document are delta-encoded in
ascending order, independent of the order tokens appeared in the text.
"""
from nind.nind_engine import NindIndexer
from nind.NindLocalindex import NindLocalindex


def write_local_index(index_dir, corpus_tokens, term_to_id):
    indexer = NindIndexer(str(index_dir), prefix="corpus")
    indexer._write_local_index(corpus_tokens, term_to_id)
    return str(index_dir / "corpus.nindlocalindex")


def test_single_document_round_trips(tmp_path):
    term_to_id = {"alpha": 1, "beta": 2}
    path = write_local_index(tmp_path, [["alpha", "beta", "alpha"]], term_to_id)

    index = NindLocalindex(path)
    terms = index.donneListeTermes(0)
    assert [t[0] for t in terms] == [1, 2]          # ascending term id order
    alpha_entry, beta_entry = terms
    assert alpha_entry[2] == [(0, 1), (2, 1)]        # positions of "alpha"
    assert beta_entry[2] == [(1, 1)]                 # position of "beta"


def test_donneidentifiantsExternes_maps_external_to_internal_ids(tmp_path):
    term_to_id = {"a": 1, "b": 2, "c": 3}
    corpus_tokens = [["a", "b"], ["b", "c"]]
    path = write_local_index(tmp_path, corpus_tokens, term_to_id)

    index = NindLocalindex(path)
    assert sorted(index.donneidentifiantsExternes()) == [(0, 1), (1, 2)]


def test_multiple_documents_are_independent(tmp_path):
    term_to_id = {"a": 1, "b": 2, "c": 3}
    corpus_tokens = [["a", "b"], ["b", "c", "c"]]
    path = write_local_index(tmp_path, corpus_tokens, term_to_id)

    index = NindLocalindex(path)
    doc0_terms = {t[0]: t[2] for t in index.donneListeTermes(0)}
    doc1_terms = {t[0]: t[2] for t in index.donneListeTermes(1)}
    assert doc0_terms == {1: [(0, 1)], 2: [(1, 1)]}
    assert doc1_terms == {2: [(0, 1)], 3: [(1, 1), (2, 1)]}


def test_term_ids_are_returned_in_ascending_order_regardless_of_token_order(tmp_path):
    # Term idents are stored as signed deltas from the previous one; this
    # only round-trips correctly if a non-trivial, non-monotonic assignment
    # of ids to tokens still comes back sorted.
    term_to_id = {"z": 50, "a": 3, "m": 3000}
    path = write_local_index(tmp_path, [["z", "a", "m"]], term_to_id)

    index = NindLocalindex(path)
    terms = index.donneListeTermes(0)
    assert [t[0] for t in terms] == [3, 50, 3000]
    positions = {t[0]: t[2][0][0] for t in terms}
    assert positions == {3: 1, 50: 0, 3000: 2}       # original token positions


def test_unknown_external_id_returns_empty_list(tmp_path):
    path = write_local_index(tmp_path, [["alpha"]], {"alpha": 1})
    index = NindLocalindex(path)
    assert index.donneListeTermes(999) == []


def test_document_count_matches_number_of_documents_indexed(tmp_path):
    term_to_id = {"a": 1}
    path = write_local_index(tmp_path, [["a"], ["a"], ["a"]], term_to_id)
    index = NindLocalindex(path)
    assert len(index.donneidentifiantsExternes()) == 3
