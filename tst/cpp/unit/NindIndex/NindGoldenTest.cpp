// Golden files: a small index (lexicon + retrolexicon, term index, local index) of the
// current format (v1), written once and checked into golden/v1/.
//
// - NindGolden.WritersReproduceTheGoldenFiles: today's writers must produce exactly these
//   bytes (except the identification's timestamp, the last 4 bytes of every file). An
//   accidental format change, or a writer and a reader sharing the same bug, breaks it.
// - NindGolden.ReadersDecodeTheGoldenFiles: today's readers must decode them to the values
//   written. Future versions of the format keep reading them (v1 compatibility).
//
// To regenerate them after an *intended* format change, run the test executable with
// NIND_WRITE_GOLDEN=1 (writes into the source tree), then review and commit the new files.
////////////////////////////////////////////////////////////
#include "NindIndex/NindLexiconIndex.h"
#include "NindIndex/NindTermIndex.h"
#include "NindIndex/NindLocalIndex.h"
#include "NindExceptions.h"
#include "TestTempDir.h"
#include "TestFileBytes.h"
#include "doctest.h"
#include <cstdlib>
#include <list>
#include <string>
#include <vector>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
#ifndef NIND_GOLDEN_DIR
#error "NIND_GOLDEN_DIR must be defined (tst/cpp/unit/NindIndex/CMakeLists.txt)"
#endif
namespace {
typedef NindTermIndex::TermCG TermCG;
typedef NindTermIndex::Document Document;
typedef NindLocalIndex::Term Term;
typedef NindLocalIndex::Localisation Localisation;

const char *kExtensions[] = {".nindlexiconindex", ".nindretrolexicon", ".nindtermindex", ".nindlocalindex"};

// the words of the lexicon, in insertion order: simple, compound (2 and 3 components),
// UTF-8, an Amose-style named entity prefix; a tiny modulo (7) makes hash collisions
vector<list<string> > goldenWords() {
    const char *spec[][3] = {
        {"alpha", 0, 0}, {"beta", 0, 0}, {"gamma", 0, 0}, {"delta", 0, 0},
        {"\xc3\xa9pist\xc3\xa9mologie", 0, 0}, {"alpha", "beta", 0}, {"beta", "gamma", "delta"},
        {"\xc2\xa7", "Person.PERSON", "Diego_Maradona"}, {"epsilon", 0, 0}, {"zeta", 0, 0},
    };
    vector<list<string> > words;
    for (size_t i = 0; i < sizeof(spec) / sizeof(spec[0]); i++) {
        list<string> components;
        for (int j = 0; j < 3 && spec[i][j] != 0; j++) components.push_back(spec[i][j]);
        words.push_back(components);
    }
    return words;
}

list<unsigned int> goldenSpecifics() {
    list<unsigned int> specifics;
    specifics.push_back(3);
    specifics.push_back(300000);
    return specifics;
}

// a term definition: sparse external document ids (CLEF-like), several categories,
// frequencies over 127 (2-byte varints)
list<TermCG> termDef(const unsigned int seed, const unsigned int docsNb, const bool twoCategories) {
    list<TermCG> def;
    def.push_back(TermCG(0, 0));
    for (unsigned int i = 0; i < docsNb; i++) {
        const unsigned int frequency = 1 + (seed * 7 + i * 13) % 300;
        def.back().documents.push_back(Document(917024 + i * 1000003 + seed, frequency));
        def.back().frequency += frequency;
    }
    if (twoCategories) {
        def.push_back(TermCG(3, 0));
        def.back().documents.push_back(Document(917024 + seed, 2));
        def.back().documents.push_back(Document(137841028, 1));
        def.back().frequency = 3;
    }
    return def;
}

// a local definition: terms in document order, with localisations
list<Term> localDef(const vector<unsigned int> &ids, const unsigned int seed, const unsigned int termsNb) {
    list<Term> def;
    unsigned int position = 10 * seed;
    for (unsigned int i = 0; i < termsNb; i++) {
        def.push_back(Term(ids[(seed + i * 3) % ids.size()], i % 2));
        def.back().localisation.push_back(Localisation(position, 4 + i % 5));
        position += 7 + (i * 11) % 40;
        if (i % 3 == 0) {
            def.back().localisation.push_back(Localisation(position, 3));
            position += 5;
        }
    }
    return def;
}

const unsigned int kDocs[] = {917024, 42, 137841028, 5000000, 77};

// builds the golden index at base; returns the words' ids
vector<unsigned int> buildGolden(const string &base) {
    vector<unsigned int> ids;
    NindIndex::Identification identification;
    {
        NindLexiconIndex lexicon(base, true, true, 7, 4);
        const vector<list<string> > words = goldenWords();
        for (size_t i = 0; i < words.size(); i++) ids.push_back(lexicon.addWord(words[i]));
        identification = lexicon.getIdentification();
    }
    {
        NindTermIndex terms(base, true, identification, 2, 4);
        const list<unsigned int> specifics = goldenSpecifics();
        for (size_t i = 0; i < ids.size(); i++)
            terms.setTermDef(ids[i], termDef(i, 1 + (i * 5) % 9, i % 3 == 0), identification, specifics);
        // rewritten smaller (in place), larger (moved: leaves a hole), erased
        terms.setTermDef(ids[1], termDef(101, 1, false), identification, specifics);
        terms.setTermDef(ids[2], termDef(102, 40, true), identification, specifics);
        terms.setTermDef(ids[3], list<TermCG>(), identification, specifics);
    }
    {
        NindLocalIndex local(base, true, identification, 4);
        for (size_t d = 0; d < sizeof(kDocs) / sizeof(kDocs[0]); d++)
            local.setLocalDef(kDocs[d], localDef(ids, d, 3 + d * 4), identification);
        local.setLocalDef(kDocs[1], localDef(ids, 9, 30), identification);    //replaced, larger
        local.setLocalDef(kDocs[4], list<Term>(), identification);            //erased
    }
    return ids;
}

// the bytes of a file, the identification's timestamp (last 4 bytes) zeroed
vector<unsigned char> maskedBytes(const string &path) {
    vector<unsigned char> bytes = readFileBytes(path);
    for (size_t i = bytes.size() >= 4 ? bytes.size() - 4 : 0; i < bytes.size(); i++) bytes[i] = 0;
    return bytes;
}

const string goldenBase() { return string(NIND_GOLDEN_DIR) + "/golden"; }
}
////////////////////////////////////////////////////////////
TEST_CASE("NindGolden.WritersReproduceTheGoldenFiles") {
    TestTempDir tmp;
    const string base = tmp.file("golden");
    buildGolden(base);
    if (getenv("NIND_WRITE_GOLDEN") != 0) {
        for (size_t e = 0; e < 4; e++)
            writeFileBytes(goldenBase() + kExtensions[e], readFileBytes(base + kExtensions[e]));
        MESSAGE("golden files written into " NIND_GOLDEN_DIR);
    }
    for (size_t e = 0; e < 4; e++) {
        INFO("file " << kExtensions[e]);
        const vector<unsigned char> golden = maskedBytes(goldenBase() + kExtensions[e]);
        REQUIRE_FALSE(golden.empty());
        CHECK(maskedBytes(base + kExtensions[e]) == golden);
    }
}
////////////////////////////////////////////////////////////
TEST_CASE("NindGolden.ReadersDecodeTheGoldenFiles") {
    // the readers open copies (a reader may not write, but the test must not risk the sources)
    TestTempDir tmp;
    const string base = tmp.file("golden");
    for (size_t e = 0; e < 4; e++)
        writeFileBytes(base + kExtensions[e], readFileBytes(goldenBase() + kExtensions[e]));
    // the values written by buildGolden, rebuilt independently of the files
    const vector<list<string> > words = goldenWords();
    // ids: one per simple word, component or compound prefix, in insertion order
    // (§ Person.PERSON Diego_Maradona: §=9, Person.PERSON=10, §_Person.PERSON=11, Diego_Maradona=12, the word 13)
    const unsigned int expectedIds[] = {1, 2, 3, 4, 5, 6, 8, 13, 14, 15};
    NindLexiconIndex lexicon(base, false, true);
    const NindIndex::Identification identification = lexicon.getIdentification();
    CHECK_EQ(15u, identification.lexiconWordsNb);
    vector<unsigned int> ids;
    for (size_t i = 0; i < words.size(); i++) {
        INFO("word " << i);
        ids.push_back(lexicon.getWordId(words[i]));
        CHECK_EQ(expectedIds[i], ids.back());
        list<string> components;
        REQUIRE(lexicon.getComponents(ids.back(), components));
        CHECK(components == words[i]);
    }
    list<string> unknown;
    unknown.push_back("omega");
    CHECK_EQ(0u, lexicon.getWordId(unknown));

    NindTermIndex terms(base, false, identification, 2);
    list<unsigned int> specifics;
    terms.getSpecificWords(specifics);
    CHECK(specifics == goldenSpecifics());
    for (size_t i = 0; i < ids.size(); i++) {
        INFO("term " << i);
        list<TermCG> expected = termDef(i, 1 + (i * 5) % 9, i % 3 == 0);
        if (i == 1) expected = termDef(101, 1, false);
        if (i == 2) expected = termDef(102, 40, true);
        list<TermCG> read;
        if (i == 3) { CHECK_FALSE(terms.getTermDef(ids[i], read)); continue; }
        REQUIRE(terms.getTermDef(ids[i], read));
        REQUIRE_EQ(expected.size(), read.size());
        list<TermCG>::const_iterator e = expected.begin(), r = read.begin();
        for (; e != expected.end(); e++, r++) {
            CHECK_EQ(e->cg, r->cg);
            CHECK_EQ(e->frequency, r->frequency);
            REQUIRE_EQ(e->documents.size(), r->documents.size());
            list<Document>::const_iterator ed = e->documents.begin(), rd = r->documents.begin();
            for (; ed != e->documents.end(); ed++, rd++) {
                CHECK_EQ(ed->ident, rd->ident);
                CHECK_EQ(ed->frequency, rd->frequency);
            }
        }
    }

    NindLocalIndex local(base, false, identification);
    for (size_t d = 0; d < sizeof(kDocs) / sizeof(kDocs[0]); d++) {
        INFO("document " << kDocs[d]);
        list<Term> read;
        if (d == 4) { CHECK_FALSE(local.getLocalDef(kDocs[d], read)); continue; }
        const list<Term> expected = (d == 1) ? localDef(ids, 9, 30) : localDef(ids, d, 3 + d * 4);
        REQUIRE(local.getLocalDef(kDocs[d], read));
        REQUIRE_EQ(expected.size(), read.size());
        list<Term>::const_iterator e = expected.begin(), r = read.begin();
        for (; e != expected.end(); e++, r++) {
            CHECK_EQ(e->term, r->term);
            CHECK_EQ(e->cg, r->cg);
            REQUIRE_EQ(e->localisation.size(), r->localisation.size());
            list<Localisation>::const_iterator el = e->localisation.begin(), rl = r->localisation.begin();
            for (; el != e->localisation.end(); el++, rl++) {
                CHECK_EQ(el->position, rl->position);
                CHECK_EQ(el->length, rl->length);
            }
        }
    }
}
