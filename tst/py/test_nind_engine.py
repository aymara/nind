"""End-to-end tests for nind_engine.py: NindIndexer writes a small corpus to
real .nindlexiconindex/.nindtermindex/.nindlocalindex files, and NindEngine
reads them back through the public search API.

This is the same path exercised manually while debugging the original
AttributeError/format bugs in nind_engine.py; these tests pin that behaviour
down permanently.
"""
import pytest

from nind.nind_engine import NindEngine, NindIndexer


def make_corpus(tmp_path):
    """3 documents with a known, hand-countable vocabulary:
    doc 0: "cat cat dog" (length 3)
    doc 1: "dog dog dog" (length 3)
    doc 2: "bird"        (length 1)
    """
    contents = ["cat cat dog", "dog dog dog", "bird"]
    paths = []
    for i, text in enumerate(contents):
        path = tmp_path / f"doc{i}.txt"
        path.write_text(text, encoding="utf-8")
        paths.append(str(path))
    return paths


@pytest.fixture
def engine(tmp_path):
    index_dir = tmp_path / "indices"
    index_dir.mkdir()
    docs_dir = tmp_path / "docs"
    docs_dir.mkdir()
    file_paths = make_corpus(docs_dir)

    indexer = NindIndexer(index_dir=str(index_dir), prefix="corpus")
    indexer.index_files(file_paths)
    return NindEngine(str(index_dir))


def test_total_docs_and_avg_doc_len(engine):
    assert engine.total_docs == 3
    assert engine.avg_doc_len == pytest.approx(7 / 3)


def test_get_term_id_known_and_unknown_words(engine):
    assert engine.get_term_id("cat") != 0
    assert engine.get_term_id("dog") != 0
    assert engine.get_term_id("bird") != 0
    assert engine.get_term_id("cat") != engine.get_term_id("dog") != engine.get_term_id("bird")
    assert engine.get_term_id("fish") == 0


def test_term_frequency_per_document(engine):
    assert engine.get_tf("cat", 0) == 2
    assert engine.get_tf("dog", 0) == 1
    assert engine.get_tf("dog", 1) == 3
    assert engine.get_tf("bird", 2) == 1
    assert engine.get_tf("cat", 1) == 0
    assert engine.get_tf("fish", 0) == 0


def test_document_frequency(engine):
    assert engine.get_df("dog") == 2      # appears in doc 0 and doc 1
    assert engine.get_df("cat") == 1
    assert engine.get_df("bird") == 1
    assert engine.get_df("fish") == 0


def test_document_length(engine):
    assert engine.get_doc_len(0) == 3
    assert engine.get_doc_len(1) == 3
    assert engine.get_doc_len(2) == 1


def test_search_ranks_higher_term_frequency_first_at_equal_doc_length(engine):
    # doc 0 and doc 1 have the same length and the same idf("dog"); only the
    # term frequency differs (1 vs 3), so BM25 must rank doc 1 first.
    results = engine.search("dog")
    doc_ids = [doc_id for doc_id, _ in results]
    assert doc_ids == [1, 0]
    assert results[0][1] > results[1][1] > 0


def test_search_single_matching_document(engine):
    results = engine.search("cat")
    assert [doc_id for doc_id, _ in results] == [0]
    assert results[0][1] > 0


def test_search_unknown_term_returns_no_results(engine):
    assert engine.search("fish") == []


def test_search_multi_term_query_excludes_non_matching_documents(engine):
    results = engine.search("dog cat")
    doc_ids = {doc_id for doc_id, _ in results}
    assert doc_ids == {0, 1}      # doc 2 ("bird") matches neither term
    assert all(score > 0 for _, score in results)


def test_search_respects_top_k(engine):
    results = engine.search("dog", top_k=1)
    assert len(results) == 1


def test_underlying_files_are_structurally_consistent(engine):
    engine.lexicon.analyseFichierLexiconindex(False)   # raises on inconsistency
    assert engine.term_index.analyseFichierIndex(False) is True
    assert engine.local_index.analyseFichierIndex(False) is True


def test_indexing_into_a_missing_directory_reports_the_missing_lexicon(tmp_path):
    with pytest.raises(FileNotFoundError):
        NindEngine(str(tmp_path))   # empty directory, no .nindlexiconindex


@pytest.mark.parametrize("text, expected_tokens", [
    ("AuthenticationManager", ["Authentication", "Manager"]),
    ("snake_case_word", ["snake", "case", "word"]),
    ("term123", ["term", "123"]),
    ("XMLHttpRequest", ["XML", "Http", "Request"]),
    ("", []),
])
def test_tokenize_splits_identifiers_like_source_code(text, expected_tokens):
    # tokenize() doesn't use any instance state, so it can be exercised
    # without a real index directory.
    engine = NindEngine.__new__(NindEngine)
    assert engine.tokenize(text) == expected_tokens
