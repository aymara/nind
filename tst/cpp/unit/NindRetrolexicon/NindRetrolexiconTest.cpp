#include "NindRetrolexicon/NindRetrolexicon.h"
#include "NindBasics/NindPadFile.h"
#include "NindExceptions.h"
#include "TestTempDir.h"
#include "doctest.h"
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
TEST_CASE("NindRetrolexiconTest.SimpleWordRoundTrip") {
    TestTempDir tmp;
    NindRetrolexicon retro(tmp.file("simple"), true, kNoCheck, 8);
    retro.addRetroWords(oneWord(RetroWord(1, "alpha")), kNoCheck);

    list<string> components;
    REQUIRE(retro.getComponents(1, components));
    REQUIRE_EQ(1u, components.size());
    CHECK_EQ("alpha", components.front());
}
////////////////////////////////////////////////////////////
TEST_CASE("NindRetrolexiconTest.TwoComponentCompoundRoundTrip") {
    TestTempDir tmp;
    NindRetrolexicon retro(tmp.file("compound2"), true, kNoCheck, 8);
    retro.addRetroWords(oneWord(RetroWord(1, "alpha")), kNoCheck);
    retro.addRetroWords(oneWord(RetroWord(2, "beta")), kNoCheck);
    // ident 3 = "alpha_beta" : identA points at the "alpha" prefix, identS at
    // the newly-appended simple word "beta" (matches how NindLexiconIndex
    // builds these entries, see setDefinitionWords()).
    retro.addRetroWords(oneWord(RetroWord(3, /*identA=*/1, /*identS=*/2)), kNoCheck);

    list<string> components;
    REQUIRE(retro.getComponents(3, components));
    REQUIRE_EQ(2u, components.size());
    list<string>::const_iterator it = components.begin();
    CHECK_EQ("alpha", *it++);
    CHECK_EQ("beta", *it++);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindRetrolexiconTest.ThreeComponentCompoundRoundTrip") {
    TestTempDir tmp;
    NindRetrolexicon retro(tmp.file("compound3"), true, kNoCheck, 8);
    retro.addRetroWords(oneWord(RetroWord(1, "alpha")), kNoCheck);
    retro.addRetroWords(oneWord(RetroWord(2, "beta")), kNoCheck);
    retro.addRetroWords(oneWord(RetroWord(3, "gamma")), kNoCheck);
    retro.addRetroWords(oneWord(RetroWord(4, /*identA=*/1, /*identS=*/2)), kNoCheck);  // alpha_beta
    retro.addRetroWords(oneWord(RetroWord(5, /*identA=*/4, /*identS=*/3)), kNoCheck);  // alpha_beta_gamma

    list<string> components;
    REQUIRE(retro.getComponents(5, components));
    REQUIRE_EQ(3u, components.size());
    list<string>::const_iterator it = components.begin();
    CHECK_EQ("alpha", *it++);
    CHECK_EQ("beta", *it++);
    CHECK_EQ("gamma", *it++);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindRetrolexiconTest.UnknownIdentReturnsFalse") {
    TestTempDir tmp;
    NindRetrolexicon retro(tmp.file("unknown"), true, kNoCheck, 8);
    retro.addRetroWords(oneWord(RetroWord(1, "alpha")), kNoCheck);

    list<string> components;
    CHECK_FALSE(retro.getComponents(999, components));
    CHECK(components.empty());
}
////////////////////////////////////////////////////////////
TEST_CASE("NindRetrolexiconTest.ReAddingSameIdentOverwritesIt") {
    TestTempDir tmp;
    NindRetrolexicon retro(tmp.file("overwrite"), true, kNoCheck, 8);
    retro.addRetroWords(oneWord(RetroWord(1, "alpha")), kNoCheck);
    retro.addRetroWords(oneWord(RetroWord(1, "omega")), kNoCheck);

    list<string> components;
    REQUIRE(retro.getComponents(1, components));
    REQUIRE_EQ(1u, components.size());
    CHECK_EQ("omega", components.front());
}
////////////////////////////////////////////////////////////
TEST_CASE("NindRetrolexiconTest.GrowsPastInitialBlocSizeAndStaysReadable") {
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
        { INFO("ident=" << ident); REQUIRE(retro.getComponents(ident, components)); }
        REQUIRE_EQ(1u, components.size());
        CHECK_EQ(string(lemmas[ident - 1]), components.front());
    }
}
////////////////////////////////////////////////////////////
TEST_CASE("NindRetrolexiconTest.PersistsAcrossReopenAsReader") {
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
    REQUIRE(reader.getComponents(3, components));
    REQUIRE_EQ(2u, components.size());
    list<string>::const_iterator it = components.begin();
    CHECK_EQ("alpha", *it++);
    CHECK_EQ("beta", *it++);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindRetrolexiconTest.WritingOnAReaderThrows") {
    TestTempDir tmp;
    const string path = tmp.file("readonly");
    { NindRetrolexicon writer(path, true, kNoCheck, 8); }
    NindRetrolexicon reader(path, false, kNoCheck, 8);
    CHECK_THROWS_AS(reader.addRetroWords(oneWord(RetroWord(1, "alpha")), kNoCheck), NindRetrolexiconException);
}
