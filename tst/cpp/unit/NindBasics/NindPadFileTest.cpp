#include "NindBasics/NindPadFile.h"
#include "NindExceptions.h"
#include "TestTempDir.h"
#include <gtest/gtest.h>
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
TEST(NindPadFileTest, NewWriterAllocatesFirstBlock) {
    TestTempDir tmp;
    TestPadFile pad(tmp.file("a.pad"), true, NindPadFile::Identification(0, 0), 4, 8, 4);
    EXPECT_EQ(4u, pad.getFirstEntriesBlockSize());
    EXPECT_EQ(4u, pad.getMaxIdent());
    EXPECT_NE(0u, pad.getEntryPos(0));
    EXPECT_NE(0u, pad.getEntryPos(3));
    EXPECT_EQ(0u, pad.getEntryPos(4));    // out of the single allocated block
}
////////////////////////////////////////////////////////////
TEST(NindPadFileTest, AddEntriesBlockExtendsIndirectionRange) {
    TestTempDir tmp;
    TestPadFile pad(tmp.file("b.pad"), true, NindPadFile::Identification(0, 0), 4, 8, 4);
    pad.addEntriesBlock(NindPadFile::Identification(0, 0));
    EXPECT_EQ(8u, pad.getMaxIdent());
    for (unsigned int ident = 0; ident < 8; ident++) EXPECT_NE(0u, pad.getEntryPos(ident)) << "ident=" << ident;
    EXPECT_EQ(0u, pad.getEntryPos(8));
}
////////////////////////////////////////////////////////////
TEST(NindPadFileTest, MultiBlockChainingGivesDistinctPositions) {
    TestTempDir tmp;
    TestPadFile pad(tmp.file("c.pad"), true, NindPadFile::Identification(0, 0), 0, 8, 2);
    pad.addEntriesBlock(NindPadFile::Identification(0, 0));
    pad.addEntriesBlock(NindPadFile::Identification(0, 0));
    ASSERT_EQ(6u, pad.getMaxIdent());
    std::set<unsigned long int> positions;
    for (unsigned int ident = 0; ident < 6; ident++) {
        const unsigned long int pos = pad.getEntryPos(ident);
        EXPECT_NE(0u, pos) << "ident=" << ident;
        EXPECT_TRUE(positions.insert(pos).second) << "duplicate position for ident=" << ident;
    }
    EXPECT_EQ(0u, pad.getEntryPos(6));
}
////////////////////////////////////////////////////////////
TEST(NindPadFileTest, SpecificsStartZeroFilled) {
    TestTempDir tmp;
    TestPadFile pad(tmp.file("d.pad"), true, NindPadFile::Identification(0, 0), 4, 8, 4);
    pad.getSpecifics();
    EXPECT_EQ(0u, pad.m_file.getInt1());
    EXPECT_EQ(0u, pad.m_file.getInt1());
    EXPECT_EQ(0u, pad.m_file.getInt1());
    EXPECT_EQ(0u, pad.m_file.getInt1());
}
////////////////////////////////////////////////////////////
TEST(NindPadFileTest, IdentificationIsPersistedAndReadBack) {
    TestTempDir tmp;
    const string path = tmp.file("e.pad");
    const NindPadFile::Identification identification(42, 1000);
    {
        TestPadFile pad(path, true, identification, 0, 8, 4);
        NindPadFile::Identification readBack;
        pad.getFileIdentification(readBack);
        EXPECT_EQ(identification, readBack);
    }
    // Reopening as a reader with the same non-zero reference must succeed
    // and must still report the same identification.
    TestPadFile reopened(path, false, identification, 0, 8, 4);
    NindPadFile::Identification readBack;
    reopened.getFileIdentification(readBack);
    EXPECT_EQ(identification, readBack);
}
////////////////////////////////////////////////////////////
TEST(NindPadFileTest, ZeroReferenceIdentificationSkipsCheck) {
    TestTempDir tmp;
    const string path = tmp.file("f.pad");
    { TestPadFile pad(path, true, NindPadFile::Identification(7, 77), 0, 8, 4); }
    // A zero reference means "don't check": must not throw even though the
    // file's real identification (7, 77) differs.
    EXPECT_NO_THROW(TestPadFile(path, false, NindPadFile::Identification(0, 0), 0, 8, 4));
}
////////////////////////////////////////////////////////////
TEST(NindPadFileTest, MismatchedIdentificationThrowsOnReopen) {
    TestTempDir tmp;
    const string path = tmp.file("g.pad");
    { TestPadFile pad(path, true, NindPadFile::Identification(7, 77), 0, 8, 4); }
    EXPECT_THROW(TestPadFile(path, false, NindPadFile::Identification(1, 1), 0, 8, 4), NindPadFileException);
    EXPECT_THROW(TestPadFile(path, true, NindPadFile::Identification(1, 1), 0, 8, 4), NindPadFileException);
}
////////////////////////////////////////////////////////////
TEST(NindPadFileTest, NewWriterRequiresNonZeroDataEntrySize) {
    TestTempDir tmp;
    EXPECT_THROW(TestPadFile(tmp.file("h.pad"), true, NindPadFile::Identification(0, 0), 0, 0, 4),
                 NindPadFileException);
}
////////////////////////////////////////////////////////////
TEST(NindPadFileTest, NewWriterRequiresNonZeroBlocSize) {
    TestTempDir tmp;
    EXPECT_THROW(TestPadFile(tmp.file("i.pad"), true, NindPadFile::Identification(0, 0), 0, 8, 0),
                 NindPadFileException);
}
////////////////////////////////////////////////////////////
TEST(NindPadFileTest, ReaderOnMissingFileThrows) {
    TestTempDir tmp;
    EXPECT_THROW(TestPadFile(tmp.file("missing.pad"), false, NindPadFile::Identification(0, 0), 0, 8, 4),
                 NindPadFileException);
}
////////////////////////////////////////////////////////////
TEST(NindPadFileTest, ReopenWriterWithDifferentDataEntrySizeThrows) {
    TestTempDir tmp;
    const string path = tmp.file("j.pad");
    { TestPadFile pad(path, true, NindPadFile::Identification(0, 0), 0, 8, 4); }
    EXPECT_THROW(TestPadFile(path, true, NindPadFile::Identification(0, 0), 0, 4, 4), NindPadFileException);
}
