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


def test_search_bm25_parameters(engine):
    # the defaults are those of bm25_score
    assert engine.search("dog cat") == [(d, engine.bm25_score("dog cat", d)) for d, _ in engine.search("dog cat")]
    for k1, b in ((0.9, 0.4), (1.2, 0.0), (2.0, 1.0)):
        results = engine.search("dog cat", k1=k1, b=b)
        assert results == [(d, engine.bm25_score("dog cat", d, k1=k1, b=b)) for d, _ in results]
        assert {d for d, _ in results} == {0, 1}
    # b = 0: no length normalization; k1 = 0: term frequency ignored, docs 0 and 1 tie on "dog"
    assert [s for _, s in engine.search("dog", k1=0.0, b=0.0)] == [engine.bm25_score("dog", 0, k1=0.0, b=0.0)] * 2
    # the per-document length normalization is cached for the last (k1, b): switching back and forth
    for k1, b in ((1.5, 0.75), (0.9, 0.4), (1.5, 0.75), (1.5, 0.4), (0.9, 0.4)) * 2:
        results = engine.search("dog cat bird", k1=k1, b=b)
        assert results == [(d, engine.bm25_score("dog cat bird", d, k1=k1, b=b)) for d, _ in results], (k1, b)


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


#: Files of one generation of an index written by NindIndexer (with a local index).
INDEX_FILES = ["corpus.ninddoclengths", "corpus.nindlexiconindex", "corpus.nindlocalindex", "corpus.nindmeta",
               "corpus.nindtermindex", "generation.lock"]


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
        INDEX_FILES


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
    assert index_files_of(index_dir) == INDEX_FILES


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
    # older versions wrote neither the metadata nor the document lengths: read from the local index
    os.remove(str(index_dir / "corpus.nindmeta")) if (index_dir / "corpus.nindmeta").exists() else None
    os.remove(str(index_dir / "corpus.ninddoclengths"))
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
    assert index_files_of(index_dir) == INDEX_FILES


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


# -- JSON Lines input -----------------------------------------------------------

def write_jsonl(path, records, blank_lines=False):
    import json
    with open(path, "w", encoding="utf-8") as f:
        for record in records:
            f.write(json.dumps(record) + "\n")
            if blank_lines:
                f.write("\n")
    return str(path)


def assert_same_corpus(engine, direct, n_docs, terms):
    assert engine.total_docs == direct.total_docs == n_docs
    assert engine.avg_doc_len == pytest.approx(direct.avg_doc_len)
    for term in terms:
        assert engine.get_df(term) == direct.get_df(term)
        for doc_id in range(n_docs):
            assert engine.get_tf(term, doc_id) == direct.get_tf(term, doc_id)


def test_index_jsonl_equals_index_files(tmp_path):
    direct = build_engine(tmp_path, "direct", SHARDED_TEXTS)
    records = [{"id": "d%d" % (100 + i), "contents": text} for i, text in enumerate(SHARDED_TEXTS)]
    path = write_jsonl(tmp_path / "corpus.jsonl", records, blank_lines=True)
    index_dir = tmp_path / "jsonl"
    index_dir.mkdir()
    ids = NindIndexer(str(index_dir), prefix="corpus").index_jsonl(path)
    assert ids == [record["id"] for record in records]
    assert_same_corpus(NindEngine(str(index_dir)), direct, len(SHARDED_TEXTS),
                      ["cat", "dog", "bird", "fish", "eel", "unknown"])


def test_index_jsonl_limit_and_custom_fields(tmp_path):
    records = [{"docid": i, "text": text} for i, text in enumerate(SHARDED_TEXTS)]
    path = write_jsonl(tmp_path / "corpus.jsonl", records, blank_lines=True)
    index_dir = tmp_path / "jsonl"
    index_dir.mkdir()
    ids = NindIndexer(str(index_dir)).index_jsonl(path, id_field="docid", text_field="text", limit=3)
    assert ids == [0, 1, 2]
    engine = NindEngine(str(index_dir))
    assert engine.total_docs == 3
    assert engine.get_df("fish") == 0          # only in records past the limit
    assert engine.get_df("dog") == 2


