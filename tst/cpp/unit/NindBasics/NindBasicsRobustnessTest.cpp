//
// Robustness / memory-safety regression tests for NindBasics:
// integer decoding at every width and tier boundary (was signed-overflow UB),
// read/write buffer bounds, oversized reads driven by corrupt lengths,
// non-copyability, control-C critical sections, and pad files whose header or
// indirection-block chain is corrupt (was: infinite loop on a cyclic chain).
////////////////////////////////////////////////////////////
#include "NindBasics/NindFile.h"
#include "NindBasics/NindPadFile.h"
#include "NindBasics/NindSignalCatcher.h"
#include "NindExceptions.h"
#include "TestTempDir.h"
#include "TestFileBytes.h"
#include "doctest.h"
#include <climits>
#include <csignal>
#include <string>
#include <type_traits>
#include <vector>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
namespace {
// Writes whatever `fill` puts into a write buffer, then reopens the file and
// loads it all into the read buffer.
template <typename Fill>
void writeThenLoad(const string &path, const unsigned int bufferSize, Fill fill, NindFile &reader) {
    NindFile writer(path);
    REQUIRE(writer.open("wb"));
    writer.createBuffer(bufferSize);
    fill(writer);
    const unsigned int written = writer.getOutBufferSize();
    writer.writeBuffer();
    writer.close();
    REQUIRE(reader.open("rb"));
    reader.setPos(0, SEEK_SET);
    reader.readBuffer(written);
}

const unsigned int kUnsignedTiers[] = {
    0u, 127u, 128u, 16383u, 16384u, 2097151u, 2097152u, 268435455u, 268435456u,
    0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFEu, 0xFFFFFFFFu };
const signed int kSignedTiers[] = {
    0, -1, 63, -64, 64, -65, 8191, -8192, 8192, -8193, 1048575, -1048576, 1048576, -1048577,
    134217727, -134217728, 134217728, -134217729, INT_MAX, INT_MIN, INT_MIN + 1 };
}
////////////////////////////////////////////////////////////
TEST_CASE("NindBasicsRobustness.FixedWidthIntegersAtExtremeValues") {
    TestTempDir tmp;
    NindFile reader(tmp.file("fixed.bin"));
    writeThenLoad(tmp.file("fixed.bin"), 64, [](NindFile &w) {
        w.putInt2(0xFFFF);
        w.putInt3(0xFFFFFF);
        w.putInt3(0x800000);
        w.putInt4(0xFFFFFFFFu);
        w.putInt4(0x80000000u);
        w.putInt4(0x7FFFFFFFu);
        w.putInt5(0xFFFFFFFFFFULL);
    }, reader);
    CHECK_EQ(0xFFFFu, reader.getInt2());
    CHECK_EQ(-1, reader.getSInt3());
    CHECK_EQ(-8388608, reader.getSInt3());
    CHECK_EQ(0xFFFFFFFFu, reader.getInt4());
    CHECK_EQ(INT_MIN, reader.getSInt4());
    CHECK_EQ(INT_MAX, reader.getSInt4());
    CHECK_EQ(0xFFFFFFFFFFULL, reader.getInt5());
    CHECK(reader.endOfInBuffer());
}
////////////////////////////////////////////////////////////
TEST_CASE("NindBasicsRobustness.LateconIntegersRoundTripAtEveryTierBoundary") {
    TestTempDir tmp;
    NindFile reader(tmp.file("lat.bin"));
    writeThenLoad(tmp.file("lat.bin"), 512, [](NindFile &w) {
        for (unsigned int v : kUnsignedTiers) w.putUIntLat(v);
        for (signed int v : kSignedTiers) w.putSIntLat(v);
    }, reader);
    for (unsigned int v : kUnsignedTiers) { INFO("unsigned value=" << v); CHECK_EQ(v, reader.getUIntLat()); }
    for (signed int v : kSignedTiers) { INFO("signed value=" << v); CHECK_EQ(v, reader.getSIntLat()); }
    CHECK(reader.endOfInBuffer());

    // the unbuffered readers (readUIntLat/readSIntLat) decode the very same bytes
    NindFile direct(tmp.file("lat.bin"));
    REQUIRE(direct.open("rb"));
    direct.setPos(0, SEEK_SET);
    for (unsigned int v : kUnsignedTiers) { INFO("unsigned value=" << v); CHECK_EQ(v, direct.readUIntLat()); }
    for (signed int v : kSignedTiers) { INFO("signed value=" << v); CHECK_EQ(v, direct.readSIntLat()); }
}
////////////////////////////////////////////////////////////
TEST_CASE("NindBasicsRobustness.TruncatedValuesThrowWithoutReadingPastBuffer") {
    TestTempDir tmp;
    const string path = tmp.file("trunc.bin");
    // each prefix announces more bytes than the (logical) buffer holds
    const unsigned char prefixes[][2] = { {0x80, 1}, {0xC0, 2}, {0xE0, 3}, {0xF0, 4} };
    for (const auto &prefix : prefixes) {
        NindFile reader(path);
        writeThenLoad(path, 8, [&](NindFile &w) {
            w.putInt1(prefix[0]);
            for (int i = 0; i < 4; i++) w.putInt1(0);
        }, reader);
        reader.setInBufferPtr(0);
        reader.setAbsEndInBuffer(prefix[1]);
        CHECK_THROWS_AS(reader.getUIntLat(), OutReadBufferException);
        reader.setInBufferPtr(0);
        CHECK_THROWS_AS(reader.getSIntLat(), OutReadBufferException);
    }
    NindFile reader(path);
    writeThenLoad(path, 8, [](NindFile &w) { w.putInt1(200); w.putInt1('a'); }, reader);
    CHECK_THROWS_AS(reader.getString(), OutReadBufferException);   // announces 200 bytes, holds 1
    reader.setInBufferPtr(0);
    CHECK_THROWS_AS(reader.getInt5(), OutReadBufferException);
    reader.setInBufferPtr(0);
    CHECK_THROWS_AS(reader.getStringAsBytes(3), OutReadBufferException);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindBasicsRobustness.ReadPointerCannotBeMovedOutsideBuffer") {
    TestTempDir tmp;
    NindFile reader(tmp.file("ptr.bin"));
    writeThenLoad(tmp.file("ptr.bin"), 4, [](NindFile &w) { w.putInt4(0); }, reader);
    CHECK_NOTHROW(reader.setInBufferPtr(4));
    CHECK_THROWS_AS(reader.setInBufferPtr(5), OutReadBufferException);
    reader.setInBufferPtr(2);
    CHECK_THROWS_AS(reader.setRelInBufferPtr(3), OutReadBufferException);
    CHECK_THROWS_AS(reader.setRelInBufferPtr(0xFFFFFFFFu), OutReadBufferException);
    CHECK_THROWS_AS(reader.setEndInBuffer(0xFFFFFFFFu), OutReadBufferException);
    CHECK_THROWS_AS(reader.setAbsEndInBuffer(5), OutReadBufferException);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindBasicsRobustness.WriteBufferBoundsAreEnforced") {
    TestTempDir tmp;
    NindFile writer(tmp.file("w.bin"));
    REQUIRE(writer.open("wb"));
    writer.createBuffer(4);
    CHECK_THROWS_AS(writer.putInt3(1, 2), OutWriteBufferException);           // bytes 2..4 of a 4-byte buffer
    CHECK_THROWS_AS(writer.putInt3(1, 0xFFFFFFFFu), OutWriteBufferException);  // offset wrapping around
    CHECK_THROWS_AS(writer.putInt5(0), OutWriteBufferException);
    CHECK_EQ(0u, writer.getOutBufferSize());                                   // failed puts do not move the pointer
    CHECK_THROWS_AS(writer.putPad(0xFFFFFFFFu), OutWriteBufferException);
    CHECK_THROWS_AS(writer.putString(string(10, 'x')), OutWriteBufferException);
    writer.putInt4(7);
    CHECK_THROWS_AS(writer.writeBuffer(2, 4), OutWriteBufferException);        // would read past the buffer
    CHECK_NOTHROW(writer.writeBuffer(0, 4));
}
////////////////////////////////////////////////////////////
TEST_CASE("NindBasicsRobustness.OversizedReadIsRejectedBeforeAllocating") {
    TestTempDir tmp;
    NindFile reader(tmp.file("small.bin"));
    writeThenLoad(tmp.file("small.bin"), 4, [](NindFile &w) { w.putInt4(0); }, reader);
    reader.setPos(0, SEEK_SET);
    // a corrupt length field asking for ~4 GB must not try to allocate 4 GB
    CHECK_THROWS_AS(reader.readBuffer(0xFFFFFFF0u), EofException);
    // and the reader is left in a consistent (empty) state
    CHECK_THROWS_AS(reader.getInt1(), OutReadBufferException);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindBasicsRobustness.FilesAreNotCopyable") {
    // copies would share one FILE* and two heap buffers (double fclose / double delete[])
    CHECK_FALSE(std::is_copy_constructible<NindFile>::value);
    CHECK_FALSE(std::is_copy_assignable<NindFile>::value);
    CHECK_FALSE(std::is_copy_constructible<NindPadFile>::value);
    CHECK_FALSE(std::is_copy_assignable<NindPadFile>::value);
}
////////////////////////////////////////////////////////////
namespace {
volatile std::sig_atomic_t gSigintCount = 0;
void countSigint(int) { gSigintCount = gSigintCount + 1; }
typedef void (*SignalHandler)(int);
SignalHandler currentSigintHandler() {
    SignalHandler h = signal(SIGINT, SIG_DFL);
    signal(SIGINT, h);
    return h;
}
}
TEST_CASE("NindBasicsRobustness.SigintHandlerOnlyReplacedInsideCriticalSections") {
    signal(SIGINT, countSigint);
    gSigintCount = 0;
    TestTempDir tmp;
    NindFile file(tmp.file("sig.bin"));        // used to install a process-wide handler for good
    REQUIRE(file.open("wb"));
    CHECK_EQ(&countSigint, currentSigintHandler());
    CHECK_FALSE(NindSignalCatcher::isUp());
    {
        NindCriticalSection section(file);
        CHECK(NindSignalCatcher::isUp());
        CHECK_NE(&countSigint, currentSigintHandler());
        raise(SIGINT);                         // deferred...
        CHECK_EQ(0, gSigintCount);
        {
            NindCriticalSection nested(file);   // nesting is fine
        }
        CHECK(NindSignalCatcher::isUp());
        CHECK_EQ(0, gSigintCount);
    }
    CHECK_EQ(1, gSigintCount);                 // ...and delivered to the previous handler at the end
    CHECK_EQ(&countSigint, currentSigintHandler());
    CHECK_FALSE(NindSignalCatcher::isUp());
    signal(SIGINT, SIG_DFL);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindBasicsRobustness.CriticalSectionClosedWhenExceptionEscapes") {
    TestTempDir tmp;
    NindFile file(tmp.file("sig2.bin"));
    REQUIRE(file.open("wb"));
    try {
        NindCriticalSection section(file);
        file.createBuffer(1);
        file.putInt4(0);                      // throws OutWriteBufferException
    }
    catch (const OutWriteBufferException &) {}
    CHECK_FALSE(NindSignalCatcher::isUp());
    // unbalanced reset is harmless
    NindSignalCatcher::Instance()->resetCatcher();
    CHECK_FALSE(NindSignalCatcher::isUp());
}
////////////////////////////////////////////////////////////
namespace {
class TestPadFile : public NindPadFile {
public:
    TestPadFile(const string &fileName, const bool isWriter,
                const unsigned int specificsSize = 4,
                const unsigned int dataEntrySize = 8,
                const unsigned int dataEntriesBlocSize = 4):
        NindPadFile(fileName, isWriter, Identification(0, 0), specificsSize,
                    dataEntrySize, dataEntriesBlocSize) {}
    using NindPadFile::addEntriesBlock;
    using NindPadFile::getMaxIdent;
    using NindPadFile::getEntryPos;
};
// <enTeste> = <tailleEntreje>(1) <tailleSpejcifiques>(3), then the first block:
// <flagIndexej=47>(1) <addrBlocSuivant>(5) <nombreIndex>(3)
const size_t kFirstBlockNext = 5;
const size_t kFirstBlockCount = 10;

string makePadFile(const TestTempDir &tmp, const string &name, const int blocks) {
    const string path = tmp.file(name);
    TestPadFile pad(path, true);
    for (int i = 1; i < blocks; i++) pad.addEntriesBlock(NindPadFile::Identification(0, 0));
    return path;
}
unsigned long long readInt5(const vector<unsigned char> &b, const size_t offset) {
    unsigned long long v = 0;
    for (int i = 0; i < 5; i++) v = (v << 8) | b[offset + i];
    return v;
}
}
TEST_CASE("NindBasicsRobustness.PadFileSanityOfTestFixture") {
    TestTempDir tmp;
    const string path = makePadFile(tmp, "ok.pad", 3);
    TestPadFile reader(path, false);
    CHECK_EQ(12u, reader.getMaxIdent());
}
TEST_CASE("NindBasicsRobustness.PadFileWithSelfLinkedBlockIsRejected") {
    TestTempDir tmp;
    const string path = makePadFile(tmp, "self.pad", 1);
    vector<unsigned char> bytes = readFileBytes(path);
    patchInt5(bytes, kFirstBlockNext, 4);     // first block says the next one is itself
    writeFileBytes(path, bytes);
    CHECK_THROWS_AS(TestPadFile(path, false), NindPadFileException);
}
TEST_CASE("NindBasicsRobustness.PadFileWithCyclicBlockChainIsRejected") {
    TestTempDir tmp;
    const string path = makePadFile(tmp, "cycle.pad", 2);
    vector<unsigned char> bytes = readFileBytes(path);
    const unsigned long long second = readInt5(bytes, kFirstBlockNext);
    REQUIRE_NE(0u, second);
    patchInt5(bytes, second + 1, 4);          // second block links back to the first one
    writeFileBytes(path, bytes);
    CHECK_THROWS_AS(TestPadFile(path, false), NindPadFileException);
    CHECK_THROWS_AS(TestPadFile(path, true), NindPadFileException);
}
TEST_CASE("NindBasicsRobustness.PadFileWithBlockPastEndOfFileIsRejected") {
    TestTempDir tmp;
    const string path = makePadFile(tmp, "past.pad", 1);
    vector<unsigned char> bytes = readFileBytes(path);
    patchInt5(bytes, kFirstBlockNext, 0xFFFFFFFFFFULL);
    writeFileBytes(path, bytes);
    CHECK_THROWS_AS(TestPadFile(path, false), NindPadFileException);

    bytes = readFileBytes(makePadFile(tmp, "count.pad", 1));
    bytes[kFirstBlockCount] = bytes[kFirstBlockCount + 1] = bytes[kFirstBlockCount + 2] = 0xFF;  // 16M entries
    writeFileBytes(tmp.file("count.pad"), bytes);
    CHECK_THROWS_AS(TestPadFile(tmp.file("count.pad"), false), NindPadFileException);
}
TEST_CASE("NindBasicsRobustness.PadFileWithInconsistentHeaderIsRejected") {
    TestTempDir tmp;
    const string path = makePadFile(tmp, "hdr.pad", 1);
    const vector<unsigned char> good = readFileBytes(path);

    vector<unsigned char> bytes = good;
    bytes[1] = bytes[2] = bytes[3] = 0xFF;     // specifics larger than the whole file
    writeFileBytes(path, bytes);
    CHECK_THROWS_AS(TestPadFile(path, false, 0xFFFFFF), NindPadFileException);

    bytes = good;
    bytes[0] = 0;                              // null entry size
    writeFileBytes(path, bytes);
    CHECK_THROWS_AS(TestPadFile(path, false), NindPadFileException);

    bytes = good;
    bytes.resize(6);                           // truncated inside the first block header
    writeFileBytes(path, bytes);
    CHECK_THROWS_AS(TestPadFile(path, false), FileException);
}
