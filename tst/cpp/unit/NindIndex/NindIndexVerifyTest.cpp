// NindIndexVerify: sound indexes (golden v1 and v2, a consistent one) pass; damaged ones are
// reported, never crash nor throw.
////////////////////////////////////////////////////////////
#include "NindIndex/NindIndexVerify.h"
#include "NindIndex/NindLexiconIndex.h"
#include "NindIndex/NindTermIndex.h"
#include "NindIndex/NindLocalIndex.h"
#include "NindExceptions.h"
#include "TestTempDir.h"
#include "TestFileBytes.h"
#include "doctest.h"
#include <cstdio>
#include <list>
#include <string>
#include <vector>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
namespace {
typedef NindTermIndex::TermCG TermCG;
typedef NindTermIndex::Document Document;
typedef NindLocalIndex::Term Term;
typedef NindLocalIndex::Localisation Localisation;
const char *kExtensions[] = {".nindlexiconindex", ".nindretrolexicon", ".nindtermindex", ".nindlocalindex"};

string describe(const NindIndexVerify::Report &report) {
    string all;
    for (size_t i = 0; i < report.problems.size(); i++) all += report.problems[i] + "\n";
    return all;
}

// a consistent index: documents 0..docsNb-1, document d holds the words (d * 7 + k) % wordsNb, k < 1 + d % 5,
// word w at each of its places; long lists (blocks) for the first words
void buildConsistent(const string &base, const unsigned int wordsNb, const unsigned int docsNb) {
    NindIndex::Identification identification;
    vector<unsigned int> ids;
    {
        NindLexiconIndex lexicon(base, true, false, 64, 0);
        for (unsigned int w = 0; w < wordsNb; w++) ids.push_back(lexicon.addWord(list<string>(1, "w" + to_string(w))));
        identification = lexicon.getIdentification();
    }
    vector<list<TermCG> > termDefs(wordsNb);
    NindLocalIndex local(base, true, identification, 64);
    for (unsigned int d = 0; d < docsNb; d++) {
        list<Term> localDef;
        vector<unsigned int> counts(wordsNb, 0);
        unsigned int position = 0;
        //w0 in every document (a long list), then a few others
        localDef.push_back(Term(ids[0], 0));
        localDef.back().localisation.push_back(Localisation(position++, 1));
        counts[0]++;
        for (unsigned int k = 0; k < 1 + d % 5; k++) {
            const unsigned int w = (d * 7 + k) % wordsNb;
            localDef.push_back(Term(ids[w], 0));
            localDef.back().localisation.push_back(Localisation(position++, 1));
            counts[w]++;
        }
        local.setLocalDef(d, localDef, identification);
        for (unsigned int w = 0; w < wordsNb; w++) {
            if (counts[w] == 0) continue;
            if (termDefs[w].empty()) termDefs[w].push_back(TermCG(0, 0));
            termDefs[w].back().documents.push_back(Document(d, counts[w]));
            termDefs[w].back().frequency += counts[w];
        }
    }
    NindTermIndex terms(base, true, identification, 0, 64);
    for (unsigned int w = 0; w < wordsNb; w++) terms.setTermDef(ids[w], termDefs[w], identification, list<unsigned int>());
}
}
////////////////////////////////////////////////////////////
TEST_CASE("NindIndexVerify.GoldenFiles") {
    for (const char *version : {"v1", "v2"}) {
        INFO("golden " << string(version));
        TestTempDir tmp;
        const string base = tmp.file("golden");
        for (size_t e = 0; e < 4; e++)
            writeFileBytes(base + kExtensions[e], readFileBytes(string(NIND_GOLDEN_DIR) + "/" + version + "/golden" + kExtensions[e]));
        //(the golden term and local indexes aren't each other's: no cross check)
        const NindIndexVerify::Report report = NindIndexVerify::verify(base);
        INFO(describe(report));
        CHECK(report.ok());
        CHECK_EQ(string(version) == "v2", report.formatV2);
        CHECK_EQ(15u, report.wordsNb);          //10 words, 5 prefixes of compound words
        CHECK_EQ(9u, report.termsNb);
        CHECK(report.withLocalIndex);
        CHECK_EQ(4u, report.documentsNb);
    }
}
////////////////////////////////////////////////////////////
TEST_CASE("NindIndexVerify.CrossCheck") {
    TestTempDir tmp;
    const string base = tmp.file("consistent");
    buildConsistent(base, 40, 300);
    NindIndexVerify::Report report = NindIndexVerify::verify(base, true);
    INFO(describe(report));
    CHECK(report.ok());
    CHECK_EQ(300u, report.documentsNb);
    CHECK_EQ(40u, report.termsNb);
    //one document's frequency changed in the term index only: both triples reported
    {
        NindLexiconIndex lexicon(base, false);
        const NindIndex::Identification identification = lexicon.getIdentification();
        NindTermIndex terms(base, true, identification, 0, 64);
        list<TermCG> def;
        REQUIRE(terms.getTermDef(5, def));
        def.front().documents.front().frequency += 1;
        def.front().frequency += 1;
        terms.setTermDef(5, def, identification, list<unsigned int>());
    }
    report = NindIndexVerify::verify(base, true);
    CHECK_EQ(2u, report.problemsNb);
    CHECK(NindIndexVerify::verify(base, false).ok());
}
////////////////////////////////////////////////////////////
TEST_CASE("NindIndexVerify.DamagedFiles") {
    // bytes of the term index (with blocks) and of the local index, corrupted in turn (every byte
    // of the headers, a sample after: kept short enough for TSan builds): every
    // verification returns (under ASan: no memory error either), and finds something in some
    TestTempDir tmp;
    const string base = tmp.file("consistent");
    buildConsistent(base, 40, 300);
    const string damagedBase = tmp.file("damaged");
    writeFileBytes(damagedBase + ".nindlexiconindex", readFileBytes(base + ".nindlexiconindex"));
    for (const char *extension : {".nindtermindex", ".nindlocalindex"}) {
        const vector<unsigned char> bytes = readFileBytes(base + extension);
        writeFileBytes(damagedBase + ".nindtermindex", readFileBytes(base + ".nindtermindex"));
        writeFileBytes(damagedBase + ".nindlocalindex", readFileBytes(base + ".nindlocalindex"));
        unsigned int variants = 0, reported = 0;
        for (size_t i = 0; i < bytes.size(); i += (i < 512 ? 1 : 13)) {
            for (unsigned char mask : {0xFFu, 0x01u}) {
                vector<unsigned char> damaged = bytes;
                damaged[i] ^= mask;
                remove((damagedBase + extension).c_str());
                writeFileBytes(damagedBase + extension, damaged);
                variants++;
                const NindIndexVerify::Report report = NindIndexVerify::verify(damagedBase, true, 3);
                if (!report.ok()) reported++;
                CHECK(report.problems.size() <= 3);
            }
        }
        MESSAGE(string(extension) << ": " << variants << " damaged variants, " << reported << " reported");
        CHECK(reported > variants / 2);
    }
}
////////////////////////////////////////////////////////////
TEST_CASE("NindIndexVerify.MissingFiles") {
    TestTempDir tmp;
    const NindIndexVerify::Report report = NindIndexVerify::verify(tmp.file("nothing"));
    CHECK_FALSE(report.ok());
    REQUIRE_EQ(1u, report.problems.size());
    CHECK(report.problems[0].find("lexicon") == 0);
}
////////////////////////////////////////////////////////////
