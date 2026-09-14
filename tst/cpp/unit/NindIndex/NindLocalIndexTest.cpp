#include "NindIndex/NindLocalIndex.h"
#include "NindExceptions.h"
#include "TestTempDir.h"
#include <gtest/gtest.h>
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
TEST(NindLocalIndexTest, SetThenGetLocalDefRoundTrips) {
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
    ASSERT_TRUE(index.getLocalDef(42, read));
    ASSERT_EQ(2u, read.size());
    list<Term>::const_iterator termIt = read.begin();
    EXPECT_EQ(5u, termIt->term);
    EXPECT_EQ(1, termIt->cg);
    ASSERT_EQ(2u, termIt->localisation.size());
    list<Localisation>::const_iterator locIt = termIt->localisation.begin();
    EXPECT_EQ(0u, locIt->position);
    EXPECT_EQ(4u, locIt->length);
    ++locIt;
    EXPECT_EQ(10u, locIt->position);
    EXPECT_EQ(4u, locIt->length);
    ++termIt;
    EXPECT_EQ(9u, termIt->term);
    EXPECT_EQ(2, termIt->cg);
    ASSERT_EQ(1u, termIt->localisation.size());
    EXPECT_EQ(20u, termIt->localisation.front().position);
}
////////////////////////////////////////////////////////////
TEST(NindLocalIndexTest, GetLocalLengthCountsDistinctTermEntries) {
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
    ASSERT_TRUE(index.getLocalLength(1, length));
    EXPECT_EQ(2u, length);
}
////////////////////////////////////////////////////////////
TEST(NindLocalIndexTest, GetTermIdentsReturnsUniqueTermSet) {
    TestTempDir tmp;
    NindLocalIndex index(tmp.file("termidents"), true, kNoCheck, 8);
    list<Term> written;
    written.push_back(Term(7, 0));
    written.back().localisation.push_back(Localisation(0, 1));
    written.push_back(Term(3, 0));
    written.back().localisation.push_back(Localisation(1, 1));
    index.setLocalDef(1, written, kNoCheck);

    set<unsigned int> idents;
    ASSERT_TRUE(index.getTermIdents(1, idents));
    EXPECT_EQ((set<unsigned int>{3, 7}), idents);
}
////////////////////////////////////////////////////////////
TEST(NindLocalIndexTest, UnknownExternalIdentReturnsFalse) {
    TestTempDir tmp;
    NindLocalIndex index(tmp.file("unknown"), true, kNoCheck, 8);
    list<Term> read;
    EXPECT_FALSE(index.getLocalDef(999, read));
    unsigned int length = 123;
    EXPECT_FALSE(index.getLocalLength(999, length));
}
////////////////////////////////////////////////////////////
TEST(NindLocalIndexTest, DocCountTracksDistinctExternalIdentsAndUpdatesDontDuplicate) {
    TestTempDir tmp;
    NindLocalIndex index(tmp.file("doccount"), true, kNoCheck, 8);
    list<Term> term1;
    term1.push_back(Term(1, 0));
    term1.back().localisation.push_back(Localisation(0, 1));

    index.setLocalDef(100, term1, kNoCheck);
    index.setLocalDef(200, term1, kNoCheck);
    index.setLocalDef(300, term1, kNoCheck);
    EXPECT_EQ(3u, index.getDocCount());

    // Re-writing an existing external ident updates it in place, it is not a new doc.
    index.setLocalDef(100, term1, kNoCheck);
    EXPECT_EQ(3u, index.getDocCount());
}
////////////////////////////////////////////////////////////
TEST(NindLocalIndexTest, EmptyLocalDefDeletesTheDocumentAndDecrementsCount) {
    TestTempDir tmp;
    NindLocalIndex index(tmp.file("delete"), true, kNoCheck, 8);
    list<Term> term1;
    term1.push_back(Term(1, 0));
    term1.back().localisation.push_back(Localisation(0, 1));
    index.setLocalDef(1, term1, kNoCheck);
    index.setLocalDef(2, term1, kNoCheck);
    ASSERT_EQ(2u, index.getDocCount());

    index.setLocalDef(1, list<Term>(), kNoCheck);
    EXPECT_EQ(1u, index.getDocCount());
    list<Term> read;
    EXPECT_FALSE(index.getLocalDef(1, read));
}
////////////////////////////////////////////////////////////
TEST(NindLocalIndexTest, TermIdentDeltaEncodingSurvivesNonMonotonicOrder) {
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
    ASSERT_TRUE(index.getLocalDef(1, read));
    ASSERT_EQ(4u, read.size());
    int i = 0;
    for (list<Term>::const_iterator it = read.begin(); it != read.end(); ++it, ++i)
        EXPECT_EQ(termIdents[i], it->term);
}
////////////////////////////////////////////////////////////
TEST(NindLocalIndexTest, GrowsPastInitialBlocSizeAndStaysReadable) {
    TestTempDir tmp;
    NindLocalIndex index(tmp.file("grow"), true, kNoCheck, 2);
    for (unsigned int externalId = 0; externalId < 6; externalId++) {
        list<Term> written;
        written.push_back(Term(externalId, 0));
        written.back().localisation.push_back(Localisation(0, 1));
        index.setLocalDef(externalId, written, kNoCheck);
    }
    EXPECT_EQ(6u, index.getDocCount());
    for (unsigned int externalId = 0; externalId < 6; externalId++) {
        list<Term> read;
        ASSERT_TRUE(index.getLocalDef(externalId, read)) << "externalId=" << externalId;
        EXPECT_EQ(externalId, read.front().term);
    }
}
////////////////////////////////////////////////////////////
TEST(NindLocalIndexTest, PersistsAcrossReopenAsReaderWithMatchingIdentification) {
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
    ASSERT_TRUE(reader.getLocalDef(42, read));
    EXPECT_EQ(4u, read.front().term);
    EXPECT_EQ(1u, reader.getDocCount());
}
