#include "NindAmose/NindTermAmose.h"
#include "NindAmose/NindLexiconAmose.h"
#include "TestTempDir.h"
#include <gtest/gtest.h>
#include <list>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
namespace {
typedef NindTermIndex::Document Document;

list<Document> oneDoc(unsigned int ident, unsigned int freq) {
    list<Document> l;
    l.push_back(Document(ident, freq));
    return l;
}
}
////////////////////////////////////////////////////////////
TEST(NindTermAmoseTest, AddDocsToTermAggregatesCountsAndDocList) {
    TestTempDir tmp;
    NindLexiconAmose lexicon(tmp.file("term1"), true, 16, 16);
    const unsigned int catId = lexicon.addWord("cat", SIMPLE_TERM);
    const NindIndex::Identification identification = lexicon.getIdentification();

    NindTermAmose termIndex(tmp.file("term1"), true, identification, 8);
    termIndex.addDocsToTerm(catId, SIMPLE_TERM, oneDoc(1, 3), identification);
    termIndex.addDocsToTerm(catId, SIMPLE_TERM, oneDoc(2, 5), identification);

    list<unsigned int> docs;
    ASSERT_TRUE(termIndex.getDocList(catId, docs));
    ASSERT_EQ(2u, docs.size());
    EXPECT_EQ(2u, termIndex.getDocFreq(catId));

    EXPECT_EQ(1u, termIndex.getUniqueTermCount(SIMPLE_TERM));
    EXPECT_EQ(1u, termIndex.getUniqueTermCount(ALL));
    EXPECT_EQ(8u, termIndex.getTermOccurrences(SIMPLE_TERM));
    EXPECT_EQ(8u, termIndex.getTermOccurrences(ALL));
}
////////////////////////////////////////////////////////////
TEST(NindTermAmoseTest, AddingSameDocAgainAccumulatesFrequency) {
    TestTempDir tmp;
    NindLexiconAmose lexicon(tmp.file("term2"), true, 16, 16);
    const unsigned int catId = lexicon.addWord("cat", SIMPLE_TERM);
    const NindIndex::Identification identification = lexicon.getIdentification();

    NindTermAmose termIndex(tmp.file("term2"), true, identification, 8);
    termIndex.addDocsToTerm(catId, SIMPLE_TERM, oneDoc(1, 3), identification);
    termIndex.addDocsToTerm(catId, SIMPLE_TERM, oneDoc(1, 4), identification);

    EXPECT_EQ(1u, termIndex.getDocFreq(catId));            // still a single document
    EXPECT_EQ(7u, termIndex.getTermOccurrences(ALL));       // 3 + 4
}
////////////////////////////////////////////////////////////
TEST(NindTermAmoseTest, RemovingLastDocFromTermErasesItAndDecrementsUniqueCount) {
    TestTempDir tmp;
    NindLexiconAmose lexicon(tmp.file("term3"), true, 16, 16);
    const unsigned int catId = lexicon.addWord("cat", SIMPLE_TERM);
    const NindIndex::Identification identification = lexicon.getIdentification();

    NindTermAmose termIndex(tmp.file("term3"), true, identification, 8);
    termIndex.addDocsToTerm(catId, SIMPLE_TERM, oneDoc(1, 3), identification);
    ASSERT_EQ(1u, termIndex.getUniqueTermCount(SIMPLE_TERM));

    termIndex.removeDocFromTerm(catId, SIMPLE_TERM, 1, identification);
    EXPECT_EQ(0u, termIndex.getUniqueTermCount(SIMPLE_TERM));
    EXPECT_EQ(0u, termIndex.getDocFreq(catId));
    list<unsigned int> docs;
    EXPECT_FALSE(termIndex.getDocList(catId, docs));
}
////////////////////////////////////////////////////////////
TEST(NindTermAmoseTest, DistinctTermTypesAreCountedSeparately) {
    TestTempDir tmp;
    NindLexiconAmose lexicon(tmp.file("term4"), true, 16, 16);
    const unsigned int catId = lexicon.addWord("cat", SIMPLE_TERM);
    const unsigned int parisId = lexicon.addWord("Paris", NAMED_ENTITY, "LOC");
    const NindIndex::Identification identification = lexicon.getIdentification();

    NindTermAmose termIndex(tmp.file("term4"), true, identification, 8);
    termIndex.addDocsToTerm(catId, SIMPLE_TERM, oneDoc(1, 2), identification);
    termIndex.addDocsToTerm(parisId, NAMED_ENTITY, oneDoc(1, 1), identification);

    EXPECT_EQ(1u, termIndex.getUniqueTermCount(SIMPLE_TERM));
    EXPECT_EQ(1u, termIndex.getUniqueTermCount(NAMED_ENTITY));
    EXPECT_EQ(2u, termIndex.getUniqueTermCount(ALL));
    EXPECT_EQ(2u, termIndex.getTermOccurrences(SIMPLE_TERM));
    EXPECT_EQ(1u, termIndex.getTermOccurrences(NAMED_ENTITY));
    EXPECT_EQ(3u, termIndex.getTermOccurrences(ALL));
}
////////////////////////////////////////////////////////////
TEST(NindTermAmoseTest, CountsPersistAcrossReopenAsReader) {
    TestTempDir tmp;
    const string path = tmp.file("term5");
    NindLexiconAmose lexicon(path, true, 16, 16);
    const unsigned int catId = lexicon.addWord("cat", SIMPLE_TERM);
    const NindIndex::Identification identification = lexicon.getIdentification();
    {
        NindTermAmose writer(path, true, identification, 8);
        writer.addDocsToTerm(catId, SIMPLE_TERM, oneDoc(1, 3), identification);
    }
    NindTermAmose reader(path, false, identification, 8);
    EXPECT_EQ(1u, reader.getUniqueTermCount(SIMPLE_TERM));
    EXPECT_EQ(3u, reader.getTermOccurrences(SIMPLE_TERM));
    EXPECT_EQ(1u, reader.getDocFreq(catId));
}
