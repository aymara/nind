"""End-to-end tests for nind_engine.py: NindIndexer writes a small corpus to
real .nindlexiconindex/.nindtermindex/.nindlocalindex files, and NindEngine
reads them back through the public search API.

This is the same path exercised manually while debugging the original
AttributeError/format bugs in nind_engine.py; these tests pin that behaviour
down permanently.
"""
import os
import time

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


def test_indexer_handles_term_repeated_over_255_times(tmp_path):
    # the local index stores at most 255 positions per entry: longer runs are split, not rejected
    index_dir = tmp_path / "indices"
    index_dir.mkdir()
    doc = tmp_path / "doc.txt"
    doc.write_text("self " * 600 + "other", encoding="utf-8")
    NindIndexer(index_dir=str(index_dir), prefix="corpus").index_files([str(doc)])
    engine = NindEngine(str(index_dir))
    assert engine.get_tf("self", 0) == 600
    assert engine.get_doc_len(0) == 601


def test_index_files_replaces_an_existing_index(tmp_path):
    index_dir = tmp_path / "indices"
    index_dir.mkdir()
    first, second = tmp_path / "first.txt", tmp_path / "second.txt"
    first.write_text("cat dog", encoding="utf-8")
    second.write_text("bird fish", encoding="utf-8")
    indexer = NindIndexer(index_dir=str(index_dir), prefix="corpus")
    indexer.index_files([str(first)])
    indexer.index_files([str(second)])
    engine = NindEngine(str(index_dir))
    assert engine.total_docs == 1
    assert engine.get_term_id("cat") == 0
    assert engine.get_tf("bird", 0) == 1


def generations(index_dir):
    return sorted(p for p in index_dir.iterdir() if p.is_dir())


def index_files_of(index_dir):
    """Files of the only generation of index_dir (checking it's the published one)."""
    [generation] = generations(index_dir)
    assert (index_dir / "corpus.CURRENT").read_text().strip() == generation.name
    return sorted(p.name for p in generation.iterdir())


SHARDED_TEXTS = ["cat cat dog", "dog dog dog", "bird", "cat bird fish", "fish fish",
                 "dog cat", "eel", "cat dog bird fish eel"]


def build_engine(tmp_path, name, texts, **kwargs):
    index_dir = tmp_path / name
    index_dir.mkdir()
    paths = []
    for i, text in enumerate(texts):
        path = tmp_path / ("%s-doc%d.txt" % (name, i))
        path.write_text(text, encoding="utf-8")
        paths.append(str(path))
    NindIndexer(index_dir=str(index_dir), prefix="corpus").index_files(paths, **kwargs)
    return NindEngine(str(index_dir))


@pytest.mark.parametrize("shards", [2, 3, 20])
def test_sharded_index_equals_direct_index(tmp_path, shards):
    direct = build_engine(tmp_path, "direct", SHARDED_TEXTS)
    sharded = build_engine(tmp_path, "sharded", SHARDED_TEXTS, shards=shards, workers=2)
    assert sharded.total_docs == direct.total_docs == len(SHARDED_TEXTS)
    assert sharded.avg_doc_len == pytest.approx(direct.avg_doc_len)
    for term in ["cat", "dog", "bird", "fish", "eel", "unknown"]:
        assert sharded.get_df(term) == direct.get_df(term)
        for doc_id in range(len(SHARDED_TEXTS)):
            assert sharded.get_tf(term, doc_id) == direct.get_tf(term, doc_id)
    for doc_id in range(len(SHARDED_TEXTS)):
        assert sharded.get_doc_len(doc_id) == direct.get_doc_len(doc_id)
    for query in ["cat", "dog fish", "eel bird"]:
        assert sharded.search(query) == pytest.approx(direct.search(query))
    # the shards were built in a temporary directory inside the new generation, and removed
    assert index_files_of(tmp_path / "sharded") == \
        ["corpus.nindlexiconindex", "corpus.nindlocalindex", "corpus.nindtermindex", "generation.lock"]


def write_texts(directory, texts):
    directory.mkdir(exist_ok=True)
    paths = []
    for i, text in enumerate(texts):
        path = directory / ("doc%d.txt" % i)
        path.write_text(text, encoding="utf-8")
        paths.append(str(path))
    return paths


