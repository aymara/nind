#include "NindIndex/NindLocalIndex.h"
#include "NindExceptions.h"
#include "TestTempDir.h"
#include "doctest.h"
#include <list>
#include <set>
#include <string>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
namespace {
typedef NindLocalIndex::Term Term;
typedef NindLocalIndex::Localisation Localisation;
const NindIndex::Identification kNoCheck(0, 0);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindLocalIndexTest.SetThenGetLocalDefRoundTrips") {
    TestTempDir tmp;
    NindLocalIndex index(tmp.file("basic"), true, kNoCheck, 8);

    list<Term> written;
    written.push_back(Term(5, 1));
    written.back().localisation.push_back(Localisation(0, 4));
    written.back().localisation.push_back(Localisation(10, 4));
    written.push_back(Term(9, 2));
    written.back().localisation.push_back(Localisation(20, 3));
    index.setLocalDef(42, written, kNoCheck);

    list<Term> read;
    REQUIRE(index.getLocalDef(42, read));
    REQUIRE_EQ(2u, read.size());
    list<Term>::const_iterator termIt = read.begin();
    CHECK_EQ(5u, termIt->term);
    CHECK_EQ(1, termIt->cg);
    REQUIRE_EQ(2u, termIt->localisation.size());
    list<Localisation>::const_iterator locIt = termIt->localisation.begin();
    CHECK_EQ(0u, locIt->position);
    CHECK_EQ(4u, locIt->length);
    ++locIt;
    CHECK_EQ(10u, locIt->position);
    CHECK_EQ(4u, locIt->length);
    ++termIt;
    CHECK_EQ(9u, termIt->term);
    CHECK_EQ(2, termIt->cg);
    REQUIRE_EQ(1u, termIt->localisation.size());
    CHECK_EQ(20u, termIt->localisation.front().position);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindLocalIndexTest.GetLocalLengthCountsDistinctTermEntries") {
    // getLocalLength counts the number of term entries in the document, not
    // the total number of localisations across them.
    TestTempDir tmp;
    NindLocalIndex index(tmp.file("length"), true, kNoCheck, 8);
    list<Term> written;
    written.push_back(Term(1, 0));
    written.back().localisation.push_back(Localisation(0, 1));
    written.back().localisation.push_back(Localisation(5, 1));
    written.push_back(Term(2, 0));
    written.back().localisation.push_back(Localisation(10, 1));
    index.setLocalDef(1, written, kNoCheck);

    unsigned int length = 0;
    REQUIRE(index.getLocalLength(1, length));
    CHECK_EQ(2u, length);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindLocalIndexTest.GetTermIdentsReturnsUniqueTermSet") {
    TestTempDir tmp;
    NindLocalIndex index(tmp.file("termidents"), true, kNoCheck, 8);
    list<Term> written;
    written.push_back(Term(7, 0));
    written.back().localisation.push_back(Localisation(0, 1));
    written.push_back(Term(3, 0));
    written.back().localisation.push_back(Localisation(1, 1));
    index.setLocalDef(1, written, kNoCheck);

