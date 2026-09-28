#include "NindAmose/NindAmoseMerge.h"
#include "NindAmose/NindLexiconAmose.h"
#include "NindAmose/NindTermAmose.h"
#include "NindAmose/NindLocalAmose.h"
#include "NindExceptions.h"
#include "TestTempDir.h"
#include "doctest.h"
#include <list>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
namespace {
typedef NindLocalIndex::Term Term;
typedef NindLocalIndex::Localisation Localisation;
typedef tuple<string, AmoseTypes, string> AmoseWord;       //lemma, type, named entity
typedef map<unsigned int, vector<AmoseWord> > AmoseCorpus;

//indexes like NindAmose_indexeCorpus: one localisation per occurrence, postings per document
void buildAmoseIndex(const string &base, const AmoseCorpus &corpus) {
    NindLexiconAmose lexicon(base, true, 8, 8);
    map<unsigned int, pair<AmoseTypes, list<NindTermIndex::Document> > > postings;
    map<unsigned int, list<Term> > locals;
    for (AmoseCorpus::const_iterator it = corpus.begin(); it != corpus.end(); it++) {
        map<unsigned int, pair<AmoseTypes, unsigned int> > counts;
        for (size_t position = 0; position < it->second.size(); position++) {
            const AmoseWord &w = it->second[position];
            const unsigned int id = lexicon.addWord(get<0>(w), get<1>(w), get<2>(w));
            counts[id].first = get<1>(w);
            counts[id].second++;
            locals[it->first].push_back(Term(id, 0));
            locals[it->first].back().localisation.push_back(Localisation(position, 1));
        }
        for (auto itc = counts.begin(); itc != counts.end(); itc++) {
            postings[itc->first].first = itc->second.first;
            postings[itc->first].second.push_back(NindTermIndex::Document(it->first, itc->second.second));
        }
    }
    const NindIndex::Identification identification = lexicon.getIdentification();
    NindTermAmose termIndex(base, true, identification, 8);
    for (auto it = postings.begin(); it != postings.end(); it++)
        termIndex.addDocsToTerm(it->first, it->second.first, it->second.second, identification);
    NindLocalAmose localIndex(base, true, identification, 8);
    for (auto it = locals.begin(); it != locals.end(); it++)
        localIndex.setLocalDef(it->first, it->second, identification);
}

AmoseCorpus sampleAmoseCorpus() {
    const AmoseWord cat("cat", SIMPLE_TERM, ""), dog("dog", SIMPLE_TERM, "");
    const AmoseWord blackCat("black_cat", MULTI_TERM, ""), paris("Paris", NAMED_ENTITY, "LOCATION");
    const AmoseWord hotDog("hot_dog", MULTI_TERM, ""), newYork("New_York", NAMED_ENTITY, "LOCATION");
    AmoseCorpus corpus;
    corpus[1] = {cat, blackCat, cat, paris};
    corpus[2] = {dog, hotDog, paris};
    corpus[4] = {cat, newYork, dog};
    corpus[7] = {blackCat, dog, dog, newYork, paris};
    return corpus;
}
AmoseCorpus part(const AmoseCorpus &corpus, const vector<unsigned int> &docIds) {
    AmoseCorpus result;
    for (size_t i = 0; i < docIds.size(); i++) result[docIds[i]] = corpus.at(docIds[i]);
    return result;
}
}
////////////////////////////////////////////////////////////
TEST_CASE("NindAmoseMergeTest.MergedIndexHasTheCountsTypesAndPostingsOfASingleIndex") {
    TestTempDir tmp;
    const AmoseCorpus corpus = sampleAmoseCorpus();
    buildAmoseIndex(tmp.file("reference"), corpus);
    buildAmoseIndex(tmp.file("shard0"), part(corpus, {1, 4}));
    buildAmoseIndex(tmp.file("shard1"), part(corpus, {2, 7}));
    NindAmoseMerge({tmp.file("shard0"), tmp.file("shard1")}).merge(tmp.file("merged"));

    NindLexiconAmose referenceLexicon(tmp.file("reference"));
    NindLexiconAmose mergedLexicon(tmp.file("merged"));
    NindTermAmose referenceTerms(tmp.file("reference"), false, referenceLexicon.getIdentification());
    NindTermAmose mergedTerms(tmp.file("merged"), false, mergedLexicon.getIdentification());
    const AmoseTypes types[] = {ALL, SIMPLE_TERM, MULTI_TERM, NAMED_ENTITY};
    for (unsigned int i = 0; i < 4; i++) {
        CHECK_EQ(referenceTerms.getUniqueTermCount(types[i]), mergedTerms.getUniqueTermCount(types[i]));
        CHECK_EQ(referenceTerms.getTermOccurrences(types[i]), mergedTerms.getTermOccurrences(types[i]));
    }
    //prefixes of compound words (black, hot, New, §...) are in the lexicon but aren't terms
    CHECK_EQ(2u, mergedTerms.getUniqueTermCount(SIMPLE_TERM));     // cat, dog
    CHECK_EQ(2u, mergedTerms.getUniqueTermCount(MULTI_TERM));      // black_cat, hot_dog
    CHECK_EQ(2u, mergedTerms.getUniqueTermCount(NAMED_ENTITY));    // Paris, New_York
    CHECK_EQ(15u, mergedTerms.getTermOccurrences(ALL));
    const AmoseWord words[] = {AmoseWord("cat", SIMPLE_TERM, ""), AmoseWord("hot_dog", MULTI_TERM, ""),
                               AmoseWord("New_York", NAMED_ENTITY, "LOCATION")};
    for (unsigned int i = 0; i < 3; i++) {
        const unsigned int referenceId = referenceLexicon.getWordId(get<0>(words[i]), get<1>(words[i]), get<2>(words[i]));
        const unsigned int mergedId = mergedLexicon.getWordId(get<0>(words[i]), get<1>(words[i]), get<2>(words[i]));
        REQUIRE_NE(0u, mergedId);
        list<unsigned int> referenceDocs, mergedDocs;
        referenceTerms.getDocList(referenceId, referenceDocs);
        mergedTerms.getDocList(mergedId, mergedDocs);
        CHECK(referenceDocs == mergedDocs);
        CHECK_EQ(referenceTerms.getTermFreq(referenceId), mergedTerms.getTermFreq(mergedId));
        //the merged lexicon has a retro lexicon, so words come back with their type
        string lemma, namedEntity;
        AmoseTypes type;
        REQUIRE(mergedLexicon.getWord(mergedId, lemma, type, namedEntity));
        CHECK_EQ(get<0>(words[i]), lemma);
        CHECK_EQ(get<1>(words[i]), type);
        CHECK_EQ(get<2>(words[i]), namedEntity);
    }
    NindLocalAmose referenceLocals(tmp.file("reference"), false, referenceLexicon.getIdentification());
    NindLocalAmose mergedLocals(tmp.file("merged"), false, mergedLexicon.getIdentification());
    CHECK_EQ(referenceLocals.getDocCount(), mergedLocals.getDocCount());
    for (unsigned int docId : {1u, 2u, 4u, 7u}) {
        CHECK_EQ(referenceLocals.getDocLength(docId), mergedLocals.getDocLength(docId));
        set<string> referenceSet, mergedSet;
        REQUIRE(mergedLocals.getDocTerms(docId, NAMED_ENTITY, mergedSet));
        referenceLocals.getDocTerms(docId, NAMED_ENTITY, referenceSet);
        CHECK(referenceSet == mergedSet);
    }
}
////////////////////////////////////////////////////////////
TEST_CASE("NindAmoseMergeTest.PlainMergeRefusesAmoseShards") {
    TestTempDir tmp;
    buildAmoseIndex(tmp.file("shard0"), sampleAmoseCorpus());
    //the Amose term index has 8 counters as specifics, that a plain merge can't recompute
    CHECK_THROWS_AS(NindIndexMerge({tmp.file("shard0")}).merge(tmp.file("merged")), NindIndexException);
}
