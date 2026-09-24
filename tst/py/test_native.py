"""Smoke tests for the nind._native pybind11 extension: each leaf class
round-tripped writer->reader in-process, confirming the wheel's compiled
C++ index stack is usable from Python.
"""
import subprocess
from array import array
import sys
import textwrap

import pytest

native = pytest.importorskip("nind._native")


def test_lexicon_index_add_and_read_back(tmp_path):
    base = str(tmp_path / "corpus")
    lexicon = native.NindLexiconIndex(base, True, with_retrolexicon=True,
                                       indirection_bloc_size=64, retro_indirection_bloc_size=64)
    chat_id = lexicon.add_word(["chat"])
    chien_id = lexicon.add_word(["chien"])

    assert lexicon.get_word_id(["chat"]) == chat_id
    assert lexicon.get_word_id(["unknown"]) == 0
    assert lexicon.get_components(chat_id) == ["chat"]
    assert lexicon.get_components(999999) is None
    assert lexicon.get_identification().lexicon_words_nb == 2
    assert chat_id != chien_id


def test_term_index_write_and_read(tmp_path):
    base = str(tmp_path / "corpus")
    identification = native.Identification(1, 12345)
    term_index = native.NindTermIndex(base, True, identification, 0, indirection_bloc_size=64)

    term_cg = native.TermCG(cg=1, frequency=1)
    term_cg.documents = [native.Document(ident=7, frequency=3)]
    term_index.set_term_def(42, [term_cg], identification, [])

    got = term_index.get_term_def(42)
    assert len(got) == 1
    assert got[0].cg == 1
    assert [(d.ident, d.frequency) for d in got[0].documents] == [(7, 3)]
    assert term_index.get_term_def(999999) is None


def test_local_index_write_and_read(tmp_path):
    base = str(tmp_path / "corpus")
    identification = native.Identification(1, 12345)
    local_index = native.NindLocalIndex(base, True, identification, indirection_bloc_size=64)

    term = native.Term(term=42, cg=1)
    term.localisation = [native.Localisation(position=0, length=3)]
    local_index.set_local_def(7, [term], identification)

    assert local_index.get_local_length(7) == 1
    assert local_index.get_term_idents(7) == {42}
    assert local_index.get_doc_count() == 1
    assert local_index.get_local_def(999999) is None


def test_retrolexicon_add_and_read(tmp_path):
    base = str(tmp_path / "corpus")
    identification = native.Identification(2, 12345)
    retro = native.NindRetrolexicon(base, True, identification, indirection_bloc_size=64)

    retro.add_retro_words([native.RetroWord(1, "chat"), native.RetroWord(2, "chien")], identification)

    assert retro.get_components(1) == ["chat"]
    assert retro.get_components(999999) is None


def test_in_memory_lexicon(tmp_path):
    path = str(tmp_path / "corpus.lexicon")
    lexicon = native.NindLexicon(path, True)

    ident = lexicon.add_word(["chat"])
    assert lexicon.get_id(["chat"]) == ident
    assert lexicon.get_id(["unknown"]) == 0

    words_nb, _identification = lexicon.get_identification()
    assert words_nb == 1

    counts = lexicon.integrity_and_counts()
    assert counts.is_ok
    assert counts.sw_nb == 1


def test_errors_are_translated_to_nind_error(tmp_path):
    with pytest.raises(native.NindError):
        native.NindLexiconIndex(str(tmp_path / "does-not-exist"), False)


