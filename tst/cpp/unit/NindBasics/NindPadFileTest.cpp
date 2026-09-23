#include "NindBasics/NindPadFile.h"
#include "NindExceptions.h"
#include "TestTempDir.h"
#include "doctest.h"
#include <string>
#include <set>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
namespace {
// NindPadFile's constructor and most of its interesting behaviour are
// protected (it is meant to be subclassed, as NindIndex/NindRetrolexicon do).
// This thin subclass re-exposes them so the envelope mechanics (header,
// indirection blocks, specifics, identification) can be unit-tested on
// their own, independently of any concrete file format built on top of them.
class TestPadFile : public NindPadFile {
public:
    TestPadFile(const string &fileName,
                const bool isWriter,
                const Identification &referenceIdentification,
                const unsigned int specificsSize,
                const unsigned int dataEntrySize = 0,
                const unsigned int dataEntriesBlocSize = 0):
        NindPadFile(fileName, isWriter, referenceIdentification, specificsSize,
                    dataEntrySize, dataEntriesBlocSize) {}

    using NindPadFile::getEntryPos;
    using NindPadFile::addEntriesBlock;
    using NindPadFile::getFirstEntriesBlockSize;
    using NindPadFile::getMaxIdent;
    using NindPadFile::getSpecifics;
    using NindPadFile::getFileIdentification;
};
}
////////////////////////////////////////////////////////////
TEST_CASE("NindPadFileTest.NewWriterAllocatesFirstBlock") {
    TestTempDir tmp;
    TestPadFile pad(tmp.file("a.pad"), true, NindPadFile::Identification(0, 0), 4, 8, 4);
    CHECK_EQ(4u, pad.getFirstEntriesBlockSize());
    CHECK_EQ(4u, pad.getMaxIdent());
    CHECK_NE(0u, pad.getEntryPos(0));
    CHECK_NE(0u, pad.getEntryPos(3));
    CHECK_EQ(0u, pad.getEntryPos(4));    // out of the single allocated block
}
////////////////////////////////////////////////////////////
TEST_CASE("NindPadFileTest.AddEntriesBlockExtendsIndirectionRange") {
    TestTempDir tmp;
    TestPadFile pad(tmp.file("b.pad"), true, NindPadFile::Identification(0, 0), 4, 8, 4);
    pad.addEntriesBlock(NindPadFile::Identification(0, 0));
    CHECK_EQ(8u, pad.getMaxIdent());
    for (unsigned int ident = 0; ident < 8; ident++) { INFO("ident=" << ident); CHECK_NE(0u, pad.getEntryPos(ident)); }
    CHECK_EQ(0u, pad.getEntryPos(8));
}
////////////////////////////////////////////////////////////
TEST_CASE("NindPadFileTest.MultiBlockChainingGivesDistinctPositions") {
    TestTempDir tmp;
    TestPadFile pad(tmp.file("c.pad"), true, NindPadFile::Identification(0, 0), 0, 8, 2);
    pad.addEntriesBlock(NindPadFile::Identification(0, 0));
    pad.addEntriesBlock(NindPadFile::Identification(0, 0));
    REQUIRE_EQ(6u, pad.getMaxIdent());
    std::set<unsigned long int> positions;
    for (unsigned int ident = 0; ident < 6; ident++) {
        const unsigned long int pos = pad.getEntryPos(ident);
        { INFO("ident=" << ident); CHECK_NE(0u, pos); }
        { INFO("duplicate position for ident=" << ident); CHECK(positions.insert(pos).second); }
    }
    CHECK_EQ(0u, pad.getEntryPos(6));
}
////////////////////////////////////////////////////////////
TEST_CASE("NindPadFileTest.SpecificsStartZeroFilled") {
    TestTempDir tmp;
    TestPadFile pad(tmp.file("d.pad"), true, NindPadFile::Identification(0, 0), 4, 8, 4);
    pad.getSpecifics();
    CHECK_EQ(0u, pad.m_file.getInt1());
    CHECK_EQ(0u, pad.m_file.getInt1());
    CHECK_EQ(0u, pad.m_file.getInt1());
    CHECK_EQ(0u, pad.m_file.getInt1());
}
////////////////////////////////////////////////////////////
TEST_CASE("NindPadFileTest.IdentificationIsPersistedAndReadBack") {
    TestTempDir tmp;
    const string path = tmp.file("e.pad");
    const NindPadFile::Identification identification(42, 1000);
    {
        TestPadFile pad(path, true, identification, 0, 8, 4);
        NindPadFile::Identification readBack;
        pad.getFileIdentification(readBack);
        CHECK_EQ(identification, readBack);
    }
    // Reopening as a reader with the same non-zero reference must succeed
    // and must still report the same identification.
    TestPadFile reopened(path, false, identification, 0, 8, 4);
    NindPadFile::Identification readBack;
    reopened.getFileIdentification(readBack);
    CHECK_EQ(identification, readBack);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindPadFileTest.ZeroReferenceIdentificationSkipsCheck") {
    TestTempDir tmp;
    const string path = tmp.file("f.pad");
    { TestPadFile pad(path, true, NindPadFile::Identification(7, 77), 0, 8, 4); }
    // A zero reference means "don't check": must not throw even though the
    // file's real identification (7, 77) differs.
    CHECK_NOTHROW(TestPadFile(path, false, NindPadFile::Identification(0, 0), 0, 8, 4));
}
////////////////////////////////////////////////////////////
TEST_CASE("NindPadFileTest.MismatchedIdentificationThrowsOnReopen") {
    TestTempDir tmp;
    const string path = tmp.file("g.pad");
    { TestPadFile pad(path, true, NindPadFile::Identification(7, 77), 0, 8, 4); }
    CHECK_THROWS_AS(TestPadFile(path, false, NindPadFile::Identification(1, 1), 0, 8, 4), NindPadFileException);
    CHECK_THROWS_AS(TestPadFile(path, true, NindPadFile::Identification(1, 1), 0, 8, 4), NindPadFileException);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindPadFileTest.NewWriterRequiresNonZeroDataEntrySize") {
    TestTempDir tmp;
    CHECK_THROWS_AS(TestPadFile(tmp.file("h.pad"), true, NindPadFile::Identification(0, 0), 0, 0, 4),
                 NindPadFileException);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindPadFileTest.NewWriterRequiresNonZeroBlocSize") {
    TestTempDir tmp;
    CHECK_THROWS_AS(TestPadFile(tmp.file("i.pad"), true, NindPadFile::Identification(0, 0), 0, 8, 0),
                 NindPadFileException);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindPadFileTest.ReaderOnMissingFileThrows") {
    TestTempDir tmp;
    // NindPadFile throws the more specific OpenFileException (rather than the
    // generic NindPadFileException) when the underlying file can't be opened at all.
    CHECK_THROWS_AS(TestPadFile(tmp.file("missing.pad"), false, NindPadFile::Identification(0, 0), 0, 8, 4),
                 OpenFileException);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindPadFileTest.ReopenWriterWithDifferentDataEntrySizeThrows") {
    TestTempDir tmp;
    const string path = tmp.file("j.pad");
    { TestPadFile pad(path, true, NindPadFile::Identification(0, 0), 0, 8, 4); }
    CHECK_THROWS_AS(TestPadFile(path, true, NindPadFile::Identification(0, 0), 0, 4, 4), NindPadFileException);
}
