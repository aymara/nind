// Format v2: what v1 couldn't hold (64-bit external document ids), and v1 files: read, but a
// writer refuses to extend one (rebuild it).
////////////////////////////////////////////////////////////
#include "NindIndex/NindLexiconIndex.h"
#include "NindIndex/NindTermIndex.h"
#include "NindIndex/NindLocalIndex.h"
#include "NindExceptions.h"
#include "TestTempDir.h"
#include "TestFileBytes.h"
#include "doctest.h"
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
const NindIndex::Identification kNoCheck(0, 0);
const char *kExtensions[] = {".nindlexiconindex", ".nindretrolexicon", ".nindtermindex", ".nindlocalindex"};
}
////////////////////////////////////////////////////////////
TEST_CASE("NindFormatV2.ExternalDocumentIdsOver32Bits") {
    TestTempDir tmp;
    const string base = tmp.file("big");
    const NindDocId ids[] = {7, 0x100000000ull, 0x123456789ABCDEFull, 0xFFFFFFFFFFFFFFFFull};
    {
        NindTermIndex terms(base, true, kNoCheck, 0, 8);
        list<TermCG> def;
        def.push_back(TermCG(0, 4));
        for (size_t i = 0; i < 4; i++) def.back().documents.push_back(Document(ids[i], 1));
        terms.setTermDef(1, def, kNoCheck, list<unsigned int>());
        NindLocalIndex local(base, true, kNoCheck, 8);
        for (size_t i = 0; i < 4; i++) {
            list<Term> localDef;
            localDef.push_back(Term(1, 0));
            localDef.back().localisation.push_back(Localisation((unsigned int)i, 3));
            local.setLocalDef(ids[i], localDef, kNoCheck);
        }
    }
    NindTermIndex terms(base, false, kNoCheck, 0);
    list<TermCG> read;
    REQUIRE(terms.getTermDef(1, read));
    REQUIRE_EQ(4u, read.front().documents.size());
    size_t i = 0;
    for (list<Document>::const_iterator it = read.front().documents.begin(); it != read.front().documents.end(); it++, i++)
        CHECK_EQ(ids[i], it->ident);
    NindLocalIndex local(base, false, kNoCheck);
    vector<NindDocId> docIdents;
    local.getDocIdents(docIdents);
    CHECK(docIdents == vector<NindDocId>(ids, ids + 4));
    for (size_t d = 0; d < 4; d++) {
        INFO("document " << ids[d]);
        list<Term> localDef;
        REQUIRE(local.getLocalDef(ids[d], localDef));
        CHECK_EQ(d, localDef.front().localisation.front().position);
    }
}
////////////////////////////////////////////////////////////
TEST_CASE("NindFormatV2.AWriterRefusesToExtendAV1File") {
    // v1 files (written by older nind) are read; extending one would mix formats: rebuild it
    TestTempDir tmp;
    const string base = tmp.file("golden");
    for (size_t e = 0; e < 4; e++)
        writeFileBytes(base + kExtensions[e], readFileBytes(string(NIND_GOLDEN_DIR) + "/v1/golden" + kExtensions[e]));
    NindLexiconIndex reader(base, false, true);
    const NindIndex::Identification identification = reader.getIdentification();
    CHECK_THROWS_WITH_AS(NindTermIndex(base, true, identification, 2), doctest::Contains("format v1 file"),
                         NindPadFileException);
    CHECK_THROWS_WITH_AS(NindLocalIndex(base, true, identification), doctest::Contains("format v1 file"),
                         NindPadFileException);
    // and the files are untouched: still readable
    NindTermIndex terms(base, false, identification, 2);
    list<TermCG> def;
    CHECK(terms.getTermDef(1, def));
}
