#include "NindAmose/NindLocalAmose.h"
#include "NindAmose/NindLexiconAmose.h"
#include "TestTempDir.h"
#include <gtest/gtest.h>
#include <list>
#include <set>
#include <string>
#include <vector>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
namespace {
typedef NindLocalIndex::Term Term;
typedef NindLocalIndex::Localisation Localisation;
}
////////////////////////////////////////////////////////////
TEST(NindLocalAmoseTest, GetDocTermsAndDocLengthAndDocCount) {
    TestTempDir tmp;
    const string path = tmp.file("local1");

    // The lexicon must exist and already contain the words used below:
    // NindLocalAmose opens its own read-only NindLexiconAmose on construction.
    NindLexiconAmose lexicon(path, true, 16, 16);
    const unsigned int catId = lexicon.addWord("cat", SIMPLE_TERM);
    const unsigned int dogId = lexicon.addWord("dog", SIMPLE_TERM);
    const NindIndex::Identification identification = lexicon.getIdentification();

    NindLocalAmose localIndex(path, true, identification, 8);
    list<Term> doc;
    doc.push_back(Term(catId, SIMPLE_TERM));
    doc.back().localisation.push_back(Localisation(0, 3));
    doc.push_back(Term(dogId, SIMPLE_TERM));
    doc.back().localisation.push_back(Localisation(4, 3));
    doc.back().localisation.push_back(Localisation(8, 3));
    localIndex.setLocalDef(1, doc, identification);

    set<string> terms;
    ASSERT_TRUE(localIndex.getDocTerms(1, SIMPLE_TERM, terms));
    EXPECT_EQ((set<string>{"cat", "dog"}), terms);

    EXPECT_EQ(2u, localIndex.getDocLength(1));   // 2 distinct term entries
    EXPECT_EQ(1u, localIndex.getDocCount());
}
////////////////////////////////////////////////////////////
TEST(NindLocalAmoseTest, GetDocTermsFiltersByType) {
    TestTempDir tmp;
    const string path = tmp.file("local2");
    NindLexiconAmose lexicon(path, true, 16, 16);
    const unsigned int catId = lexicon.addWord("cat", SIMPLE_TERM);
    const unsigned int parisId = lexicon.addWord("Paris", NAMED_ENTITY, "LOC");
    const NindIndex::Identification identification = lexicon.getIdentification();

    NindLocalAmose localIndex(path, true, identification, 8);
    list<Term> doc;
    doc.push_back(Term(catId, SIMPLE_TERM));
    doc.back().localisation.push_back(Localisation(0, 3));
    doc.push_back(Term(parisId, NAMED_ENTITY));
    doc.back().localisation.push_back(Localisation(4, 5));
    localIndex.setLocalDef(1, doc, identification);

    set<string> simpleTerms;
    ASSERT_TRUE(localIndex.getDocTerms(1, SIMPLE_TERM, simpleTerms));
    EXPECT_EQ((set<string>{"cat"}), simpleTerms);

    set<string> namedEntities;
    ASSERT_TRUE(localIndex.getDocTerms(1, NAMED_ENTITY, namedEntities));
    EXPECT_EQ((set<string>{"LOC:Paris"}), namedEntities);
}
////////////////////////////////////////////////////////////
TEST(NindLocalAmoseTest, UnknownDocumentReturnsFalseOrZero) {
    TestTempDir tmp;
    const string path = tmp.file("local3");
    NindLexiconAmose lexicon(path, true, 16, 16);
    const NindIndex::Identification identification = lexicon.getIdentification();
    NindLocalAmose localIndex(path, true, identification, 8);

    set<string> terms;
    EXPECT_FALSE(localIndex.getDocTerms(999, SIMPLE_TERM, terms));
    EXPECT_EQ(0u, localIndex.getDocLength(999));
}
////////////////////////////////////////////////////////////
TEST(NindLocalAmoseTest, GetTermPositionIndocsKeepsOnlyFirstLocalisationPerEntry) {
    // Documented behaviour: Amose doesn't index fractional localisations, so
    // only the first Localisation of each term entry is returned.
    TestTempDir tmp;
    const string path = tmp.file("local4");
    NindLexiconAmose lexicon(path, true, 16, 16);
    const unsigned int catId = lexicon.addWord("cat", SIMPLE_TERM);
    const NindIndex::Identification identification = lexicon.getIdentification();

    NindLocalAmose localIndex(path, true, identification, 8);
    list<Term> doc;
    doc.push_back(Term(catId, SIMPLE_TERM));
    doc.back().localisation.push_back(Localisation(4, 3));
    doc.back().localisation.push_back(Localisation(8, 3));
    localIndex.setLocalDef(1, doc, identification);

    vector<unsigned int> termIds;
    termIds.push_back(catId);
    vector<unsigned int> documents;
    documents.push_back(1);
    NindLocalAmose::Positions positions;
    localIndex.getTermPositionIndocs(termIds, documents, positions);

    ASSERT_EQ(1u, positions.size());
    ASSERT_EQ(1u, positions[0].size());
    ASSERT_EQ(1u, positions[0][0].size());
    EXPECT_EQ(4u, positions[0][0].front().position);
}