def test_engine_keeps_working_across_rebuilds_and_switches_by_itself(tmp_path):
    index_dir = tmp_path / "indices"
    index_dir.mkdir()
    indexer = NindIndexer(str(index_dir), prefix="corpus")
    indexer.index_files(write_texts(tmp_path / "v1", ["cat dog", "dog"]))
    engine = NindEngine(str(index_dir))
    assert engine.get_df("dog") == 2

    indexer.index_files(write_texts(tmp_path / "v2", ["bird", "bird fish", "fish"]))
    # no reopen needed: the next call answers from the new build
    assert engine.total_docs == 3
    assert engine.get_df("dog") == 0
    assert engine.get_df("fish") == 2
    # and the previous generation was deleted, since nobody was opening it
    assert index_files_of(index_dir) == ["corpus.nindlexiconindex", "corpus.nindlocalindex",
                                         "corpus.nindtermindex", "generation.lock"]


def test_objects_taken_from_an_engine_keep_reading_their_build_after_a_rebuild(tmp_path):
    index_dir = tmp_path / "indices"
    index_dir.mkdir()
    indexer = NindIndexer(str(index_dir), prefix="corpus")
    indexer.index_files(write_texts(tmp_path / "v1", ["cat dog", "dog"]))
    old_term_index = NindEngine(str(index_dir)).term_index
    old_lexicon = NindEngine(str(index_dir)).lexicon
    indexer.index_files(write_texts(tmp_path / "v2", ["bird"]))
    # its generation is gone from the directory, but its open files still read (POSIX)
    dog = old_lexicon.donneIdentifiant(["dog"])
    assert sum(len(cg.documents) for cg in old_term_index.donneListeTermesCG(dog)) == 2


def test_failed_indexing_leaves_the_published_index_untouched(tmp_path):
    index_dir = tmp_path / "indices"
    index_dir.mkdir()
    indexer = NindIndexer(str(index_dir), prefix="corpus")
    indexer.index_files(write_texts(tmp_path / "v1", ["cat dog", "dog"]))
    engine = NindEngine(str(index_dir))
    before = index_files_of(index_dir)
    with pytest.raises(FileNotFoundError):
        indexer.index_files([str(tmp_path / "missing.txt")])
    assert index_files_of(index_dir) == before          # failed generation removed, CURRENT unchanged
    assert engine.get_df("dog") == 2
    assert NindEngine(str(index_dir)).get_df("dog") == 2


def test_concurrent_indexing_of_the_same_index_is_serialized(tmp_path):
    import threading
    index_dir = tmp_path / "indices"
    index_dir.mkdir()
    words = ["alpha", "beta", "gamma", "delta"]
    corpora = [write_texts(tmp_path / ("v%d" % i), [words[i]] * (i + 1)) for i in range(4)]
    errors = []

    def build(paths):
        try:
            NindIndexer(str(index_dir), prefix="corpus").index_files(paths, workers=1)
        except Exception as e:      # pragma: no cover - reported below
            errors.append(e)

    threads = [threading.Thread(target=build, args=(paths,)) for paths in corpora]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert errors == []
    # exactly one complete build is published: whichever ran last
    engine = NindEngine(str(index_dir))
    assert engine.total_docs in (1, 2, 3, 4)
    assert engine.get_df(words[engine.total_docs - 1]) == engine.total_docs
    assert len(generations(index_dir)) == 1


def test_engine_is_safe_from_several_threads_during_rebuilds(tmp_path):
    import threading
    index_dir = tmp_path / "indices"
    index_dir.mkdir()
    indexer = NindIndexer(str(index_dir), prefix="corpus")
    small = write_texts(tmp_path / "small", ["cat dog"] * 3)
    large = write_texts(tmp_path / "large", ["cat dog"] * 7)
    indexer.index_files(small)
    engine = NindEngine(str(index_dir))
    stop = threading.Event()
    errors = []

    def query():
        while not stop.is_set():
            try:
                # every answer comes from one complete build: 3 or 7 documents, never a mix
                results = engine.search("cat")
                assert len(results) in (3, 7)
                assert engine.get_df("dog") in (3, 7)
            except Exception as e:  # pragma: no cover - reported below
                errors.append(e)
                return

    readers = [threading.Thread(target=query) for _ in range(4)]
    for t in readers:
        t.start()
    for i in range(6):
        indexer.index_files(large if i % 2 == 0 else small)
    stop.set()
    for t in readers:
        t.join()
    assert errors == []
    assert engine.total_docs == 3