def test_index_jsonl_in_parallel_parts(tmp_path, monkeypatch):
    # many parts, some lines longer than a part: every record is read exactly once, in order
    def tag(i):     # one distinct, letters-only word per record (the tokenizer splits digits off)
        return "tag" + "".join("abcdefghij"[int(c)] for c in str(i))
    texts = ["%s common %s" % (tag(i), "pad " * (i % 50)) for i in range(400)]
    records = [{"id": "d%d" % i, "contents": text} for i, text in enumerate(texts)]
    path = write_jsonl(tmp_path / "corpus.jsonl", records)
    monkeypatch.setattr(NindIndexer, "JSONL_PARALLEL_MIN_BYTES", 0)
    index_dir = tmp_path / "jsonl"
    index_dir.mkdir()
    ids = NindIndexer(str(index_dir)).index_jsonl(path, workers=8, limit=399)
    assert ids == ["d%d" % i for i in range(399)]
    engine = NindEngine(str(index_dir))
    assert engine.get_df("common") == 399
    for i in (0, 1, 57, 398):
        assert engine.get_tf(tag(i), i) == 1
    assert engine.get_df(tag(399)) == 0


def test_jsonl_ranges_are_line_aligned(tmp_path):
    path = write_jsonl(tmp_path / "corpus.jsonl", [{"id": i, "contents": "x" * (i * 7)} for i in range(30)])
    data = open(path, "rb").read()
    for pieces in (1, 2, 7, 64, 1000):
        ranges = NindIndexer._jsonl_ranges(path, len(data), pieces)
        assert ranges[0][0] == 0 and ranges[-1][1] == len(data)
        for (_, stop), (start, _) in zip(ranges, ranges[1:]):
            assert stop == start and data[start - 1:start] == b"\n"


def test_index_jsonl_rejects_bad_records_and_keeps_previous_index(tmp_path):
    index_dir = tmp_path / "jsonl"
    index_dir.mkdir()
    indexer = NindIndexer(str(index_dir), prefix="corpus")
    indexer.index_jsonl(write_jsonl(tmp_path / "good.jsonl", [{"id": "a", "contents": "cat"}]))
    before = index_files_of(index_dir)
    for bad in ('{"id": "b"}\n', 'not json\n', '{"id": "c", "contents": 3}\n'):
        path = tmp_path / "bad.jsonl"
        path.write_text('{"id": "a", "contents": "dog"}\n' + bad, encoding="utf-8")
        with pytest.raises(ValueError):
            indexer.index_jsonl(str(path))
    assert index_files_of(index_dir) == before
    assert NindEngine(str(index_dir)).get_df("cat") == 1


# -- local_index=False, tokenizer options, long tokens, Python tokenizers ------------

import json

from nind.nind_engine import TOKENIZER_DEFAULTS


def build_with(tmp_path, name, texts, indexer_kwargs=None, **kwargs):
    index_dir = tmp_path / name
    index_dir.mkdir()
    indexer = NindIndexer(str(index_dir), prefix="corpus", **(indexer_kwargs or {}))
    indexer.index_files(write_texts(tmp_path / (name + "-docs"), texts), **kwargs)
    return indexer, NindEngine(str(index_dir))


@pytest.mark.parametrize("kwargs", [{}, {"shards": 3, "workers": 2}])
def test_index_without_local_index_answers_like_one_with(tmp_path, kwargs):
    _, full = build_with(tmp_path, "full", SHARDED_TEXTS)
    _, light = build_with(tmp_path, "light", SHARDED_TEXTS, {"local_index": False}, **kwargs)
    assert light.local_index is None
    assert "corpus.nindlocalindex" not in index_files_of(tmp_path / "light")
    assert_same_index(light, full, INCREMENTAL_TERMS, range(len(SHARDED_TEXTS)))


def test_updates_of_an_index_without_local_index(tmp_path):
    texts = ["cat cat dog", "dog fish", "bird", "eel"]
    indexer, light = build_with(tmp_path, "light", texts, {"local_index": False})
    full_indexer, full = build_with(tmp_path, "full", texts)
    for each in (indexer, full_indexer):
        with each.update() as update:
            update.add_text("fish fish fish", doc_id=0)     # replaced
            update.remove(1)
            update.add_text("whale cat")
    assert light.local_index is None
    assert light.total_docs == 4
    assert_same_index(light, full, INCREMENTAL_TERMS + ["whale"], range(6))
    # the layout of an existing index can't be changed by an update
    with pytest.raises(ValueError):
        with NindIndexer(str(tmp_path / "light"), prefix="corpus", local_index=True).update() as update:
            update.add_text("cat")
    assert light.get_df("cat") == 1


