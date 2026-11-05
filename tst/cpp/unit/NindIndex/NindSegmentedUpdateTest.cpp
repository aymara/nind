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
