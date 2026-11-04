// NindIndexUpdate: an index updated document by document equals the one built at once from the
// final documents (compared by words: term ids may differ), with and without local index, through
// a lexicon rebuild, and for compound words with categories and localisations.
////////////////////////////////////////////////////////////
#include "NindIndex/NindIndexUpdate.h"
#include "NindIndex/NindIndexVerify.h"
#include "NindIndex/NindLexiconIndex.h"
#include "NindIndex/NindTermIndex.h"
#include "NindIndex/NindLocalIndex.h"
#include "NindExceptions.h"
#include "TestTempDir.h"
#include "doctest.h"
#include <cstdint>
#include <list>
#include <map>
#include <set>
#include <string>
#include <vector>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
namespace {
typedef NindTermIndex::TermCG TermCG;
typedef NindTermIndex::Document Document;
typedef NindLocalIndex::Term Term;

uint64_t next(uint64_t &state) {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return state >> 33;
}
// a document of simple words: Zipf-like, a few very frequent words
vector<string> randomDocument(uint64_t &state) {
    vector<string> words;
    const unsigned int length = 1 + next(state) % 40;
    for (unsigned int i = 0; i < length; i++) {
        const unsigned int rank = (unsigned int)(next(state) % 1000);
        words.push_back("w" + to_string(rank * rank / 4000));      //0..249, small ranks frequent
    }
    return words;
}
// everything an index says, by words: word -> cg -> document -> frequency; document -> its terms'
// words with (cg, localisations)
struct Content {
    map<string, map<unsigned char, map<NindDocId, unsigned int> > > terms;
    map<NindDocId, map<pair<string, unsigned char>, vector<pair<unsigned int, unsigned int> > > > documents;
    bool operator==(const Content &other) const { return terms == other.terms && documents == other.documents; }
};
string join(const list<string> &components) {
    string all;
    for (list<string>::const_iterator c = components.begin(); c != components.end(); c++) all += (all.empty() ? "" : "+") + *c;
    return all;
}
Content contentOf(const string &base) {
    Content content;
    NindLexiconIndex lexicon(base, false);
    vector<NindLexiconIndex::Entry> entries;
    lexicon.getEntries(entries);
    //every word's components (a prefix precedes its compound words)
    map<unsigned int, list<string> > words;
    for (size_t i = 0; i < entries.size(); i++) {
        list<string> components;
        if (entries[i].prefixIdent != 0) components = words[entries[i].prefixIdent];
        components.push_back(entries[i].lastComponent);
        words[entries[i].ident] = components;
    }
    const NindIndex::Identification identification = lexicon.getIdentification();
    NindTermIndex terms(base, false, identification, 0);
    for (map<unsigned int, list<string> >::const_iterator w = words.begin(); w != words.end(); w++) {
        list<TermCG> def;
        if (!terms.getTermDef(w->first, def)) continue;
        for (list<TermCG>::const_iterator cg = def.begin(); cg != def.end(); cg++)
            for (list<Document>::const_iterator d = cg->documents.begin(); d != cg->documents.end(); d++)
                content.terms[join(w->second)][cg->cg][d->ident] = d->frequency;
    }
    FILE *file = fopen((base + ".nindlocalindex").c_str(), "rb");
    if (file != 0) {
        fclose(file);
        NindLocalIndex local(base, false, identification);
        vector<NindDocId> docs;
        local.getDocIdents(docs);
        for (size_t i = 0; i < docs.size(); i++) {
            list<Term> def;
            if (!local.getLocalDef(docs[i], def)) continue;
            for (list<Term>::const_iterator t = def.begin(); t != def.end(); t++)
                for (list<NindLocalIndex::Localisation>::const_iterator l = t->localisation.begin(); l != t->localisation.end(); l++)
                    content.documents[docs[i]][make_pair(join(words[t->term]), t->cg)].push_back(make_pair(l->position, l->length));
        }
    }
    return content;
}
void checkSound(const string &base) {
    const NindIndexVerify::Report report = NindIndexVerify::verify(base, true);
    for (size_t i = 0; i < report.problems.size(); i++) MESSAGE(report.problems[i]);
    CHECK(report.ok());
}
}
////////////////////////////////////////////////////////////
TEST_CASE("NindIndexUpdate.UpdatedEqualsBuiltAtOnce") {
    for (const bool withLocalIndex : {true, false}) {
        for (const unsigned int maxLoad : {4u, 0u}) {
            //(a load of 0: the lexicon rebuilt after every update)
            INFO("local index " << withLocalIndex << ", lexicon max load " << maxLoad);
            TestTempDir tmp;
            NindIndexUpdate::Options options;
            options.withLocalIndex = withLocalIndex;
            options.lexiconMaxLoad = maxLoad;
            options.indirectionBlocMin = 64;
            uint64_t state = 7;
            map<NindDocId, vector<string> > final;
            const string updated = tmp.file("updated");
            unsigned int inPlace = 0;
            bool rebuilt = false;
            NindDocId nextDoc = 0;
            for (unsigned int round = 0; round < 8; round++) {
                NindIndexUpdate update(updated, options);
                const unsigned int adds = (round == 0) ? 1500 : 60;
                for (unsigned int i = 0; i < adds; i++, nextDoc++) {
                    const vector<string> words = randomDocument(state);
                    update.addDocument(nextDoc, words);
                    final[nextDoc] = words;
                }
                if (round > 0) {
                    //replacements, removals (one of a document that doesn't exist), a document emptied
                    for (unsigned int i = 0; i < 10; i++) {
                        const NindDocId doc = next(state) % nextDoc;
                        if (i % 3 == 0) { update.removeDocument(doc); final.erase(doc); }
                        else { const vector<string> words = randomDocument(state); update.addDocument(doc, words); final[doc] = words; }
                    }
                    update.removeDocument(nextDoc + 1000000);
                    const NindDocId emptied = next(state) % nextDoc;
                    update.addDocument(emptied, vector<string>());
                    final.erase(emptied);
                }
                const NindIndexUpdate::Stats stats = update.commit();
                inPlace += stats.inPlaceTermsNb;
                rebuilt = rebuilt || stats.rebuilt;
                checkSound(updated);
            }
            //the same documents, at once
            const string atOnce = tmp.file("atonce");
            {
                NindIndexUpdate update(atOnce, options);
                for (map<NindDocId, vector<string> >::const_iterator d = final.begin(); d != final.end(); d++)
                    update.addDocument(d->first, d->second);
                update.commit();
            }
            CHECK(contentOf(updated) == contentOf(atOnce));
            MESSAGE(inPlace << " terms rewritten in place" << string(rebuilt ? ", lexicon rebuilt" : ""));
            if (maxLoad != 0) CHECK(inPlace > 0);
            CHECK_EQ(maxLoad == 0, rebuilt);
        }
    }
}
////////////////////////////////////////////////////////////
TEST_CASE("NindIndexUpdate.CompoundWordsCategoriesLocalisations") {
    TestTempDir tmp;
    const string base = tmp.file("amose");
    typedef NindIndexUpdate::Occurrence Occurrence;
    list<string> simple(1, "paris"), compound;
    compound.push_back("tour");
    compound.push_back("eiffel");
    {
        NindIndexUpdate update(base);
        vector<Occurrence> occurrences;
        occurrences.push_back(Occurrence{compound, 2, 10, 11});
        occurrences.push_back(Occurrence{simple, 1, 25, 5});
        occurrences.push_back(Occurrence{simple, 3, 40, 5});        //another category
        occurrences.push_back(Occurrence{simple, 1, 60, 5});
        update.addDocument(5, occurrences, 4);
        vector<Occurrence> other(1, Occurrence{simple, 1, 0, 5});
        update.addDocument(9, other);
        update.commit();
    }
    Content content = contentOf(base);
    CHECK_EQ(1u, content.terms["tour+eiffel"][2][5]);
    CHECK_EQ(2u, content.terms["paris"][1][5]);
    CHECK_EQ(1u, content.terms["paris"][1][9]);
    CHECK_EQ(1u, content.terms["paris"][3][5]);
    CHECK((content.documents[5][make_pair(string("paris"), (unsigned char)1)] ==
           vector<pair<unsigned int, unsigned int> >{{25, 5}, {60, 5}}));
    CHECK((content.documents[5][make_pair(string("tour+eiffel"), (unsigned char)2)] ==
           vector<pair<unsigned int, unsigned int> >{{10, 11}}));
    checkSound(base);
    //document 5 replaced: it leaves every category of its terms
    {
        NindIndexUpdate update(base);
        update.addDocument(5, vector<Occurrence>(1, Occurrence{compound, 2, 0, 11}));
        update.commit();
    }
    content = contentOf(base);
    CHECK_EQ(0u, content.terms["paris"][1].count(5));
    CHECK_EQ(0u, content.terms["paris"].count(3));
    CHECK_EQ(1u, content.terms["paris"][1][9]);
    CHECK_EQ(1u, content.terms["tour+eiffel"][2][5]);
    checkSound(base);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindIndexUpdate.Misuse") {
    TestTempDir tmp;
    NindIndexUpdate update(tmp.file("misuse"));
    update.addDocument(1, vector<string>(1, "a"));
    update.commit();
    CHECK_THROWS_AS(update.commit(), NindIndexException);
    CHECK_THROWS_AS(update.addDocument(2, vector<string>(1, "b")), NindIndexException);
    //nothing to do: nothing written
    NindIndexUpdate empty(tmp.file("nothing"));
    const NindIndexUpdate::Stats stats = empty.commit();
    CHECK_EQ(0u, stats.termsNb);
    FILE *file = fopen((tmp.file("nothing") + ".nindlexiconindex").c_str(), "rb");
    CHECK(file == 0);
    if (file != 0) fclose(file);
}
////////////////////////////////////////////////////////////
