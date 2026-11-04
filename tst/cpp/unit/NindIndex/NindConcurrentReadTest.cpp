// The thread-safe (const) read path: several threads reading one lexicon, term index and local
// index at once, each with its own NindReadBuffer (NindFile::readAt), must get exactly what the
// sequential API returns; a reader must see what a writer appends meanwhile.
// Meant to be run under ThreadSanitizer too (gbuild.sh -s ON): doctest assertions aren't
// thread-safe, so threads count their mismatches and the test checks the counts.
////////////////////////////////////////////////////////////
#include "NindIndex/NindLexiconIndex.h"
#include "NindIndex/NindTermIndex.h"
#include "NindIndex/NindLocalIndex.h"
#include "NindBasics/NindReadBuffer.h"
#include "NindExceptions.h"
#include "TestTempDir.h"
#include "doctest.h"
#include <atomic>
#include <list>
#include <map>
#include <string>
#include <thread>
#include <vector>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
namespace {
typedef NindTermIndex::TermCG TermCG;
typedef NindTermIndex::Document Document;
typedef NindLocalIndex::Term Term;
typedef NindLocalIndex::Localisation Localisation;
const NindIndex::Identification kNoCheck(0, 0);

// deterministic pseudo-random numbers (no shared state between threads)
struct Rng {
    uint64_t state;
    explicit Rng(uint64_t seed): state(seed * 2654435761u + 1) {}
    unsigned int next(const unsigned int bound) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return (unsigned int)((state >> 33) % bound);
    }
};

list<string> word(const unsigned int i) {
    list<string> components;
    components.push_back("w" + to_string(i));
    if (i % 5 == 0) components.push_back("w" + to_string(i / 5));      //some compound words
    return components;
}

// a term's definition, from its ident alone: sparse document ids, sometimes two categories
list<TermCG> termDef(const unsigned int ident) {
    Rng rng(ident);
    list<TermCG> def;
    def.push_back(TermCG(0, 0));
    const unsigned int docsNb = 1 + rng.next(ident % 7 == 0 ? 400 : 20);
    unsigned int doc = 13;
    for (unsigned int i = 0; i < docsNb; i++) {
        doc += 1 + rng.next(5000);
        const unsigned int frequency = 1 + rng.next(200);
        def.back().documents.push_back(Document(doc, frequency));
        def.back().frequency += frequency;
    }
    if (ident % 11 == 0) {
        def.push_back(TermCG(2, 1));
        def.back().documents.push_back(Document(17, 1));
    }
    return def;
}

list<Term> localDef(const unsigned int doc, const vector<unsigned int> &termIds) {
    Rng rng(doc);
    list<Term> def;
    unsigned int position = 0;
    const unsigned int termsNb = 1 + rng.next(60);
    for (unsigned int i = 0; i < termsNb; i++) {
        def.push_back(Term(termIds[rng.next(termIds.size())], 0));
        position += 1 + rng.next(30);
        def.back().localisation.push_back(Localisation(position, 1 + rng.next(12)));
    }
    return def;
}

bool sameTermDef(const list<TermCG> &a, const list<TermCG> &b) {
    if (a.size() != b.size()) return false;
    for (list<TermCG>::const_iterator x = a.begin(), y = b.begin(); x != a.end(); x++, y++) {
        if (x->cg != y->cg || x->frequency != y->frequency || x->documents.size() != y->documents.size()) return false;
        for (list<Document>::const_iterator dx = x->documents.begin(), dy = y->documents.begin(); dx != x->documents.end(); dx++, dy++)
            if (dx->ident != dy->ident || dx->frequency != dy->frequency) return false;
    }
    return true;
}

bool samePostings(const list<TermCG> &def, const vector<Document> &postings, const vector<size_t> &ends) {
    size_t i = 0, category = 0;
    for (list<TermCG>::const_iterator cg = def.begin(); cg != def.end(); cg++, category++) {
        for (list<Document>::const_iterator d = cg->documents.begin(); d != cg->documents.end(); d++, i++)
            if (i >= postings.size() || postings[i].ident != d->ident || postings[i].frequency != d->frequency) return false;
        if (category >= ends.size() || ends[category] != i) return false;
    }
    return i == postings.size() && category == ends.size();
}

bool sameLocalDef(const list<Term> &a, const list<Term> &b) {
    if (a.size() != b.size()) return false;
    for (list<Term>::const_iterator x = a.begin(), y = b.begin(); x != a.end(); x++, y++) {
        if (x->term != y->term || x->cg != y->cg || x->localisation.size() != y->localisation.size()) return false;
        for (list<Localisation>::const_iterator lx = x->localisation.begin(), ly = y->localisation.begin(); lx != x->localisation.end(); lx++, ly++)
            if (lx->position != ly->position || lx->length != ly->length) return false;
    }
    return true;
}

