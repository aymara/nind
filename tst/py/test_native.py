"""Smoke tests for the nind._native pybind11 extension: each leaf class
round-tripped writer->reader in-process, confirming the wheel's compiled
C++ index stack is usable from Python.
"""
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
