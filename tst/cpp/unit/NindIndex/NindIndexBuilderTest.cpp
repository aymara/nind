// NindIndexBuilder: a corpus written at once holds what NindIndexUpdate writes for the same
// documents (term and local definitions, blocks' shortest lengths), and bad input is refused.
////////////////////////////////////////////////////////////
#include "NindIndex/NindIndexBuilder.h"
#include "NindIndex/NindIndexUpdate.h"
#include "NindIndex/NindIndexVerify.h"
#include "NindIndex/NindLexiconIndex.h"
#include "NindIndex/NindTermIndex.h"
#include "NindIndex/NindLocalIndex.h"
#include "NindBasics/NindReadBuffer.h"
#include "TestTempDir.h"
#include "TestFileBytes.h"
#include "doctest.h"
#include <cstdint>
#include <list>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
namespace {
uint64_t next(uint64_t &state) {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return state >> 33;
}
// word -> document -> frequency, and the blocks' (first, min length) per word
struct Seen {
    map<string, map<NindDocId, unsigned int> > postings;
    map<string, vector<pair<NindDocId, unsigned int> > > blocks;
    map<NindDocId, size_t> localTerms;
    bool operator==(const Seen &o) const { return postings == o.postings && blocks == o.blocks && localTerms == o.localTerms; }
};
Seen seen(const string &base, const vector<string> &words) {
    Seen result;
    NindLexiconIndex lexicon(base, false);
    const NindIndex::Identification identification = lexicon.getIdentification();
    NindTermIndex terms(base, false, identification, 0);
    NindReadBuffer buffer;
    for (size_t w = 0; w < words.size(); w++) {
        const unsigned int ident = lexicon.getWordId(list<string>(1, words[w]));
        if (ident == 0) continue;
        list<NindTermIndex::TermCG> def;
        if (!terms.getTermDef(ident, def)) continue;
        for (list<NindTermIndex::Document>::const_iterator d = def.front().documents.begin(); d != def.front().documents.end(); d++)
            result.postings[words[w]][d->ident] = d->frequency;
        NindTermIndex::TermDirectory directory;
        terms.getTermDirectory(ident, directory, buffer);
        for (size_t b = 0; b < directory.categories[0].blocks.size(); b++)
            result.blocks[words[w]].push_back(make_pair(directory.categories[0].blocks[b].first,
                                                        directory.categories[0].blocks[b].minLength));
    }
    NindLocalIndex local(base, false, identification);
    vector<NindDocId> docs;
    local.getDocIdents(docs);
    for (size_t i = 0; i < docs.size(); i++) {
        list<NindLocalIndex::Term> def;
        local.getLocalDef(docs[i], def);
        size_t occurrences = 0;
        for (list<NindLocalIndex::Term>::const_iterator t = def.begin(); t != def.end(); t++) occurrences += t->localisation.size();
        result.localTerms[docs[i]] = occurrences;
    }
    return result;
}
}
////////////////////////////////////////////////////////////
TEST_CASE("NindIndexBuilder.SameAsAnUpdate") {
    TestTempDir tmp;
    vector<string> vocabulary;
    for (unsigned int w = 0; w < 120; w++) vocabulary.push_back("v" + to_string(w));
    //documents 100.. : Zipf-like word indexes
    uint64_t state = 3;
    vector<vector<uint32_t> > documents;
    for (unsigned int d = 0; d < 900; d++) {
        vector<uint32_t> words;
        const unsigned int length = 1 + next(state) % 50;
        for (unsigned int i = 0; i < length; i++) {
            const unsigned int rank = (unsigned int)(next(state) % 1000);
            words.push_back(rank * rank / 8400);           //0..118
        }
        documents.push_back(words);
    }
    //1) the builder: the lexicon first, then two batches of documents (local ids: the vocabulary's)
    const string built = tmp.file("built");
    {
        NindLexiconIndex lexicon(built, true, false, 256);
        vector<list<string> > words;
        for (size_t w = 0; w < vocabulary.size(); w++) words.push_back(list<string>(1, vocabulary[w]));
        vector<unsigned int> idents;
        lexicon.addWords(words, idents);
        const NindIndex::Identification identification = lexicon.getIdentification();
        NindIndexBuilder builder;
        vector<uint32_t> idMap(idents.begin(), idents.end());
        vector<uint32_t> ends, localIds;
        for (size_t batch = 0; batch < 2; batch++) {
            ends.clear();
            localIds.clear();
            for (size_t d = batch * 450; d < (batch + 1) * 450; d++) {
                localIds.insert(localIds.end(), documents[d].begin(), documents[d].end());
                ends.push_back((uint32_t)localIds.size());
            }
            builder.addDocuments(100 + batch * 450, ends.data(), ends.size(), localIds.data(), localIds.size(),
                                 idMap.data(), idMap.size());
        }
        CHECK_EQ(900u, builder.docsNb());
        NindTermIndex terms(built, true, identification, 0, 256);
        terms.beginBulk();
        builder.writeTermIndex(terms, identification);
        terms.endBulk();
        NindLocalIndex local(built, true, identification, 1024);
        builder.writeLocalIndex(local, identification);
        vector<NindDocId> docIds;
        vector<uint32_t> lengths;
        builder.docLengths(docIds, lengths);
        REQUIRE_EQ(900u, docIds.size());
        CHECK_EQ(100u, docIds.front());
        CHECK_EQ(documents[5].size(), lengths[5]);
        //bad input
        CHECK_THROWS_AS(builder.addDocuments(50, ends.data(), ends.size(), localIds.data(), localIds.size(), idMap.data(), idMap.size()),
                        std::invalid_argument);
        const uint32_t badEnds[] = {2, 1};
        CHECK_THROWS_AS(builder.addDocuments(5000, badEnds, 2, localIds.data(), 2, idMap.data(), idMap.size()), std::invalid_argument);
        const uint32_t badMap[] = {0};
        const uint32_t oneEnd[] = {1};
        const uint32_t zero[] = {0};
        CHECK_THROWS_AS(builder.addDocuments(5000, oneEnd, 1, zero, 1, badMap, 1), std::invalid_argument);
    }
    //2) the same documents through an update
    const string updated = tmp.file("updated");
    {
        NindIndexUpdate update(updated);
        for (size_t d = 0; d < documents.size(); d++) {
            vector<string> words;
            for (size_t i = 0; i < documents[d].size(); i++) words.push_back(vocabulary[documents[d][i]]);
            update.addDocument(100 + d, words);
        }
        update.commit();
    }
    CHECK(seen(built, vocabulary) == seen(updated, vocabulary));
    CHECK(!seen(built, vocabulary).blocks.empty());
    const NindIndexVerify::Report report = NindIndexVerify::verify(built, true);
    for (size_t i = 0; i < report.problems.size(); i++) MESSAGE(report.problems[i]);
    CHECK(report.ok());
}
////////////////////////////////////////////////////////////
TEST_CASE("NindIndexBuilder.PostingsBudgetWritesTheSameFile") {
    //Zipf-like documents, some with frequencies of 255 and more (kept apart), written a range of
    //terms at a time: each term alone, ranges of a few postings, all at once
    TestTempDir tmp;
    uint64_t state = 5;
    NindIndexBuilder builder;
    for (unsigned int d = 0; d < 700; d++) {
        vector<uint32_t> termIds;
        const unsigned int length = 1 + next(state) % 60;
        for (unsigned int i = 0; i < length; i++) {
            const unsigned int rank = (unsigned int)(next(state) % 1000);
            termIds.push_back(1 + rank * rank / 8400);          //1..119
        }
        if (d % 97 == 0) termIds.insert(termIds.end(), 300 + d, 1 + d % 119);
        builder.addDocument(40 + d, termIds.data(), termIds.size());
    }
    const NindIndex::Identification identification(1, 2);
    const size_t budgets[] = {0, 1, 5, 5 * 37, 5 * 1000, 1 << 30};
    vector<unsigned char> expected;
    for (size_t b = 0; b < sizeof(budgets) / sizeof(budgets[0]); b++) {
        CAPTURE(budgets[b]);
        builder.setPostingsBudget(budgets[b]);
        const string base = tmp.file("budget" + to_string(b));
        {
            NindTermIndex terms(base, true, identification, 0, 64);
            terms.beginBulk();
            builder.writeTermIndex(terms, identification);
            terms.endBulk();
        }
        const vector<unsigned char> bytes = readFileBytes(base + ".nindtermindex");
        if (b == 0) expected = bytes;
        else CHECK(bytes == expected);
    }
    CHECK(expected.size() > 1000);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindIndexBuilder.LargeFrequencies") {
    //frequencies around the 8-bit postings' limit (255 and more are kept apart)
    TestTempDir tmp;
    const vector<string> vocabulary = {"a", "b", "c"};
    const unsigned int repeats[] = {1, 254, 255, 256, 1000, 3};
    vector<vector<uint32_t> > documents;
    for (size_t d = 0; d < 6; d++) {
        vector<uint32_t> words(repeats[d], 0);                    //"a" repeats[d] times
        words.push_back(d % 2 == 0 ? 1 : 2);
        if (d == 4) words.insert(words.end(), 256, 2);            //"c" 256 times in the same document
        documents.push_back(words);
    }
    const string built = tmp.file("built");
    {
        NindLexiconIndex lexicon(built, true, false, 16);
        vector<list<string> > words;
        for (size_t w = 0; w < vocabulary.size(); w++) words.push_back(list<string>(1, vocabulary[w]));
        vector<unsigned int> idents;
        lexicon.addWords(words, idents);
        const NindIndex::Identification identification = lexicon.getIdentification();
        //one by one, ids 7, 10, 13... (not consecutive: looked up when writing)
        NindIndexBuilder builder;
        builder.reserve(2000, documents.size());
        for (size_t d = 0; d < documents.size(); d++) {
            vector<uint32_t> termIds;
            for (size_t i = 0; i < documents[d].size(); i++) termIds.push_back(idents[documents[d][i]]);
            builder.addDocument(7 + 3 * d, termIds.data(), termIds.size());
        }
        NindTermIndex terms(built, true, identification, 0, 16);
        terms.beginBulk();
        builder.writeTermIndex(terms, identification);
        terms.endBulk();
        NindLocalIndex local(built, true, identification, 16);
        builder.writeLocalIndex(local, identification);
    }
    const string updated = tmp.file("updated");
    {
        NindIndexUpdate update(updated);
        for (size_t d = 0; d < documents.size(); d++) {
            vector<string> words;
            for (size_t i = 0; i < documents[d].size(); i++) words.push_back(vocabulary[documents[d][i]]);
            update.addDocument(7 + 3 * d, words);
        }
        update.commit();
    }
    const Seen expected = seen(updated, vocabulary);
    CHECK(seen(built, vocabulary) == expected);
    CHECK_EQ(1000u, expected.postings.at("a").at(19));
    CHECK_EQ(256u, expected.postings.at("c").at(19));
    CHECK_EQ(255u, expected.postings.at("a").at(13));
    CHECK(NindIndexVerify::verify(built, true).ok());
}
////////////////////////////////////////////////////////////
TEST_CASE("NindIndexBuilder.Definitions") {
    const uint64_t docs[] = {9, 3, 5};
    const uint32_t freqs[] = {1, 2, 3};
    const list<NindTermIndex::TermCG> def = NindIndexBuilder::termDef(docs, freqs, 0, 3, 4);
    REQUIRE_EQ(1u, def.size());
    CHECK_EQ(4u, def.front().cg);
    CHECK_EQ(6u, def.front().frequency);
    CHECK_EQ(3u, def.front().documents.front().ident);          //sorted
    const uint64_t twice[] = {3, 3};
    CHECK_THROWS_AS(NindIndexBuilder::termDef(twice, freqs, 0, 2, 0), std::invalid_argument);
    vector<pair<uint64_t, uint32_t> > scratch;
    CHECK_THROWS_AS(NindIndexBuilder::checkTermDef(twice, freqs, 0, 2, scratch), std::invalid_argument);
    //a term 300 times: two local entries (255 + 45)
    vector<uint32_t> terms(300, 7), positions(300), lengths(300, 1);
    for (size_t i = 0; i < 300; i++) positions[i] = (uint32_t)i;
    const list<NindLocalIndex::Term> local = NindIndexBuilder::localDef(terms.data(), positions.data(), lengths.data(), 0, 300, 0);
    REQUIRE_EQ(2u, local.size());
    CHECK_EQ(255u, local.front().localisation.size());
    CHECK_EQ(45u, local.back().localisation.size());
}
////////////////////////////////////////////////////////////
TEST_CASE("NindIndexBuilder.BatchesWriteWhatLexiconIdsWrite") {
    // the same documents, as tokenizer batches (the builder numbers the words, writes the lexicon)
    // and with a lexicon written first (its ids): the same files
    TestTempDir tmp;
    uint64_t state = 21;
    vector<vector<string> > documents;
    for (unsigned int d = 0; d < 500; d++) {
        vector<string> words;
        const unsigned int length = 1 + next(state) % 30;
        for (unsigned int i = 0; i < length; i++) {
            const unsigned int rank = (unsigned int)(next(state) % 1000);
            words.push_back((rank % 7 == 0 ? "é" : "w") + to_string(rank * rank / 5000));
        }
        documents.push_back(words);
    }
    const string byIds = tmp.file("ids"), byBatches = tmp.file("batches");
    {
        set<string> distinct;
        for (size_t d = 0; d < documents.size(); d++) distinct.insert(documents[d].begin(), documents[d].end());
        const vector<string> vocabulary(distinct.begin(), distinct.end());
        NindLexiconIndex lexicon(byIds, true, false, (unsigned int)vocabulary.size());
        vector<list<string> > words;
        for (size_t w = 0; w < vocabulary.size(); w++) words.push_back(list<string>(1, vocabulary[w]));
        vector<unsigned int> idents;
        lexicon.beginBulk();
        lexicon.addWords(words, idents);
        lexicon.endBulk();
        map<string, uint32_t> identOf;
        for (size_t w = 0; w < vocabulary.size(); w++) identOf[vocabulary[w]] = idents[w];
        NindIndexBuilder builder;
        for (size_t d = 0; d < documents.size(); d++) {
            vector<uint32_t> termIds;
            for (size_t i = 0; i < documents[d].size(); i++) termIds.push_back(identOf[documents[d][i]]);
            builder.addDocument(10 + d, termIds.data(), termIds.size());
        }
        const NindIndex::Identification identification = lexicon.getIdentification();
        NindTermIndex terms(byIds, true, identification, 0, (unsigned int)vocabulary.size() + 1);
        terms.beginBulk();
        builder.writeTermIndex(terms, identification);
        terms.endBulk();
        NindLocalIndex local(byIds, true, identification, 600);
        local.beginBulk();
        builder.writeLocalIndex(local, identification);
        local.endBulk();
    }
    {
        NindIndexBuilder builder;
        for (size_t first = 0; first < documents.size(); first += 100) {
            string words;
            vector<uint32_t> wordEnds, localIds, ends;
            map<string, uint32_t> local;
            for (size_t d = first; d < first + 100; d++) {
                for (size_t i = 0; i < documents[d].size(); i++) {
                    const pair<map<string, uint32_t>::iterator, bool> found =
                        local.insert(make_pair(documents[d][i], (uint32_t)wordEnds.size()));
                    if (found.second) {
                        words += documents[d][i];
                        wordEnds.push_back((uint32_t)words.size());
                    }
                    localIds.push_back(found.first->second);
                }
                ends.push_back((uint32_t)localIds.size());
            }
            builder.addBatch(10 + first, ends.data(), ends.size(), localIds.data(), localIds.size(),
                             words.data(), words.size(), wordEnds.data(), wordEnds.size());
        }
        NindLexiconIndex lexicon(byBatches, true, false, (unsigned int)builder.wordsNb());
        lexicon.beginBulk();
        builder.writeLexicon(lexicon);
        lexicon.endBulk();
        const NindIndex::Identification identification = lexicon.getIdentification();
        NindTermIndex terms(byBatches, true, identification, 0, (unsigned int)builder.wordsNb() + 1);
        terms.beginBulk();
        builder.writeTermIndex(terms, identification);
        terms.endBulk();
        NindLocalIndex local(byBatches, true, identification, 600);
        local.beginBulk();
        builder.writeLocalIndex(local, identification);
        local.endBulk();
    }
    // misuses: lexicon ids after a batch, the term index before the lexicon
    {
        NindIndexBuilder builder;
        const uint32_t oneEnd[] = {1}, zero[] = {0};
        builder.addBatch(1, oneEnd, 1, zero, 1, "a", 1, oneEnd, 1);
        CHECK_THROWS_AS(builder.addDocuments(5000, oneEnd, 1, zero, 1, oneEnd, 1), std::invalid_argument);
        NindLexiconIndex lexicon(tmp.file("early"), true, false, 1);
        const NindIndex::Identification identification = lexicon.getIdentification();
        NindTermIndex terms(tmp.file("early"), true, identification, 0, 16);
        CHECK_THROWS_AS(builder.writeTermIndex(terms, identification), std::invalid_argument);
        const uint32_t longEnd[] = {255};
        CHECK_THROWS_AS(builder.addBatch(2, oneEnd, 1, zero, 1, string(255, 'x').data(), 255, longEnd, 1),
                        std::invalid_argument);
    }
    // the same bytes, but the identification's time
    for (const char *extension : {".nindlexiconindex", ".nindtermindex", ".nindlocalindex"}) {
        const vector<unsigned char> a = readFileBytes(byIds + extension), b = readFileBytes(byBatches + extension);
        REQUIRE_EQ(a.size(), b.size());
        CHECK(vector<unsigned char>(a.begin(), a.end() - 8) == vector<unsigned char>(b.begin(), b.end() - 8));
    }
}
////////////////////////////////////////////////////////////