def test_index_in_the_flat_layout_of_older_versions_is_read_then_replaced(tmp_path):
    index_dir = tmp_path / "indices"
    index_dir.mkdir()
    # an index written before generations existed: flat files in index_dir
    old = NindIndexer(str(index_dir), prefix="corpus")
    old._write_index([old._tokenize_file(p) for p in write_texts(tmp_path / "v1", ["cat dog", "dog"])],
                     0, str(index_dir / "corpus"))
    engine = NindEngine(str(index_dir))
    assert engine.get_df("dog") == 2

    NindIndexer(str(index_dir), prefix="corpus").index_files(write_texts(tmp_path / "v2", ["bird"]))
    assert engine.get_df("bird") == 1                       # switched to the published generation
    assert not list(index_dir.glob("corpus.nind*"))         # flat files of the old index removed
    assert NindEngine(str(index_dir)).total_docs == 1


def test_engine_needs_a_prefix_only_when_several_indexes_share_a_directory(tmp_path):
    index_dir = tmp_path / "indices"
    index_dir.mkdir()
    NindIndexer(str(index_dir), prefix="a").index_files(write_texts(tmp_path / "a", ["cat"]))
    NindIndexer(str(index_dir), prefix="b").index_files(write_texts(tmp_path / "b", ["dog", "dog"]))
    with pytest.raises(ValueError):
        NindEngine(str(index_dir))
    assert NindEngine(str(index_dir), prefix="a").get_df("cat") == 1
    assert NindEngine(str(index_dir), prefix="b").total_docs == 2
    with pytest.raises(FileNotFoundError):
        NindEngine(str(tmp_path / "a"))


@pytest.mark.skipif(not hasattr(os, "fork"), reason="POSIX fork semantics")
def test_a_killed_writer_never_leaves_its_index_locked(tmp_path):
    # the writer forks a child (as its worker pools do) then dies mid-build:
    # the child outlives it but must not keep the index locked
    import subprocess
    import sys
    import textwrap
    index_dir = tmp_path / "indices"
    index_dir.mkdir()
    NindIndexer(str(index_dir), prefix="corpus").index_files(write_texts(tmp_path / "v1", ["cat dog", "dog"]))
    script = textwrap.dedent("""
        import os, signal, sys, time
        from nind._index_store import IndexStore
        with IndexStore(sys.argv[1], "corpus").new_generation():
            if os.fork() == 0:
                time.sleep(60)
                os._exit(0)
            os.kill(os.getpid(), signal.SIGKILL)
    """)
    subprocess.run([sys.executable, "-c", script, str(index_dir)], timeout=60)
    # the killed build's generation is still there, unpublished; the old index is intact
    assert len(generations(index_dir)) == 2
    assert NindEngine(str(index_dir)).get_df("dog") == 2
    started = time.monotonic()
    NindIndexer(str(index_dir), prefix="corpus").index_files(write_texts(tmp_path / "v2", ["bird"]))
    assert time.monotonic() - started < 10       # didn't wait for the orphan
    assert NindEngine(str(index_dir)).get_df("bird") == 1
    assert len(generations(index_dir)) == 1      # the killed build's leftover was deleted


# ---- incremental indexing ------------------------------------------------------

def assert_same_index(engine, expected, terms, doc_ids):
    assert engine.total_docs == expected.total_docs
    assert engine.avg_doc_len == pytest.approx(expected.avg_doc_len)
    for term in terms:
        assert engine.get_df(term) == expected.get_df(term), term
        for doc_id in doc_ids:
            assert engine.get_tf(term, doc_id) == expected.get_tf(term, doc_id), (term, doc_id)
    for doc_id in doc_ids:
        assert engine.get_doc_len(doc_id) == expected.get_doc_len(doc_id)
    for query in ["cat", "dog fish", "eel bird", "unknown"]:
        assert engine.search(query) == pytest.approx(expected.search(query))


INCREMENTAL_TERMS = ["cat", "dog", "bird", "fish", "eel", "unknown"]


def test_files_indexed_one_by_one_equal_a_full_build(tmp_path):
    expected = build_engine(tmp_path, "direct", SHARDED_TEXTS)
    index_dir = tmp_path / "incremental"
    index_dir.mkdir()
    indexer = NindIndexer(str(index_dir), prefix="corpus")
    paths = write_texts(tmp_path / "docs", SHARDED_TEXTS)
    # starts without any index: the first file creates it
    assert [indexer.add_file(path) for path in paths] == list(range(len(paths)))
    assert_same_index(NindEngine(str(index_dir)), expected, INCREMENTAL_TERMS, range(len(paths)))
    assert index_files_of(index_dir) == ["corpus.nindlexiconindex", "corpus.nindlocalindex",
                                         "corpus.nindtermindex", "generation.lock"]


