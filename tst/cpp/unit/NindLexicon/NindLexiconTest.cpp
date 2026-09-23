#include "NindLexicon/NindLexicon.h"
#include "NindExceptions.h"
#include "TestTempDir.h"
#include "TestFileBytes.h"
#include "doctest.h"
#include <algorithm>
#include <list>
#include <string>
#include <vector>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
namespace {
list<string> words(const string &a) {
    list<string> l;
    l.push_back(a);
    return l;
}
list<string> words(const string &a, const string &b) {
    list<string> l;
    l.push_back(a);
    l.push_back(b);
    return l;
}
}
////////////////////////////////////////////////////////////
TEST_CASE("NindLexiconTest.AddWordIsIdempotentAndGetIdMatches") {
    TestTempDir tmp;
    NindLexicon lexicon(tmp.file("lex.nindlexicon"), true);

    const unsigned int idAlpha = lexicon.addWord(words("alpha"));
    const unsigned int idBeta = lexicon.addWord(words("beta"));
    CHECK_NE(idAlpha, idBeta);
    CHECK_EQ(idAlpha, lexicon.addWord(words("alpha")));   // re-adding returns the same ident

    CHECK_EQ(idAlpha, lexicon.getId(words("alpha")));
    CHECK_EQ(idBeta, lexicon.getId(words("beta")));
    CHECK_EQ(0u, lexicon.getId(words("gamma")));          // unknown word
}
////////////////////////////////////////////////////////////
TEST_CASE("NindLexiconTest.CompoundWordGetsItsOwnIdentAndIsIdempotent") {
    TestTempDir tmp;
    NindLexicon lexicon(tmp.file("compound.nindlexicon"), true);

    const unsigned int idAlpha = lexicon.addWord(words("alpha"));
    const unsigned int idBeta = lexicon.addWord(words("beta"));
    const unsigned int idCompound = lexicon.addWord(words("alpha", "beta"));

    CHECK_NE(idCompound, idAlpha);
    CHECK_NE(idCompound, idBeta);
    CHECK_EQ(idCompound, lexicon.addWord(words("alpha", "beta")));
    CHECK_EQ(idCompound, lexicon.getId(words("alpha", "beta")));
    CHECK_EQ(idAlpha, lexicon.getId(words("alpha")));     // untouched by the compound addition
}
////////////////////////////////////////////////////////////
TEST_CASE("NindLexiconTest.IntegrityAndCountsReportsSimpleAndCompoundCounts") {
    TestTempDir tmp;
    NindLexicon lexicon(tmp.file("integrity.nindlexicon"), true);
    lexicon.addWord(words("alpha"));
    lexicon.addWord(words("beta"));
    lexicon.addWord(words("alpha", "beta"));

    NindLexicon::LexiconChar characteristics;
    REQUIRE(lexicon.integrityAndCounts(characteristics));
    CHECK(characteristics.isOk);
    CHECK_EQ(2u, characteristics.swNb);
    CHECK_EQ(1u, characteristics.cwNb);
    CHECK_EQ(3u, characteristics.wordsNb);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindLexiconTest.PersistsAcrossReopenAsReader") {
    TestTempDir tmp;
    const string path = tmp.file("persist.nindlexicon");
    unsigned int idAlpha, idCompound;
    {
        NindLexicon writer(path, true);
        idAlpha = writer.addWord(words("alpha"));
        writer.addWord(words("beta"));
        idCompound = writer.addWord(words("alpha", "beta"));
    }
    NindLexicon reader(path, false);
    CHECK_EQ(idAlpha, reader.getId(words("alpha")));
    CHECK_EQ(idCompound, reader.getId(words("alpha", "beta")));
    CHECK_EQ(0u, reader.getId(words("unknown")));
}
////////////////////////////////////////////////////////////
TEST_CASE("NindLexiconTest.ReopenedWriterKeepsPreviousWordsAndContinuesNumbering") {
    TestTempDir tmp;
    const string path = tmp.file("reopen.nindlexicon");
    unsigned int idAlpha;
    unsigned int wordsNbAfterFirstSession;
    {
        NindLexicon writer(path, true);
        idAlpha = writer.addWord(words("alpha"));
        writer.addWord(words("beta"));
        unsigned int identification;
        writer.getIdentification(wordsNbAfterFirstSession, identification);
    }
    NindLexicon writer2(path, true);
    CHECK_EQ(idAlpha, writer2.getId(words("alpha")));     // loaded back from file
    const unsigned int idGamma = writer2.addWord(words("gamma"));
    CHECK_EQ(wordsNbAfterFirstSession + 1, idGamma);      // numbering continues, not restarted
}
////////////////////////////////////////////////////////////
TEST_CASE("NindLexiconTest.ReaderCannotAddWord") {
    TestTempDir tmp;
    const string path = tmp.file("readonly.nindlexicon");
    { NindLexicon writer(path, true); }
    NindLexicon reader(path, false);
    CHECK_THROWS_AS(reader.addWord(words("alpha")), NindLexiconException);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindLexiconTest.OpeningMissingFileAsReaderThrows") {
    TestTempDir tmp;
    CHECK_THROWS_AS(NindLexicon(tmp.file("missing.nindlexicon"), false), NindLexiconException);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindLexiconTest.IntegrityCheckTerminatesOnCyclicCompound") {
    TestTempDir tmp;
    const string path = tmp.file("cycle.nindlexicon");
    {
        NindLexicon lexicon(path, true);
        lexicon.addWord(words("alpha"));                 // 1
        lexicon.addWord(words("beta"));                  // 2
        lexicon.addWord(words("alpha", "beta"));         // 3 = (1, 2)
    }
    // <flagComposej=29> <ident=3> <identA=1> <identS=2> (3-bytes little-endian): make word 3 = (3, 2)
    vector<unsigned char> bytes = readFileBytes(path);
    const unsigned char record[] = { 29, 3, 0, 0, 1, 0, 0, 2, 0, 0 };
    const vector<unsigned char>::iterator found = search(bytes.begin(), bytes.end(), record, record + sizeof(record));
    REQUIRE(found != bytes.end());
    found[4] = 3;
    writeFileBytes(path, bytes);

    NindLexicon lexicon(path, false);
    NindLexicon::LexiconChar characteristics;
    // used to loop forever (and grow a list without bound) on the self-referencing compound
    CHECK_FALSE(lexicon.integrityAndCounts(characteristics));
    CHECK_FALSE(characteristics.isOk);
}
