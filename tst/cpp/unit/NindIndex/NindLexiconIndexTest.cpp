#include "NindIndex/NindLexiconIndex.h"
#include "NindExceptions.h"
#include "TestTempDir.h"
#include "doctest.h"
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
TEST_CASE("NindLexiconIndexTest.AddWordIsIdempotentAndGetWordIdMatches") {
    TestTempDir tmp;
    NindLexiconIndex lexicon(tmp.file("simple"), true, false, 16);

    const unsigned int idAlpha = lexicon.addWord(words("alpha"));
    const unsigned int idBeta = lexicon.addWord(words("beta"));
    CHECK_NE(idAlpha, idBeta);
    CHECK_EQ(idAlpha, lexicon.addWord(words("alpha")));

    CHECK_EQ(idAlpha, lexicon.getWordId(words("alpha")));
    CHECK_EQ(idBeta, lexicon.getWordId(words("beta")));
    CHECK_EQ(0u, lexicon.getWordId(words("gamma")));
}
////////////////////////////////////////////////////////////
TEST_CASE("NindLexiconIndexTest.CompoundWordIsIdempotent") {
    TestTempDir tmp;
    NindLexiconIndex lexicon(tmp.file("compound"), true, false, 16);

    const unsigned int idAlpha = lexicon.addWord(words("alpha"));
    const unsigned int idCompound = lexicon.addWord(words("alpha", "beta"));
    CHECK_NE(idAlpha, idCompound);
    CHECK_EQ(idCompound, lexicon.addWord(words("alpha", "beta")));
    CHECK_EQ(idCompound, lexicon.getWordId(words("alpha", "beta")));
    CHECK_EQ(0u, lexicon.getWordId(words("alpha", "gamma")));
}
////////////////////////////////////////////////////////////
TEST_CASE("NindLexiconIndexTest.ManyWordsCollideOnASmallModuloButStayDistinguishable") {
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
        for (int j = i + 1; j < 10; j++) { INFO(lemmas[i] << " vs " << lemmas[j]); CHECK_NE(ids[i], ids[j]); }
    }
    for (int i = 0; i < 10; i++) { INFO(lemmas[i]); CHECK_EQ(ids[i], lexicon.getWordId(words(lemmas[i]))); }
}
////////////////////////////////////////////////////////////
TEST_CASE("NindLexiconIndexTest.GetIdentificationTracksWordsNb") {
    TestTempDir tmp;
    NindLexiconIndex lexicon(tmp.file("ident"), true, false, 16);
    lexicon.addWord(words("alpha"));
    lexicon.addWord(words("beta"));
    lexicon.addWord(words("alpha", "beta"));
    CHECK_EQ(3u, lexicon.getIdentification().lexiconWordsNb);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindLexiconIndexTest.GetComponentsWithoutRetrolexiconThrows") {
    TestTempDir tmp;
    NindLexiconIndex lexicon(tmp.file("noretro"), true, false, 16);
    const unsigned int id = lexicon.addWord(words("alpha"));
    list<string> components;
    CHECK_THROWS_AS(lexicon.getComponents(id, components), NindLexiconIndexException);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindLexiconIndexTest.GetComponentsWithRetrolexiconRoundTrips") {
    TestTempDir tmp;
    NindLexiconIndex lexicon(tmp.file("withretro"), true, true, 16, 16);
    const unsigned int idAlpha = lexicon.addWord(words("alpha"));
    const unsigned int idCompound = lexicon.addWord(words("alpha", "beta"));

    list<string> simpleComponents;
    REQUIRE(lexicon.getComponents(idAlpha, simpleComponents));
    REQUIRE_EQ(1u, simpleComponents.size());
    CHECK_EQ("alpha", simpleComponents.front());

    list<string> compoundComponents;
    REQUIRE(lexicon.getComponents(idCompound, compoundComponents));
    REQUIRE_EQ(2u, compoundComponents.size());
    list<string>::const_iterator it = compoundComponents.begin();
    CHECK_EQ("alpha", *it++);
    CHECK_EQ("beta", *it++);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindLexiconIndexTest.PersistsAcrossReopenAsReader") {
    TestTempDir tmp;
    const string path = tmp.file("persist");
    unsigned int idAlpha, idCompound;
    {
        NindLexiconIndex writer(path, true, false, 16);
        idAlpha = writer.addWord(words("alpha"));
        idCompound = writer.addWord(words("alpha", "beta"));
    }
    NindLexiconIndex reader(path, false);
    CHECK_EQ(idAlpha, reader.getWordId(words("alpha")));
    CHECK_EQ(idCompound, reader.getWordId(words("alpha", "beta")));
    CHECK_EQ(0u, reader.getWordId(words("unknown")));
}
////////////////////////////////////////////////////////////
TEST_CASE("NindLexiconIndexTest.ReaderCannotAddWord") {
    TestTempDir tmp;
    const string path = tmp.file("readonly");
    { NindLexiconIndex writer(path, true, false, 16); }
    NindLexiconIndex reader(path, false);
    CHECK_THROWS_AS(reader.addWord(words("alpha")), NindLexiconIndexException);
}
