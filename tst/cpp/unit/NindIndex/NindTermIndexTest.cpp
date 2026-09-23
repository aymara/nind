#include "NindIndex/NindTermIndex.h"
#include "NindExceptions.h"
#include "TestTempDir.h"
#include "doctest.h"
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
TEST_CASE("NindTermIndexTest.SetThenGetTermDefRoundTrips") {
    TestTempDir tmp;
    NindTermIndex index(tmp.file("basic"), true, kNoCheck, 2, 8);

    list<TermCG> written;
    written.push_back(TermCG(1, 3));
    written.back().documents.push_back(Document(2, 1));
    written.back().documents.push_back(Document(5, 2));
    index.setTermDef(5, written, kNoCheck, specifics(10, 20));

    list<TermCG> read;
    REQUIRE(index.getTermDef(5, read));
    REQUIRE_EQ(1u, read.size());
    CHECK_EQ(1, read.front().cg);
    CHECK_EQ(3u, read.front().frequency);
    REQUIRE_EQ(2u, read.front().documents.size());
    list<Document>::const_iterator it = read.front().documents.begin();
    CHECK_EQ(2u, it->ident);
    CHECK_EQ(1u, it->frequency);
    ++it;
    CHECK_EQ(5u, it->ident);
    CHECK_EQ(2u, it->frequency);

    list<unsigned int> readSpecifics;
    index.getSpecificWords(readSpecifics);
    REQUIRE_EQ(2u, readSpecifics.size());
    CHECK_EQ(10u, readSpecifics.front());
    CHECK_EQ(20u, readSpecifics.back());
}
////////////////////////////////////////////////////////////
TEST_CASE("NindTermIndexTest.UnknownIdentReturnsFalse") {
    TestTempDir tmp;
    NindTermIndex index(tmp.file("unknown"), true, kNoCheck, 0, 8);
    list<TermCG> read;
    CHECK_FALSE(index.getTermDef(3, read));    // never written, still inside the block
    CHECK_FALSE(index.getTermDef(999, read));  // out of the allocated indirection range
    CHECK(read.empty());
}
////////////////////////////////////////////////////////////
TEST_CASE("NindTermIndexTest.DocumentIdentsWithGapsSurviveDeltaEncoding") {
    TestTempDir tmp;
    NindTermIndex index(tmp.file("gaps"), true, kNoCheck, 0, 8);

    list<TermCG> written;
    written.push_back(TermCG(0, 6));
    written.back().documents.push_back(Document(1, 1));
    written.back().documents.push_back(Document(4, 2));
    written.back().documents.push_back(Document(10, 3));
    index.setTermDef(1, written, kNoCheck, list<unsigned int>());

    list<TermCG> read;
    REQUIRE(index.getTermDef(1, read));
    REQUIRE_EQ(1u, read.size());
    REQUIRE_EQ(3u, read.front().documents.size());
    const unsigned int expectedIdents[] = {1, 4, 10};
    const unsigned int expectedFreqs[] = {1, 2, 3};
    int i = 0;
    for (list<Document>::const_iterator it = read.front().documents.begin();
         it != read.front().documents.end(); ++it, ++i) {
        CHECK_EQ(expectedIdents[i], it->ident);
        CHECK_EQ(expectedFreqs[i], it->frequency);
    }
}
////////////////////////////////////////////////////////////
TEST_CASE("NindTermIndexTest.SetTermDefTwiceOverwritesPreviousContent") {
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
    REQUIRE(index.getTermDef(1, read));
    REQUIRE_EQ(1u, read.size());
    CHECK_EQ(5u, read.front().frequency);
    REQUIRE_EQ(1u, read.front().documents.size());
    CHECK_EQ(2u, read.front().documents.front().ident);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindTermIndexTest.EmptyTermDefDeletesTheEntry") {
    TestTempDir tmp;
    NindTermIndex index(tmp.file("delete"), true, kNoCheck, 0, 8);
    list<TermCG> written;
    written.push_back(TermCG(0, 1));
    written.back().documents.push_back(Document(1, 1));
    index.setTermDef(1, written, kNoCheck, list<unsigned int>());
    REQUIRE(index.getTermDef(1, written));

    index.setTermDef(1, list<TermCG>(), kNoCheck, list<unsigned int>());
    list<TermCG> read;
    CHECK_FALSE(index.getTermDef(1, read));
}
////////////////////////////////////////////////////////////
TEST_CASE("NindTermIndexTest.WrongSpecificsCountThrows") {
    TestTempDir tmp;
    NindTermIndex index(tmp.file("badspecifics"), true, kNoCheck, 2, 8);
    list<TermCG> written;
    written.push_back(TermCG(0, 1));
    written.back().documents.push_back(Document(1, 1));
    REQUIRE_NOTHROW(index.setTermDef(1, written, kNoCheck, specifics(1, 2) /* ok, size 2 */));
    // Now with a mismatching specifics count (0 given, 2 expected).
    CHECK_THROWS_AS(index.setTermDef(1, written, kNoCheck, list<unsigned int>()), NindTermIndexException);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindTermIndexTest.GrowsPastInitialBlocSizeAndStaysReadable") {
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
        { INFO("ident=" << ident); REQUIRE(index.getTermDef(ident, read)); }
        CHECK_EQ(ident + 1, read.front().frequency);
    }
}
////////////////////////////////////////////////////////////
TEST_CASE("NindTermIndexTest.PersistsAcrossReopenAsReaderWithMatchingIdentification") {
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
    REQUIRE(reader.getTermDef(9, read));
    CHECK_EQ(2, read.front().cg);
    CHECK_EQ(7u, read.front().frequency);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindTermIndexTest.ReopenWithWrongIdentificationThrows") {
    TestTempDir tmp;
    const string path = tmp.file("badident");
    { NindTermIndex writer(path, true, NindIndex::Identification(1, 111), 0, 8); }
    CHECK_THROWS_AS(NindTermIndex(path, false, NindIndex::Identification(2, 222), 0, 8), NindPadFileException);
}
