// NindSegmentedIndex: answers exactly as one index built from its live documents.
////////////////////////////////////////////////////////////
#include "NindIndex/NindSegmentedIndex.h"
#include "NindIndex/NindDocNames.h"
#include "NindIndex/NindIndexUpdate.h"
#include "NindIndex/NindLexiconIndex.h"
#include "NindIndex/NindLocalIndex.h"
#include "NindExceptions.h"
#include "TestTempDir.h"
#include "TestFileBytes.h"
#include "doctest.h"
#include <atomic>
#include <cmath>
#include <cstdint>
#include <list>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
namespace {
typedef map<NindDocId, vector<string> > Documents;

uint64_t next(uint64_t &state) {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return state >> 33;
}
vector<string> randomDocument(uint64_t &state) {
    vector<string> words;
    const unsigned int length = 1 + next(state) % 60;
    for (unsigned int i = 0; i < length; i++) {
        const unsigned int rank = (unsigned int)(next(state) % 1000);
        words.push_back("w" + to_string(rank * rank / 2500));      //0..399, small ranks frequent
    }
    return words;
}
string nameOf(const NindDocId doc) { return "doc" + to_string(doc); }

// the standard index of documents at base, with its lengths and names
void writeIndex(const string &base, const Documents &documents) {
    vector<NindDocId> docIds;
    vector<uint32_t> lengths;
    vector<string> names;
    {
        NindIndexUpdate update(base);
        for (Documents::const_iterator d = documents.begin(); d != documents.end(); d++) {
            update.addDocument(d->first, d->second);
            docIds.push_back(d->first);
            lengths.push_back((uint32_t)d->second.size());
            names.push_back(nameOf(d->first));
        }
        update.commit();
    }
    NindDocLengths::write(base + ".ninddoclengths", docIds, lengths);
    NindDocNames::write(base + ".ninddocnames", docIds, names);
}
// the deletes file of these documents of a segment: their postings per term, from its local index
void writeDeletes(const string &segmentBase, const set<NindDocId> &deleted) {
    NindSegmentDeletes deletes;
    NindLexiconIndex lexicon(segmentBase, false);
    NindLocalIndex local(segmentBase, false, lexicon.getIdentification());
    map<unsigned int, uint32_t> counts;
    for (set<NindDocId>::const_iterator d = deleted.begin(); d != deleted.end(); d++) {
        deletes.docIds.push_back(*d);
        list<NindLocalIndex::Term> localDef;
        REQUIRE(local.getLocalDef(*d, localDef));
        set<unsigned int> terms;
        for (list<NindLocalIndex::Term>::const_iterator t = localDef.begin(); t != localDef.end(); t++) terms.insert(t->term);
        for (set<unsigned int>::const_iterator t = terms.begin(); t != terms.end(); t++) counts[*t]++;
    }
    deletes.termCounts.assign(counts.begin(), counts.end());
    deletes.write(segmentBase + ".ninddeletes");
}
void checkSameResults(const NindSegmentedIndex &index, const NindSegmentedIndex &reference, const vector<list<string> > &query,
                      const long topK, const bool prune, const unsigned int threads) {
    const vector<pair<NindDocId, double> > got = index.search(query, 1.2, 0.75, topK, prune, threads);
    const vector<pair<NindDocId, double> > expected = reference.search(query, 1.2, 0.75, topK, false, 1);
    CAPTURE(topK);
    CAPTURE(prune);
    CAPTURE(threads);
    REQUIRE_EQ(expected.size(), got.size());
    for (size_t i = 0; i < got.size(); i++) {
        REQUIRE_EQ(expected[i].first, got[i].first);
        REQUIRE(fabs(expected[i].second - got[i].second) <= 1e-9 * expected[i].second);
    }
}
list<string> word(const string &text) { return list<string>(1, text); }

// a segmented index and the same documents in one index, with what's in each to compare them
struct Fixture {
    TestTempDir tmp;
    string base;
    Documents live;
    vector<vector<list<string> > > queries;
    Fixture(): tmp(), base(tmp.file("index")), live(), queries() {
        uint64_t state = 11;
        //1200 documents in 4 segments of unequal sizes: consecutive ids, then interleaved ones
        vector<Documents> segments(4);
        const unsigned int sizes[] = {700, 300, 150, 50};
        NindDocId doc = 1000;
        for (unsigned int s = 0; s < 2; s++)
            for (unsigned int d = 0; d < sizes[s]; d++) segments[s][doc++] = randomDocument(state);
        for (unsigned int s = 2; s < 4; s++)
            for (unsigned int d = 0; d < sizes[s]; d++) segments[s][doc + 3 * d + s] = randomDocument(state);
        //deleted documents; replaced ones: deleted in their segment, in a later one with new words
        vector<set<NindDocId> > deleted(4);
        for (unsigned int s = 0; s < 3; s++) {
            for (Documents::const_iterator d = segments[s].begin(); d != segments[s].end(); d++) {
                const uint64_t r = next(state) % 10;
                if (r == 0) deleted[s].insert(d->first);
                if (r == 1 && s < 2) {
                    deleted[s].insert(d->first);
                    segments[3][d->first] = randomDocument(state);
                }
            }
        }
        //the highest scoring document for a word deleted: it mustn't raise theta
        segments[0][5] = vector<string>(1, "rare");
        segments[0][6] = vector<string>(30, "rare");
        deleted[0].insert(6);
        vector<string> names;
        for (unsigned int s = 0; s < 4; s++) {
            const string name = "seg" + to_string(s);
            names.push_back(name);
            const string segment = NindSegmentedIndex::segmentBase(base, name);
            REQUIRE_EQ(0, mkdir(segment.substr(0, segment.find_last_of('/')).c_str(), 0777));
            writeIndex(segment, segments[s]);
            if (!deleted[s].empty()) writeDeletes(segment, deleted[s]);
            for (Documents::const_iterator d = segments[s].begin(); d != segments[s].end(); d++)
                if (deleted[s].count(d->first) == 0) live[d->first] = d->second;
        }
        NindSegmentedIndex::writeManifest(base, names);
        writeIndex(tmp.file("reference"), live);
        for (unsigned int q = 0; q < 60; q++) {
            vector<list<string> > query;
            const unsigned int terms = 1 + next(state) % 5;
            for (unsigned int t = 0; t < terms; t++) query.push_back(word("w" + to_string(next(state) % 400)));
            if (q % 7 == 0) query.push_back(query[0]);          //a repeated word
            if (q % 11 == 0) query.push_back(word("unknown"));
            queries.push_back(query);
        }
        queries.push_back(vector<list<string> >(1, word("rare")));
    }
};
}
////////////////////////////////////////////////////////////
TEST_CASE("NindSegmentedIndex.AnswersAsOneIndexOfItsLiveDocuments") {
    Fixture f;
    const NindSegmentedIndex index(f.base);
    const NindSegmentedIndex reference(f.tmp.file("reference"));
    CHECK_EQ(4u, index.segments().size());
    CHECK_EQ(1u, reference.segments().size());
    CHECK_EQ(reference.docsNb(), index.docsNb());
    CHECK_EQ(f.live.size(), index.docsNb());
    CHECK_EQ(reference.avgDocLength(), index.avgDocLength());
    const long topKs[] = {1, 10, 100, 100000, -1};
    for (size_t q = 0; q < f.queries.size(); q++) {
        CAPTURE(q);
        for (size_t k = 0; k < 5; k++) {
            for (unsigned int threads = 1; threads <= 8; threads *= 2) {
                checkSameResults(index, reference, f.queries[q], topKs[k], false, threads);
                checkSameResults(index, reference, f.queries[q], topKs[k], true, threads);
            }
        }
    }
}
////////////////////////////////////////////////////////////
TEST_CASE("NindSegmentedIndex.DocumentLevelAnswersAreTheLiveDocuments") {
    Fixture f;
    const NindSegmentedIndex index(f.base);
    const NindSegmentedIndex reference(f.tmp.file("reference"));
    for (unsigned int w = 0; w < 400; w += 13)
        CHECK_EQ(reference.documentFrequency(word("w" + to_string(w))), index.documentFrequency(word("w" + to_string(w))));
    CHECK_EQ(1u, index.documentFrequency(word("rare")));
    CHECK_EQ(0u, index.documentFrequency(word("unknown")));
    for (Documents::const_iterator d = f.live.begin(); d != f.live.end(); d++) {
        CHECK(index.contains(d->first));
        CHECK_EQ(d->second.size(), index.documentLength(d->first));
        CHECK_EQ(reference.termFrequency(word(d->second[0]), d->first), index.termFrequency(word(d->second[0]), d->first));
        string name;
        REQUIRE(index.getName(d->first, name));
        CHECK_EQ(nameOf(d->first), name);
        NindDocId found = 0;
        REQUIRE(index.getDocIdent(name, found));
        CHECK_EQ(d->first, found);
    }
    // a deleted document is nowhere
    CHECK_FALSE(index.contains(6));
    CHECK_EQ(0u, index.documentLength(6));
    CHECK_EQ(0u, index.termFrequency(word("rare"), 6));
    string name;
    CHECK_FALSE(index.getName(6, name));
    NindDocId found = 0;
    CHECK_FALSE(index.getDocIdent(nameOf(6), found));
    vector<NindDocId> docIds;
    vector<uint32_t> lengths;
    index.getDocuments(docIds, lengths);
    REQUIRE_EQ(f.live.size(), docIds.size());
    size_t i = 0;
    for (Documents::const_iterator d = f.live.begin(); d != f.live.end(); d++, i++) {
        CHECK_EQ(d->first, docIds[i]);
        CHECK_EQ(d->second.size(), lengths[i]);
    }
}
////////////////////////////////////////////////////////////
TEST_CASE("NindSegmentedIndex.SharesTheSegmentsOfAnIndexOpenedEarlier") {
    Fixture f;
    const NindSegmentedIndex first(f.base);
    // a later generation of the same segments: another document deleted in the last one
    const vector<NindSegmentedIndex::SegmentInfo> segments = first.segments();
    vector<NindDocId> docIds;
    vector<uint32_t> lengths;
    NindDocLengths::read(segments[0].base + ".ninddoclengths", docIds, lengths);
    NindSegmentDeletes deletes;
    REQUIRE(deletes.read(segments[0].base + ".ninddeletes"));
    const uint64_t deletedBefore = deletes.docIds.size();
    const NindSegmentedIndex second(f.base, &first);
    CHECK_EQ(first.docsNb(), second.docsNb());
    CHECK_EQ(deletedBefore, second.segments()[0].deletedNb);
    for (size_t q = 0; q < 5; q++) {
        CHECK(first.search(f.queries[q], 1.2, 0.75, 10, true) == second.search(f.queries[q], 1.2, 0.75, 10, true));
    }
}
////////////////////////////////////////////////////////////
TEST_CASE("NindSegmentedIndex.ConcurrentSearches") {
    Fixture f;
    const NindSegmentedIndex index(f.base);
    const NindSegmentedIndex reference(f.tmp.file("reference"));
    vector<vector<pair<NindDocId, double> > > expected;
    for (size_t q = 0; q < f.queries.size(); q++) expected.push_back(reference.search(f.queries[q], 1.2, 0.75, 10, false));
    std::atomic<unsigned int> mismatches(0);
    vector<std::thread> threads;
    for (unsigned int t = 0; t < 4; t++) {
        threads.push_back(std::thread([&, t]() {
            for (size_t q = 0; q < f.queries.size(); q++) {
                const vector<pair<NindDocId, double> > got = index.search(f.queries[q], 1.2, 0.75, 10, true, 1 + t);
                if (got.size() != expected[q].size()) mismatches++;
                else for (size_t i = 0; i < got.size(); i++)
                    if (got[i].first != expected[q][i].first) mismatches++;
            }
        }));
    }
    for (size_t t = 0; t < threads.size(); t++) threads[t].join();
    CHECK_EQ(0u, mismatches.load());
}
////////////////////////////////////////////////////////////
TEST_CASE("NindSegmentedIndex.NoManifestIsOneSegment") {
    TestTempDir tmp;
    Documents documents;
    documents[3] = vector<string>{"a", "b"};
    documents[4] = vector<string>{"a"};
    writeIndex(tmp.file("flat"), documents);
    const NindSegmentedIndex index(tmp.file("flat"));
    CHECK_EQ(2u, index.docsNb());
    CHECK_EQ(1.5, index.avgDocLength());
    REQUIRE_EQ(1u, index.segments().size());
    CHECK_EQ("", index.segments()[0].name);
    NindDocId maxIdent = 0;
    REQUIRE(index.getMaxDocIdent(maxIdent));
    CHECK_EQ(4u, maxIdent);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindSegmentedIndex.InvalidFilesAreRefused") {
    TestTempDir tmp;
    Documents documents;
    documents[3] = vector<string>{"a", "b"};
    const string base = tmp.file("index");
    const string segment = NindSegmentedIndex::segmentBase(base, "s");
    REQUIRE_EQ(0, mkdir(segment.substr(0, segment.find_last_of('/')).c_str(), 0777));
    writeIndex(segment, documents);
    // a segment's path escaping the index
    NindSegmentedIndex::writeManifest(base, vector<string>{"../s"});
    CHECK_THROWS_AS(NindSegmentedIndex index(base), FileException);
    writeFileBytes(base + ".nindsegments", vector<unsigned char>{'x', '\n'});
    CHECK_THROWS_AS(NindSegmentedIndex index(base), FileException);
    // a missing segment
    NindSegmentedIndex::writeManifest(base, vector<string>{"none"});
    CHECK_THROWS_AS(NindSegmentedIndex index(base), FileException);
    NindSegmentedIndex::writeManifest(base, vector<string>{"s"});
    // a deleted document the segment hasn't; a truncated deletes file; truncated lengths
    NindSegmentDeletes deletes;
    deletes.docIds.push_back(99);
    deletes.write(segment + ".ninddeletes");
    CHECK_THROWS_AS(NindSegmentedIndex index(base), FileException);
    vector<unsigned char> bytes = readFileBytes(segment + ".ninddeletes");
    bytes.pop_back();
    writeFileBytes(segment + ".ninddeletes", bytes);
    CHECK_THROWS_AS(NindSegmentedIndex index(base), FileException);
    remove((segment + ".ninddeletes").c_str());
    CHECK_EQ(1u, NindSegmentedIndex(base).docsNb());
    bytes = readFileBytes(segment + ".ninddoclengths");
    bytes.pop_back();
    writeFileBytes(segment + ".ninddoclengths", bytes);
    CHECK_THROWS_AS(NindSegmentedIndex index(base), FileException);
}
////////////////////////////////////////////////////////////
