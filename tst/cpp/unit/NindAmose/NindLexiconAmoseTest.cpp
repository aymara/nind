#include "NindAmose/NindLexiconAmose.h"
#include "TestTempDir.h"
#include <gtest/gtest.h>
#include <string>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
TEST(NindLexiconAmoseTest, SimpleTermRoundTrip) {
    TestTempDir tmp;
    NindLexiconAmose lexicon(tmp.file("simple"), true, 16, 16);

    const unsigned int id = lexicon.addWord("cat", SIMPLE_TERM);
    EXPECT_EQ(id, lexicon.getWordId("cat", SIMPLE_TERM));

    string lemma, namedEntity;
    AmoseTypes type;
    ASSERT_TRUE(lexicon.getWord(id, lemma, type, namedEntity));
    EXPECT_EQ("cat", lemma);
    EXPECT_EQ(SIMPLE_TERM, type);
    EXPECT_EQ("", namedEntity);
}
////////////////////////////////////////////////////////////
TEST(NindLexiconAmoseTest, MultiTermRoundTrip) {
    TestTempDir tmp;
    NindLexiconAmose lexicon(tmp.file("multi"), true, 16, 16);

    const unsigned int id = lexicon.addWord("big_cat", MULTI_TERM);
    EXPECT_EQ(id, lexicon.getWordId("big_cat", MULTI_TERM));

    string lemma, namedEntity;
    AmoseTypes type;
    ASSERT_TRUE(lexicon.getWord(id, lemma, type, namedEntity));
    EXPECT_EQ("big_cat", lemma);
    EXPECT_EQ(MULTI_TERM, type);
}
////////////////////////////////////////////////////////////
TEST(NindLexiconAmoseTest, NamedEntityRoundTrip) {
    TestTempDir tmp;
    NindLexiconAmose lexicon(tmp.file("ne"), true, 16, 16);

    const unsigned int id = lexicon.addWord("Paris", NAMED_ENTITY, "LOC");
    EXPECT_EQ(id, lexicon.getWordId("Paris", NAMED_ENTITY, "LOC"));

    string lemma, namedEntity;
    AmoseTypes type;
    ASSERT_TRUE(lexicon.getWord(id, lemma, type, namedEntity));
    EXPECT_EQ("Paris", lemma);
    EXPECT_EQ(NAMED_ENTITY, type);
    EXPECT_EQ("LOC", namedEntity);
}
////////////////////////////////////////////////////////////
TEST(NindLexiconAmoseTest, AllTypeIsAlwaysRejected) {
    TestTempDir tmp;
    NindLexiconAmose lexicon(tmp.file("all"), true, 16, 16);
    EXPECT_EQ(0u, lexicon.addWord("whatever", ALL));
    EXPECT_EQ(0u, lexicon.getWordId("whatever", ALL));
}
////////////////////////////////////////////////////////////
TEST(NindLexiconAmoseTest, UnknownWordIdReturnsFalse) {
    TestTempDir tmp;
    NindLexiconAmose lexicon(tmp.file("unknown"), true, 16, 16);
    lexicon.addWord("cat", SIMPLE_TERM);
    string lemma, namedEntity;
    AmoseTypes type;
    EXPECT_FALSE(lexicon.getWord(999999, lemma, type, namedEntity));
}
////////////////////////////////////////////////////////////
TEST(NindLexiconAmoseTest, DifferentTypesOfTheSameSpellingAreDistinctEntries) {
    TestTempDir tmp;
    NindLexiconAmose lexicon(tmp.file("distinct"), true, 16, 16);
    const unsigned int simpleId = lexicon.addWord("paris", SIMPLE_TERM);
    const unsigned int neId = lexicon.addWord("paris", NAMED_ENTITY, "LOC");
    EXPECT_NE(simpleId, neId);
}