def test_tokenizer_options_are_stored_and_used_for_queries_and_updates(tmp_path):
    indexer, engine = build_with(tmp_path, "lower", ["getHTTPServer", "my_var"],
                                 {"tokenizer": {"lowercase": True, "split_underscore": False}})
    meta = json.loads((generations(tmp_path / "lower")[0] / "corpus.nindmeta").read_text())
    assert meta["tokenizer"] == dict(TOKENIZER_DEFAULTS, lowercase=True, split_underscore=False)
    assert meta["local_index"] is True
    assert engine.tokenize("GetHTTP my_var") == ["get", "http", "my_var"]
    assert engine.get_df("http") == 1 and engine.get_df("my_var") == 1
    assert [doc_id for doc_id, _ in engine.search("HTTP")] == [0]
    # an update tokenizes like the index, even from an indexer with default options
    with NindIndexer(str(tmp_path / "lower"), prefix="corpus").update() as update:
        update.add_text("HTTP_Client")
    assert engine.get_df("http_client") == 1
    # but refuses explicitly different options
    with pytest.raises(ValueError):
        with NindIndexer(str(tmp_path / "lower"), prefix="corpus", tokenizer={"lowercase": False}).update():
            pass
    with pytest.raises(ValueError):
        NindIndexer(str(tmp_path), tokenizer={"no_such_option": True})
    with pytest.raises(ValueError):
        NindIndexer(str(tmp_path), tokenizer={"max_length": 255})


def test_tokens_too_long_for_the_lexicon_are_dropped(tmp_path):
    # a word of a nind lexicon is at most 254 bytes: longer tokens (base64 blobs, separator lines...)
    # used to make indexing fail
    long_token = "a" * 300
    texts = ["cat %s dog" % long_token, "x" * 254 + " " + "y" * 255]
    _, engine = build_with(tmp_path, "files", texts)
    assert engine.get_df("cat") == 1 and engine.get_doc_len(0) == 2
    assert engine.get_df("x" * 254) == 1 and engine.get_doc_len(1) == 1
    path = write_jsonl(tmp_path / "corpus.jsonl", [{"id": i, "contents": t} for i, t in enumerate(texts)])
    (tmp_path / "jsonl").mkdir()
    NindIndexer(str(tmp_path / "jsonl")).index_jsonl(path)
    assert NindEngine(str(tmp_path / "jsonl")).get_doc_len(0) == 2

    class WholeWords(NindIndexer):
        def _default_tokenize(self, text):
            return text.split()
    (tmp_path / "python").mkdir()
    WholeWords(str(tmp_path / "python"), prefix="corpus").index_files(write_texts(tmp_path / "python-docs", texts))
    assert NindEngine(str(tmp_path / "python")).get_doc_len(0) == 2


class SplitOnSpaces(NindIndexer):
    """A tokenizer in Python: overriding _default_tokenize bypasses the native one."""

    def _default_tokenize(self, text):
        return text.split(" ")


@pytest.mark.parametrize("kwargs", [{}, {"shards": 2, "workers": 2}])
def test_an_overridden_tokenizer_is_used(tmp_path, kwargs):
    texts = ["fooBar baz", "fooBar", "baz_qux baz"]
    index_dir = tmp_path / "python"
    index_dir.mkdir()
    SplitOnSpaces(str(index_dir), prefix="corpus").index_files(write_texts(tmp_path / "docs", texts), **kwargs)
    engine = NindEngine(str(index_dir))
    assert engine.get_df("fooBar") == 2 and engine.get_df("foo") == 0
    assert engine.get_tf("baz", 2) == 1 and engine.get_df("baz_qux") == 1
    path = write_jsonl(tmp_path / "corpus.jsonl", [{"id": i, "contents": t} for i, t in enumerate(texts)])
    (tmp_path / "jsonl").mkdir()
    SplitOnSpaces(str(tmp_path / "jsonl")).index_jsonl(path)
    assert NindEngine(str(tmp_path / "jsonl")).get_df("fooBar") == 2


