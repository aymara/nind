"""Tests for NindLexiconindex.py (the .nindlexiconindex reader) against files
produced by NindIndexer._write_lexicon (see nind_engine.py).

NindLexiconindex is a hash table: donneIdentifiant looks a word up in bucket
clefB(word) % nombreIndirection, where several words can collide in the same
bucket. These tests exercise both the direct-hit and the collision path.

Term ids are assigned by the nind._native writer itself (sequentially, in
the order words are added), not supplied by the caller, so each test writes
a list of terms and gets back the resulting {term: ident} mapping.
"""
from nind.nind_engine import NindIndexer
from nind.NindLexiconindex import NindLexiconindex


def write_lexicon(index_dir, terms):
    indexer = NindIndexer(str(index_dir), prefix="corpus")
    term_to_id, _ = indexer._write_lexicon(terms)
    return str(index_dir / "corpus.nindlexiconindex"), term_to_id


def test_donneIdentifiant_finds_every_written_term(tmp_path):
    path, term_to_id = write_lexicon(tmp_path, ["alpha", "beta", "gamma"])

    lexicon = NindLexiconindex(path)
    for term, ident in term_to_id.items():
        assert lexicon.donneIdentifiant([term]) == ident


def test_donneIdentifiant_returns_zero_for_unknown_word(tmp_path):
    path, _ = write_lexicon(tmp_path, ["alpha"])
    lexicon = NindLexiconindex(path)
    assert lexicon.donneIdentifiant(["unknown"]) == 0


def test_many_words_collide_on_a_small_modulo_but_stay_distinguishable(tmp_path):
    # NindIndexer sizes the hash table to len(terms), so with 10 words
    # several inevitably collide into the same bucket; donneIdentifiant must
    # still resolve each one to its own ident.
    words = ["un", "deux", "trois", "quatre", "cinq",
             "six", "sept", "huit", "neuf", "dix"]
    path, term_to_id = write_lexicon(tmp_path, words)

    lexicon = NindLexiconindex(path)
    seen_idents = set()
    for word in words:
        ident = lexicon.donneIdentifiant([word])
        assert ident == term_to_id[word]
        assert ident not in seen_idents
        seen_idents.add(ident)


def test_compound_lookup_of_two_known_words_is_unknown(tmp_path):
    # NindIndexer only ever writes simple (non-compound) words, so looking a
    # word up as if it were the first component of a compound must fail even
    # when both components exist individually.
    path, _ = write_lexicon(tmp_path, ["alpha", "beta"])
    lexicon = NindLexiconindex(path)
    assert lexicon.donneIdentifiant(["alpha", "beta"]) == 0


def test_donneIdentification_matches_native_writer(tmp_path):
    indexer = NindIndexer(str(tmp_path), prefix="corpus")
    term_to_id, lexicon_identification = indexer._write_lexicon(["alpha", "beta", "gamma"])

    lexicon = NindLexiconindex(str(tmp_path / "corpus.nindlexiconindex"))
    identification = lexicon.donneIdentification()
    assert identification.lexicon_words_nb == len(term_to_id)
    assert identification == lexicon_identification


def test_structural_analysis_does_not_raise(tmp_path):
    path, _ = write_lexicon(tmp_path, ["alpha", "beta", "gamma"])
    lexicon = NindLexiconindex(path)
    # analyseFichierLexiconindex re-raises on any structural inconsistency,
    # so simply not raising is a real integrity check.
    lexicon.analyseFichierLexiconindex(False)
