#include "NindIndex/NindTermIndex.h"
#include "NindExceptions.h"
#include "TestTempDir.h"
#include "TestFileBytes.h"
#include "doctest.h"
#include <list>
#include <vector>
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
TEST_CASE("NindTermIndexTest.GetTermPostingsMatchesGetTermDef") {
    TestTempDir tmp;
    NindTermIndex index(tmp.file("postings"), true, kNoCheck, 0, 8);
    list<TermCG> written;
    written.push_back(TermCG(1, 4));
    written.back().documents.push_back(Document(2, 1));
    written.back().documents.push_back(Document(70000, 3));
    written.push_back(TermCG(7, 2));
    written.back().documents.push_back(Document(2, 2));
    written.push_back(TermCG(9, 0));                       // a category without documents
    written.push_back(TermCG(3, 1));
    written.back().documents.push_back(Document(4000000000u, 1));
    index.setTermDef(5, written, kNoCheck, list<unsigned int>());

    vector<Document> postings(3, Document(99, 99));        // cleared, capacity reused
    vector<size_t> categoryEnds(2, 42);
    REQUIRE(index.getTermPostings(5, postings, categoryEnds));
    vector<size_t> expectedEnds;
    expectedEnds.push_back(2);
    expectedEnds.push_back(3);
    expectedEnds.push_back(3);
    expectedEnds.push_back(4);
    CHECK(categoryEnds == expectedEnds);
    size_t i = 0;
    for (list<TermCG>::const_iterator cg = written.begin(); cg != written.end(); cg++)
        for (list<Document>::const_iterator doc = cg->documents.begin(); doc != cg->documents.end(); doc++, i++) {
            REQUIRE(i < postings.size());
            CHECK_EQ(doc->ident, postings[i].ident);
            CHECK_EQ(doc->frequency, postings[i].frequency);
        }
    CHECK_EQ(i, postings.size());

    CHECK_FALSE(index.getTermPostings(3, postings, categoryEnds));     // never written
    CHECK_FALSE(index.getTermPostings(999, postings, categoryEnds));   // out of range
    CHECK(postings.empty());
    CHECK(categoryEnds.empty());
}
////////////////////////////////////////////////////////////
TEST_CASE("NindTermIndexTest.DefinitionOver16MiBRoundTrips") {
    // format v1's 3-byte length capped a definition at 16 MiB (a longer one was refused, and
    // before that written truncated); format v2's 4-byte length holds it
    TestTempDir tmp;
    NindTermIndex index(tmp.file("big"), true, kNoCheck, 0, 8);
    list<TermCG> small;
    small.push_back(TermCG(0, 1));
    small.back().documents.push_back(Document(3, 1));
    index.setTermDef(1, small, kNoCheck, list<unsigned int>());

    list<TermCG> big;
    big.push_back(TermCG(0, 0));
    // 2-byte deltas, 2-byte frequencies: 4 bytes per posting, ~18 MB
    for (unsigned int i = 0; i < 4500000; i++) big.back().documents.push_back(Document(i * 900, 200));
    index.setTermDef(2, big, kNoCheck, list<unsigned int>());

    NindTermIndex reader(tmp.file("big"), false, kNoCheck, 0);
    list<TermCG> read;
    REQUIRE(reader.getTermDef(1, read));
    CHECK_EQ(3u, read.front().documents.front().ident);
    read.clear();
    REQUIRE(reader.getTermDef(2, read));
    REQUIRE_EQ(1u, read.size());
    REQUIRE_EQ(4500000u, read.front().documents.size());
    CHECK_EQ(4499999ull * 900, read.front().documents.back().ident);
    CHECK_EQ(200u, read.front().documents.back().frequency);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindTermIndexTest.SetTermPostingsWritesTheSameFileAsSetTermDef") {
    // the flat writer (no list node per posting) and the list one: byte-identical files, for
    // inline lists, lists of blocks, several categories, 64-bit gaps (the buffer's exact bound
    // for bit-packed blocks: 64 bits per gap) and an erasure
    TestTempDir tmp;
    vector<vector<TermCG> > terms(5);
    terms[0].push_back(TermCG(0, 5));
    terms[0].back().documents.push_back(Document(7, 2));
    terms[0].back().documents.push_back(Document(9, 3));
    for (unsigned int c = 0; c < 2; c++) {
        terms[1].push_back(TermCG(3 + c, 0));
        for (unsigned int i = 0; i < 1000 + 300 * c; i++) {
            terms[1].back().documents.push_back(Document(i * (2 + c) + c, 1 + i % 7));
            terms[1].back().frequency += 1 + i % 7;
        }
    }
    terms[2].push_back(TermCG(1, 0));
    unsigned long long ident = 1;
    for (unsigned int i = 0; i < 700; i++) {
        terms[2].back().documents.push_back(Document(ident, i % 3 ? 1 : 1000000));
        terms[2].back().frequency += i % 3 ? 1 : 1000000;
        ident += (i % 5 == 0) ? (1ull << 52) + i : 1;
    }
    terms[3].push_back(TermCG(0, 1));
    terms[3].back().documents.push_back(Document(4, 1));
    // terms[4] stays empty: written after terms[3] at the same ident, it erases it
    {
        NindTermIndex index(tmp.file("list"), true, kNoCheck, 0, 8);
        for (unsigned int t = 0; t < 4; t++)
            index.setTermDef(t + 1, list<TermCG>(terms[t].begin(), terms[t].end()), kNoCheck, list<unsigned int>());
        index.setTermDef(4, list<TermCG>(), kNoCheck, list<unsigned int>());
    }
    {
        NindTermIndex index(tmp.file("flat"), true, kNoCheck, 0, 8);
        vector<vector<Document> > documents;
        for (unsigned int t = 0; t < 5; t++) {
            vector<NindTermIndex::CategoryPostings> categories;
            documents.assign(terms[t].size(), vector<Document>());
            for (size_t c = 0; c < terms[t].size(); c++) {
                documents[c].assign(terms[t][c].documents.begin(), terms[t][c].documents.end());
                categories.push_back(NindTermIndex::CategoryPostings(terms[t][c].cg, terms[t][c].frequency,
                                                                     documents[c].data(), documents[c].size()));
            }
            index.setTermPostings(t < 4 ? t + 1 : 4, categories, kNoCheck, list<unsigned int>());
        }
    }
    CHECK(readFileBytes(tmp.file("flat") + ".nindtermindex") == readFileBytes(tmp.file("list") + ".nindtermindex"));

    // the categories read back with the postings
    NindTermIndex reader(tmp.file("flat"), false, kNoCheck, 0);
    vector<Document> postings;
    vector<size_t> categoryEnds;
    vector<pair<unsigned char, unsigned int> > categories;
    REQUIRE(reader.getTermPostings(2, postings, categoryEnds, categories));
    REQUIRE_EQ(2u, categories.size());
    CHECK_EQ(3, categories[0].first);
    CHECK_EQ(terms[1][0].frequency, categories[0].second);
    CHECK_EQ(4, categories[1].first);
    CHECK_EQ(terms[1][1].frequency, categories[1].second);
    CHECK_EQ(1000u, categoryEnds[0]);
    CHECK_EQ(2300u, categoryEnds[1]);
    REQUIRE(reader.getTermPostings(3, postings, categoryEnds, categories));
    CHECK(vector<Document>(terms[2][0].documents.begin(), terms[2][0].documents.end()).back().ident == postings.back().ident);
    CHECK_FALSE(reader.getTermPostings(4, postings, categoryEnds, categories));
}
////////////////////////////////////////////////////////////
TEST_CASE("NindTermIndexTest.SetTermPostingsRefusesUnorderedDocuments") {
    TestTempDir tmp;
    NindTermIndex index(tmp.file("unordered"), true, kNoCheck, 0, 8);
    const Document documents[] = {Document(5, 1), Document(3, 1)};
    const vector<NindTermIndex::CategoryPostings> categories(1, NindTermIndex::CategoryPostings(0, 2, documents, 2));
    CHECK_THROWS_AS(index.setTermPostings(1, categories, kNoCheck, list<unsigned int>()), NindTermIndexException);
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
////////////////////////////////////////////////////////////
TEST_CASE("NindTermIndexTest.SecondWriterOnTheSameFileIsRefused") {
    TestTempDir tmp;
    const string base = tmp.file("locked");
    list<TermCG> termDef(1, TermCG(1, 1));
    termDef.back().documents.push_back(Document(3, 1));
    {
        NindTermIndex writer(base, true, kNoCheck, 0, 8);
        writer.setTermDef(1, termDef, kNoCheck, list<unsigned int>());
        //a 2nd writer is refused, even in-process
        CHECK_THROWS_AS(NindTermIndex(base, true, kNoCheck, 0, 8), NindPadFileException);
        //readers are never blocked by the writer
        NindTermIndex reader(base, false, kNoCheck, 0);
        list<TermCG> readDef;
        CHECK(reader.getTermDef(1, readDef));
        //and the refused writer didn't damage the file
        CHECK_EQ(3u, readDef.front().documents.front().ident);
    }
    //once the writer is closed, the file can be written again
    NindTermIndex writer(base, true, kNoCheck, 0, 8);
    writer.setTermDef(2, termDef, kNoCheck, list<unsigned int>());
}
