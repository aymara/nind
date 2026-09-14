#include "NindIndex/NindTermIndex.h"
#include "NindExceptions.h"
#include "TestTempDir.h"
#include <gtest/gtest.h>
#include <list>
#include <string>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
namespace {
typedef NindTermIndex::Document Document;
typedef NindTermIndex::TermCG TermCG;
const NindIndex::Identification kNoCheck(0, 0);

list<unsigned int> specifics(unsigned int a, unsigned int b) {
    list<unsigned int> l;
    l.push_back(a);
    l.push_back(b);
    return l;
}
}
////////////////////////////////////////////////////////////
TEST(NindTermIndexTest, SetThenGetTermDefRoundTrips) {
    TestTempDir tmp;
    NindTermIndex index(tmp.file("basic"), true, kNoCheck, 2, 8);

    list<TermCG> written;
    written.push_back(TermCG(1, 3));
    written.back().documents.push_back(Document(2, 1));
    written.back().documents.push_back(Document(5, 2));
    index.setTermDef(5, written, kNoCheck, specifics(10, 20));

    list<TermCG> read;
    ASSERT_TRUE(index.getTermDef(5, read));
    ASSERT_EQ(1u, read.size());
    EXPECT_EQ(1, read.front().cg);
    EXPECT_EQ(3u, read.front().frequency);
    ASSERT_EQ(2u, read.front().documents.size());
    list<Document>::const_iterator it = read.front().documents.begin();
    EXPECT_EQ(2u, it->ident);
    EXPECT_EQ(1u, it->frequency);
    ++it;
    EXPECT_EQ(5u, it->ident);
    EXPECT_EQ(2u, it->frequency);

    list<unsigned int> readSpecifics;
    index.getSpecificWords(readSpecifics);
    ASSERT_EQ(2u, readSpecifics.size());
    EXPECT_EQ(10u, readSpecifics.front());
    EXPECT_EQ(20u, readSpecifics.back());
}
////////////////////////////////////////////////////////////
TEST(NindTermIndexTest, UnknownIdentReturnsFalse) {
    TestTempDir tmp;
    NindTermIndex index(tmp.file("unknown"), true, kNoCheck, 0, 8);
    list<TermCG> read;
    EXPECT_FALSE(index.getTermDef(3, read));    // never written, still inside the block
    EXPECT_FALSE(index.getTermDef(999, read));  // out of the allocated indirection range
    EXPECT_TRUE(read.empty());
}
////////////////////////////////////////////////////////////
TEST(NindTermIndexTest, DocumentIdentsWithGapsSurviveDeltaEncoding) {
    TestTempDir tmp;
    NindTermIndex index(tmp.file("gaps"), true, kNoCheck, 0, 8);

    list<TermCG> written;
    written.push_back(TermCG(0, 6));
    written.back().documents.push_back(Document(1, 1));
    written.back().documents.push_back(Document(4, 2));
    written.back().documents.push_back(Document(10, 3));
    index.setTermDef(1, written, kNoCheck, list<unsigned int>());

    list<TermCG> read;
    ASSERT_TRUE(index.getTermDef(1, read));
    ASSERT_EQ(1u, read.size());
    ASSERT_EQ(3u, read.front().documents.size());
    const unsigned int expectedIdents[] = {1, 4, 10};
    const unsigned int expectedFreqs[] = {1, 2, 3};
    int i = 0;
    for (list<Document>::const_iterator it = read.front().documents.begin();
         it != read.front().documents.end(); ++it, ++i) {
        EXPECT_EQ(expectedIdents[i], it->ident);
        EXPECT_EQ(expectedFreqs[i], it->frequency);
    }
}
////////////////////////////////////////////////////////////
TEST(NindTermIndexTest, SetTermDefTwiceOverwritesPreviousContent) {
    TestTempDir tmp;
    NindTermIndex index(tmp.file("overwrite"), true, kNoCheck, 0, 8);

    list<TermCG> first;
    first.push_back(TermCG(0, 1));
    first.back().documents.push_back(Document(1, 1));
    index.setTermDef(1, first, kNoCheck, list<unsigned int>());

    list<TermCG> second;
    second.push_back(TermCG(0, 5));
    second.back().documents.push_back(Document(2, 5));
    index.setTermDef(1, second, kNoCheck, list<unsigned int>());

    list<TermCG> read;
    ASSERT_TRUE(index.getTermDef(1, read));
    ASSERT_EQ(1u, read.size());
    EXPECT_EQ(5u, read.front().frequency);
    ASSERT_EQ(1u, read.front().documents.size());
    EXPECT_EQ(2u, read.front().documents.front().ident);
}
////////////////////////////////////////////////////////////
TEST(NindTermIndexTest, EmptyTermDefDeletesTheEntry) {
    TestTempDir tmp;
    NindTermIndex index(tmp.file("delete"), true, kNoCheck, 0, 8);
    list<TermCG> written;
    written.push_back(TermCG(0, 1));
    written.back().documents.push_back(Document(1, 1));
    index.setTermDef(1, written, kNoCheck, list<unsigned int>());
    ASSERT_TRUE(index.getTermDef(1, written));

    index.setTermDef(1, list<TermCG>(), kNoCheck, list<unsigned int>());
    list<TermCG> read;
    EXPECT_FALSE(index.getTermDef(1, read));
}
////////////////////////////////////////////////////////////
TEST(NindTermIndexTest, WrongSpecificsCountThrows) {
    TestTempDir tmp;
    NindTermIndex index(tmp.file("badspecifics"), true, kNoCheck, 2, 8);
    list<TermCG> written;
    written.push_back(TermCG(0, 1));
    written.back().documents.push_back(Document(1, 1));
    ASSERT_NO_THROW(index.setTermDef(1, written, kNoCheck, specifics(1, 2) /* ok, size 2 */));
    // Now with a mismatching specifics count (0 given, 2 expected).
    EXPECT_THROW(index.setTermDef(1, written, kNoCheck, list<unsigned int>()), NindTermIndexException);
}
////////////////////////////////////////////////////////////
TEST(NindTermIndexTest, GrowsPastInitialBlocSizeAndStaysReadable) {
    TestTempDir tmp;
    NindTermIndex index(tmp.file("grow"), true, kNoCheck, 0, 2);
    for (unsigned int ident = 0; ident < 6; ident++) {
        list<TermCG> written;
        written.push_back(TermCG(0, ident + 1));
        written.back().documents.push_back(Document(0, ident + 1));
        index.setTermDef(ident, written, kNoCheck, list<unsigned int>());
    }
    for (unsigned int ident = 0; ident < 6; ident++) {
        list<TermCG> read;
        ASSERT_TRUE(index.getTermDef(ident, read)) << "ident=" << ident;
        EXPECT_EQ(ident + 1, read.front().frequency);
    }
}
////////////////////////////////////////////////////////////
TEST(NindTermIndexTest, PersistsAcrossReopenAsReaderWithMatchingIdentification) {
    TestTempDir tmp;
    const string path = tmp.file("persist");
    const NindIndex::Identification identification(1, 111);
    {
        NindTermIndex writer(path, true, identification, 0, 8);
        list<TermCG> written;
        written.push_back(TermCG(2, 7));
        written.back().documents.push_back(Document(3, 7));
        writer.setTermDef(9, written, identification, list<unsigned int>());
    }
    NindTermIndex reader(path, false, identification, 0, 8);
    list<TermCG> read;
    ASSERT_TRUE(reader.getTermDef(9, read));
    EXPECT_EQ(2, read.front().cg);
    EXPECT_EQ(7u, read.front().frequency);
}
////////////////////////////////////////////////////////////
TEST(NindTermIndexTest, ReopenWithWrongIdentificationThrows) {
    TestTempDir tmp;
    const string path = tmp.file("badident");
    { NindTermIndex writer(path, true, NindIndex::Identification(1, 111), 0, 8); }
    EXPECT_THROW(NindTermIndex(path, false, NindIndex::Identification(2, 222), 0, 8), NindPadFileException);
}
