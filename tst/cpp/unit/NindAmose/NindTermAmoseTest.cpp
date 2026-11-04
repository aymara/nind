#include "NindAmose/NindTermAmose.h"
#include "NindAmose/NindLexiconAmose.h"
#include "TestTempDir.h"
#include "doctest.h"
#include <list>
#include <map>
#include <string>
#include <vector>
#include "NindBasics/NindReadBuffer.h"
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

    list<NindDocId> docs;
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
    list<NindDocId> docs;
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
    list<NindDocId> docs;
    REQUIRE(termIndex.getDocList(catId, docs));
    REQUIRE_EQ(1u, docs.size());
    CHECK_EQ(1u, docs.front());
    CHECK_EQ(3u, termIndex.getTermFreq(catId));
    CHECK_EQ(1u, termIndex.getUniqueTermCount(SIMPLE_TERM));
    CHECK_EQ(3u, termIndex.getTermOccurrences(ALL));
}
////////////////////////////////////////////////////////////
TEST_CASE("NindTermAmoseTest.IndexingNodeByNodeAppendsInPlace") {
    // as Amose indexes: one content node at a time (no bulk mode), ids increasing, an occasional
    // node indexed again or out of order; the postings (with frequencies) and counters as a model
    TestTempDir tmp;
    const string base = tmp.file("nodes");
    NindLexiconAmose lexicon(base, true, 64, 64);
    vector<unsigned int> ids;
    for (unsigned int w = 0; w < 60; w++) ids.push_back(lexicon.addWord("w" + to_string(w), SIMPLE_TERM));
    const NindIndex::Identification identification = lexicon.getIdentification();
    map<unsigned int, map<NindDocId, unsigned int> > model;
    unsigned int occurrences = 0;
    uint64_t state = 11;
    {
        NindTermAmose termIndex(base, true, identification, 64);
        for (unsigned int node = 1; node <= 2500; node++) {
            //an occasional node out of order (an earlier id), or indexed again
            const NindDocId doc = (node % 97 == 0) ? node / 2 : node;
            map<unsigned int, unsigned int> counts;
            const unsigned int length = 1 + (state = state * 6364136223846793005ull + 1442695040888963407ull) % 20;
            for (unsigned int i = 0; i < length; i++) {
                state = state * 6364136223846793005ull + 1442695040888963407ull;
                const unsigned int rank = (unsigned int)((state >> 33) % 100);
                counts[ids[rank * rank / 170]]++;              //w0 in most nodes: a long list
            }
            for (map<unsigned int, unsigned int>::const_iterator c = counts.begin(); c != counts.end(); c++) {
                termIndex.addDocsToTerm(c->first, SIMPLE_TERM, oneDoc((unsigned int)doc, c->second), identification);
                model[c->first][doc] += c->second;
                occurrences += c->second;
            }
        }
        CHECK_EQ(occurrences, termIndex.getTermOccurrences(SIMPLE_TERM));
        CHECK_EQ(model.size(), termIndex.getUniqueTermCount(SIMPLE_TERM));
    }
    NindTermAmose reader(base, false, identification);
    CHECK_EQ(occurrences, reader.getTermOccurrences(ALL));
    NindReadBuffer buffer;
    for (map<unsigned int, map<NindDocId, unsigned int> >::const_iterator t = model.begin(); t != model.end(); t++) {
        INFO("term " << t->first);
        vector<Document> postings, postingsConst;
        REQUIRE(reader.getDocPostings(t->first, postings));
        REQUIRE(reader.getDocPostings(t->first, postingsConst, buffer));
        REQUIRE_EQ(t->second.size(), postings.size());
        size_t i = 0;
        for (map<NindDocId, unsigned int>::const_iterator d = t->second.begin(); d != t->second.end(); d++, i++) {
            CHECK_EQ(d->first, postings[i].ident);
            CHECK_EQ(d->second, postings[i].frequency);
            CHECK_EQ(d->first, postingsConst[i].ident);
        }
        list<NindDocId> docs;
        REQUIRE(reader.getDocList(t->first, docs));
        CHECK_EQ(postings.size(), docs.size());
        CHECK_EQ(t->second.size(), reader.getDocFreq(t->first));
    }
    vector<Document> none;
    CHECK_FALSE(reader.getDocPostings(ids[59] + 1000, none));
}
////////////////////////////////////////////////////////////
