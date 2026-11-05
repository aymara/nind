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
void checkAll(const NindDocNames &names, const vector<NindDocId> &idents, const vector<string> &expected) {
    REQUIRE_EQ(idents.size(), names.size());
    for (size_t i = 0; i < idents.size(); i++) {
        string name;
        REQUIRE(names.getName(idents[i], name));
        CHECK_EQ(expected[i], name);
        NindDocId ident = 0;
        REQUIRE(names.getDocIdent(expected[i], ident));
        CHECK_EQ(idents[i], ident);
    }
}
}
////////////////////////////////////////////////////////////
TEST_CASE("NindDocNamesTest.MergeWritesTheSameFileAsWrite") {
    TestTempDir tmp;
    // consecutive documents (their idents not stored), then with gaps; an empty name; an empty
    // shard; names sorted across shards; shards listed out of order
    const vector<vector<NindDocId> > allIdents = {{10, 11, 12, 13, 14, 15}, {10, 11, 40, 41, 77, 1ull << 40}};
    for (size_t variant = 0; variant < allIdents.size(); variant++) {
        CAPTURE(variant);
        const vector<NindDocId> &idents = allIdents[variant];
        const vector<string> names = {"m", "", "a", "z", "ab", "\xc3\xa9t\xc3\xa9"};
        const string prefix = "v" + to_string(variant) + "-";
        NindDocNames::write(tmp.file(prefix + "all"), idents, names);
        NindDocNames::write(tmp.file(prefix + "s0"), vector<NindDocId>(idents.begin(), idents.begin() + 2),
                            vector<string>(names.begin(), names.begin() + 2));
        NindDocNames::write(tmp.file(prefix + "s1"), vector<NindDocId>(), vector<string>());
        NindDocNames::write(tmp.file(prefix + "s2"), vector<NindDocId>(idents.begin() + 2, idents.end()),
                            vector<string>(names.begin() + 2, names.end()));
        NindDocNames::merge({tmp.file(prefix + "s2"), tmp.file(prefix + "s1"), tmp.file(prefix + "s0")}, tmp.file(prefix + "merged"));
        CHECK(readFileBytes(tmp.file(prefix + "merged")) == readFileBytes(tmp.file(prefix + "all")));
    }
    NindDocNames::merge({tmp.file("v0-s1")}, tmp.file("empty"));
    CHECK_EQ(0u, NindDocNames(tmp.file("empty")).size());
}
////////////////////////////////////////////////////////////
TEST_CASE("NindDocNamesTest.MergeRefusesOverlapsAndDuplicatesAndRemovesItsFile") {
    TestTempDir tmp;
    NindDocNames::write(tmp.file("a"), {1, 2}, {"a", "b"});
    NindDocNames::write(tmp.file("overlap"), {2, 3}, {"c", "d"});
    NindDocNames::write(tmp.file("duplicate"), {3, 4}, {"d", "b"});
    CHECK_THROWS_AS(NindDocNames::merge({tmp.file("a"), tmp.file("overlap")}, tmp.file("m1")), std::invalid_argument);
    CHECK(readFileBytes(tmp.file("m1")).empty());
    CHECK_THROWS_AS(NindDocNames::merge({tmp.file("a"), tmp.file("duplicate")}, tmp.file("m2")), std::invalid_argument);
    CHECK_THROWS_AS(NindDocNames(tmp.file("m2")), OpenFileException);
    // an existing target isn't overwritten
    CHECK_THROWS_AS(NindDocNames::merge({tmp.file("a")}, tmp.file("overlap")), OpenFileException);
    CHECK_EQ(2u, NindDocNames(tmp.file("overlap")).size());
}
////////////////////////////////////////////////////////////
TEST_CASE("NindDocNamesTest.WideOffsetsAreRead") {
    // NINDDN03 is written only for names over 4 GiB: built by hand here
    TestTempDir tmp;
    NindDocNames::write(tmp.file("narrow"), {5, 6}, {"bb", "a"});
    const vector<unsigned char> narrow = readFileBytes(tmp.file("narrow"));
    vector<unsigned char> wide(narrow.begin(), narrow.begin() + 24);
    wide[7] = '3';
    for (size_t i = 0; i < 3; i++) {
        wide.insert(wide.end(), narrow.begin() + 24 + 4 * i, narrow.begin() + 28 + 4 * i);
        wide.insert(wide.end(), 4, 0);
    }
    wide.insert(wide.end(), narrow.begin() + 36, narrow.end());
    writeFileBytes(tmp.file("wide"), wide);
    checkAll(NindDocNames(tmp.file("wide")), {5, 6}, {"bb", "a"});
    // an offset beyond 32 bits, as read from a corrupt file: refused, not wrapped
    vector<unsigned char> corrupt = wide;
    corrupt[24 + 8 * 2 + 4] = 1;
    writeFileBytes(tmp.file("corrupt"), corrupt);
    CHECK_THROWS_AS(NindDocNames(tmp.file("corrupt")), FormatFileException);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindDocNamesTest.ConsecutiveIdentsRoundTrip") {
    TestTempDir tmp;
    const string file = tmp.file("dense.ninddocnames");
    const vector<NindDocId> idents = {12, 10, 11, 13};
    const vector<string> names = {"d3", "b1", "", "\xc3\xa9t\xc3\xa9"};
    NindDocNames::write(file, idents, names);
    const NindDocNames read(file);
    checkAll(read, idents, names);
    string name;
    CHECK_FALSE(read.getName(9, name));
    CHECK_FALSE(read.getName(14, name));
    NindDocId ident;
    CHECK_FALSE(read.getDocIdent("d", ident));
    CHECK_FALSE(read.getDocIdent("d30", ident));
    vector<NindDocId> entryIdents;
    vector<string> entryNames;
    read.getEntries(entryIdents, entryNames);
    CHECK(entryIdents == vector<NindDocId>({10, 11, 12, 13}));
    CHECK(entryNames == vector<string>({"b1", "", "d3", "\xc3\xa9t\xc3\xa9"}));
    // consecutive idents are not stored: header + offsets + ranks + names
    CHECK_EQ(24u + 4 * 5 + 4 * 4 + 2 + 2 + 0 + 5, readFileBytes(file).size());
}
////////////////////////////////////////////////////////////
TEST_CASE("NindDocNamesTest.SparseIdentsRoundTrip") {
    TestTempDir tmp;
    const string file = tmp.file("sparse.ninddocnames");
    const vector<NindDocId> idents = {0, 7, 0xFFFFFFFFu, 3, 0xFFFFFFFFFFFFFFFFull, 0x100000000ull};
    const vector<string> names = {"zero", "seven", "max32", "three", "max64", "over32"};
    NindDocNames::write(file, idents, names);
    const NindDocNames read(file);
    checkAll(read, idents, names);
    string name;
    CHECK_FALSE(read.getName(4, name));
}
////////////////////////////////////////////////////////////
TEST_CASE("NindDocNamesTest.EmptyAndInvalid") {
    TestTempDir tmp;
    NindDocNames::write(tmp.file("empty"), vector<NindDocId>(), vector<string>());
    const NindDocNames empty(tmp.file("empty"));
    CHECK_EQ(0u, empty.size());
    NindDocId ident;
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
        const vector<NindDocId> idents = sparse ? vector<NindDocId>({5, 9, 100}) : vector<NindDocId>({5, 6, 7});
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
                    vector<NindDocId> entryIdents;
                    vector<string> entryNames;
                    read.getEntries(entryIdents, entryNames);
                    for (size_t k = 0; k < entryNames.size(); k++) {
                        string name;
                        NindDocId ident;
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
