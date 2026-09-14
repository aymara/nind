#include "NindLexicon/NindLexicon.h"
#include "NindExceptions.h"
#include "TestTempDir.h"
#include <gtest/gtest.h>
#include <list>
#include <string>
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
TEST(NindLexiconTest, AddWordIsIdempotentAndGetIdMatches) {
    TestTempDir tmp;
    NindLexicon lexicon(tmp.file("lex.nindlexicon"), true);

    const unsigned int idAlpha = lexicon.addWord(words("alpha"));
    const unsigned int idBeta = lexicon.addWord(words("beta"));
    EXPECT_NE(idAlpha, idBeta);
    EXPECT_EQ(idAlpha, lexicon.addWord(words("alpha")));   // re-adding returns the same ident

    EXPECT_EQ(idAlpha, lexicon.getId(words("alpha")));
    EXPECT_EQ(idBeta, lexicon.getId(words("beta")));
    EXPECT_EQ(0u, lexicon.getId(words("gamma")));          // unknown word
}
////////////////////////////////////////////////////////////
TEST(NindLexiconTest, CompoundWordGetsItsOwnIdentAndIsIdempotent) {
    TestTempDir tmp;
    NindLexicon lexicon(tmp.file("compound.nindlexicon"), true);

    const unsigned int idAlpha = lexicon.addWord(words("alpha"));
    const unsigned int idBeta = lexicon.addWord(words("beta"));
    const unsigned int idCompound = lexicon.addWord(words("alpha", "beta"));

    EXPECT_NE(idCompound, idAlpha);
    EXPECT_NE(idCompound, idBeta);
    EXPECT_EQ(idCompound, lexicon.addWord(words("alpha", "beta")));
    EXPECT_EQ(idCompound, lexicon.getId(words("alpha", "beta")));
    EXPECT_EQ(idAlpha, lexicon.getId(words("alpha")));     // untouched by the compound addition
}
////////////////////////////////////////////////////////////
TEST(NindLexiconTest, IntegrityAndCountsReportsSimpleAndCompoundCounts) {
    TestTempDir tmp;
    NindLexicon lexicon(tmp.file("integrity.nindlexicon"), true);
    lexicon.addWord(words("alpha"));
    lexicon.addWord(words("beta"));
    lexicon.addWord(words("alpha", "beta"));

    NindLexicon::LexiconChar characteristics;
    ASSERT_TRUE(lexicon.integrityAndCounts(characteristics));
    EXPECT_TRUE(characteristics.isOk);
    EXPECT_EQ(2u, characteristics.swNb);
    EXPECT_EQ(1u, characteristics.cwNb);
    EXPECT_EQ(3u, characteristics.wordsNb);
}
////////////////////////////////////////////////////////////
TEST(NindLexiconTest, PersistsAcrossReopenAsReader) {
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
    EXPECT_EQ(idAlpha, reader.getId(words("alpha")));
    EXPECT_EQ(idCompound, reader.getId(words("alpha", "beta")));
    EXPECT_EQ(0u, reader.getId(words("unknown")));
}
////////////////////////////////////////////////////////////
TEST(NindLexiconTest, ReopenedWriterKeepsPreviousWordsAndContinuesNumbering) {
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
    EXPECT_EQ(idAlpha, writer2.getId(words("alpha")));     // loaded back from file
    const unsigned int idGamma = writer2.addWord(words("gamma"));
    EXPECT_EQ(wordsNbAfterFirstSession + 1, idGamma);      // numbering continues, not restarted
}
////////////////////////////////////////////////////////////
TEST(NindLexiconTest, ReaderCannotAddWord) {
    TestTempDir tmp;
    const string path = tmp.file("readonly.nindlexicon");
    { NindLexicon writer(path, true); }
    NindLexicon reader(path, false);
    EXPECT_THROW(reader.addWord(words("alpha")), NindLexiconException);
}
////////////////////////////////////////////////////////////
TEST(NindLexiconTest, OpeningMissingFileAsReaderThrows) {
    TestTempDir tmp;
    EXPECT_THROW(NindLexicon(tmp.file("missing.nindlexicon"), false), NindLexiconException);
}
