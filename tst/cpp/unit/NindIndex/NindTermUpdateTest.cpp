// NindTermIndex::updateTermDef: the same definitions as a model, whatever the mix of additions,
// replacements and removals; appends in bulk mode rewritten in place once the term has room.
////////////////////////////////////////////////////////////
#include "NindIndex/NindTermIndex.h"
#include "NindIndex/NindLexiconIndex.h"
#include "NindIndex/NindIndexVerify.h"
#include "NindBasics/NindReadBuffer.h"
#include "NindExceptions.h"
#include "TestTempDir.h"
#include "doctest.h"
#include <algorithm>
#include <cstdint>
#include <list>
#include <map>
#include <string>
#include <vector>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
namespace {
typedef NindTermIndex::TermCG TermCG;
typedef NindTermIndex::Document Document;
// the model: term -> cg -> document -> frequency
typedef map<unsigned int, map<unsigned char, map<NindDocId, unsigned int> > > Model;

uint64_t next(uint64_t &state) {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return state >> 33;
}

void checkAgainstModel(NindTermIndex &terms, const Model &model, const unsigned int termsNb) {
    NindReadBuffer buffer;
    for (unsigned int t = 1; t <= termsNb; t++) {
        INFO("term " << t);
        list<TermCG> def, defConst;
        const bool found = terms.getTermDef(t, def);
        REQUIRE_EQ(found, terms.getTermDef(t, defConst, buffer));
        const Model::const_iterator m = model.find(t);
        size_t categories = 0;
        if (m != model.end())
            for (map<unsigned char, map<NindDocId, unsigned int> >::const_iterator c = m->second.begin(); c != m->second.end(); c++)
                if (!c->second.empty()) categories++;
        REQUIRE_EQ(categories != 0, found);
        if (!found) continue;
        REQUIRE_EQ(categories, def.size());
        for (list<TermCG>::const_iterator cg = def.begin(); cg != def.end(); cg++) {
            const map<NindDocId, unsigned int> &expected = m->second.at(cg->cg);
            REQUIRE_EQ(expected.size(), cg->documents.size());
            unsigned int total = 0;
            map<NindDocId, unsigned int>::const_iterator e = expected.begin();
            for (list<Document>::const_iterator d = cg->documents.begin(); d != cg->documents.end(); d++, e++) {
                CHECK_EQ(e->first, d->ident);
                CHECK_EQ(e->second, d->frequency);
                total += d->frequency;
            }
            CHECK_EQ(total, cg->frequency);
        }
    }
}
}
////////////////////////////////////////////////////////////
TEST_CASE("NindTermUpdate.AgainstAModel") {
    TestTempDir tmp;
    const string base = tmp.file("update");
    const unsigned int termsNb = 12;
    NindIndex::Identification identification;
    {
        NindLexiconIndex lexicon(base, true, false, 16);
        for (unsigned int t = 0; t < termsNb; t++) lexicon.addWord(list<string>(1, "t" + to_string(t)));
        identification = lexicon.getIdentification();
    }
    Model model;
    uint64_t state = 42;
    NindDocId nextDoc = 1;
    unsigned int inPlace = 0, appends = 0;
    {
        // an initial build (no room), as a bulk build writes it
        NindTermIndex terms(base, true, identification, 0, 16);
        terms.beginBulk();
        for (unsigned int t = 1; t <= termsNb; t++) {
            const unsigned int docsNb = (t % 3 == 0) ? 50 : 300 + 97 * t;     //short and long lists
            vector<Document> added;
            for (unsigned int d = 0; d < docsNb; d++) {
                const NindDocId doc = 1 + d * (1 + t % 4);
                const unsigned int frequency = 1 + next(state) % 9;
                added.push_back(Document(doc, frequency));
                model[t][0][doc] = frequency;
            }
            terms.updateTermDef(t, vector<NindDocId>(), 0, added, identification, list<unsigned int>());
        }
        terms.endBulk();
        nextDoc = 1 + 4000 * 4;
    }
    for (unsigned int round = 0; round < 12; round++) {
        INFO("round " << round);
        NindTermIndex terms(base, true, identification, 0, 16);
        terms.beginBulk();
        for (unsigned int t = 1; t <= termsNb; t++) {
            const unsigned int kind = (next(state) + round) % 5;
            vector<NindDocId> removed;
            vector<Document> added;
            unsigned char cg = 0;
            if (kind <= 2) {
                //appends after the last document
                const unsigned int nb = 1 + next(state) % (kind == 0 ? 3 : 200);
                for (unsigned int i = 0; i < nb; i++) {
                    const NindDocId doc = nextDoc + i * 2 + t;
                    added.push_back(Document(doc, 1 + next(state) % 7));
                }
                appends++;
            }
            else if (kind == 3) {
                //replacements and removals of existing documents
                map<NindDocId, unsigned int> &docs = model[t][0];
                for (map<NindDocId, unsigned int>::const_iterator d = docs.begin(); d != docs.end(); d++) {
                    const uint64_t r = next(state) % 20;
                    if (r == 0) removed.push_back(d->first);
                    else if (r == 1) added.push_back(Document(d->first, 1 + next(state) % 5));
                }
            }
            else {
                //a second category
                cg = 3;
                for (unsigned int i = 0; i < 5; i++) added.push_back(Document(nextDoc + 7 * i, 2));
            }
            for (size_t i = 0; i < removed.size(); i++)
                for (map<unsigned char, map<NindDocId, unsigned int> >::iterator c = model[t].begin(); c != model[t].end(); c++)
                    c->second.erase(removed[i]);
            for (size_t i = 0; i < added.size(); i++) model[t][cg][added[i].ident] = added[i].frequency;
            if (terms.updateTermDef(t, removed, cg, added, identification, list<unsigned int>())) inPlace++;
        }
        nextDoc += 1000;
        terms.endBulk();
        NindTermIndex reader(base, false, identification, 0);
        checkAgainstModel(reader, model, termsNb);
    }
    MESSAGE(appends << " appends, " << inPlace << " in place");
    CHECK(inPlace > appends / 4);
    //a term emptied is erased
    {
        NindTermIndex terms(base, true, identification, 0, 16);
        terms.beginBulk();
        vector<NindDocId> all;
        for (map<unsigned char, map<NindDocId, unsigned int> >::const_iterator c = model[2].begin(); c != model[2].end(); c++)
            for (map<NindDocId, unsigned int>::const_iterator d = c->second.begin(); d != c->second.end(); d++) all.push_back(d->first);
        sort(all.begin(), all.end());
        all.erase(unique(all.begin(), all.end()), all.end());
        terms.updateTermDef(2, all, 0, vector<Document>(), identification, list<unsigned int>());
        model.erase(2);
        terms.endBulk();
    }
    NindTermIndex reader(base, false, identification, 0);
    checkAgainstModel(reader, model, termsNb);
    const NindIndexVerify::Report report = NindIndexVerify::verify(base);
    for (size_t i = 0; i < report.problems.size(); i++) MESSAGE(report.problems[i]);
    CHECK(report.ok());
}
////////////////////////////////////////////////////////////
TEST_CASE("NindTermUpdate.InPlaceOutsideBulkMode") {
    // outside bulk mode too (as Amose writes, document by document): appends in place
    TestTempDir tmp;
    const string base = tmp.file("nobulk");
    NindIndex::Identification identification;
    {
        NindLexiconIndex lexicon(base, true, false, 16);
        lexicon.addWord(list<string>(1, "t"));
        identification = lexicon.getIdentification();
    }
    NindTermIndex terms(base, true, identification, 0, 16);
    vector<Document> added;
    for (unsigned int d = 1; d <= 1000; d++) added.push_back(Document(d, 1));
    CHECK_FALSE(terms.updateTermDef(1, vector<NindDocId>(), 0, added, identification, list<unsigned int>()));
    CHECK(terms.updateTermDef(1, vector<NindDocId>(), 0, vector<Document>(1, Document(2000, 3)), identification, list<unsigned int>()));
    list<TermCG> def;
    REQUIRE(terms.getTermDef(1, def));
    CHECK_EQ(1001u, def.front().documents.size());
    CHECK_EQ(1003u, def.front().frequency);
    //bad input
    CHECK_THROWS_AS(terms.updateTermDef(1, vector<NindDocId>{5, 3}, 0, vector<Document>(), identification, list<unsigned int>()),
                    NindTermIndexException);
}
////////////////////////////////////////////////////////////
