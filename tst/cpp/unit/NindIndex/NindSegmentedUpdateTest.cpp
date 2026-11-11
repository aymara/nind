// NindSegmentedUpdate: after every commit, the index answers as one built from its live documents.
////////////////////////////////////////////////////////////
#include "NindIndex/NindSegmentedUpdate.h"
#include "NindIndex/NindSegmentedIndex.h"
#include "NindIndex/NindDocNames.h"
#include "NindIndex/NindIndexMerge.h"
#include "NindIndex/NindIndexUpdate.h"
#include "NindIndex/NindIndexVerify.h"
#include "NindExceptions.h"
#include "TestTempDir.h"
#include "TestFileBytes.h"
#include "doctest.h"
#include <cmath>
#include <cstdint>
#include <list>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
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
    const unsigned int length = 1 + next(state) % 40;
    for (unsigned int i = 0; i < length; i++) {
        const unsigned int rank = (unsigned int)(next(state) % 1000);
        words.push_back("w" + to_string(rank * rank / 2500));
    }
    return words;
}
string nameOf(const NindDocId doc) { return "doc" + to_string(doc); }

// one index of the documents: what the segmented index must answer like
void writeReference(const string &base, const Documents &documents) {
    vector<NindDocId> docIds;
    vector<uint32_t> lengths;
    {
        NindIndexUpdate update(base);
        for (Documents::const_iterator d = documents.begin(); d != documents.end(); d++) {
            update.addDocument(d->first, d->second);
            docIds.push_back(d->first);
            lengths.push_back((uint32_t)d->second.size());
        }
        update.commit();
    }
    NindDocLengths::write(base + ".ninddoclengths", docIds, lengths);
}
void checkAnswersAsOneIndex(const string &base, const string &referenceBase, const Documents &live, uint64_t &state) {
    if (live.empty()) {
        CHECK_EQ(0u, NindSegmentedIndex(base).docsNb());
        return;
    }
    writeReference(referenceBase, live);
    const NindSegmentedIndex index(base);
    const NindSegmentedIndex reference(referenceBase);
    REQUIRE_EQ(live.size(), index.docsNb());
    CHECK_EQ(reference.avgDocLength(), index.avgDocLength());
    const long topKs[] = {1, 10, -1};
    for (unsigned int q = 0; q < 12; q++) {
        vector<list<string> > query;
        const unsigned int terms = 1 + next(state) % 4;
        for (unsigned int t = 0; t < terms; t++) query.push_back(list<string>(1, "w" + to_string(next(state) % 400)));
        for (unsigned int k = 0; k < 3; k++) {
            CAPTURE(q);
            CAPTURE(topKs[k]);
            const vector<pair<NindDocId, double> > expected = reference.search(query, 1.2, 0.75, topKs[k], false);
            const vector<pair<NindDocId, double> > pruned = index.search(query, 1.2, 0.75, topKs[k], true, 2);
            const vector<pair<NindDocId, double> > exhaustive = index.search(query, 1.2, 0.75, topKs[k], false);
            REQUIRE_EQ(expected.size(), pruned.size());
            REQUIRE_EQ(expected.size(), exhaustive.size());
            for (size_t i = 0; i < expected.size(); i++) {
                REQUIRE_EQ(expected[i].first, pruned[i].first);
                REQUIRE_EQ(expected[i].first, exhaustive[i].first);
                REQUIRE(fabs(expected[i].second - pruned[i].second) <= 1e-9 * expected[i].second);
                REQUIRE(fabs(expected[i].second - exhaustive[i].second) <= 1e-9 * expected[i].second);
            }
        }
        CHECK_EQ(reference.documentFrequency(query[0]), index.documentFrequency(query[0]));
    }
}
void removeFiles(const string &base) {
    const char *extensions[] = {".nindlexiconindex", ".nindtermindex", ".nindlocalindex", ".ninddoclengths"};
    for (unsigned int e = 0; e < 4; e++) remove((base + extensions[e]).c_str());
}
void randomUpdates(const string &base, const string &referenceBase, const NindSegmentedUpdate::Options &options,
                   Documents &live, uint64_t &state, const unsigned int commits) {
    NindDocId nextId = 100;
    for (unsigned int c = 0; c < commits; c++) {
        CAPTURE(c);
        NindSegmentedUpdate update(base, options);
        const unsigned int changes = 1 + next(state) % 120;
        for (unsigned int i = 0; i < changes; i++) {
            const uint64_t r = next(state) % 10;
            if (r < 6 || live.empty()) {
                //new document (a few of them with a gap of ids)
                const NindDocId doc = nextId;
                nextId += 1 + (next(state) % 20 == 0 ? 50 : 0);
                vector<string> words = randomDocument(state);
                live[doc] = words;
                update.addDocument(doc, words, nameOf(doc));
            }
            else {
                Documents::iterator d = live.lower_bound(100 + next(state) % nextId);
                if (d == live.end()) d = live.begin();
                const NindDocId doc = d->first;
                if (r < 8) {
                    live[doc] = randomDocument(state);
                    update.addDocument(doc, live[doc], nameOf(doc));
                }
                else if (r == 8) {
                    live.erase(d);
                    update.removeDocument(doc);
                }
                else {
                    //no word left: removed
                    live.erase(d);
                    update.addDocument(doc, vector<string>());
                }
            }
        }
        const NindSegmentedUpdate::Stats stats = update.commit();
        CHECK_EQ(stats.segmentsNb, NindSegmentedIndex::readManifest(base).size());
        removeFiles(referenceBase);
        checkAnswersAsOneIndex(base, referenceBase, live, state);
    }
}
}
////////////////////////////////////////////////////////////
TEST_CASE("NindSegmentedUpdate.EveryCommitAnswersAsOneIndexOfTheLiveDocuments") {
    const unsigned int factors[] = {0, 3, 10};
    for (unsigned int f = 0; f < 3; f++) {
        for (unsigned int withLocal = 0; withLocal < 2; withLocal++) {
            CAPTURE(factors[f]);
            CAPTURE(withLocal);
            TestTempDir tmp;
            NindSegmentedUpdate::Options options;
            options.mergeFactor = factors[f];
            options.withLocalIndex = withLocal != 0;
            Documents live;
            uint64_t state = 7 + f;
            randomUpdates(tmp.file("index"), tmp.file("reference"), options, live, state, 12);
            const NindSegmentedIndex index(tmp.file("index"));
            if (factors[f] == 0) CHECK_EQ(12u, index.segments().size());
            else CHECK_LT(index.segments().size(), 12u);
            // the names: of live documents only
            string name;
            for (Documents::const_iterator d = live.begin(); d != live.end(); d++) {
                REQUIRE(index.getName(d->first, name));
                CHECK_EQ(nameOf(d->first), name);
            }
            // merged into one: its deleted documents gone, nothing masked, a sound index
            const NindSegmentedUpdate::Stats stats = NindSegmentedUpdate::forceMerge(tmp.file("index"));
            CHECK_EQ(1u, stats.segmentsNb);
            removeFiles(tmp.file("reference"));
            checkAnswersAsOneIndex(tmp.file("index"), tmp.file("reference"), live, state);
            const NindSegmentedIndex merged(tmp.file("index"));
            REQUIRE_EQ(1u, merged.segments().size());
            CHECK_EQ(0u, merged.segments()[0].deletedNb);
            CHECK_EQ(live.size(), merged.segments()[0].docsNb);
            const NindIndexVerify::Report report = NindIndexVerify::verify(merged.segments()[0].base, withLocal != 0);
            CHECK(report.ok());
            for (Documents::const_iterator d = live.begin(); d != live.end(); d++) {
                REQUIRE(merged.getName(d->first, name));
                CHECK_EQ(nameOf(d->first), name);
            }
        }
    }
}
////////////////////////////////////////////////////////////
TEST_CASE("NindSegmentedUpdate.CostsWhatChanges") {
    TestTempDir tmp;
    const string base = tmp.file("index");
    uint64_t state = 5;
    {
        NindSegmentedUpdate update(base);
        for (unsigned int d = 0; d < 50; d++) update.addDocument(d, randomDocument(state));
        update.commit();
    }
    const vector<string> before = NindSegmentedIndex::readManifest(base);
    REQUIRE_EQ(1u, before.size());
    // a new id, above the largest: nothing in the old segment is opened nor changed
    NindDocId maxIdent = 0;
    REQUIRE(NindSegmentedUpdate::getMaxDocIdent(base, maxIdent));
    CHECK_EQ(49u, maxIdent);
    NindSegmentedUpdate update(base);
    update.addDocument(maxIdent + 1, randomDocument(state));
    const NindSegmentedUpdate::Stats stats = update.commit();
    CHECK_EQ(1u, stats.addedDocsNb);
    CHECK_EQ(0u, stats.removedDocsNb);
    CHECK_EQ(2u, stats.segmentsNb);
    CHECK_EQ(0u, stats.mergesNb);
    NindSegmentDeletes deletes;
    CHECK_FALSE(deletes.read(NindSegmentedIndex::segmentBase(base, before[0]) + ".ninddeletes"));
}
////////////////////////////////////////////////////////////
TEST_CASE("NindSegmentedUpdate.SegmentsStayFewUnderSteadyUpdates") {
    // however many commits: full segments (at least half the limit), and few below them in each
    // tier; no merge beyond the limit
    TestTempDir tmp;
    const string base = tmp.file("index");
    NindSegmentedUpdate::Options options;
    options.mergeFactor = 3;
    options.maxMergedDocs = 1000;
    options.withLocalIndex = false;
    uint64_t state = 13;
    Documents live;
    NindDocId next_id = 0;
    unsigned int merges = 0;
    for (unsigned int c = 0; c < 300; c++) {
        NindSegmentedUpdate update(base, options);
        for (unsigned int d = 0; d < 20; d++) {
            live[next_id] = vector<string>(1, "w" + to_string(next(state) % 50));
            update.addDocument(next_id, live[next_id]);
            next_id++;
        }
        const NindSegmentedUpdate::Stats stats = update.commit();
        merges += stats.mergesNb;
        CAPTURE(c);
        //(2 N / limit full ones; below half the limit, 7 tiers of less than the factor each)
        REQUIRE_LE(stats.segmentsNb, 2 * live.size() / 1000 + 3 * 7);
    }
    CHECK_GE(merges, 50u);
    const NindSegmentedIndex index(base);
    CHECK_EQ(live.size(), index.docsNb());
    const vector<NindSegmentedIndex::SegmentInfo> segments = index.segments();
    unsigned int full = 0;
    for (size_t s = 0; s < segments.size(); s++) {
        CHECK_LE(segments[s].docsNb, 1000u);
        if (segments[s].docsNb >= 500) full++;
    }
    CHECK_GE(full, 3u);
    for (Documents::const_iterator d = live.begin(); d != live.end(); d++) REQUIRE(index.contains(d->first));
}
////////////////////////////////////////////////////////////
TEST_CASE("NindSegmentedUpdate.LargeSegmentsAreNotMerged") {
    // 6 segments of 30 documents (a tier of 25..124), factor 5, limit 50: each of them full
    TestTempDir tmp;
    const string base = tmp.file("index");
    uint64_t state = 9;
    NindSegmentedUpdate::Options options;
    options.mergeFactor = 0;
    for (unsigned int s = 0; s < 6; s++) {
        NindSegmentedUpdate update(base, options);
        for (unsigned int d = 0; d < 30; d++) update.addDocument(100 * s + d, randomDocument(state));
        update.commit();
    }
    options.mergeFactor = 5;
    options.maxMergedDocs = 50;
    {
        NindSegmentedUpdate update(base, options);
        update.addDocument(1000, randomDocument(state));
        const NindSegmentedUpdate::Stats stats = update.commit();
        CHECK_EQ(0u, stats.mergesNb);
        CHECK_EQ(7u, stats.segmentsNb);
    }
    // but a segment over half deleted is rewritten, whatever its size
    {
        NindSegmentedUpdate update(base, options);
        for (unsigned int d = 0; d < 20; d++) update.removeDocument(d);
        const NindSegmentedUpdate::Stats stats = update.commit();
        CHECK_EQ(1u, stats.mergesNb);
        CHECK_EQ(10u, stats.mergedDocsNb);
        CHECK_EQ(7u, stats.segmentsNb);
    }
    // without limit, the tier is merged
    options.maxMergedDocs = 0;
    {
        NindSegmentedUpdate update(base, options);
        update.addDocument(1001, randomDocument(state));
        const NindSegmentedUpdate::Stats stats = update.commit();
        CHECK_GE(stats.mergesNb, 1u);
        CHECK_LT(stats.segmentsNb, 8u);
    }
    CHECK_EQ(6u * 30 - 20 + 2, NindSegmentedIndex(base).docsNb());
}
////////////////////////////////////////////////////////////
TEST_CASE("NindSegmentedUpdate.RemovingEverythingLeavesAnEmptyIndex") {
    TestTempDir tmp;
    const string base = tmp.file("index");
    {
        NindSegmentedUpdate update(base);
        update.addDocument(1, vector<string>{"a", "b"});
        update.addDocument(2, vector<string>{"a"});
        update.commit();
    }
    {
        NindSegmentedUpdate update(base);
        update.removeDocument(1);
        update.removeDocument(2);
        update.removeDocument(3);      //(not in the index)
        const NindSegmentedUpdate::Stats stats = update.commit();
        CHECK_EQ(2u, stats.removedDocsNb);
        CHECK_EQ(0u, stats.segmentsNb);     //over half deleted: merged, nothing left
    }
    const NindSegmentedIndex index(base);
    CHECK_EQ(0u, index.docsNb());
    CHECK(index.search(vector<list<string> >(1, list<string>(1, "a")), 1.2, 0.75, 10, true).empty());
    // and it can grow again
    NindSegmentedUpdate update(base);
    update.addDocument(9, vector<string>{"a"});
    update.commit();
    CHECK_EQ(1u, NindSegmentedIndex(base).docsNb());
}
////////////////////////////////////////////////////////////
TEST_CASE("NindSegmentedUpdate.NameInTwoDocumentsIsRefused") {
    TestTempDir tmp;
    NindSegmentedUpdate update(tmp.file("index"));
    update.addDocument(1, vector<string>{"a"}, "same");
    update.addDocument(2, vector<string>{"b"}, "same");
    CHECK_THROWS_AS(update.commit(), std::invalid_argument);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindSegmentedUpdate.BatchesWriteWhatOneByOneWrites") {
    // the same documents, one by one and as tokenizer batches (a word looked up once per batch),
    // a batch's document replaced afterwards: the same segment files
    TestTempDir tmp;
    uint64_t state = 11;
    Documents documents;
    for (NindDocId d = 0; d < 300; d++) documents[d] = randomDocument(state);
    const vector<string> replacement{"w1", "fresh"};
    const string oneByOne = tmp.file("one"), batches = tmp.file("batch");
    {
        NindSegmentedUpdate update(oneByOne);
        for (Documents::const_iterator d = documents.begin(); d != documents.end(); d++)
            update.addDocument(d->first, d->second, nameOf(d->first));
        update.addDocument(7, replacement, nameOf(7));
        update.commit();
    }
    {
        NindSegmentedUpdate update(batches);
        for (NindDocId first = 0; first < 300; first += 100) {
            //a batch: its own vocabulary, local ids, ends
            vector<string> vocabulary;
            map<string, uint32_t> local;
            vector<uint32_t> localIds, ends;
            vector<NindDocId> idents;
            vector<string> names;
            for (NindDocId d = first; d < first + 100; d++) {
                for (size_t i = 0; i < documents[d].size(); i++) {
                    const pair<map<string, uint32_t>::iterator, bool> found =
                        local.insert(make_pair(documents[d][i], (uint32_t)vocabulary.size()));
                    if (found.second) vocabulary.push_back(documents[d][i]);
                    localIds.push_back(found.first->second);
                }
                ends.push_back((uint32_t)localIds.size());
                idents.push_back(d);
                names.push_back(nameOf(d));
            }
            update.addDocuments(idents, ends.data(), localIds.data(), localIds.size(), vocabulary, names);
        }
        update.addDocument(7, replacement, nameOf(7));
        // bad batches
        const uint32_t badEnds[] = {2, 1}, ids[] = {0, 0}, beyond[] = {5};
        CHECK_THROWS_AS(update.addDocuments({1000, 1001}, badEnds, ids, 2, {"x"}, {"", ""}), std::invalid_argument);
        CHECK_THROWS_AS(update.addDocuments({1000}, badEnds + 1, beyond, 1, {"x"}, {""}), std::invalid_argument);
        CHECK_THROWS_AS(update.addDocuments({1000}, badEnds + 1, ids, 1, {"x"}, {"", ""}), std::invalid_argument);
        update.commit();
    }
    const vector<string> segments = NindSegmentedIndex::readManifest(oneByOne);
    REQUIRE_EQ(1u, segments.size());
    const string a = NindSegmentedIndex::segmentBase(oneByOne, segments[0]);
    const string b = NindSegmentedIndex::segmentBase(batches, NindSegmentedIndex::readManifest(batches)[0]);
    for (const char *extension : {".ninddoclengths", ".ninddocnames"})
        CHECK(readFileBytes(a + extension) == readFileBytes(b + extension));
    const NindSegmentedIndex one(oneByOne), batch(batches);
    CHECK_EQ(one.docsNb(), batch.docsNb());
    for (const string word : {"w0", "w1", "w9", "w99", "fresh"}) {
        const vector<list<string> > query(1, list<string>(1, word));
        CHECK_EQ(one.documentFrequency(query[0]), batch.documentFrequency(query[0]));
        CHECK(one.search(query, 1.2, 0.75, 20, false) == batch.search(query, 1.2, 0.75, 20, false));
    }
    CHECK(NindIndexVerify::verify(batches, true).ok());
}
////////////////////////////////////////////////////////////
