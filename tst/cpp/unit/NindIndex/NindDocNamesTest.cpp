#include "NindIndex/NindDocNames.h"
#include "NindExceptions.h"
#include "TestTempDir.h"
#include "TestFileBytes.h"
#include "doctest.h"
#include <stdexcept>
#include <string>
#include <vector>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
namespace {
void checkAll(const NindDocNames &names, const vector<uint32_t> &idents, const vector<string> &expected) {
    REQUIRE_EQ(idents.size(), names.size());
    for (size_t i = 0; i < idents.size(); i++) {
        string name;
        REQUIRE(names.getName(idents[i], name));
        CHECK_EQ(expected[i], name);
        uint32_t ident = 0;
        REQUIRE(names.getDocIdent(expected[i], ident));
        CHECK_EQ(idents[i], ident);
    }
}
}
////////////////////////////////////////////////////////////
TEST_CASE("NindDocNamesTest.ConsecutiveIdentsRoundTrip") {
    TestTempDir tmp;
    const string file = tmp.file("dense.ninddocnames");
    const vector<uint32_t> idents = {12, 10, 11, 13};
    const vector<string> names = {"d3", "b1", "", "\xc3\xa9t\xc3\xa9"};
    NindDocNames::write(file, idents, names);
    const NindDocNames read(file);
    checkAll(read, idents, names);
    string name;
    CHECK_FALSE(read.getName(9, name));
    CHECK_FALSE(read.getName(14, name));
    uint32_t ident;
    CHECK_FALSE(read.getDocIdent("d", ident));
    CHECK_FALSE(read.getDocIdent("d30", ident));
    vector<uint32_t> entryIdents;
    vector<string> entryNames;
    read.getEntries(entryIdents, entryNames);
    CHECK(entryIdents == vector<uint32_t>({10, 11, 12, 13}));
    CHECK(entryNames == vector<string>({"b1", "", "d3", "\xc3\xa9t\xc3\xa9"}));
    // consecutive idents are not stored: header + offsets + ranks + names
    CHECK_EQ(20u + 4 * 5 + 4 * 4 + 2 + 2 + 0 + 5, readFileBytes(file).size());
}
////////////////////////////////////////////////////////////
TEST_CASE("NindDocNamesTest.SparseIdentsRoundTrip") {
    TestTempDir tmp;
    const string file = tmp.file("sparse.ninddocnames");
    const vector<uint32_t> idents = {0, 7, 0xFFFFFFFFu, 3};
    const vector<string> names = {"zero", "seven", "max", "three"};
    NindDocNames::write(file, idents, names);
    const NindDocNames read(file);
    checkAll(read, idents, names);
    string name;
    CHECK_FALSE(read.getName(4, name));
}
////////////////////////////////////////////////////////////
TEST_CASE("NindDocNamesTest.EmptyAndInvalid") {
    TestTempDir tmp;
    NindDocNames::write(tmp.file("empty"), vector<uint32_t>(), vector<string>());
    const NindDocNames empty(tmp.file("empty"));
    CHECK_EQ(0u, empty.size());
    uint32_t ident;
    CHECK_FALSE(empty.getDocIdent("", ident));
    CHECK_THROWS_AS(NindDocNames::write(tmp.file("dup"), {1, 2}, {"a", "a"}), invalid_argument);
    CHECK_THROWS_AS(NindDocNames::write(tmp.file("dup"), {1, 1}, {"a", "b"}), invalid_argument);
    CHECK_THROWS_AS(NindDocNames::write(tmp.file("dup"), {1}, {"a", "b"}), invalid_argument);
    // never overwrites a file
    CHECK_THROWS_AS(NindDocNames::write(tmp.file("empty"), {1}, {"a"}), FileException);
    CHECK_THROWS_AS(NindDocNames(tmp.file("missing")), FileException);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindDocNamesRobustness.EveryTruncationAndCorruptionIsRejectedOrHarmless") {
    TestTempDir tmp;
    for (const bool sparse : {false, true}) {
        const string file = tmp.file(sparse ? "sparse" : "dense");
        const vector<uint32_t> idents = sparse ? vector<uint32_t>({5, 9, 100}) : vector<uint32_t>({5, 6, 7});
        NindDocNames::write(file, idents, {"bb", "a", "ccc"});
        const vector<unsigned char> original = readFileBytes(file);
        const string corrupt = tmp.file("corrupt");
        for (size_t size = 0; size < original.size(); size++) {
            writeFileBytes(corrupt, vector<unsigned char>(original.begin(), original.begin() + size));
            CHECK_THROWS_AS(NindDocNames{corrupt}, FileException);
        }
        for (size_t i = 0; i < original.size(); i++) {
            for (const unsigned char value : {(unsigned char)0x00, (unsigned char)0xFF, (unsigned char)(original[i] ^ 0x01)}) {
                vector<unsigned char> bytes = original;
                bytes[i] = value;
                writeFileBytes(corrupt, bytes);
                try {
                    const NindDocNames read(corrupt);
                    // accepted: every lookup stays within the file
                    vector<uint32_t> entryIdents;
                    vector<string> entryNames;
                    read.getEntries(entryIdents, entryNames);
                    for (size_t k = 0; k < entryNames.size(); k++) {
                        string name;
                        uint32_t ident;
                        CHECK(read.getName(entryIdents[k], name));
                        CHECK(read.getDocIdent(entryNames[k], ident));
                    }
                }
                catch (FileException &) {
                }
            }
        }
    }
}
////////////////////////////////////////////////////////////
