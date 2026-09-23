#include "NindAmose/NindLexiconAmose.h"
#include "TestTempDir.h"
#include "doctest.h"
#include <string>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
TEST_CASE("NindLexiconAmoseTest.SimpleTermRoundTrip") {
    TestTempDir tmp;
    NindLexiconAmose lexicon(tmp.file("simple"), true, 16, 16);

    const unsigned int id = lexicon.addWord("cat", SIMPLE_TERM);
    CHECK_EQ(id, lexicon.getWordId("cat", SIMPLE_TERM));

    string lemma, namedEntity;
    AmoseTypes type;
    REQUIRE(lexicon.getWord(id, lemma, type, namedEntity));
    CHECK_EQ("cat", lemma);
    CHECK_EQ(SIMPLE_TERM, type);
    CHECK_EQ("", namedEntity);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindLexiconAmoseTest.MultiTermRoundTrip") {
    TestTempDir tmp;
    NindLexiconAmose lexicon(tmp.file("multi"), true, 16, 16);

    const unsigned int id = lexicon.addWord("big_cat", MULTI_TERM);
    CHECK_EQ(id, lexicon.getWordId("big_cat", MULTI_TERM));

    string lemma, namedEntity;
    AmoseTypes type;
    REQUIRE(lexicon.getWord(id, lemma, type, namedEntity));
    CHECK_EQ("big_cat", lemma);
    CHECK_EQ(MULTI_TERM, type);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindLexiconAmoseTest.NamedEntityRoundTrip") {
    TestTempDir tmp;
    NindLexiconAmose lexicon(tmp.file("ne"), true, 16, 16);

    const unsigned int id = lexicon.addWord("Paris", NAMED_ENTITY, "LOC");
    CHECK_EQ(id, lexicon.getWordId("Paris", NAMED_ENTITY, "LOC"));

    string lemma, namedEntity;
    AmoseTypes type;
    REQUIRE(lexicon.getWord(id, lemma, type, namedEntity));
    CHECK_EQ("Paris", lemma);
    CHECK_EQ(NAMED_ENTITY, type);
    CHECK_EQ("LOC", namedEntity);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindLexiconAmoseTest.AllTypeIsAlwaysRejected") {
    TestTempDir tmp;
    NindLexiconAmose lexicon(tmp.file("all"), true, 16, 16);
    CHECK_EQ(0u, lexicon.addWord("whatever", ALL));
    CHECK_EQ(0u, lexicon.getWordId("whatever", ALL));
}
////////////////////////////////////////////////////////////
TEST_CASE("NindLexiconAmoseTest.UnknownWordIdReturnsFalse") {
    TestTempDir tmp;
    NindLexiconAmose lexicon(tmp.file("unknown"), true, 16, 16);
    lexicon.addWord("cat", SIMPLE_TERM);
    string lemma, namedEntity;
    AmoseTypes type;
    CHECK_FALSE(lexicon.getWord(999999, lemma, type, namedEntity));
}
////////////////////////////////////////////////////////////
TEST_CASE("NindLexiconAmoseTest.DifferentTypesOfTheSameSpellingAreDistinctEntries") {
    TestTempDir tmp;
    NindLexiconAmose lexicon(tmp.file("distinct"), true, 16, 16);
    const unsigned int simpleId = lexicon.addWord("paris", SIMPLE_TERM);
    const unsigned int neId = lexicon.addWord("paris", NAMED_ENTITY, "LOC");
    CHECK_NE(simpleId, neId);
}