const unsigned int kWords = 600;
const unsigned int kDocs = 300;
unsigned int docIdent(const unsigned int d) { return 13 + d * 7919; }       //sparse external ids

// a lexicon, term index and local index with small indirection blocks (many blocks)
vector<unsigned int> buildIndex(const string &base, NindIndex::Identification &identification) {
    vector<unsigned int> ids;
    {
        NindLexiconIndex lexicon(base, true, false, 101);
        for (unsigned int i = 1; i <= kWords; i++) ids.push_back(lexicon.addWord(word(i)));
        identification = lexicon.getIdentification();
    }
    {
        NindTermIndex terms(base, true, identification, 0, 50);
        for (size_t i = 0; i < ids.size(); i++) terms.setTermDef(ids[i], termDef(ids[i]), identification, list<unsigned int>());
    }
    {
        NindLocalIndex local(base, true, identification, 32);
        for (unsigned int d = 0; d < kDocs; d++) local.setLocalDef(docIdent(d), localDef(d, ids), identification);
    }
    return ids;
}
}
////////////////////////////////////////////////////////////
TEST_CASE("NindConcurrentRead.ConstReadsMatchTheSequentialApi") {
    TestTempDir tmp;
    const string base = tmp.file("index");
    NindIndex::Identification identification;
    const vector<unsigned int> ids = buildIndex(base, identification);
    NindLexiconIndex lexicon(base, false);
    NindTermIndex terms(base, false, identification, 0);
    NindLocalIndex local(base, false, identification);
    NindReadBuffer buffer;
    for (unsigned int i = 1; i <= kWords; i++) {
        INFO("word " << i);
        CHECK_EQ(lexicon.getWordId(word(i)), lexicon.getWordId(word(i), buffer));
        CHECK_EQ(ids[i - 1], lexicon.getWordId(word(i), buffer));
    }
    list<string> unknown;
    unknown.push_back("unknown");
    CHECK_EQ(0u, lexicon.getWordId(unknown, buffer));
    for (size_t i = 0; i < ids.size(); i++) {
        INFO("term " << ids[i]);
        list<TermCG> sequential, concurrent;
        REQUIRE(terms.getTermDef(ids[i], sequential));
        REQUIRE(terms.getTermDef(ids[i], concurrent, buffer));
        CHECK(sameTermDef(sequential, concurrent));
        CHECK(sameTermDef(termDef(ids[i]), concurrent));
        vector<Document> postings;
        vector<size_t> ends;
        REQUIRE(terms.getTermPostings(ids[i], postings, ends, buffer));
        CHECK(samePostings(sequential, postings, ends));
    }
    list<TermCG> none;
    CHECK_FALSE(terms.getTermDef(100000, none, buffer));     //beyond every indirection block
    for (unsigned int d = 0; d < kDocs; d++) {
        INFO("document " << docIdent(d));
        list<Term> sequential, concurrent;
        REQUIRE(local.getLocalDef(docIdent(d), sequential));
        REQUIRE(local.getLocalDef(docIdent(d), concurrent, buffer));
        CHECK(sameLocalDef(sequential, concurrent));
        CHECK(sameLocalDef(localDef(d, ids), concurrent));
    }
    list<Term> noDoc;
    CHECK_FALSE(local.getLocalDef(12, noDoc, buffer));
}
////////////////////////////////////////////////////////////
TEST_CASE("NindConcurrentRead.ThreadsShareOneReader") {
    TestTempDir tmp;
    const string base = tmp.file("index");
    NindIndex::Identification identification;
    const vector<unsigned int> ids = buildIndex(base, identification);
    // opened once, shared by every thread; the local index's id map starts empty in the
    // threads' readers (they fill it concurrently)
    NindLexiconIndex lexicon(base, false);
    NindTermIndex terms(base, false, identification, 0);
    NindLocalIndex local(base, false, identification);
    std::atomic<unsigned int> mismatches(0), exceptions(0), reads(0);
    vector<std::thread> threads;
    for (unsigned int t = 0; t < 8; t++) {
        threads.push_back(std::thread([&, t]() {
            NindReadBuffer buffer;
            vector<Document> postings;
            vector<size_t> ends;
            Rng rng(1000 + t);
            try {
                for (unsigned int n = 0; n < 1500; n++) {
                    const unsigned int i = 1 + rng.next(kWords);
                    switch (rng.next(4)) {
                    case 0:
                        if (lexicon.getWordId(word(i), buffer) != ids[i - 1]) mismatches++;
                        break;
                    case 1: {
                        list<TermCG> def;
                        if (!terms.getTermDef(ids[i - 1], def, buffer) || !sameTermDef(def, termDef(ids[i - 1]))) mismatches++;
                        break; }
                    case 2:
                        if (!terms.getTermPostings(ids[i - 1], postings, ends, buffer)
                            || !samePostings(termDef(ids[i - 1]), postings, ends)) mismatches++;
                        break;
                    default: {
                        const unsigned int d = rng.next(kDocs);
                        list<Term> def;
                        if (!local.getLocalDef(docIdent(d), def, buffer) || !sameLocalDef(def, localDef(d, ids))) mismatches++;
                        break; }
                    }
                    reads++;
                }
            }
            catch (...) { exceptions++; }
        }));
    }
    // meanwhile, one thread on the sequential API of the same objects (allowed: it shares
    // nothing with the const path but the published maps and the local index's id map)
    threads.push_back(std::thread([&]() {
        try {
            for (unsigned int n = 0; n < 300; n++) {
                const unsigned int i = 1 + (n * 37) % kWords;
                list<TermCG> def;
                if (lexicon.getWordId(word(i)) != ids[i - 1]) mismatches++;
                if (!terms.getTermDef(ids[i - 1], def) || !sameTermDef(def, termDef(ids[i - 1]))) mismatches++;
                list<Term> localDefRead;
                const unsigned int d = (n * 13) % kDocs;
                if (!local.getLocalDef(docIdent(d), localDefRead) || !sameLocalDef(localDefRead, localDef(d, ids))) mismatches++;
                reads++;
            }
        }
        catch (...) { exceptions++; }
    }));
    for (size_t t = 0; t < threads.size(); t++) threads[t].join();
    CHECK_EQ(0u, exceptions.load());
    CHECK_EQ(0u, mismatches.load());
    CHECK_EQ(8u * 1500 + 300, reads.load());
}
////////////////////////////////////////////////////////////
TEST_CASE("NindConcurrentRead.ReadersSeeWhatAWriterAppends") {
    // a writer appends terms (new indirection blocks included) while readers look them up:
    // a definition is written before its indirection, and nothing is rewritten in place here,
    // so a reader finds a term either not yet or exactly
    TestTempDir tmp;
    const string base = tmp.file("growing");
    const unsigned int initialTerms = 40, finalTerms = 400;
    {
        NindTermIndex terms(base, true, kNoCheck, 0, 16);
        for (unsigned int ident = 1; ident <= initialTerms; ident++)
            terms.setTermDef(ident, termDef(ident), kNoCheck, list<unsigned int>());
    }
    NindTermIndex reader(base, false, kNoCheck, 0);
    std::atomic<bool> writing(true);
    std::atomic<unsigned int> mismatches(0), exceptions(0), found(0);
    std::thread writer([&]() {
        try {
            NindTermIndex terms(base, true, kNoCheck, 0, 16);
            for (unsigned int ident = initialTerms + 1; ident <= finalTerms; ident++)
                terms.setTermDef(ident, termDef(ident), kNoCheck, list<unsigned int>());
        }
        catch (...) { exceptions++; }
        writing = false;
    });
    vector<std::thread> readers;
    for (unsigned int t = 0; t < 4; t++) {
        readers.push_back(std::thread([&, t]() {
            NindReadBuffer buffer;
            vector<Document> postings;
            vector<size_t> ends;
            Rng rng(77 + t);
            try {
                while (writing) {
                    const unsigned int ident = 1 + rng.next(finalTerms);
                    if (reader.getTermPostings(ident, postings, ends, buffer)) {
                        found++;
                        if (!samePostings(termDef(ident), postings, ends)) mismatches++;
                    }
                    else if (ident <= initialTerms) mismatches++;
                }
            }
            catch (...) { exceptions++; }
        }));
    }
    writer.join();
    for (size_t t = 0; t < readers.size(); t++) readers[t].join();
    CHECK_EQ(0u, exceptions.load());
    CHECK_EQ(0u, mismatches.load());
    CHECK(found.load() > 0u);
    // afterwards, every term is there
    NindReadBuffer buffer;
    vector<Document> postings;
    vector<size_t> ends;
    for (unsigned int ident = 1; ident <= finalTerms; ident++) {
        INFO("term " << ident);
        REQUIRE(reader.getTermPostings(ident, postings, ends, buffer));
        CHECK(samePostings(termDef(ident), postings, ends));
    }
}
