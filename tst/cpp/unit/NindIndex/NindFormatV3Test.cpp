// Format v3 (docs/proposals/format-v3.md): a term of one posting in its indirection, changed into a
// definition and back, in normal and bulk modes, through every read path; the impacts' lengths
// rounded down never above the blocks' shortest documents.
////////////////////////////////////////////////////////////
#include "NindIndex/NindTermIndex.h"
#include "NindIndex/NindIndexVerify.h"
#include "NindIndex/NindLexiconIndex.h"
#include "NindExceptions.h"
#include "TestTempDir.h"
#include "doctest.h"
#include <list>
#include <string>
#include <vector>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
namespace {
typedef NindTermIndex::Document Document;
typedef NindTermIndex::TermCG TermCG;
const NindIndex::Identification kNoCheck(0, 0);

list<TermCG> definition(const vector<Document> &documents) {
    list<TermCG> termDef;
    unsigned int total = 0;
    for (size_t d = 0; d < documents.size(); d++) total += documents[d].frequency;
    termDef.push_back(TermCG(0, total));
    termDef.back().documents.assign(documents.begin(), documents.end());
    return termDef;
}
// the term's postings through every read path (sequential, const, directory): all the same
vector<Document> readEveryWay(NindTermIndex &terms, const unsigned int ident, const bool writer) {
    vector<Document> postings;
    vector<size_t> ends;
    const bool found = terms.getTermPostings(ident, postings, ends);
    list<TermCG> termDef;
    CHECK_EQ(found, terms.getTermDef(ident, termDef));
    NindTermIndex::TermDirectory directory;
    CHECK_EQ(found, terms.getTermDirectory(ident, directory));
    if (!writer) {
        NindReadBuffer buffer;
        vector<Document> constPostings;
        vector<size_t> constEnds;
        CHECK_EQ(found, terms.getTermPostings(ident, constPostings, constEnds, buffer));
        CHECK(constPostings.size() == postings.size());
        NindTermIndex::TermDirectory constDirectory;
        CHECK_EQ(found, terms.getTermDirectory(ident, constDirectory, buffer));
        if (found) CHECK_EQ(directory.categories[0].docsNb, constDirectory.categories[0].docsNb);
    }
    if (found) {
        REQUIRE_EQ(1u, termDef.size());
        CHECK_EQ(postings.size(), termDef.front().documents.size());
        CHECK_EQ(postings.size(), directory.categories[0].docsNb);
    }
    return postings;
}
}
////////////////////////////////////////////////////////////
TEST_CASE("NindFormatV3.SingletonBecomesADefinitionAndBack") {
    for (const bool bulk : {false, true}) {
        CAPTURE(bulk);
        TestTempDir tmp;
        const string base = tmp.file("terms");
        const vector<Document> one = {Document(77, 3)};
        const vector<Document> three = {Document(5, 1), Document(9, 2), Document(4000000000ull, 1)};
        const vector<Document> another = {Document((1ull << 48) + 5, 2)};     //(over 48 bits: a definition)
        {
            NindTermIndex terms(base, true, kNoCheck, 0, 8);
            REQUIRE(terms.isFormatV3());
            if (bulk) terms.beginBulk();
            terms.setTermDef(1, definition(one), kNoCheck, list<unsigned int>());
            terms.setTermDef(2, definition(three), kNoCheck, list<unsigned int>());
            terms.setTermDef(3, definition(another), kNoCheck, list<unsigned int>());
            CHECK_EQ(1u, readEveryWay(terms, 1, true).size());
            //one posting -> a definition -> one posting again -> erased
            terms.setTermDef(1, definition(three), kNoCheck, list<unsigned int>());
            CHECK_EQ(3u, readEveryWay(terms, 1, true).size());
            terms.setTermDef(2, definition(one), kNoCheck, list<unsigned int>());
            CHECK_EQ(1u, readEveryWay(terms, 2, true).size());
            terms.setTermDef(4, definition(one), kNoCheck, list<unsigned int>());
            terms.setTermDef(4, list<TermCG>(), kNoCheck, list<unsigned int>());
            if (bulk) terms.endBulk();
        }
        NindTermIndex reader(base, false, kNoCheck, 0);
        REQUIRE(reader.isFormatV3());
        const vector<Document> first = readEveryWay(reader, 1, false);
        REQUIRE_EQ(3u, first.size());
        CHECK_EQ(4000000000ull, first[2].ident);
        const vector<Document> second = readEveryWay(reader, 2, false);
        REQUIRE_EQ(1u, second.size());
        CHECK_EQ(77u, second[0].ident);
        CHECK_EQ(3u, second[0].frequency);
        const vector<Document> third = readEveryWay(reader, 3, false);
        REQUIRE_EQ(1u, third.size());
        CHECK_EQ((1ull << 48) + 5, third[0].ident);
        CHECK(readEveryWay(reader, 4, false).empty());
        //the areas freed and taken stay consistent (the free-space map, holes)
        const NindIndex::IndexStats stats = reader.analyseIndex();
        CHECK_EQ(2u, stats.usedCount);          //terms 1 and 3 (2: a singleton, no area)
    }
}
////////////////////////////////////////////////////////////
TEST_CASE("NindFormatV3.ImpactsLengthsRoundedDownStayBounds") {
    //blocks of documents of lengths 1 to 300000: each block's shortest length, as the impacts give it
    //(rounded down on a byte), at most its true shortest, its largest frequency exact
    TestTempDir tmp;
    const string base = tmp.file("impacts");
    vector<pair<NindDocId, unsigned int> > lengths;
    vector<Document> documents;
    uint64_t state = 3;
    for (unsigned int d = 0; d < 2000; d++) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        const unsigned int length = 1 + (unsigned int)((state >> 33) % (d < 1000 ? 200 : 300000));
        lengths.push_back(make_pair((NindDocId)(10 + d), length));
        documents.push_back(Document(10 + d, 1 + (unsigned int)((state >> 20) % 9)));
    }
    {
        NindTermIndex terms(base, true, kNoCheck, 0, 8);
        terms.setDocumentLengths(lengths);
        terms.setTermDef(1, definition(documents), kNoCheck, list<unsigned int>());
    }
    NindTermIndex reader(base, false, kNoCheck, 0);
    NindTermIndex::TermDirectory directory;
    REQUIRE(reader.getTermDirectory(1, directory));
    const vector<NindTermIndex::Block> &blocks = directory.categories[0].blocks;
    REQUIRE(blocks.size() == (2000 + NindTermIndex::BLOCK_SIZE - 1) / NindTermIndex::BLOCK_SIZE);
    size_t d = 0;
    for (size_t b = 0; b < blocks.size(); b++) {
        unsigned int shortest = ~0u, largest = 0;
        for (size_t i = 0; i < blocks[b].docsNb; i++, d++) {
            shortest = min(shortest, lengths[d].second);
            largest = max(largest, documents[d].frequency);
        }
        CAPTURE(b);
        REQUIRE(blocks[b].impactsNb > 0);
        CHECK(blocks[b].minLength <= shortest);
        CHECK(blocks[b].minLength * 116 / 100 + 1 >= shortest);      //(within a step of ~16%)
        CHECK_EQ(largest, blocks[b].maxFrequency);
        for (unsigned int i = 0; i < blocks[b].impactsNb; i++) CHECK(blocks[b].impacts[i].first <= largest);
    }
    vector<Document> postings;
    vector<size_t> ends;
    REQUIRE(reader.getTermPostings(1, postings, ends));
    CHECK_EQ(documents.size(), postings.size());
}
////////////////////////////////////////////////////////////
