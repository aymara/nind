// Unit tests for NindIndexMerge (offline merge of index shards) and the enumeration
// primitives it is built on (NindLexiconIndex::getEntries, NindLocalIndex::getDocIdents,
// NindTermIndex::setSpecificWords).
//
// The reference for a merge is the index built by a single writer over all the documents:
// both must have the same words, the same postings and the same local indexes, compared
// through the words' components since the merged term ids are renumbered.
#include "NindIndex/NindIndexMerge.h"
#include "NindIndex/NindLexiconIndex.h"
#include "NindIndex/NindTermIndex.h"
#include "NindIndex/NindLocalIndex.h"
#include "NindExceptions.h"
#include "TestTempDir.h"
#include "TestFileBytes.h"
#include "doctest.h"
#include <cstdio>
#include <exception>
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
typedef NindTermIndex::TermCG TermCG;
typedef NindTermIndex::Document Document;
typedef list<string> Word;
typedef vector<Word> Doc;                       //words in reading order
typedef map<unsigned int, Doc> Corpus;          //external doc id -> doc

Word word(const string &text) {
    //"a b c" -> compound word {a, b, c}
    Word result;
    string component;
    for (size_t i = 0; i <= text.size(); i++) {
        if (i == text.size() || text[i] == ' ') { result.push_back(component); component.clear(); }
        else component += text[i];
    }
    return result;
}
Doc doc(const string &a, const string &b = "", const string &c = "", const string &d = "") {
    Doc result;
    const string texts[] = {a, b, c, d};
    for (unsigned int i = 0; i < 4; i++) if (!texts[i].empty()) result.push_back(word(texts[i]));
    return result;
}
//a simple word gets a category depending on the document, so some terms have several TermCG
unsigned char categoryOf(const Word &w, const unsigned int docId) {
    return w.size() == 1 ? (unsigned char)(1 + docId % 2) : 9;
}

//indexes corpus the way an indexer would, with small blocks (several indirection blocks, hash collisions)
void buildIndex(const string &base, const Corpus &corpus, const unsigned int specificsNumber = 0) {
    NindLexiconIndex lexicon(base, true, false, 4, 4);
    map<unsigned int, map<unsigned char, map<unsigned int, unsigned int> > > postings;  //term -> cg -> doc -> freq
    map<unsigned int, list<Term> > locals;
    for (Corpus::const_iterator it = corpus.begin(); it != corpus.end(); it++) {
        list<Term> &localDef = locals[it->first];
        for (size_t position = 0; position < it->second.size(); position++) {
            const Word &w = it->second[position];
            const unsigned int id = lexicon.addWord(w);
            const unsigned char cg = categoryOf(w, it->first);
            postings[id][cg][it->first]++;
            localDef.push_back(Term(id, cg));
            localDef.back().localisation.push_back(Localisation(10 * position, w.size()));
        }
    }
    const NindIndex::Identification identification = lexicon.getIdentification();
    const list<unsigned int> specifics(specificsNumber, 7);
    NindTermIndex termIndex(base, true, identification, specificsNumber, 4);
    for (auto itTerm = postings.begin(); itTerm != postings.end(); itTerm++) {
        list<TermCG> termDef;
        for (auto itCg = itTerm->second.begin(); itCg != itTerm->second.end(); itCg++) {
            termDef.push_back(TermCG(itCg->first, 0));
            for (auto itDoc = itCg->second.begin(); itDoc != itCg->second.end(); itDoc++) {
                termDef.back().documents.push_back(Document(itDoc->first, itDoc->second));
                termDef.back().frequency += itDoc->second;
            }
        }
        termIndex.setTermDef(itTerm->first, termDef, identification, specifics);
    }
    //an empty term index still gets its specifics
    termIndex.setSpecificWords(specifics, identification);
    NindLocalIndex localIndex(base, true, identification, 4);
    for (auto itDoc = locals.begin(); itDoc != locals.end(); itDoc++)
        localIndex.setLocalDef(itDoc->first, itDoc->second, identification);
}

