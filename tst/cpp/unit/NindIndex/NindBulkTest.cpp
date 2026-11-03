// Bulk mode of the index writers (NindIndex::beginBulk/endBulk), batched lexicon
// writes (NindLexiconIndex::addWords) and merges of shards without local index.
#include "NindIndex/NindTermIndex.h"
#include "NindIndex/NindLocalIndex.h"
#include "NindIndex/NindLexiconIndex.h"
#include "NindIndex/NindIndexMerge.h"
#include "NindExceptions.h"
#include "TestTempDir.h"
#include "TestFileBytes.h"
#include "doctest.h"
#include <cstdio>
#include <list>
#include <string>
#include <vector>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
namespace {
typedef NindTermIndex::Document Document;
typedef NindTermIndex::TermCG TermCG;
typedef NindLocalIndex::Term Term;
typedef NindLocalIndex::Localisation Localisation;
const NindIndex::Identification kIdent(7, 1234);

list<TermCG> termDef(const unsigned int seed, const unsigned int docsNb) {
    list<TermCG> def;
    def.push_back(TermCG(0, 0));
    for (unsigned int i = 0; i < docsNb; i++) {
        def.back().documents.push_back(Document(seed + 3 * i, 1 + (seed + i) % 5));
        def.back().frequency += 1 + (seed + i) % 5;
    }
    return def;
}

list<Term> localDef(const unsigned int seed, const unsigned int termsNb) {
    list<Term> def;
    for (unsigned int t = 0; t < termsNb; t++) {
        def.push_back(Term(seed + 2 * t, 0));
        for (unsigned int p = 0; p <= (seed + t) % 3; p++) def.back().localisation.push_back(Localisation(t * 7 + p, 1));
    }
    return def;
}

// the writes of one term-index build, in normal or bulk mode
void writeTerms(const string &base, const bool bulk) {
    NindTermIndex index(base, true, kIdent, 2, 3);       // 3 entries per block: several blocks
    if (bulk) index.beginBulk();
    list<unsigned int> specifics;
    specifics.push_back(0);
    specifics.push_back(0);
    for (unsigned int ident = 1; ident < 20; ident++) {
        specifics.front() = ident;
        specifics.back() = 100 + ident;
        index.setTermDef(ident, termDef(ident, 1 + ident % 7), kIdent, specifics);
    }
    if (bulk) index.endBulk();
}

void writeLocals(const string &base, const bool bulk) {
    NindLocalIndex index(base, true, kIdent, 4);
    if (bulk) index.beginBulk();
    for (unsigned int doc = 10; doc < 40; doc += 3) index.setLocalDef(doc, localDef(doc, 1 + doc % 6), kIdent);
    if (bulk) index.endBulk();
}

void checkTerm(NindTermIndex &index, const unsigned int ident, const list<TermCG> &expected) {
    list<TermCG> read;
    REQUIRE(index.getTermDef(ident, read));
    REQUIRE_EQ(expected.size(), read.size());
    REQUIRE_EQ(expected.front().documents.size(), read.front().documents.size());
    CHECK_EQ(expected.front().frequency, read.front().frequency);
    list<Document>::const_iterator itE = expected.front().documents.begin();
    for (list<Document>::const_iterator it = read.front().documents.begin(); it != read.front().documents.end(); it++, itE++) {
        CHECK_EQ(itE->ident, it->ident);
        CHECK_EQ(itE->frequency, it->frequency);
    }
}

list<string> word(const string &a) { list<string> w; w.push_back(a); return w; }
list<string> word(const string &a, const string &b) { list<string> w = word(a); w.push_back(b); return w; }
}
////////////////////////////////////////////////////////////
TEST_CASE("NindBulkTest.TermIndexBytesIdenticalToNormalMode") {
    TestTempDir tmp;
    writeTerms(tmp.file("normal"), false);
    writeTerms(tmp.file("bulk"), true);
    const vector<unsigned char> normal = readFileBytes(tmp.file("normal") + ".nindtermindex");
    REQUIRE(!normal.empty());
    CHECK(normal == readFileBytes(tmp.file("bulk") + ".nindtermindex"));
}
////////////////////////////////////////////////////////////
TEST_CASE("NindBulkTest.LocalIndexBytesIdenticalToNormalMode") {
    TestTempDir tmp;
    writeLocals(tmp.file("normal"), false);
    writeLocals(tmp.file("bulk"), true);
    const vector<unsigned char> normal = readFileBytes(tmp.file("normal") + ".nindlocalindex");
    REQUIRE(!normal.empty());
    CHECK(normal == readFileBytes(tmp.file("bulk") + ".nindlocalindex"));
    NindLocalIndex reader(tmp.file("bulk"), false, kIdent);
    CHECK_EQ(10u, reader.getDocCount());
}
////////////////////////////////////////////////////////////
TEST_CASE("NindBulkTest.RewritesErasuresAndReadsDuringBulk") {
    TestTempDir tmp;
    const string base = tmp.file("rewrite");
    const list<unsigned int> noSpecifics;
    {
        NindTermIndex index(base, true, kIdent, 0, 4);
        index.beginBulk();
        CHECK(index.isBulk());
        index.setTermDef(1, termDef(1, 6), kIdent, noSpecifics);
        index.setTermDef(2, termDef(2, 3), kIdent, noSpecifics);
        index.setTermDef(6, termDef(9, 2), kIdent, noSpecifics);      // in a new block
        index.setTermDef(1, termDef(50, 2), kIdent, noSpecifics);     // smaller: in place
        index.setTermDef(2, termDef(60, 9), kIdent, noSpecifics);     // larger: moved to the end
        index.setTermDef(6, list<TermCG>(), kIdent, noSpecifics);     // erased
        // reads see the writes made in bulk mode
        checkTerm(index, 1, termDef(50, 2));
        checkTerm(index, 2, termDef(60, 9));
        list<TermCG> read;
        CHECK_FALSE(index.getTermDef(6, read));
        index.endBulk();
        CHECK_FALSE(index.isBulk());
        // back to normal mode: holes left by the bulk writes are reused or skipped consistently
        index.setTermDef(3, termDef(3, 4), kIdent, noSpecifics);
        index.setTermDef(1, termDef(70, 8), kIdent, noSpecifics);
        checkTerm(index, 1, termDef(70, 8));
        // the file's structure is valid (no overlapping definitions)
        CHECK_NOTHROW(index.analyseIndex());
    }
    {
        NindTermIndex reader(base, false, kIdent, 0);
        checkTerm(reader, 1, termDef(70, 8));
        checkTerm(reader, 2, termDef(60, 9));
        checkTerm(reader, 3, termDef(3, 4));
        list<TermCG> read;
        CHECK_FALSE(reader.getTermDef(6, read));
        CHECK_NOTHROW(reader.analyseIndex());
    }
    {
        // an existing writer reopened in bulk mode, then normal mode again
        NindTermIndex index(base, true, kIdent, 0, 4);
        index.beginBulk();
        index.setTermDef(4, termDef(4, 5), kIdent, noSpecifics);
        index.setTermDef(2, termDef(80, 1), kIdent, noSpecifics);
        index.endBulk();
        index.setTermDef(5, termDef(5, 2), kIdent, noSpecifics);
    }
    NindTermIndex reader(base, false, kIdent, 0);
    checkTerm(reader, 1, termDef(70, 8));
    checkTerm(reader, 2, termDef(80, 1));
    checkTerm(reader, 4, termDef(4, 5));
    checkTerm(reader, 5, termDef(5, 2));
    CHECK_NOTHROW(reader.analyseIndex());
}
////////////////////////////////////////////////////////////
TEST_CASE("NindBulkTest.SpecificsAndIdentificationKeptInMemory") {
    TestTempDir tmp;
    const string base = tmp.file("specifics");
    {
        NindTermIndex index(base, true, kIdent, 2, 8);
        index.beginBulk();
        list<unsigned int> specifics;
        specifics.push_back(1);
        specifics.push_back(2);
        index.setTermDef(1, termDef(1, 2), kIdent, specifics);
        specifics.front() = 11;
        specifics.back() = 22;
        index.setSpecificWords(specifics, NindIndex::Identification(8, 99));
        list<unsigned int> read;
        index.getSpecificWords(read);
        CHECK_EQ(11u, read.front());
        CHECK_EQ(22u, read.back());
        index.endBulk();
    }
    NindTermIndex reader(base, false, NindIndex::Identification(8, 99), 2);
    list<unsigned int> read;
    reader.getSpecificWords(read);
    CHECK_EQ(11u, read.front());
    CHECK_EQ(22u, read.back());
}
////////////////////////////////////////////////////////////
TEST_CASE("NindBulkTest.DestructorEndsBulkMode") {
    TestTempDir tmp;
    const string base = tmp.file("destructor");
    {
        NindLocalIndex index(base, true, kIdent, 2);
        index.beginBulk();
        for (unsigned int doc = 1; doc <= 5; doc++) index.setLocalDef(doc, localDef(doc, 3), kIdent);
    }
    NindLocalIndex reader(base, false, kIdent);
    CHECK_EQ(5u, reader.getDocCount());
    list<Term> read;
    REQUIRE(reader.getLocalDef(4, read));
    CHECK_EQ(3u, read.size());
}
////////////////////////////////////////////////////////////
TEST_CASE("NindBulkTest.ReaderCantEnterBulkMode") {
    TestTempDir tmp;
    writeTerms(tmp.file("reader"), false);
    NindTermIndex reader(tmp.file("reader"), false, kIdent, 2);
    CHECK_THROWS_AS(reader.beginBulk(), FileException);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindBulkTest.AddWordsGivesTheIdentsOfAddWord") {
    TestTempDir tmp;
    vector<list<string> > words;
    words.push_back(word("alpha"));
    words.push_back(word("beta"));
    words.push_back(word("alpha"));                 // repeated
    words.push_back(word("gamma", "delta"));        // compound, both components new
    words.push_back(word("beta", "alpha"));         // compound of known words
    words.push_back(word("gamma"));                 // known as a component already
    words.push_back(word("gamma", "delta"));        // repeated compound
    for (int i = 0; i < 40; i++) words.push_back(word("w" + to_string(i)));
    vector<unsigned int> expected;
    {
        NindLexiconIndex lexicon(tmp.file("one"), true, false, 7);
        for (size_t i = 0; i < words.size(); i++) expected.push_back(lexicon.addWord(words[i]));
    }
    vector<unsigned int> idents;
    {
        NindLexiconIndex lexicon(tmp.file("batch"), true, false, 7);
        lexicon.beginBulk();
        lexicon.addWords(words, idents);
        lexicon.endBulk();
        CHECK_EQ(expected.back(), lexicon.getIdentification().lexiconWordsNb);
        // added again: nothing new
        vector<unsigned int> again;
        lexicon.addWords(words, again);
        CHECK(again == idents);
    }
    CHECK(idents == expected);
    NindLexiconIndex reader(tmp.file("batch"), false);
    for (size_t i = 0; i < words.size(); i++) CHECK_EQ(expected[i], reader.getWordId(words[i]));
    vector<NindLexiconIndex::Entry> entriesOne, entriesBatch;
    NindLexiconIndex(tmp.file("one"), false).getEntries(entriesOne);
    reader.getEntries(entriesBatch);
    REQUIRE_EQ(entriesOne.size(), entriesBatch.size());
    for (size_t i = 0; i < entriesOne.size(); i++) {
        CHECK_EQ(entriesOne[i].ident, entriesBatch[i].ident);
        CHECK_EQ(entriesOne[i].prefixIdent, entriesBatch[i].prefixIdent);
        CHECK_EQ(entriesOne[i].lastComponent, entriesBatch[i].lastComponent);
    }
}
////////////////////////////////////////////////////////////
TEST_CASE("NindBulkTest.AddWordsOnAnExistingLexiconAndTooLongWords") {
    TestTempDir tmp;
    const string base = tmp.file("existing");
    {
        NindLexiconIndex lexicon(base, true, true, 5, 5);
        lexicon.addWord(word("one"));
        lexicon.addWord(word("two"));
    }
    NindLexiconIndex lexicon(base, true, true);
    vector<list<string> > words;
    words.push_back(word("two"));
    words.push_back(word("three"));
    words.push_back(word("one", "three"));
    vector<unsigned int> idents;
    lexicon.addWords(words, idents);
    REQUIRE_EQ(3u, idents.size());
    CHECK_EQ(2u, idents[0]);
    CHECK_EQ(3u, idents[1]);
    CHECK_EQ(4u, idents[2]);
    list<string> components;
    REQUIRE(lexicon.getComponents(4, components));       // the retro lexicon follows
    CHECK(components == word("one", "three"));
    words.push_back(word(string(256, 'x')));
    CHECK_THROWS_AS(lexicon.addWords(words, idents), FileException);
    CHECK_EQ(4u, lexicon.getIdentification().lexiconWordsNb);   // nothing added
}
////////////////////////////////////////////////////////////
TEST_CASE("NindBulkTest.MergeShardsWithoutLocalIndex") {
    TestTempDir tmp;
    for (unsigned int shard = 0; shard < 2; shard++) {
        const string base = tmp.file("shard" + to_string(shard));
        NindLexiconIndex lexicon(base, true, false, 4);
        const unsigned int common = lexicon.addWord(word("common"));
        const unsigned int own = lexicon.addWord(word("own" + to_string(shard)));
        NindTermIndex terms(base, true, lexicon.getIdentification(), 0, 4);
        list<TermCG> def;
        def.push_back(TermCG(0, 1));
        def.back().documents.push_back(Document(10 + shard, 1));
        terms.setTermDef(common, def, lexicon.getIdentification(), list<unsigned int>());
        terms.setTermDef(own, def, lexicon.getIdentification(), list<unsigned int>());
    }
    vector<string> shards;
    shards.push_back(tmp.file("shard0"));
    shards.push_back(tmp.file("shard1"));
    const NindIndexMerge::Stats stats = NindIndexMerge(shards).merge(tmp.file("merged"));
    CHECK_EQ(3u, stats.wordsNb);
    CHECK_EQ(3u, stats.termsNb);
    CHECK_EQ(0u, stats.docsNb);
    FILE *local = fopen((tmp.file("merged") + ".nindlocalindex").c_str(), "rb");
    CHECK(local == 0);
    if (local) fclose(local);
    NindLexiconIndex lexicon(tmp.file("merged"), false);
    NindTermIndex terms(tmp.file("merged"), false, lexicon.getIdentification(), 0);
    list<TermCG> read;
    REQUIRE(terms.getTermDef(lexicon.getWordId(word("common")), read));
    REQUIRE_EQ(2u, read.front().documents.size());
    CHECK_EQ(10u, read.front().documents.front().ident);
    CHECK_EQ(11u, read.front().documents.back().ident);

    // some shards with a local index, some without: refused
    {
        NindLocalIndex local1(tmp.file("shard1"), true, NindIndex::Identification(2, 0), 4);
    }
    CHECK_THROWS_AS(NindIndexMerge(shards).merge(tmp.file("merged2")), FileException);
}
////////////////////////////////////////////////////////////
