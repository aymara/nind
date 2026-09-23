//
// Robustness / memory-safety regression tests for the index files:
// - specific bugs (leaked retro lexicon, runaway id-translation loop,
//   silently-truncated localisation count, critical section left open);
// - a corruption sweep: small valid lexicon/retro-lexicon/term/local index
//   files are damaged one byte at a time (and truncated at every length), then
//   opened and read through every read API. Each operation must either succeed
//   or throw a FileException-derived exception: no crash, no out-of-bounds
//   access, no leak (the suite is meant to run under ASan/UBSan/LSan), no hang,
//   no other exception type (e.g. std::bad_alloc from a corrupt length).
////////////////////////////////////////////////////////////
#include "NindIndex/NindLexiconIndex.h"
#include "NindIndex/NindTermIndex.h"
#include "NindIndex/NindLocalIndex.h"
#include "NindRetrolexicon/NindRetrolexicon.h"
#include "NindBasics/NindSignalCatcher.h"
#include "NindExceptions.h"
#include "TestTempDir.h"
#include "TestFileBytes.h"
#include "doctest.h"
#include <dirent.h>
#include <exception>
#include <functional>
#include <list>
#include <set>
#include <sstream>
#include <string>
#include <vector>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
namespace {
typedef NindLocalIndex::Term Term;
typedef NindLocalIndex::Localisation Localisation;
typedef NindTermIndex::TermCG TermCG;
typedef NindTermIndex::Document Document;
const NindIndex::Identification kNoCheck(0, 0);

list<string> words(const string &a) { return list<string>(1, a); }
list<string> words(const string &a, const string &b) { list<string> l(1, a); l.push_back(b); return l; }
list<string> words(const string &a, const string &b, const string &c) { list<string> l = words(a, b); l.push_back(c); return l; }

int openFileDescriptors() {
    int count = 0;
    DIR *d = opendir("/proc/self/fd");
    if (!d) return -1;
    while (readdir(d) != NULL) count++;
    closedir(d);
    return count;
}

Term term(const unsigned int id, const unsigned char cg, const unsigned int locs) {
    Term t(id, cg);
    for (unsigned int i = 0; i < locs; i++) t.localisation.push_back(Localisation(10 * i, 3));
    return t;
}
}
////////////////////////////////////////////////////////////
TEST_CASE("NindIndexRobustness.LexiconIndexReleasesItsRetrolexicon") {
    TestTempDir tmp;
    {
        NindLexiconIndex writer(tmp.file("lex"), true, true, 8, 8);
        writer.addWord(words("alpha"));
        writer.addWord(words("alpha", "beta"));
    }
    // every NindLexiconIndex used to leak its NindRetrolexicon (and its open FILE*)
    const int before = openFileDescriptors();
    for (int i = 0; i < 50; i++) {
        NindLexiconIndex reader(tmp.file("lex"), false, true);
        list<string> components;
        CHECK(reader.getComponents(2, components));
    }
    CHECK_EQ(before, openFileDescriptors());
}
////////////////////////////////////////////////////////////
TEST_CASE("NindIndexRobustness.RetrolexiconRejectsCompoundWhoseSimplePartIsCompound") {
    TestTempDir tmp;
    NindRetrolexicon retro(tmp.file("retro"), true, kNoCheck, 8);
    list<NindRetrolexicon::RetroWord> w;
    w.push_back(NindRetrolexicon::RetroWord(1, "alpha"));
    w.push_back(NindRetrolexicon::RetroWord(2, "beta"));
    w.push_back(NindRetrolexicon::RetroWord(3, 1, 2));   // alpha_beta
    w.push_back(NindRetrolexicon::RetroWord(4, 1, 3));   // invalid: identS (3) is not a simple word
    retro.addRetroWords(w, kNoCheck);
    list<string> components;
    CHECK(retro.getComponents(3, components));
    // the check existed but its exception was built and never thrown
    CHECK_THROWS_AS(retro.getComponents(4, components), NindRetrolexiconException);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindIndexRobustness.LocalIndexWithHugeMaxInternalIdOpensPromptly") {
    TestTempDir tmp;
    {
        NindLocalIndex writer(tmp.file("local"), true, kNoCheck, 8);
        list<Term> doc(1, term(5, 1, 2));
        writer.setLocalDef(100, doc, kNoCheck);
        writer.setLocalDef(200, doc, kNoCheck);
    }
    // <spejcifiques> = <maxIdentifiantInterne>(4) <nombreDocuments>(4), followed by the 9-bytes identification
    const string path = tmp.file("local") + ".nindlocalindex";
    vector<unsigned char> bytes = readFileBytes(path);
    const size_t maxIdentOffset = bytes.size() - 9 - 8;
    // 0xFFFFFFFF + 1 used to wrap to 0, so the id loop ran ~4 billion times (and 0 did the same)
    const unsigned int hostile[] = { 0xFFFFFFFFu, 0xFFFFFFFEu, 0u };
    for (unsigned int value : hostile) {
        patchInt4(bytes, maxIdentOffset, value);
        writeFileBytes(path, bytes);
        NindLocalIndex reader(tmp.file("local"), false, kNoCheck);
        list<Term> read;
        INFO("maxIdentifiantInterne=" << value);
        CHECK_EQ(value != 0, reader.getLocalDef(100, read));
        CHECK_FALSE(reader.getLocalDef(300, read));
    }
}
////////////////////////////////////////////////////////////
TEST_CASE("NindIndexRobustness.LocalIndexRefusesMoreThan255LocalisationsPerTerm") {
    TestTempDir tmp;
    NindLocalIndex index(tmp.file("local"), true, kNoCheck, 8);
    list<Term> ok(1, term(7, 1, 255));
    index.setLocalDef(1, ok, kNoCheck);

    // <nbreLocalisations> is one byte: 256 used to be written as 0 followed by
    // 256 localisations, making the whole document unreadable
    list<Term> tooMany(1, term(7, 1, 256));
    CHECK_THROWS_AS(index.setLocalDef(2, tooMany, kNoCheck), NindLocalIndexException);
    CHECK_FALSE(NindSignalCatcher::isUp());
    CHECK_EQ(1u, index.getDocCount());
    list<Term> read;
    CHECK_FALSE(index.getLocalDef(2, read));
    REQUIRE(index.getLocalDef(1, read));
    REQUIRE_EQ(1u, read.size());
    CHECK_EQ(255u, read.front().localisation.size());

    // a later document still gets written and read back correctly
    index.setLocalDef(3, list<Term>(1, term(9, 2, 3)), kNoCheck);
    REQUIRE(index.getLocalDef(3, read));
    CHECK_EQ(3u, read.front().localisation.size());
}
////////////////////////////////////////////////////////////
TEST_CASE("NindIndexRobustness.LocalIndexSaturatesLengthsAbove255") {
    TestTempDir tmp;
    NindLocalIndex index(tmp.file("local"), true, kNoCheck, 8);
    Term t(7, 1);
    t.localisation.push_back(Localisation(0, 300));   // used to be stored as 300 % 256 = 44
    t.localisation.push_back(Localisation(5, 12));
    index.setLocalDef(1, list<Term>(1, t), kNoCheck);
    list<Term> read;
    REQUIRE(index.getLocalDef(1, read));
    REQUIRE_EQ(2u, read.front().localisation.size());
    CHECK_EQ(255u, read.front().localisation.front().length);
    CHECK_EQ(12u, read.front().localisation.back().length);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindIndexRobustness.WriteFailureDoesNotLeaveCriticalSectionOpen") {
    TestTempDir tmp;
    NindLocalIndex local(tmp.file("local"), true, kNoCheck, 8);
    CHECK_THROWS(local.setLocalDef(1, list<Term>(1, term(7, 1, 300)), kNoCheck));
    CHECK_FALSE(NindSignalCatcher::isUp());
    // deleting an unknown document returns early from inside the critical section
    local.setLocalDef(42, list<Term>(), kNoCheck);
    CHECK_FALSE(NindSignalCatcher::isUp());

    NindLexiconIndex lexicon(tmp.file("lex"), true, false, 8);
    // a simple word longer than 254 bytes cannot be stored (<longueur> is one byte)
    CHECK_THROWS(lexicon.addWord(words(string(300, 'x'))));
    CHECK_FALSE(NindSignalCatcher::isUp());
}
////////////////////////////////////////////////////////////
// Corruption sweep
////////////////////////////////////////////////////////////
namespace {
struct SweepFile {
    string path;                                // file to corrupt
    function<void()> exercise;                  // opens the index(es) read-only and reads everything
};

struct SweepResult {
    string failures;        // variants that ended with something else than success or a FileException
    int variants;
    int detected;           // variants rejected with a FileException
    SweepResult(): failures(), variants(0), detected(0) {}
};

// Runs `exercise` once per corrupted variant of `file`.
SweepResult sweep(const SweepFile &file) {
    const vector<unsigned char> original = readFileBytes(file.path);
    SweepResult result;
    ostringstream failures;
    int failuresNb = 0;
    const auto attempt = [&](const vector<unsigned char> &bytes, const string &what) {
        writeFileBytes(file.path, bytes);
        result.variants++;
        try {
            file.exercise();
        }
        catch (const FileException &) { result.detected++; }   // expected: corrupt input detected
        catch (const std::exception &e) {
            if (failuresNb++ < 10) failures << what << ": " << typeid(e).name() << " " << e.what() << "\n";
        }
        catch (...) {
            if (failuresNb++ < 10) failures << what << ": non-std exception\n";
        }
    };
    const unsigned char patterns[] = { 0x00, 0xFF, 0x80, 0x7F };
    for (size_t offset = 0; offset < original.size(); offset++) {
        for (unsigned char pattern : patterns) {
            vector<unsigned char> bytes = original;
            if (bytes[offset] == pattern) continue;
            bytes[offset] = pattern;
            ostringstream what;
            what << "byte " << offset << " := " << int(pattern);
            attempt(bytes, what.str());
        }
        vector<unsigned char> flipped = original;
        flipped[offset] ^= 0x01;
        ostringstream what;
        what << "byte " << offset << " ^= 1";
        attempt(flipped, what.str());
    }
    for (size_t length = 0; length < original.size(); length++) {
        ostringstream what;
        what << "truncated to " << length;
        attempt(vector<unsigned char>(original.begin(), original.begin() + length), what.str());
    }
    writeFileBytes(file.path, original);
    result.failures = failures.str();
    return result;
}

void checkSweep(const SweepFile &file) {
    const SweepResult result = sweep(file);
    MESSAGE(file.path.substr(file.path.rfind('/') + 1) << ": " << result.variants << " corrupted variants, "
            << result.detected << " rejected with a FileException, the others read without error");
    CHECK_EQ("", result.failures);
    CHECK_GT(result.variants, 500);
    CHECK_GT(result.detected, result.variants / 4);
}

void buildLexicon(const string &base) {
    NindLexiconIndex lexicon(base, true, true, 4, 4);   // small blocks: several indirection blocks
    lexicon.addWord(words("alpha"));
    lexicon.addWord(words("beta"));
    lexicon.addWord(words("alpha", "beta"));
    lexicon.addWord(words("alpha", "beta", "gamma"));
    lexicon.addWord(words(string(40, 'l')));
    lexicon.addWord(words("delta"));
}
void readLexicon(const string &base) {
    NindLexiconIndex lexicon(base, false, true);
    lexicon.getWordId(words("alpha"));
    lexicon.getWordId(words("alpha", "beta", "gamma"));
    lexicon.getWordId(words("delta"));
    lexicon.getWordId(words("unknown"));
    list<string> components;
    for (unsigned int id = 0; id < 12; id++) lexicon.getComponents(id, components);
    lexicon.analyseIndex();
    lexicon.analysePadFile();
}
void buildTermIndex(const string &base) {
    NindTermIndex index(base, true, kNoCheck, 2, 4);
    list<unsigned int> specifics;
    specifics.push_back(3);
    specifics.push_back(300000);
    for (unsigned int id = 1; id <= 6; id++) {
        list<TermCG> def;
        def.push_back(TermCG(1, 3 * id));
        def.back().documents.push_back(Document(id, 1));
        def.back().documents.push_back(Document(1000 * id, 200));
        def.push_back(TermCG(2, 1));
        def.back().documents.push_back(Document(70000 * id, 1));
        index.setTermDef(id, def, kNoCheck, specifics);
    }
}
void readTermIndex(const string &base) {
    NindTermIndex index(base, false, kNoCheck, 2);
    list<TermCG> def;
    for (unsigned int id = 0; id < 12; id++) index.getTermDef(id, def);
    list<unsigned int> specifics;
    index.getSpecificWords(specifics);
    index.analyseIndex();
    index.analysePadFile();
}
void buildLocalIndex(const string &base) {
    NindLocalIndex index(base, true, kNoCheck, 4);
    for (unsigned int doc = 1; doc <= 6; doc++) {
        list<Term> def;
        def.push_back(term(5 * doc, 1, 3));
        def.push_back(term(100000 * doc, 2, 1));
        index.setLocalDef(1000 * doc, def, kNoCheck);
    }
}
void readLocalIndex(const string &base) {
    NindLocalIndex index(base, false, kNoCheck);
    list<Term> def;
    unsigned int length;
    set<unsigned int> termIdents;
    for (unsigned int doc = 0; doc <= 7; doc++) {
        index.getLocalDef(1000 * doc, def);
        index.getLocalLength(1000 * doc, length);
        index.getTermIdents(1000 * doc, termIdents);
    }
    index.getDocCount();
    index.analyseIndex();
    index.analysePadFile();
}
}
////////////////////////////////////////////////////////////
TEST_CASE("NindIndexRobustness.CorruptLexiconIndex") {
    TestTempDir tmp;
    const string base = tmp.file("lex");
    buildLexicon(base);
    REQUIRE_NOTHROW(readLexicon(base));
    checkSweep(SweepFile{base + ".nindlexiconindex", [&] { readLexicon(base); }});
}
TEST_CASE("NindIndexRobustness.CorruptRetrolexicon") {
    TestTempDir tmp;
    const string base = tmp.file("lex");
    buildLexicon(base);
    checkSweep(SweepFile{base + ".nindretrolexicon", [&] { readLexicon(base); }});
}
TEST_CASE("NindIndexRobustness.CorruptTermIndex") {
    TestTempDir tmp;
    const string base = tmp.file("term");
    buildTermIndex(base);
    REQUIRE_NOTHROW(readTermIndex(base));
    checkSweep(SweepFile{base + ".nindtermindex", [&] { readTermIndex(base); }});
}
TEST_CASE("NindIndexRobustness.CorruptLocalIndex") {
    TestTempDir tmp;
    const string base = tmp.file("local");
    buildLocalIndex(base);
    REQUIRE_NOTHROW(readLocalIndex(base));
    checkSweep(SweepFile{base + ".nindlocalindex", [&] { readLocalIndex(base); }});
}