//everything an index holds, with terms named by their components instead of their ids
struct Content {
    set<Word> words;
    map<Word, vector<tuple<unsigned int, unsigned int, vector<pair<unsigned int, unsigned int> > > > > terms;
    map<unsigned int, vector<tuple<Word, unsigned int, vector<pair<unsigned int, unsigned int> > > > > locals;
    unsigned int docCount;
    bool operator==(const Content &c) const {
        return words == c.words && terms == c.terms && locals == c.locals && docCount == c.docCount;
    }
};
Content readContent(const string &base) {
    Content content;
    NindLexiconIndex lexicon(base, false);
    vector<NindLexiconIndex::Entry> entries;
    lexicon.getEntries(entries);
    map<unsigned int, Word> components;
    for (size_t i = 0; i < entries.size(); i++) {
        Word w = entries[i].prefixIdent ? components.at(entries[i].prefixIdent) : Word();
        w.push_back(entries[i].lastComponent);
        components[entries[i].ident] = w;
        content.words.insert(w);
    }
    const NindIndex::Identification identification = lexicon.getIdentification();
    NindTermIndex termIndex(base, false, identification, 0);
    for (auto it = components.begin(); it != components.end(); it++) {
        list<TermCG> termDef;
        if (!termIndex.getTermDef(it->first, termDef)) continue;
        auto &term = content.terms[it->second];
        for (auto itCg = termDef.begin(); itCg != termDef.end(); itCg++) {
            vector<pair<unsigned int, unsigned int> > docs;
            for (auto itDoc = itCg->documents.begin(); itDoc != itCg->documents.end(); itDoc++)
                docs.push_back(make_pair(itDoc->ident, itDoc->frequency));
            term.push_back(make_tuple((unsigned int)itCg->cg, itCg->frequency, docs));
        }
    }
    NindLocalIndex localIndex(base, false, identification);
    vector<unsigned int> docIdents;
    localIndex.getDocIdents(docIdents);
    for (size_t i = 0; i < docIdents.size(); i++) {
        list<Term> localDef;
        REQUIRE(localIndex.getLocalDef(docIdents[i], localDef));
        auto &local = content.locals[docIdents[i]];
        for (auto it = localDef.begin(); it != localDef.end(); it++) {
            vector<pair<unsigned int, unsigned int> > localisations;
            for (auto itLoc = it->localisation.begin(); itLoc != it->localisation.end(); itLoc++)
                localisations.push_back(make_pair(itLoc->position, itLoc->length));
            local.push_back(make_tuple(components.at(it->term), (unsigned int)it->cg, localisations));
        }
    }
    content.docCount = localIndex.getDocCount();
    return content;
}

Corpus part(const Corpus &corpus, const vector<unsigned int> &docIds) {
    Corpus result;
    for (size_t i = 0; i < docIds.size(); i++) result[docIds[i]] = corpus.at(docIds[i]);
    return result;
}

//shared vocabulary, compound words (also sharing prefixes with simple words), repeated words
Corpus sampleCorpus() {
    Corpus corpus;
    corpus[1] = doc("alpha", "beta", "alpha beta", "alpha");
    corpus[2] = doc("gamma", "alpha beta gamma", "delta");
    corpus[3] = doc("beta", "beta gamma", "epsilon", "alpha");
    corpus[5] = doc("delta", "alpha beta gamma", "zeta eta", "gamma");
    corpus[8] = doc("eta", "alpha", "alpha beta");
    corpus[13] = doc("theta iota kappa", "iota", "alpha beta");
    return corpus;
}

