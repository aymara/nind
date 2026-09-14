#include "NindIndex/NindLexiconIndex.h"
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
TEST(NindLexiconIndexTest, AddWordIsIdempotentAndGetWordIdMatches) {
    TestTempDir tmp;
    NindLexiconIndex lexicon(tmp.file("simple"), true, false, 16);

    const unsigned int idAlpha = lexicon.addWord(words("alpha"));
    const unsigned int idBeta = lexicon.addWord(words("beta"));
    EXPECT_NE(idAlpha, idBeta);
    EXPECT_EQ(idAlpha, lexicon.addWord(words("alpha")));

    EXPECT_EQ(idAlpha, lexicon.getWordId(words("alpha")));
    EXPECT_EQ(idBeta, lexicon.getWordId(words("beta")));
    EXPECT_EQ(0u, lexicon.getWordId(words("gamma")));
}
////////////////////////////////////////////////////////////
TEST(NindLexiconIndexTest, CompoundWordIsIdempotent) {
    TestTempDir tmp;
    NindLexiconIndex lexicon(tmp.file("compound"), true, false, 16);

    const unsigned int idAlpha = lexicon.addWord(words("alpha"));
    const unsigned int idCompound = lexicon.addWord(words("alpha", "beta"));
    EXPECT_NE(idAlpha, idCompound);
    EXPECT_EQ(idCompound, lexicon.addWord(words("alpha", "beta")));
    EXPECT_EQ(idCompound, lexicon.getWordId(words("alpha", "beta")));
    EXPECT_EQ(0u, lexicon.getWordId(words("alpha", "gamma")));
}
////////////////////////////////////////////////////////////
TEST(NindLexiconIndexTest, ManyWordsCollideOnASmallModuloButStayDistinguishable) {
    // A tiny indirection bloc size forces several words to share the same
    // hash bucket, exercising the "several words chained inside one
    // definition" path of getIdentifiant/getDefinitionWords.
    TestTempDir tmp;
    NindLexiconIndex lexicon(tmp.file("collide"), true, false, 4);
    const char *lemmas[] = {"un", "deux", "trois", "quatre", "cinq",
                             "six", "sept", "huit", "neuf", "dix"};
    unsigned int ids[10];
    for (int i = 0; i < 10; i++) ids[i] = lexicon.addWord(words(lemmas[i]));
    for (int i = 0; i < 10; i++) {
        for (int j = i + 1; j < 10; j++) EXPECT_NE(ids[i], ids[j]) << lemmas[i] << " vs " << lemmas[j];
    }
    for (int i = 0; i < 10; i++) EXPECT_EQ(ids[i], lexicon.getWordId(words(lemmas[i]))) << lemmas[i];
}
////////////////////////////////////////////////////////////
TEST(NindLexiconIndexTest, GetIdentificationTracksWordsNb) {
    TestTempDir tmp;
    NindLexiconIndex lexicon(tmp.file("ident"), true, false, 16);
    lexicon.addWord(words("alpha"));
    lexicon.addWord(words("beta"));
    lexicon.addWord(words("alpha", "beta"));
    EXPECT_EQ(3u, lexicon.getIdentification().lexiconWordsNb);
}
////////////////////////////////////////////////////////////
TEST(NindLexiconIndexTest, GetComponentsWithoutRetrolexiconThrows) {
    TestTempDir tmp;
    NindLexiconIndex lexicon(tmp.file("noretro"), true, false, 16);
    const unsigned int id = lexicon.addWord(words("alpha"));
    list<string> components;
    EXPECT_THROW(lexicon.getComponents(id, components), NindLexiconIndexException);
}
////////////////////////////////////////////////////////////
TEST(NindLexiconIndexTest, GetComponentsWithRetrolexiconRoundTrips) {
    TestTempDir tmp;
    NindLexiconIndex lexicon(tmp.file("withretro"), true, true, 16, 16);
    const unsigned int idAlpha = lexicon.addWord(words("alpha"));
    const unsigned int idCompound = lexicon.addWord(words("alpha", "beta"));

    list<string> simpleComponents;
    ASSERT_TRUE(lexicon.getComponents(idAlpha, simpleComponents));
    ASSERT_EQ(1u, simpleComponents.size());
    EXPECT_EQ("alpha", simpleComponents.front());

    list<string> compoundComponents;
    ASSERT_TRUE(lexicon.getComponents(idCompound, compoundComponents));
    ASSERT_EQ(2u, compoundComponents.size());
    list<string>::const_iterator it = compoundComponents.begin();
    EXPECT_EQ("alpha", *it++);
    EXPECT_EQ("beta", *it++);
}
////////////////////////////////////////////////////////////
TEST(NindLexiconIndexTest, PersistsAcrossReopenAsReader) {
    TestTempDir tmp;
    const string path = tmp.file("persist");
    unsigned int idAlpha, idCompound;
    {
        NindLexiconIndex writer(path, true, false, 16);
        idAlpha = writer.addWord(words("alpha"));
        idCompound = writer.addWord(words("alpha", "beta"));
    }
    NindLexiconIndex reader(path, false);
    EXPECT_EQ(idAlpha, reader.getWordId(words("alpha")));
    EXPECT_EQ(idCompound, reader.getWordId(words("alpha", "beta")));
    EXPECT_EQ(0u, reader.getWordId(words("unknown")));
}
////////////////////////////////////////////////////////////
TEST(NindLexiconIndexTest, ReaderCannotAddWord) {
    TestTempDir tmp;
    const string path = tmp.file("readonly");
    { NindLexiconIndex writer(path, true, false, 16); }
    NindLexiconIndex reader(path, false);
    EXPECT_THROW(reader.addWord(words("alpha")), NindLexiconIndexException);
}
