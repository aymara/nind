// NindBm25Scorer: the BM25 formula, block-max MaxScore equal to the exhaustive search, concurrent
// searches on one scorer (run under TSan).
////////////////////////////////////////////////////////////
#include "NindIndex/NindBm25Scorer.h"
#include "NindIndex/NindIndexUpdate.h"
#include "NindIndex/NindLexiconIndex.h"
#include "NindIndex/NindTermIndex.h"
#include "TestTempDir.h"
#include "doctest.h"
#include <atomic>
#include <cmath>
#include <cstdint>
#include <list>
#include <map>
#include <string>
#include <thread>
#include <vector>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
namespace {
uint64_t next(uint64_t &state) {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return state >> 33;
}
// a corpus of docsNb documents, Zipf-like words w0..: its index at base, its lengths
struct Corpus {
    vector<NindDocId> docIds;
    vector<uint32_t> lengths;
    double avgLength;
    vector<string> vocabulary;
};
Corpus buildCorpus(const string &base, const unsigned int docsNb, uint64_t state) {
    Corpus corpus;
    NindIndexUpdate update(base);
    double total = 0;
    for (unsigned int d = 0; d < docsNb; d++) {
        vector<string> words;
        const unsigned int length = 3 + next(state) % 60;
        for (unsigned int i = 0; i < length; i++) {
            const unsigned int rank = (unsigned int)(next(state) % 1000);
            words.push_back("w" + to_string(rank * rank / 2500));      //0..399, small ranks frequent
        }
        const NindDocId doc = 10 + 3 * (NindDocId)d;                   //(not dense from 0)
        update.addDocument(doc, words);
        corpus.docIds.push_back(doc);
        corpus.lengths.push_back(length);
        total += length;
    }
    update.commit();
    corpus.avgLength = total / docsNb;
    for (unsigned int w = 0; w < 400; w++) corpus.vocabulary.push_back("w" + to_string(w));
    return corpus;
}
}
////////////////////////////////////////////////////////////
TEST_CASE("NindBm25Scorer.Formula") {
    TestTempDir tmp;
    const string base = tmp.file("tiny");
    {
        NindIndexUpdate update(base);
        update.addDocument(1, vector<string>{"a", "b", "a"});
        update.addDocument(2, vector<string>{"b", "c"});
        update.commit();
    }
    NindLexiconIndex lexicon(base, false);
    NindTermIndex terms(base, false, lexicon.getIdentification(), 0);
    const unsigned int a = lexicon.getWordId(list<string>(1, "a"));
    NindBm25Scorer scorer(vector<NindDocId>{1, 2}, vector<uint32_t>{3, 2}, 2.5);
    CHECK_EQ(2u, scorer.docsNb());
    const vector<pair<NindDocId, double> > results = scorer.search(terms, vector<unsigned int>{a, 0}, 1.2, 0.75, -1, false);
    REQUIRE_EQ(1u, results.size());
    CHECK_EQ(1u, results[0].first);
    const double idf = log((2.0 - 1 + 0.5) / (1 + 0.5) + 1.0);
    const double expected = idf * (2 * (1.2 + 1) / (2 + 1.2 * (1 - 0.75 + 0.75 * (3 / 2.5))));
    CHECK(fabs(results[0].second - expected) < 1e-12);
    CHECK_THROWS_AS(NindBm25Scorer(vector<NindDocId>{1}, vector<uint32_t>{1, 2}, 1.0), std::invalid_argument);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindBm25Scorer.MaxScoreEqualsExhaustive") {
    TestTempDir tmp;
    const string base = tmp.file("zipf");
    const Corpus corpus = buildCorpus(base, 3000, 5);
    NindLexiconIndex lexicon(base, false);
    NindTermIndex terms(base, false, lexicon.getIdentification(), 0);
    NindBm25Scorer scorer(corpus.docIds, corpus.lengths, corpus.avgLength);
    uint64_t state = 99;
    unsigned int queries = 0;
    for (unsigned int q = 0; q < 300; q++) {
        vector<unsigned int> termIds;
        const unsigned int length = 1 + next(state) % 6;
        for (unsigned int i = 0; i < length; i++) {
            const unsigned int rank = (unsigned int)(next(state) % 1000);
            termIds.push_back(lexicon.getWordId(list<string>(1, corpus.vocabulary[rank * rank / 2500])));
        }
        if (q % 7 == 0) termIds.push_back(termIds.front());            //a repeated term
        if (q % 11 == 0) termIds.push_back(0);                          //an unknown one
        const vector<pair<NindDocId, double> > all = scorer.search(terms, termIds, 1.2, 0.75, -1, false);
        for (const long k : {1L, 3L, 10L, 50L}) {
            INFO("query " << q << ", k " << k);
            vector<pair<NindDocId, double> > top(all.begin(), all.begin() + min((size_t)k, all.size()));
            CHECK(scorer.search(terms, termIds, 1.2, 0.75, k, true) == top);
            CHECK(scorer.search(terms, termIds, 1.2, 0.75, k, false) == top);
        }
        queries++;
    }
    CHECK_EQ(300u, queries);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindBm25Scorer.ConcurrentSearches") {
    // one scorer, one reader, many threads: the answers of a single thread
    TestTempDir tmp;
    const string base = tmp.file("concurrent");
    const Corpus corpus = buildCorpus(base, 1500, 17);
    NindLexiconIndex lexicon(base, false);
    NindTermIndex terms(base, false, lexicon.getIdentification(), 0);
    NindBm25Scorer scorer(corpus.docIds, corpus.lengths, corpus.avgLength);
    vector<vector<unsigned int> > queries;
    for (unsigned int q = 0; q < 40; q++)
        queries.push_back(vector<unsigned int>{lexicon.getWordId(list<string>(1, corpus.vocabulary[q % 30])),
                                               lexicon.getWordId(list<string>(1, corpus.vocabulary[(q * 7) % 90]))});
    vector<vector<pair<NindDocId, double> > > expected;
    for (size_t q = 0; q < queries.size(); q++) expected.push_back(scorer.search(terms, queries[q], 1.2, 0.75, 10, q % 2 == 0));
    atomic<unsigned int> mismatches(0);
    vector<thread> threads;
    for (unsigned int t = 0; t < 8; t++)
        threads.push_back(thread([&]() {
            for (unsigned int round = 0; round < 5; round++)
                for (size_t q = 0; q < queries.size(); q++)
                    if (scorer.search(terms, queries[q], 1.2, 0.75, 10, q % 2 == 0) != expected[q]) mismatches++;
        }));
    for (size_t t = 0; t < threads.size(); t++) threads[t].join();
    CHECK_EQ(0u, mismatches.load());
}
////////////////////////////////////////////////////////////
