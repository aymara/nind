#include "NindRetrolexicon/NindRetrolexicon.h"
#include "NindBasics/NindPadFile.h"
#include "NindExceptions.h"
#include "TestTempDir.h"
#include <gtest/gtest.h>
#include <list>
#include <string>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
namespace {
typedef NindRetrolexicon::RetroWord RetroWord;
const NindPadFile::Identification kNoCheck(0, 0);

list<RetroWord> oneWord(const RetroWord &w) {
    list<RetroWord> l;
    l.push_back(w);
    return l;
}
}
////////////////////////////////////////////////////////////
TEST(NindRetrolexiconTest, SimpleWordRoundTrip) {
    TestTempDir tmp;
    NindRetrolexicon retro(tmp.file("simple"), true, kNoCheck, 8);
    retro.addRetroWords(oneWord(RetroWord(1, "alpha")), kNoCheck);

    list<string> components;
    ASSERT_TRUE(retro.getComponents(1, components));
    ASSERT_EQ(1u, components.size());
    EXPECT_EQ("alpha", components.front());
}
////////////////////////////////////////////////////////////
TEST(NindRetrolexiconTest, TwoComponentCompoundRoundTrip) {
    TestTempDir tmp;
    NindRetrolexicon retro(tmp.file("compound2"), true, kNoCheck, 8);
    retro.addRetroWords(oneWord(RetroWord(1, "alpha")), kNoCheck);
    retro.addRetroWords(oneWord(RetroWord(2, "beta")), kNoCheck);
    // ident 3 = "alpha_beta" : identA points at the "alpha" prefix, identS at
    // the newly-appended simple word "beta" (matches how NindLexiconIndex
    // builds these entries, see setDefinitionWords()).
    retro.addRetroWords(oneWord(RetroWord(3, /*identA=*/1, /*identS=*/2)), kNoCheck);

    list<string> components;
    ASSERT_TRUE(retro.getComponents(3, components));
    ASSERT_EQ(2u, components.size());
    list<string>::const_iterator it = components.begin();
    EXPECT_EQ("alpha", *it++);
    EXPECT_EQ("beta", *it++);
}
////////////////////////////////////////////////////////////
TEST(NindRetrolexiconTest, ThreeComponentCompoundRoundTrip) {
    TestTempDir tmp;
    NindRetrolexicon retro(tmp.file("compound3"), true, kNoCheck, 8);
    retro.addRetroWords(oneWord(RetroWord(1, "alpha")), kNoCheck);
    retro.addRetroWords(oneWord(RetroWord(2, "beta")), kNoCheck);
    retro.addRetroWords(oneWord(RetroWord(3, "gamma")), kNoCheck);
    retro.addRetroWords(oneWord(RetroWord(4, /*identA=*/1, /*identS=*/2)), kNoCheck);  // alpha_beta
    retro.addRetroWords(oneWord(RetroWord(5, /*identA=*/4, /*identS=*/3)), kNoCheck);  // alpha_beta_gamma

    list<string> components;
    ASSERT_TRUE(retro.getComponents(5, components));
    ASSERT_EQ(3u, components.size());
    list<string>::const_iterator it = components.begin();
    EXPECT_EQ("alpha", *it++);
    EXPECT_EQ("beta", *it++);
    EXPECT_EQ("gamma", *it++);
}
////////////////////////////////////////////////////////////
TEST(NindRetrolexiconTest, UnknownIdentReturnsFalse) {
    TestTempDir tmp;
    NindRetrolexicon retro(tmp.file("unknown"), true, kNoCheck, 8);
    retro.addRetroWords(oneWord(RetroWord(1, "alpha")), kNoCheck);

    list<string> components;
    EXPECT_FALSE(retro.getComponents(999, components));
    EXPECT_TRUE(components.empty());
}
////////////////////////////////////////////////////////////
TEST(NindRetrolexiconTest, ReAddingSameIdentOverwritesIt) {
    TestTempDir tmp;
    NindRetrolexicon retro(tmp.file("overwrite"), true, kNoCheck, 8);
    retro.addRetroWords(oneWord(RetroWord(1, "alpha")), kNoCheck);
    retro.addRetroWords(oneWord(RetroWord(1, "omega")), kNoCheck);

    list<string> components;
    ASSERT_TRUE(retro.getComponents(1, components));
    ASSERT_EQ(1u, components.size());
    EXPECT_EQ("omega", components.front());
}
////////////////////////////////////////////////////////////
TEST(NindRetrolexiconTest, GrowsPastInitialBlocSizeAndStaysReadable) {
    TestTempDir tmp;
    // A definitions block of only 2 entries forces several block extensions
    // while adding 6 words, exercising NindPadFile's block-chaining.
    NindRetrolexicon retro(tmp.file("grow"), true, kNoCheck, 2);
    const char *lemmas[] = {"a", "b", "c", "d", "e", "f"};
    for (unsigned int ident = 1; ident <= 6; ident++) {
        retro.addRetroWords(oneWord(RetroWord(ident, lemmas[ident - 1])), kNoCheck);
    }
    for (unsigned int ident = 1; ident <= 6; ident++) {
        list<string> components;
        ASSERT_TRUE(retro.getComponents(ident, components)) << "ident=" << ident;
        ASSERT_EQ(1u, components.size());
        EXPECT_EQ(string(lemmas[ident - 1]), components.front());
    }
}
////////////////////////////////////////////////////////////
TEST(NindRetrolexiconTest, PersistsAcrossReopenAsReader) {
    TestTempDir tmp;
    const string path = tmp.file("persist");
    {
        NindRetrolexicon writer(path, true, kNoCheck, 8);
        writer.addRetroWords(oneWord(RetroWord(1, "alpha")), kNoCheck);
        writer.addRetroWords(oneWord(RetroWord(2, "beta")), kNoCheck);
        writer.addRetroWords(oneWord(RetroWord(3, 1, 2)), kNoCheck);
    }
    NindRetrolexicon reader(path, false, kNoCheck, 8);
    list<string> components;
    ASSERT_TRUE(reader.getComponents(3, components));
    ASSERT_EQ(2u, components.size());
    list<string>::const_iterator it = components.begin();
    EXPECT_EQ("alpha", *it++);
    EXPECT_EQ("beta", *it++);
}
////////////////////////////////////////////////////////////
TEST(NindRetrolexiconTest, WritingOnAReaderThrows) {
    TestTempDir tmp;
    const string path = tmp.file("readonly");
    { NindRetrolexicon writer(path, true, kNoCheck, 8); }
    NindRetrolexicon reader(path, false, kNoCheck, 8);
    EXPECT_THROW(reader.addRetroWords(oneWord(RetroWord(1, "alpha")), kNoCheck), NindRetrolexiconException);
}