@pytest.mark.skipif(sys.platform == "win32", reason="POSIX signals")
def test_ctrl_c_still_raises_keyboard_interrupt_after_using_indexes(tmp_path):
    # Opening any nind file used to install a process-wide SIGINT handler that
    # called exit(): Ctrl-C then killed the interpreter instead of raising
    # KeyboardInterrupt. Run in a child process so a regression cannot kill pytest.
    script = textwrap.dedent("""
        import os, signal, sys
        from nind import _native as native
        lexicon = native.NindLexiconIndex(sys.argv[1], True, with_retrolexicon=True,
                                           indirection_bloc_size=8, retro_indirection_bloc_size=8)
        lexicon.add_word(["chat"])          # writes go through a critical section
        try:
            os.kill(os.getpid(), signal.SIGINT)
            for _ in range(1000000):        # Python delivers KeyboardInterrupt between bytecodes
                pass
        except KeyboardInterrupt:
            print("KeyboardInterrupt")
            sys.exit(0)
        sys.exit(3)
    """)
    result = subprocess.run([sys.executable, "-c", script, str(tmp_path / "corpus")],
                            capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    assert result.stdout.strip() == "KeyboardInterrupt"


def test_term_index_set_term_def_arrays(tmp_path):
    base = str(tmp_path / "corpus")
    identification = native.Identification(1, 12345)
    term_index = native.NindTermIndex(base, True, identification, 0, indirection_bloc_size=64)

    # unsorted input is sorted by document id
    term_index.set_term_def_arrays(42, array("I", [9, 3, 5]), array("I", [1, 4, 2]), identification)

    got = term_index.get_term_def(42)
    assert len(got) == 1
    assert got[0].cg == 0
    assert got[0].frequency == 7
    assert [(d.ident, d.frequency) for d in got[0].documents] == [(3, 4), (5, 2), (9, 1)]


def test_term_index_set_term_def_arrays_matches_object_form(tmp_path):
    identification = native.Identification(1, 12345)
    docs = [(2, 1), (10, 3), (11, 1)]
    by_objects = native.NindTermIndex(str(tmp_path / "objects"), True, identification, 0, indirection_bloc_size=64)
    term_cg = native.TermCG(cg=2, frequency=5)
    term_cg.documents = [native.Document(ident=d, frequency=f) for d, f in docs]
    by_objects.set_term_def(7, [term_cg], identification, [])
    by_arrays = native.NindTermIndex(str(tmp_path / "arrays"), True, identification, 0, indirection_bloc_size=64)
    by_arrays.set_term_def_arrays(7, array("I", [d for d, _ in docs]), array("I", [f for _, f in docs]),
                                  identification, cg=2)
    del by_objects, by_arrays
    assert (tmp_path / "objects.nindtermindex").read_bytes() == (tmp_path / "arrays.nindtermindex").read_bytes()


def test_term_index_set_term_def_arrays_rejects_bad_input(tmp_path):
    identification = native.Identification(1, 12345)
    term_index = native.NindTermIndex(str(tmp_path / "corpus"), True, identification, 0, indirection_bloc_size=64)
    with pytest.raises(ValueError):
        term_index.set_term_def_arrays(1, array("I", [1, 2]), array("I", [1]), identification)
    with pytest.raises(ValueError):
        term_index.set_term_def_arrays(1, array("I", [4, 4]), array("I", [1, 1]), identification)
    with pytest.raises(ValueError):
        term_index.set_term_def_arrays(1, array("H", [1]), array("I", [1]), identification)
    with pytest.raises(ValueError):
        term_index.set_term_def_arrays(1, array("I", [1, 2]), array("I", [0xFFFFFFFF, 1]), identification)


def test_local_index_set_local_def_arrays(tmp_path):
    identification = native.Identification(1, 12345)
    # occurrences in document order: term 42 twice, term 5 once
    occurrences = [(42, 0, 3), (5, 4, 2), (42, 7, 3)]
    by_arrays = native.NindLocalIndex(str(tmp_path / "arrays"), True, identification, indirection_bloc_size=64)
    by_arrays.set_local_def_arrays(7, array("I", [t for t, _, _ in occurrences]),
                                   array("I", [p for _, p, _ in occurrences]),
                                   array("I", [l for _, _, l in occurrences]), identification)

    got = by_arrays.get_local_def(7)
    assert [(t.term, t.cg, [(l.position, l.length) for l in t.localisation]) for t in got] == \
        [(5, 0, [(4, 2)]), (42, 0, [(0, 3), (7, 3)])]

    # same bytes as the object form with terms ascending
    by_objects = native.NindLocalIndex(str(tmp_path / "objects"), True, identification, indirection_bloc_size=64)
    t5 = native.Term(term=5, cg=0)
    t5.localisation = [native.Localisation(position=4, length=2)]
    t42 = native.Term(term=42, cg=0)
    t42.localisation = [native.Localisation(position=0, length=3), native.Localisation(position=7, length=3)]
    by_objects.set_local_def(7, [t5, t42], identification)
    del by_objects, by_arrays
    assert (tmp_path / "objects.nindlocalindex").read_bytes() == (tmp_path / "arrays.nindlocalindex").read_bytes()


def test_local_index_set_local_def_arrays_rejects_mismatched_lengths(tmp_path):
    identification = native.Identification(1, 12345)
    local_index = native.NindLocalIndex(str(tmp_path / "corpus"), True, identification, indirection_bloc_size=64)
    with pytest.raises(ValueError):
        local_index.set_local_def_arrays(7, array("I", [1, 2]), array("I", [0]), array("I", [1, 1]), identification)


def test_term_index_set_term_defs_arrays(tmp_path):
    identification = native.Identification(1, 12345)
    terms = {3: [(1, 2), (4, 1)], 8: [(2, 5)], 9: []}
    by_single = native.NindTermIndex(str(tmp_path / "single"), True, identification, 0, indirection_bloc_size=64)
    for tid, docs in terms.items():
        if docs:
            by_single.set_term_def_arrays(tid, array("I", [d for d, _ in docs]), array("I", [f for _, f in docs]),
                                          identification)
    by_batch = native.NindTermIndex(str(tmp_path / "batch"), True, identification, 0, indirection_bloc_size=64)
    idents, ends, doc_ids, freqs = array("I"), array("I"), array("I"), array("I")
    for tid, docs in terms.items():
        if docs:
            idents.append(tid)
            doc_ids.extend(d for d, _ in docs)
            freqs.extend(f for _, f in docs)
            ends.append(len(doc_ids))
    by_batch.set_term_defs_arrays(idents, ends, doc_ids, freqs, identification)

    assert [(d.ident, d.frequency) for d in by_batch.get_term_def(3)[0].documents] == [(1, 2), (4, 1)]
    del by_single, by_batch
    assert (tmp_path / "single.nindtermindex").read_bytes() == (tmp_path / "batch.nindtermindex").read_bytes()


def test_term_index_set_term_defs_arrays_rejects_bad_offsets_before_writing(tmp_path):
    identification = native.Identification(1, 12345)
    term_index = native.NindTermIndex(str(tmp_path / "corpus"), True, identification, 0, indirection_bloc_size=64)
    docs, freqs = array("I", [1, 2, 3]), array("I", [1, 1, 1])
    for ends in ([2, 1], [1, 4], [1, 2]):   # descending, past the end, not covering the data
        with pytest.raises(ValueError):
            term_index.set_term_defs_arrays(array("I", [5, 6]), array("I", ends), docs, freqs, identification)
    # a duplicate in the second term: the first one must not have been written either
    with pytest.raises(ValueError):
        term_index.set_term_defs_arrays(array("I", [5, 6]), array("I", [1, 3]), array("I", [1, 2, 2]),
                                        freqs, identification)
    assert term_index.get_term_def(5) is None


def test_local_index_set_local_defs_arrays(tmp_path):
    identification = native.Identification(1, 12345)
    docs = {7: [(42, 0, 3), (5, 4, 2), (42, 7, 3)], 9: [], 12: [(1, 0, 1)]}
    by_single = native.NindLocalIndex(str(tmp_path / "single"), True, identification, indirection_bloc_size=64)
    for doc_id, occ in docs.items():
        by_single.set_local_def_arrays(doc_id, array("I", [t for t, _, _ in occ]), array("I", [p for _, p, _ in occ]),
                                       array("I", [l for _, _, l in occ]), identification)
    by_batch = native.NindLocalIndex(str(tmp_path / "batch"), True, identification, indirection_bloc_size=64)
    idents, ends, term_ids, positions, lengths = (array("I") for _ in range(5))
    for doc_id, occ in docs.items():
        idents.append(doc_id)
        for t, p, l in occ:
            term_ids.append(t)
            positions.append(p)
            lengths.append(l)
        ends.append(len(term_ids))
    by_batch.set_local_defs_arrays(idents, ends, term_ids, positions, lengths, identification)

    assert by_batch.get_term_idents(7) == {5, 42}
    assert by_batch.get_term_idents(12) == {1}
    del by_single, by_batch
    assert (tmp_path / "single.nindlocalindex").read_bytes() == (tmp_path / "batch.nindlocalindex").read_bytes()


def test_local_index_set_local_defs_arrays_rejects_bad_offsets(tmp_path):
    identification = native.Identification(1, 12345)
    local_index = native.NindLocalIndex(str(tmp_path / "corpus"), True, identification, indirection_bloc_size=64)
    occ = array("I", [1, 2, 3])
    for ends in ([2, 1], [1, 4], [1, 2]):
        with pytest.raises(ValueError):
            local_index.set_local_defs_arrays(array("I", [5, 6]), array("I", ends), occ, occ, occ, identification)
    assert local_index.get_doc_count() == 0