def test_python_and_native_paths_build_the_same_index(tmp_path):
    class NativeInPython(NindIndexer):
        def _default_tokenize(self, text):
            return super()._default_tokenize(text)
    paths = write_texts(tmp_path / "docs", SHARDED_TEXTS + ["getHTTPServer x86_64 été"])
    for name, cls in (("native", NindIndexer), ("python", NativeInPython)):
        (tmp_path / name).mkdir()
        cls(str(tmp_path / name), prefix="corpus").index_files(paths)
    # same bytes, but for the lexicon's creation time in each file's identification (its last 4 bytes)
    for extension in (".nindlexiconindex", ".nindtermindex", ".nindlocalindex"):
        native_file = generations(tmp_path / "native")[0] / ("corpus" + extension)
        python_file = generations(tmp_path / "python")[0] / ("corpus" + extension)
        assert native_file.read_bytes()[:-4] == python_file.read_bytes()[:-4], extension
    for extension in (".ninddoclengths", ".nindmeta"):
        native_file = generations(tmp_path / "native")[0] / ("corpus" + extension)
        python_file = generations(tmp_path / "python")[0] / ("corpus" + extension)
        assert native_file.read_bytes() == python_file.read_bytes(), extension


def test_update_of_an_index_written_before_tokenizer_options_keeps_its_tokenizer(tmp_path):
    index_dir = tmp_path / "indices"
    index_dir.mkdir()
    old = NindIndexer(str(index_dir), prefix="corpus")
    old._write_index([["BAR"]], 0, str(index_dir / "corpus"))
    os.remove(str(index_dir / "corpus.ninddoclengths"))     # older versions didn't write it, nor .nindmeta
    engine = NindEngine(str(index_dir))
    assert engine.tokenize("FOO_BAR") == ["BAR"]        # the original tokenizer
    with NindIndexer(str(index_dir), prefix="corpus").update() as update:
        update.add_text("FOO_BAR")
    assert engine.get_df("BAR") == 2 and engine.get_df("FOO") == 0
    meta = json.loads((generations(index_dir)[0] / "corpus.nindmeta").read_text())
    assert meta["tokenizer"] == "legacy"
    assert engine.total_docs == 2 and engine.get_doc_len(0) == 1


def test_empty_corpus(tmp_path):
    (tmp_path / "empty").mkdir()
    NindIndexer(str(tmp_path / "empty"), prefix="corpus").index_files([])
    engine = NindEngine(str(tmp_path / "empty"))
    assert engine.total_docs == 0 and engine.search("cat") == []


def test_text_not_encodable_in_utf8_is_indexed(tmp_path):
    # json.loads accepts lone surrogates ("\ud800" escapes): they must not abort indexing
    path = tmp_path / "corpus.jsonl"
    path.write_text('{"id": "a", "contents": "a\\ud800b cat"}\n{"id": "b", "contents": "dog"}\n', encoding="utf-8")
    (tmp_path / "native").mkdir()
    assert NindIndexer(str(tmp_path / "native")).index_jsonl(str(path)) == ["a", "b"]
    engine = NindEngine(str(tmp_path / "native"))
    assert engine.total_docs == 2 and engine.get_df("cat") == 1
    assert engine.tokenize("x\ud800y") == ["x", "y"]
    (tmp_path / "python").mkdir()
    SplitOnSpaces(str(tmp_path / "python")).index_jsonl(str(path))
    engine = NindEngine(str(tmp_path / "python"))
    assert engine.get_df("cat") == 1 and engine.get_doc_len(0) == 1     # "a\ud800b" can't be stored
    with NindIndexer(str(tmp_path / "native")).update() as update:
        update.add_text("fish\udfff")
    assert NindEngine(str(tmp_path / "native")).get_df("fish") == 1


# -- search from the postings lists --------------------------------------------------

import random

from nind import _native as native