def test_files_added_to_an_index_built_by_index_files(tmp_path):
    expected = build_engine(tmp_path, "direct", SHARDED_TEXTS)
    index_dir = tmp_path / "incremental"
    index_dir.mkdir()
    indexer = NindIndexer(str(index_dir), prefix="corpus")
    paths = write_texts(tmp_path / "docs", SHARDED_TEXTS)
    indexer.index_files(paths[:3])
    engine = NindEngine(str(index_dir))
    assert engine.total_docs == 3
    assert indexer.add_files(paths[3:5]) == [3, 4]
    with indexer.update() as update:
        for path in paths[5:]:
            update.add_file(path)
        # nothing is visible before the update ends
        assert engine.total_docs == 5
    # the engine switches to the update by itself
    assert_same_index(engine, expected, INCREMENTAL_TERMS, range(len(paths)))


def test_documents_are_replaced_and_removed(tmp_path):
    index_dir = tmp_path / "incremental"
    index_dir.mkdir()
    indexer = NindIndexer(str(index_dir), prefix="corpus")
    with indexer.update() as update:
        assert update.add_text("cat cat dog") == 0
        assert update.add_text("dog fish") == 1
        assert update.add_text("bird", doc_id=10) == 10
        assert update.add_text("eel") == 11
    engine = NindEngine(str(index_dir))
    assert engine.total_docs == 4
    # replace document 0, remove 1 and 10
    with indexer.update() as update:
        update.add_text("fish fish fish", doc_id=0)
        assert update.remove(1)
        assert update.remove(10)
        assert not update.remove(10)
        assert not update.remove(5)
        # added then removed in the same update: never stored
        assert update.add_text("whale") == 12
        assert update.remove(12)
    assert engine.total_docs == 2
    assert engine.get_df("cat") == 0 and engine.get_df("dog") == 0 and engine.get_df("bird") == 0
    assert engine.get_df("whale") == 0
    assert engine.get_tf("fish", 0) == 3 and engine.get_df("fish") == 1
    assert engine.get_tf("eel", 11) == 1
    assert engine.get_doc_len(0) == 3
    assert [doc_id for doc_id, _ in engine.search("fish eel")] == [0, 11]
    assert indexer.remove_documents([0, 0, 3]) == [True, False, False]
    assert engine.total_docs == 1
    # new ids follow the highest one still in the index
    assert indexer.add_file(write_texts(tmp_path / "more", ["cat"])[0]) == 12


def test_failed_or_empty_update_publishes_nothing(tmp_path):
    index_dir = tmp_path / "incremental"
    index_dir.mkdir()
    indexer = NindIndexer(str(index_dir), prefix="corpus")
    with indexer.update():
        pass
    assert list(index_dir.iterdir()) == [index_dir / "corpus.lock"]
    indexer.add_files(write_texts(tmp_path / "docs", ["cat dog", "dog"]))
    published = (index_dir / "corpus.CURRENT").read_text()
    with pytest.raises(ZeroDivisionError):
        with indexer.update() as update:
            update.add_text("bird")
            1 / 0
    with indexer.update() as update:
        assert not update.remove(7)
    assert (index_dir / "corpus.CURRENT").read_text() == published
    engine = NindEngine(str(index_dir))
    assert engine.total_docs == 2 and engine.get_df("bird") == 0
    assert len(generations(index_dir)) == 1
    with pytest.raises(RuntimeError):
        update.add_text("outside")


def test_growing_lexicon_is_rebuilt_with_more_buckets(tmp_path, monkeypatch):
    from nind import _native
    from nind.nind_engine import IndexUpdate
    monkeypatch.setattr(IndexUpdate, "LEXICON_MIN_BUCKETS", 2)
    monkeypatch.setattr(IndexUpdate, "LEXICON_MAX_LOAD", 2)
    index_dir = tmp_path / "incremental"
    index_dir.mkdir()
    indexer = NindIndexer(str(index_dir), prefix="corpus")
    words = ["w%c%c" % (a, b) for a in "abcdefgh" for b in "abcdefgh"]
    texts = [" ".join(words[i:i + 5]) for i in range(0, len(words), 4)]
    for text in texts:
        with indexer.update() as update:
            update.add_text(text)
    [generation] = generations(index_dir)
    lexicon = _native.NindLexiconIndex(str(generation / "corpus"), is_writer=False)
    assert lexicon.get_identification().lexicon_words_nb == len(words)
    assert lexicon.get_modulo() >= len(words) / 2
    expected = build_engine(tmp_path, "direct", texts)
    assert_same_index(NindEngine(str(index_dir)), expected, words[:3] + words[-3:] + ["unknown"], range(len(texts)))
