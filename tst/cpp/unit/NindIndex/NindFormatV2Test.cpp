// Format v2: what v1 couldn't hold (64-bit external document ids), and v1 files: read, but a
// writer refuses to extend one (rebuild it).
////////////////////////////////////////////////////////////
#include "NindIndex/NindLexiconIndex.h"
#include "NindIndex/NindTermIndex.h"
#include "NindIndex/NindLocalIndex.h"
#include "NindBasics/NindReadBuffer.h"
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
////////////////////////////////////////////////////////////
namespace {
// a category of docsNb documents, sparse 64-bit ids, varied frequencies
TermCG category(const unsigned char cg, const unsigned int docsNb, const NindDocId start) {
    TermCG termCG(cg, 0);
    NindDocId ident = start;
    for (unsigned int i = 0; i < docsNb; i++) {
        ident += 1 + (i * 7919u) % 100000;
        const unsigned int frequency = 1 + (i * 13u) % 300;
        termCG.documents.push_back(Document(ident, frequency));
        termCG.frequency += frequency;
    }
    return termCG;
}
bool sameDefinitions(const list<TermCG> &a, const list<TermCG> &b) {
    if (a.size() != b.size()) return false;
    for (list<TermCG>::const_iterator x = a.begin(), y = b.begin(); x != a.end(); x++, y++) {
        if (x->cg != y->cg || x->frequency != y->frequency || x->documents.size() != y->documents.size()) return false;
        for (list<Document>::const_iterator dx = x->documents.begin(), dy = y->documents.begin(); dx != x->documents.end(); dx++, dy++)
            if (dx->ident != dy->ident || dx->frequency != dy->frequency) return false;
    }
    return true;
}
// terms 1..: one category of each size, then a term mixing blocks and inline categories
vector<list<TermCG> > blockedDefinitions() {
    const unsigned int sizes[] = {1, 127, 128, 129, 256, 300, 1000};
    vector<list<TermCG> > defs;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        defs.push_back(list<TermCG>());
        defs.back().push_back(category(0, sizes[i], 0x100000000ull * i));
    }
    defs.push_back(list<TermCG>());
    defs.back().push_back(category(1, 5, 3));                   //inline
    defs.back().push_back(category(2, 400, 0x7000000000ull));   //blocks
    defs.back().push_back(category(3, 2, 9));                   //inline
    defs.back().push_back(category(4, 129, 0));                 //blocks
    return defs;
}
void writeBlocked(const string &base) {
    NindTermIndex terms(base, true, kNoCheck, 0, 4);
    const vector<list<TermCG> > defs = blockedDefinitions();
    for (size_t i = 0; i < defs.size(); i++) terms.setTermDef(i + 1, defs[i], kNoCheck, list<unsigned int>());
}
}
////////////////////////////////////////////////////////////
TEST_CASE("NindFormatV2.BlocksRoundTrip") {
    // lists over BLOCK_SIZE documents are cut into blocks after the directory; both read paths
    TestTempDir tmp;
    const string base = tmp.file("blocks");
    writeBlocked(base);
    const vector<list<TermCG> > defs = blockedDefinitions();
    NindTermIndex terms(base, false, kNoCheck, 0);
    NindReadBuffer buffer;
    for (size_t i = 0; i < defs.size(); i++) {
        INFO("term " << i + 1);
        list<TermCG> read;
        REQUIRE(terms.getTermDef(i + 1, read));
        CHECK(sameDefinitions(defs[i], read));
        list<TermCG> readConst;
        REQUIRE(terms.getTermDef(i + 1, readConst, buffer));
        CHECK(sameDefinitions(defs[i], readConst));
        vector<Document> postings;
        vector<size_t> ends;
        REQUIRE(terms.getTermPostings(i + 1, postings, ends, buffer));
        size_t n = 0, c = 0;
        for (list<TermCG>::const_iterator cg = defs[i].begin(); cg != defs[i].end(); cg++, c++) {
            for (list<Document>::const_iterator d = cg->documents.begin(); d != cg->documents.end(); d++, n++) {
                REQUIRE(n < postings.size());
                CHECK_EQ(d->ident, postings[n].ident);
            }
            REQUIRE(c < ends.size());
            CHECK_EQ(n, ends[c]);
        }
    }
    // rewritten with another size (moved or in place), still read
    {
        NindTermIndex writer(base, true, kNoCheck, 0, 4);
        writer.setTermDef(6, defs[0], kNoCheck, list<unsigned int>());
        writer.setTermDef(1, defs[6], kNoCheck, list<unsigned int>());
    }
    NindTermIndex reread(base, false, kNoCheck, 0);
    list<TermCG> read;
    REQUIRE(reread.getTermDef(1, read));
    CHECK(sameDefinitions(defs[6], read));
    read.clear();
    REQUIRE(reread.getTermDef(6, read));
    CHECK(sameDefinitions(defs[0], read));
}
////////////////////////////////////////////////////////////
TEST_CASE("NindFormatV2.CorruptBlocks") {
    // bytes of a term index with blocks, corrupted in turn: each read either succeeds or throws
    // a FileException (run under ASan: no memory error either)
    TestTempDir tmp;
    const string base = tmp.file("blocks");
    writeBlocked(base);
    const vector<unsigned char> bytes = readFileBytes(base + ".nindtermindex");
    const string corrupt = tmp.file("corrupt");
    unsigned int rejected = 0, variants = 0;
    // every byte of the first 4 KiB (header, directories, first blocks), every 7th after
    for (size_t i = 0; i < bytes.size(); i += (i < 4096 ? 1 : 7)) {
        for (unsigned char mask : {0xFFu, 0x01u, 0x80u}) {
            vector<unsigned char> damaged = bytes;
            damaged[i] ^= mask;
            remove((corrupt + ".nindtermindex").c_str());
            writeFileBytes(corrupt + ".nindtermindex", damaged);
            variants++;
            try {
                NindTermIndex terms(corrupt, false, kNoCheck, 0);
                NindReadBuffer buffer;
                vector<Document> postings;
                vector<size_t> ends;
                for (unsigned int ident = 0; ident < 12; ident++) {
                    list<TermCG> def;
                    terms.getTermDef(ident, def);
                    terms.getTermPostings(ident, postings, ends, buffer);
                }
            }
            catch (const FileException &) { rejected++; }
        }
    }
    MESSAGE(variants << " corrupted variants, " << rejected << " rejected");
    CHECK(rejected > 0);
}
