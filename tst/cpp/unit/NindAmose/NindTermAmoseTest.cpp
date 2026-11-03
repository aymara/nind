#include "NindAmose/NindTermAmose.h"
#include "NindAmose/NindLexiconAmose.h"
#include "TestTempDir.h"
#include "doctest.h"
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
TEST_CASE("NindTermAmoseTest.AddDocsToTermAggregatesCountsAndDocList") {
    TestTempDir tmp;
    NindLexiconAmose lexicon(tmp.file("term1"), true, 16, 16);
    const unsigned int catId = lexicon.addWord("cat", SIMPLE_TERM);
    const NindIndex::Identification identification = lexicon.getIdentification();

    NindTermAmose termIndex(tmp.file("term1"), true, identification, 8);
    termIndex.addDocsToTerm(catId, SIMPLE_TERM, oneDoc(1, 3), identification);
    termIndex.addDocsToTerm(catId, SIMPLE_TERM, oneDoc(2, 5), identification);

    list<unsigned int> docs;
    REQUIRE(termIndex.getDocList(catId, docs));
    REQUIRE_EQ(2u, docs.size());
    CHECK_EQ(2u, termIndex.getDocFreq(catId));

    CHECK_EQ(1u, termIndex.getUniqueTermCount(SIMPLE_TERM));
    CHECK_EQ(1u, termIndex.getUniqueTermCount(ALL));
    CHECK_EQ(8u, termIndex.getTermOccurrences(SIMPLE_TERM));
    CHECK_EQ(8u, termIndex.getTermOccurrences(ALL));
}
////////////////////////////////////////////////////////////
TEST_CASE("NindTermAmoseTest.AddingSameDocAgainAccumulatesFrequency") {
    TestTempDir tmp;
    NindLexiconAmose lexicon(tmp.file("term2"), true, 16, 16);
    const unsigned int catId = lexicon.addWord("cat", SIMPLE_TERM);
    const NindIndex::Identification identification = lexicon.getIdentification();

    NindTermAmose termIndex(tmp.file("term2"), true, identification, 8);
    termIndex.addDocsToTerm(catId, SIMPLE_TERM, oneDoc(1, 3), identification);
    termIndex.addDocsToTerm(catId, SIMPLE_TERM, oneDoc(1, 4), identification);

    CHECK_EQ(1u, termIndex.getDocFreq(catId));            // still a single document
    CHECK_EQ(7u, termIndex.getTermOccurrences(ALL));       // 3 + 4
}
////////////////////////////////////////////////////////////
TEST_CASE("NindTermAmoseTest.RemovingLastDocFromTermErasesItAndDecrementsUniqueCount") {
    TestTempDir tmp;
    NindLexiconAmose lexicon(tmp.file("term3"), true, 16, 16);
    const unsigned int catId = lexicon.addWord("cat", SIMPLE_TERM);
    const NindIndex::Identification identification = lexicon.getIdentification();

    NindTermAmose termIndex(tmp.file("term3"), true, identification, 8);
    termIndex.addDocsToTerm(catId, SIMPLE_TERM, oneDoc(1, 3), identification);
    REQUIRE_EQ(1u, termIndex.getUniqueTermCount(SIMPLE_TERM));

    termIndex.removeDocFromTerm(catId, SIMPLE_TERM, 1, identification);
    CHECK_EQ(0u, termIndex.getUniqueTermCount(SIMPLE_TERM));
    CHECK_EQ(0u, termIndex.getDocFreq(catId));
    list<unsigned int> docs;
    CHECK_FALSE(termIndex.getDocList(catId, docs));
}
////////////////////////////////////////////////////////////
TEST_CASE("NindTermAmoseTest.DistinctTermTypesAreCountedSeparately") {
    TestTempDir tmp;
    NindLexiconAmose lexicon(tmp.file("term4"), true, 16, 16);
    const unsigned int catId = lexicon.addWord("cat", SIMPLE_TERM);
    const unsigned int parisId = lexicon.addWord("Paris", NAMED_ENTITY, "LOC");
    const NindIndex::Identification identification = lexicon.getIdentification();

    NindTermAmose termIndex(tmp.file("term4"), true, identification, 8);
    termIndex.addDocsToTerm(catId, SIMPLE_TERM, oneDoc(1, 2), identification);
    termIndex.addDocsToTerm(parisId, NAMED_ENTITY, oneDoc(1, 1), identification);

    CHECK_EQ(1u, termIndex.getUniqueTermCount(SIMPLE_TERM));
    CHECK_EQ(1u, termIndex.getUniqueTermCount(NAMED_ENTITY));
    CHECK_EQ(2u, termIndex.getUniqueTermCount(ALL));
    CHECK_EQ(2u, termIndex.getTermOccurrences(SIMPLE_TERM));
    CHECK_EQ(1u, termIndex.getTermOccurrences(NAMED_ENTITY));
    CHECK_EQ(3u, termIndex.getTermOccurrences(ALL));
}
////////////////////////////////////////////////////////////
TEST_CASE("NindTermAmoseTest.CountsPersistAcrossReopenAsReader") {
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
    CHECK_EQ(1u, reader.getUniqueTermCount(SIMPLE_TERM));
    CHECK_EQ(3u, reader.getTermOccurrences(SIMPLE_TERM));
    CHECK_EQ(1u, reader.getDocFreq(catId));
}
////////////////////////////////////////////////////////////
TEST_CASE("NindTermAmoseTest.AddDocsToTermMergesUnsortedBatchWithDuplicatesIntoOrderedList") {
    TestTempDir tmp;
    NindLexiconAmose lexicon(tmp.file("term6"), true, 16, 16);
    const unsigned int catId = lexicon.addWord("cat", SIMPLE_TERM);
    const NindIndex::Identification identification = lexicon.getIdentification();

    NindTermAmose termIndex(tmp.file("term6"), true, identification, 8);
    list<Document> first;
    first.push_back(Document(10, 1));
    first.push_back(Document(30, 1));
    termIndex.addDocsToTerm(catId, SIMPLE_TERM, first, identification);
    //unsorted, with a duplicate inside the batch and one of an already indexed doc,
    //before, between and after the existing docs
    list<Document> batch;
    batch.push_back(Document(40, 2));
    batch.push_back(Document(5, 3));
    batch.push_back(Document(20, 4));
    batch.push_back(Document(30, 5));
    batch.push_back(Document(20, 6));
    batch.push_back(Document(50, 7));
    termIndex.addDocsToTerm(catId, SIMPLE_TERM, batch, identification);

    list<NindTermIndex::TermCG> termDef;
    REQUIRE(termIndex.getTermDef(catId, termDef));
    REQUIRE_EQ(1u, termDef.size());
    const list<Document> &documents = termDef.front().documents;
    const unsigned int expected[][2] = {{5, 3}, {10, 1}, {20, 10}, {30, 6}, {40, 2}, {50, 7}};
    REQUIRE_EQ(6u, documents.size());
    unsigned int i = 0;
    for (list<Document>::const_iterator it = documents.begin(); it != documents.end(); it++, i++) {
        CHECK_EQ(expected[i][0], (*it).ident);
        CHECK_EQ(expected[i][1], (*it).frequency);
    }
    CHECK_EQ(29u, termDef.front().frequency);
    CHECK_EQ(1u, termIndex.getUniqueTermCount(SIMPLE_TERM));
    CHECK_EQ(29u, termIndex.getTermOccurrences(ALL));
}
////////////////////////////////////////////////////////////
TEST_CASE("NindTermAmoseTest.RemovingLastListedDocKeepsTheOtherDocs") {
    TestTempDir tmp;
    NindLexiconAmose lexicon(tmp.file("term7"), true, 16, 16);
    const unsigned int catId = lexicon.addWord("cat", SIMPLE_TERM);
    const NindIndex::Identification identification = lexicon.getIdentification();

    NindTermAmose termIndex(tmp.file("term7"), true, identification, 8);
    termIndex.addDocsToTerm(catId, SIMPLE_TERM, oneDoc(1, 3), identification);
    termIndex.addDocsToTerm(catId, SIMPLE_TERM, oneDoc(2, 5), identification);

    //doc 2 is the last one of the ordered list, but not the only one
    termIndex.removeDocFromTerm(catId, SIMPLE_TERM, 2, identification);
    list<unsigned int> docs;
    REQUIRE(termIndex.getDocList(catId, docs));
    REQUIRE_EQ(1u, docs.size());
    CHECK_EQ(1u, docs.front());
    CHECK_EQ(3u, termIndex.getTermFreq(catId));
    CHECK_EQ(1u, termIndex.getUniqueTermCount(SIMPLE_TERM));
    CHECK_EQ(3u, termIndex.getTermOccurrences(ALL));
}