def full_scan_search(engine, query, top_k=10):
    """The former NindEngine.search: every document scored with bm25_score."""
    scores = []
    for doc_id in engine._snapshot.doc_ids:
        score = engine.bm25_score(query, doc_id)
        if score > 0:
            scores.append((doc_id, score))
    scores.sort(key=lambda x: x[1], reverse=True)
    return scores[:top_k]


def test_search_from_postings_equals_the_full_scan(tmp_path):
    rng = random.Random(42)
    words = ["w%s" % "abcdefghij"[i % 10] * (1 + i // 10) for i in range(40)]
    texts = [" ".join(rng.choice(words[:rng.randint(3, 40)]) for _ in range(rng.randint(1, 30)))
             for _ in range(300)]
    texts[7] = texts[3]         # identical documents: equal scores, kept in index order
    texts[12] = ""              # an empty document isn't stored
    _, engine = build_with(tmp_path, "random", texts)
    queries = ["wa", "wa wb", "wa wa wc", "wjjjj wa unknown", "unknown", "", " ".join(words)]
    queries += [" ".join(rng.sample(words, 3)) for _ in range(20)]
    for query in queries:
        expected = full_scan_search(engine, query, None)
        for top_k in (10, 0, 1, 1000, None, -3):
            assert engine.search(query, top_k=top_k) == expected[:top_k], (query, top_k)


def test_search_with_several_categories_per_term(tmp_path):
    # Amose-style term definitions: a term in several grammatical categories, a document in
    # more than one of them: its frequency is the first category's, df counts both
    _, engine = build_with(tmp_path, "cg", ["cat dog", "cat", "dog dog", "bird"])
    base = str(generations(tmp_path / "cg")[0] / "corpus")
    lexicon = native.NindLexiconIndex(base, is_writer=False)
    identification = lexicon.get_identification()
    cat = lexicon.get_word_id(["cat"])
    del lexicon
    first, second = native.TermCG(1, 5), native.TermCG(2, 3)
    first.documents = [native.Document(0, 2), native.Document(1, 3)]
    second.documents = [native.Document(0, 1), native.Document(3, 2)]
    term_index = native.NindTermIndex(base, is_writer=True, lexicon_identification=identification,
                                      specifics_number=0)
    term_index.set_term_def(cat, [first, second], identification, [])
    del term_index
    engine = NindEngine(str(tmp_path / "cg"))
    assert engine.get_df("cat") == 4 and engine.get_tf("cat", 0) == 2
    for query in ["cat", "cat dog", "cat cat bird"]:
        assert engine.search(query) == full_scan_search(engine, query), query
    assert [doc_id for doc_id, _ in engine.search("cat")] == [1, 3, 0]   # tf 3; tf 2 in 1 token; tf 2 in 2


def test_search_with_sparse_document_ids(tmp_path):
    # ids far above the number of documents: the scorer finds positions by binary search
    # instead of a dense table; its scratch arrays are reused (and reset) from one search to the next
    rng = random.Random(7)
    words = ["s%d" % i for i in range(15)]
    texts = [" ".join(rng.choice(words) for _ in range(rng.randint(1, 12))) for _ in range(30)]
    indexer, engine = build_with(tmp_path, "sparse", texts)
    with indexer.update() as update:
        for i, doc_id in enumerate((3_000_000, 4_000_000_000, 123_456_789)):
            update.add_text(texts[i] + " s0 rare" + "abc"[i], doc_id=doc_id)
    engine = NindEngine(str(tmp_path / "sparse"))
    assert engine.total_docs == 33
    queries = ["s0", "s0 s1", "rareb", "rarec s3 s3", "s14 unknown", " ".join(words)]
    expected = {query: full_scan_search(engine, query, None) for query in queries}
    assert [doc_id for doc_id, _ in expected["rareb"]] == [4_000_000_000]
    for _ in range(3):
        for query in queries:
            assert engine.search(query, top_k=None) == expected[query], query
            assert engine.search(query, top_k=5) == expected[query][:5], query


def test_search_scales_with_the_postings_not_the_corpus(tmp_path):
    # a corpus where the query term is in one document out of 10000: answered without
    # scoring the others (the full scan took minutes at this size)
    texts = ["filler%s" % "abcdefghij"[i % 10] for i in range(10000)]
    texts[2345] = "needle filler"
    _, engine = build_with(tmp_path, "big", texts, workers=1)
    started = time.perf_counter()
    for _ in range(100):
        assert engine.search("needle") == [(2345, engine.bm25_score("needle", 2345))]
    assert time.perf_counter() - started < 2


# -- document names (the corpus's own identifiers) ----------------------------------

def test_index_jsonl_stores_the_records_ids(tmp_path):
    records = [{"id": "d%d" % (1000 - i), "contents": text} for i, text in enumerate(SHARDED_TEXTS)]
    path = write_jsonl(tmp_path / "corpus.jsonl", records)
    (tmp_path / "jsonl").mkdir()
    NindIndexer(str(tmp_path / "jsonl")).index_jsonl(path)
    engine = NindEngine(str(tmp_path / "jsonl"))
    for doc_id, record in enumerate(records):
        assert engine.doc_name(doc_id) == record["id"]
        assert engine.doc_id(record["id"]) == doc_id
    assert engine.doc_name(len(records)) is None and engine.doc_id("nope") is None
    hits = engine.search("fish eel", doc_names=True)
    assert hits == [(engine.doc_name(doc_id), score) for doc_id, score in engine.search("fish eel")]
    assert hits[0][0] == "d994"         # document 6, "eel": the shortest match
    # numeric ids are stored as strings
    path = write_jsonl(tmp_path / "numeric.jsonl", [{"id": 7, "contents": "cat"}, {"id": 3, "contents": "dog"}])
    (tmp_path / "numeric").mkdir()
    assert NindIndexer(str(tmp_path / "numeric")).index_jsonl(path) == [7, 3]
    assert NindEngine(str(tmp_path / "numeric")).doc_id("3") == 1


def test_duplicate_record_ids_are_refused(tmp_path):
    (tmp_path / "jsonl").mkdir()
    indexer = NindIndexer(str(tmp_path / "jsonl"), prefix="corpus")
    indexer.index_jsonl(write_jsonl(tmp_path / "good.jsonl", [{"id": "a", "contents": "cat"}]))
    before = index_files_of(tmp_path / "jsonl")
    path = write_jsonl(tmp_path / "dup.jsonl", [{"id": "a", "contents": "cat"}, {"id": "a", "contents": "dog"}])
    with pytest.raises(ValueError):
        indexer.index_jsonl(path)
    assert index_files_of(tmp_path / "jsonl") == before


@pytest.mark.parametrize("kwargs", [{}, {"shards": 3, "workers": 2}])
def test_index_files_with_doc_names(tmp_path, kwargs):
    names = ["file-%s.py" % chr(ord("a") + i) for i in range(len(SHARDED_TEXTS))]
    _, engine = build_with(tmp_path, "named", SHARDED_TEXTS, doc_names=names, **kwargs)
    assert [engine.doc_name(i) for i in range(len(names))] == names
    assert engine.doc_id("file-c.py") == 2
    _, unnamed = build_with(tmp_path, "unnamed", SHARDED_TEXTS)
    assert unnamed.doc_name(0) is None and unnamed.doc_id("file-a.py") is None
    assert unnamed.search("cat", doc_names=True) == [(None, score) for _, score in unnamed.search("cat")]
    assert "corpus.ninddocnames" not in index_files_of(tmp_path / "unnamed")
    with pytest.raises(ValueError):
        NindIndexer(str(tmp_path / "unnamed"), prefix="corpus").index_files(["x"], doc_names=["a", "b"])


def test_updates_keep_document_names(tmp_path):
    path = write_jsonl(tmp_path / "corpus.jsonl",
                       [{"id": "a", "contents": "cat"}, {"id": "b", "contents": "dog"}, {"id": "c", "contents": "eel"}])
    index_dir = tmp_path / "jsonl"
    index_dir.mkdir()
    indexer = NindIndexer(str(index_dir), prefix="corpus")
    indexer.index_jsonl(str(path))
    engine = NindEngine(str(index_dir))
    with indexer.update() as update:
        assert update.doc_id("b") == 1
        assert update.add_text("bird bird", name="b") == 1           # replaces the document named b
        assert update.add_text("fish", name="d") == 3                # a new document
        assert update.add_text("whale", doc_id=0) == 0               # replaced: keeps its name
        assert update.remove(2)                                      # its name goes with it
        with pytest.raises(ValueError):
            update.add_text("x", doc_id=0, name="d")                 # d is another document
        assert update.add_text("cow", doc_id=3, name="e") == 3       # renamed
    assert [engine.doc_name(i) for i in range(5)] == ["a", "b", None, "e", None]
    assert engine.doc_id("c") is None and engine.doc_id("d") is None
    assert engine.get_tf("bird", engine.doc_id("b")) == 2
    assert engine.search("whale cow", doc_names=True)[0][0] in ("a", "e")
    assert indexer.add_file(write_texts(tmp_path / "more", ["cat"])[0], name="a") == 0
    assert engine.get_df("whale") == 0 and engine.get_tf("cat", 0) == 1
    # an index without names gets them with its first named document
    _, unnamed = build_with(tmp_path, "unnamed", ["cat", "dog"])
    with NindIndexer(str(tmp_path / "unnamed"), prefix="corpus").update() as update:
        update.add_text("eel", name="x")
    assert unnamed.doc_name(2) == "x" and unnamed.doc_name(0) is None


def test_document_names_are_stored_compactly(tmp_path):
    records = [{"id": "d%d" % i, "contents": "word%s" % "abcdefghij"[i % 10]} for i in range(2000)]
    path = write_jsonl(tmp_path / "corpus.jsonl", records)
    (tmp_path / "jsonl").mkdir()
    NindIndexer(str(tmp_path / "jsonl"), prefix="corpus").index_jsonl(path)
    names_file = generations(tmp_path / "jsonl")[0] / "corpus.ninddocnames"
    # consecutive document ids aren't stored: 8 bytes per document plus the names
    assert names_file.stat().st_size == 20 + 4 + 8 * 2000 + sum(len(r["id"]) for r in records)


# -- concurrent searches ----------------------------------------------------------

from concurrent.futures import ThreadPoolExecutor


def concurrency_corpus(seed, n=400):
    rng = random.Random(seed)
    words = ["c%d" % i for i in range(60)]
    return [" ".join(rng.choice(words[:rng.randint(5, 60)]) for _ in range(rng.randint(1, 40))) for _ in range(n)]


CONCURRENCY_QUERIES = ["c0", "c1 c2", "c3 c3 c4", "c59 unknown", "c10 c20 c30 c40 c50"] + \
    ["c%d c%d" % (i, i + 7) for i in range(0, 50, 3)]


def answers(engine):
    """Everything a query computes: the results, and a few scores and statistics."""
    return [(engine.search(q, top_k=20), engine.bm25_score(q, 3), engine.get_df(q.split()[0])) for q in CONCURRENCY_QUERIES]


def test_concurrent_searches_answer_like_sequential_ones(tmp_path):
    _, engine = build_with(tmp_path, "concurrent", concurrency_corpus(1))
    expected = answers(engine)
    with ThreadPoolExecutor(max_workers=8) as pool:
        for got in pool.map(lambda _: answers(engine), range(64)):
            assert got == expected


def test_concurrent_searches_while_a_new_build_is_published(tmp_path):
    # every call answers from one build: the old one or the new one, never a mix
    indexer, engine = build_with(tmp_path, "switch", concurrency_corpus(2))
    old = [engine.search(q, top_k=20) for q in CONCURRENCY_QUERIES]
    new_texts = concurrency_corpus(3)
    # the new build, computed apart
    _, reference = build_with(tmp_path, "reference", new_texts)
    new = [reference.search(q, top_k=20) for q in CONCURRENCY_QUERIES]
    assert old != new

    def one_pass(_):
        return [engine.search(q, top_k=20) for q in CONCURRENCY_QUERIES]

    with ThreadPoolExecutor(max_workers=8) as pool:
        futures = [pool.submit(one_pass, i) for i in range(200)]
        indexer.index_files(write_texts(tmp_path / "switch-new", new_texts))
        results = [f.result() for f in futures]
    for passes in results:
        for i, result in enumerate(passes):
            assert result in (old[i], new[i])
    # once published, every new call answers from the new build
    assert one_pass(0) == new