    set<unsigned int> idents;
    REQUIRE(index.getTermIdents(1, idents));
    CHECK_EQ((set<unsigned int>{3, 7}), idents);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindLocalIndexTest.UnknownExternalIdentReturnsFalse") {
    TestTempDir tmp;
    NindLocalIndex index(tmp.file("unknown"), true, kNoCheck, 8);
    list<Term> read;
    CHECK_FALSE(index.getLocalDef(999, read));
    unsigned int length = 123;
    CHECK_FALSE(index.getLocalLength(999, length));
}
////////////////////////////////////////////////////////////
TEST_CASE("NindLocalIndexTest.DocCountTracksDistinctExternalIdentsAndUpdatesDontDuplicate") {
    TestTempDir tmp;
    NindLocalIndex index(tmp.file("doccount"), true, kNoCheck, 8);
    list<Term> term1;
    term1.push_back(Term(1, 0));
    term1.back().localisation.push_back(Localisation(0, 1));

    index.setLocalDef(100, term1, kNoCheck);
    index.setLocalDef(200, term1, kNoCheck);
    index.setLocalDef(300, term1, kNoCheck);
    CHECK_EQ(3u, index.getDocCount());

    // Re-writing an existing external ident updates it in place, it is not a new doc.
    index.setLocalDef(100, term1, kNoCheck);
    CHECK_EQ(3u, index.getDocCount());
}
////////////////////////////////////////////////////////////
TEST_CASE("NindLocalIndexTest.EmptyLocalDefDeletesTheDocumentAndDecrementsCount") {
    TestTempDir tmp;
    NindLocalIndex index(tmp.file("delete"), true, kNoCheck, 8);
    list<Term> term1;
    term1.push_back(Term(1, 0));
    term1.back().localisation.push_back(Localisation(0, 1));
    index.setLocalDef(1, term1, kNoCheck);
    index.setLocalDef(2, term1, kNoCheck);
    REQUIRE_EQ(2u, index.getDocCount());

    index.setLocalDef(1, list<Term>(), kNoCheck);
    CHECK_EQ(1u, index.getDocCount());
    list<Term> read;
    CHECK_FALSE(index.getLocalDef(1, read));
}
////////////////////////////////////////////////////////////
TEST_CASE("NindLocalIndexTest.TermIdentDeltaEncodingSurvivesNonMonotonicOrder") {
    // Term idents inside one document are stored as signed deltas from the
    // previous one; make sure a non-trivial ordering round-trips correctly.
    TestTempDir tmp;
    NindLocalIndex index(tmp.file("deltas"), true, kNoCheck, 8);
    list<Term> written;
    const unsigned int termIdents[] = {50, 3, 3000, 3001};
    for (unsigned int t : termIdents) {
        written.push_back(Term(t, 0));
        written.back().localisation.push_back(Localisation(0, 1));
    }
    index.setLocalDef(1, written, kNoCheck);

    list<Term> read;
    REQUIRE(index.getLocalDef(1, read));
    REQUIRE_EQ(4u, read.size());
    int i = 0;
    for (list<Term>::const_iterator it = read.begin(); it != read.end(); ++it, ++i)
        CHECK_EQ(termIdents[i], it->term);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindLocalIndexTest.GrowsPastInitialBlocSizeAndStaysReadable") {
    TestTempDir tmp;
    NindLocalIndex index(tmp.file("grow"), true, kNoCheck, 2);
    for (unsigned int externalId = 0; externalId < 6; externalId++) {
        list<Term> written;
        written.push_back(Term(externalId, 0));
        written.back().localisation.push_back(Localisation(0, 1));
        index.setLocalDef(externalId, written, kNoCheck);
    }
    CHECK_EQ(6u, index.getDocCount());
    for (unsigned int externalId = 0; externalId < 6; externalId++) {
        list<Term> read;
        { INFO("externalId=" << externalId); REQUIRE(index.getLocalDef(externalId, read)); }
        CHECK_EQ(externalId, read.front().term);
    }
}
////////////////////////////////////////////////////////////
TEST_CASE("NindLocalIndexTest.PersistsAcrossReopenAsReaderWithMatchingIdentification") {
    TestTempDir tmp;
    const string path = tmp.file("persist");
    const NindIndex::Identification identification(1, 111);
    {
        NindLocalIndex writer(path, true, identification, 8);
        list<Term> written;
        written.push_back(Term(4, 0));
        written.back().localisation.push_back(Localisation(0, 1));
        writer.setLocalDef(42, written, identification);
    }
    NindLocalIndex reader(path, false, identification, 8);
    list<Term> read;
    REQUIRE(reader.getLocalDef(42, read));
    CHECK_EQ(4u, read.front().term);
    CHECK_EQ(1u, reader.getDocCount());
}