bool anyIndexFile(const string &base) {
    const char *extensions[] = {".nindlexiconindex", ".nindretrolexicon", ".nindtermindex", ".nindlocalindex"};
    for (unsigned int i = 0; i < 4; i++) if (!readFileBytes(base + extensions[i]).empty()) return true;
    return false;
}
void removeIndex(const string &base) {
    const char *extensions[] = {".nindlexiconindex", ".nindretrolexicon", ".nindtermindex", ".nindlocalindex"};
    for (unsigned int i = 0; i < 4; i++) remove((base + extensions[i]).c_str());
}
}
////////////////////////////////////////////////////////////
TEST_CASE("NindIndexMerge.LexiconEntriesRebuildEveryWordWithItsComponents") {
    TestTempDir tmp;
    const string base = tmp.file("lex");
    NindLexiconIndex writer(base, true, true, 4, 4);
    const Word words[] = {word("alpha"), word("alpha beta gamma"), word("beta"), word("delta alpha")};
    for (unsigned int i = 0; i < 4; i++) writer.addWord(words[i]);

    NindLexiconIndex lexicon(base, false, true);
    vector<NindLexiconIndex::Entry> entries;
    lexicon.getEntries(entries);
    // alpha, beta, alpha_beta, gamma, alpha_beta_gamma, delta, delta_alpha
    REQUIRE_EQ(7u, entries.size());
    for (size_t i = 0; i < entries.size(); i++) {
        CHECK_EQ(i + 1, entries[i].ident);                  // every ident, in increasing order
        list<string> expected;
        REQUIRE(lexicon.getComponents(entries[i].ident, expected));      // the retro lexicon agrees
        CHECK_EQ(expected.back(), entries[i].lastComponent);
        if (expected.size() == 1) CHECK_EQ(0u, entries[i].prefixIdent);
        else {
            expected.pop_back();
            list<string> prefix;
            REQUIRE(lexicon.getComponents(entries[i].prefixIdent, prefix));
            CHECK(prefix == expected);
        }
    }
}
////////////////////////////////////////////////////////////
TEST_CASE("NindIndexMerge.LocalIndexListsItsDocuments") {
    TestTempDir tmp;
    const string base = tmp.file("corpus");
    Corpus corpus = sampleCorpus();
    buildIndex(base, corpus);
    NindLocalIndex localIndex(base, false, NindIndex::Identification(0, 0));
    vector<unsigned int> docIdents;
    localIndex.getDocIdents(docIdents);
    const unsigned int expected[] = {1, 2, 3, 5, 8, 13};
    CHECK(docIdents == vector<unsigned int>(expected, expected + 6));
}
////////////////////////////////////////////////////////////
TEST_CASE("NindIndexMerge.MergedIndexHasTheContentOfASingleIndexOfAllDocuments") {
    TestTempDir tmp;
    const Corpus corpus = sampleCorpus();
    buildIndex(tmp.file("reference"), corpus);
    //interleaved documents, one empty shard, shards with words the others don't have
    const unsigned int docs0[] = {1, 5, 13}, docs1[] = {2, 8}, docs3[] = {3};
    buildIndex(tmp.file("shard0"), part(corpus, vector<unsigned int>(docs0, docs0 + 3)));
    buildIndex(tmp.file("shard1"), part(corpus, vector<unsigned int>(docs1, docs1 + 2)));
    buildIndex(tmp.file("shard2"), Corpus());
    buildIndex(tmp.file("shard3"), part(corpus, vector<unsigned int>(docs3, docs3 + 1)));
    vector<string> shards;
    for (unsigned int i = 0; i < 4; i++) shards.push_back(tmp.file("shard" + to_string(i)));

    const NindIndexMerge::Stats stats = NindIndexMerge(shards).merge(tmp.file("merged"));

    const Content reference = readContent(tmp.file("reference"));
    const Content merged = readContent(tmp.file("merged"));
    CHECK(merged.words == reference.words);
    CHECK(merged.terms == reference.terms);
    CHECK(merged.locals == reference.locals);
    CHECK_EQ(reference.docCount, merged.docCount);
    CHECK_EQ(4u, stats.shardsNb);
    CHECK_EQ(reference.words.size(), stats.wordsNb);
    CHECK_EQ(reference.terms.size(), stats.termsNb);
    CHECK_EQ(6u, stats.docsNb);
    CHECK_EQ(stats.identification, NindLexiconIndex(tmp.file("merged"), false).getIdentification());
}
////////////////////////////////////////////////////////////
TEST_CASE("NindIndexMerge.MergedIndexCanBeExtendedByAWriter") {
    TestTempDir tmp;
    const Corpus corpus = sampleCorpus();
    const unsigned int docs0[] = {1, 2, 3}, docs1[] = {5, 8, 13};
    buildIndex(tmp.file("shard0"), part(corpus, vector<unsigned int>(docs0, docs0 + 3)));
    buildIndex(tmp.file("shard1"), part(corpus, vector<unsigned int>(docs1, docs1 + 3)));
    vector<string> shards;
    shards.push_back(tmp.file("shard0"));
    shards.push_back(tmp.file("shard1"));
    NindIndexMerge(shards).merge(tmp.file("merged"));
    {
        //like an incremental indexer: writers are opened on the current identification (with
        //a block size to extend the files), then every write carries the enriched lexicon's one
        NindLexiconIndex lexicon(tmp.file("merged"), true);
        NindTermIndex termIndex(tmp.file("merged"), true, lexicon.getIdentification(), 0, 4);
        NindLocalIndex localIndex(tmp.file("merged"), true, lexicon.getIdentification(), 4);
        const unsigned int newId = lexicon.addWord(word("omega"));
        const NindIndex::Identification identification = lexicon.getIdentification();
        list<TermCG> termDef(1, TermCG(categoryOf(word("omega"), 21), 1));
        termDef.back().documents.push_back(Document(21, 1));
        termIndex.setTermDef(newId, termDef, identification, list<unsigned int>());
        list<Term> localDef(1, Term(newId, categoryOf(word("omega"), 21)));
        localDef.back().localisation.push_back(Localisation(0, 1));
        localIndex.setLocalDef(21, localDef, identification);
    }
    Corpus extended = corpus;
    extended[21] = doc("omega");
    buildIndex(tmp.file("reference"), extended);
    const Content reference = readContent(tmp.file("reference"));
    const Content merged = readContent(tmp.file("merged"));
    CHECK(merged.words == reference.words);
    CHECK_EQ(reference.docCount, merged.docCount);
    CHECK(merged.locals.at(21) == reference.locals.at(21));
    CHECK(merged.terms.at(word("omega")) == reference.terms.at(word("omega")));
}
////////////////////////////////////////////////////////////
TEST_CASE("NindIndexMerge.RejectsADocumentInSeveralShards") {
    TestTempDir tmp;
    const Corpus corpus = sampleCorpus();
    const unsigned int docs0[] = {1, 2}, docs1[] = {2, 3};
    buildIndex(tmp.file("shard0"), part(corpus, vector<unsigned int>(docs0, docs0 + 2)));
    buildIndex(tmp.file("shard1"), part(corpus, vector<unsigned int>(docs1, docs1 + 2)));
    vector<string> shards;
    shards.push_back(tmp.file("shard0"));
    shards.push_back(tmp.file("shard1"));
    CHECK_THROWS_AS(NindIndexMerge(shards).merge(tmp.file("merged")), NindIndexException);
    CHECK_FALSE(anyIndexFile(tmp.file("merged")));       //checked before writing anything
}
////////////////////////////////////////////////////////////
TEST_CASE("NindIndexMerge.RefusesAnExistingTarget") {
    TestTempDir tmp;
    buildIndex(tmp.file("shard0"), sampleCorpus());
    buildIndex(tmp.file("existing"), Corpus());
    const vector<string> shards(1, tmp.file("shard0"));
    CHECK_THROWS_AS(NindIndexMerge(shards).merge(tmp.file("existing")), NindIndexException);
    CHECK_THROWS_AS(NindIndexMerge(shards).merge(tmp.file("shard0")), NindIndexException);
    //nothing was written into the existing index
    CHECK(readContent(tmp.file("existing")).words.empty());
}
////////////////////////////////////////////////////////////
TEST_CASE("NindIndexMerge.RejectsShardsWithTermSpecifics") {
    TestTempDir tmp;
    buildIndex(tmp.file("shard0"), sampleCorpus(), 2);
    const vector<string> shards(1, tmp.file("shard0"));
    CHECK_THROWS_AS(NindIndexMerge(shards).merge(tmp.file("merged")), NindIndexException);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindIndexMerge.SetSpecificWordsRewritesOnlyTheSpecifics") {
    TestTempDir tmp;
    const string base = tmp.file("term");
    const NindIndex::Identification identification(3, 12345);
    list<TermCG> termDef(1, TermCG(1, 4));
    termDef.back().documents.push_back(Document(7, 4));
    {
        NindTermIndex writer(base, true, identification, 2, 4);
        writer.setTermDef(2, termDef, identification, list<unsigned int>(2, 1));
        list<unsigned int> specifics;
        specifics.push_back(10);
        specifics.push_back(20);
        writer.setSpecificWords(specifics, identification);
        CHECK_THROWS_AS(writer.setSpecificWords(list<unsigned int>(3, 1), identification), NindTermIndexException);
    }
    NindTermIndex reader(base, false, identification, 2);
    list<unsigned int> specifics;
    reader.getSpecificWords(specifics);
    CHECK_EQ(10u, specifics.front());
    CHECK_EQ(20u, specifics.back());
    list<TermCG> readDef;
    REQUIRE(reader.getTermDef(2, readDef));
    CHECK_EQ(7u, readDef.front().documents.front().ident);
    CHECK_THROWS_AS(reader.setSpecificWords(specifics, identification), NindTermIndexException);
}
////////////////////////////////////////////////////////////
//Every corrupted variant of a shard file must either merge or be rejected with a FileException
//(run under -a ON so that ASan/UBSan/LSan turn memory errors, UB and leaks into failures).
TEST_CASE("NindIndexMerge.CorruptShards") {
    TestTempDir tmp;
    const Corpus corpus = sampleCorpus();
    const unsigned int docs0[] = {1, 5, 13}, docs1[] = {2, 3, 8};
    buildIndex(tmp.file("shard0"), part(corpus, vector<unsigned int>(docs0, docs0 + 3)));
    buildIndex(tmp.file("shard1"), part(corpus, vector<unsigned int>(docs1, docs1 + 3)));
    vector<string> shards;
    shards.push_back(tmp.file("shard0"));
    shards.push_back(tmp.file("shard1"));
    const string target = tmp.file("merged");
    const char *extensions[] = {".nindlexiconindex", ".nindtermindex", ".nindlocalindex"};
    for (unsigned int e = 0; e < 3; e++) {
        const string path = tmp.file("shard0") + extensions[e];
        const vector<unsigned char> original = readFileBytes(path);
        int variants = 0, detected = 0;
        string failures;
        for (size_t offset = 0; offset < original.size(); offset++) {
            //fewer patterns than the NindIndexRobustness sweeps: each variant runs a whole merge
            for (unsigned int p = 0; p < 2; p++) {
                vector<unsigned char> bytes = original;
                bytes[offset] = (p == 0) ? 0xFF : (bytes[offset] ^ 0x01);
                if (bytes == original) continue;
                writeFileBytes(path, bytes);
                removeIndex(target);
                variants++;
                try { NindIndexMerge(shards).merge(target); }
                catch (const FileException &) {
                    detected++;
                    //a failed merge leaves no partial index behind
                    if (anyIndexFile(target)) failures += "byte " + to_string(offset) + ": partial target left\n";
                }
                catch (const std::exception &e) {
                    if (failures.size() < 1000) failures += "byte " + to_string(offset) + ": " + e.what() + "\n";
                }
            }
        }
        writeFileBytes(path, original);
        MESSAGE(string(extensions[e]) << ": " << variants << " corrupted variants, " << detected << " rejected");
        CHECK_EQ("", failures);
        CHECK_GT(detected, 0);
    }
}
