#include "NindBasics/NindFile.h"
#include "NindExceptions.h"
#include "TestTempDir.h"
#include "doctest.h"
#include <vector>
#include <string>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
TEST_CASE("NindFileTest.BufferedRoundTripOfAllIntegerWidths") {
    TestTempDir tmp;
    const string path = tmp.file("ints.bin");

    NindFile writer(path);
    REQUIRE(writer.open("wb"));
    writer.createBuffer(64);
    writer.putInt1(0xAB);
    writer.putInt2(0x1234);
    writer.putInt3(0x123456);
    writer.putInt4(0x12345678);
    writer.putInt5(0x123456789AUL);
    const unsigned int written = writer.getOutBufferSize();
    writer.writeBuffer();
    writer.close();

    NindFile reader(path);
    REQUIRE(reader.open("rb"));
    reader.setPos(0, SEEK_SET);   // open() leaves the position at EOF (it seeks there to measure the file)
    reader.readBuffer(written);
    CHECK_EQ(0xABu, reader.getInt1());
    CHECK_EQ(0x1234u, reader.getInt2());
    CHECK_EQ(0x123456u, reader.getInt3());
    CHECK_EQ(0x12345678u, reader.getInt4());
    CHECK_EQ(0x123456789AUL, reader.getInt5());
    reader.close();
}
////////////////////////////////////////////////////////////
TEST_CASE("NindFileTest.GetSInt3AndGetSInt4DecodeTwosComplement") {
    TestTempDir tmp;
    const string path = tmp.file("signed.bin");

    NindFile writer(path);
    REQUIRE(writer.open("wb"));
    writer.createBuffer(16);
    writer.putInt3(static_cast<unsigned int>(-12345) & 0xFFFFFF);
    writer.putInt4(static_cast<unsigned int>(-987654321));
    const unsigned int written = writer.getOutBufferSize();
    writer.writeBuffer();
    writer.close();

    NindFile reader(path);
    REQUIRE(reader.open("rb"));
    reader.setPos(0, SEEK_SET);   // open() leaves the position at EOF (it seeks there to measure the file)
    reader.readBuffer(written);
    CHECK_EQ(-12345, reader.getSInt3());
    CHECK_EQ(-987654321, reader.getSInt4());
    reader.close();
}
////////////////////////////////////////////////////////////
TEST_CASE("NindFileTest.PutInt3AtOffsetPatchesInPlace") {
    // Exercises the "write a placeholder, come back and patch it once the
    // real value is known" pattern used throughout the higher-level classes
    // (e.g. writing a definition's length after writing the definition).
    TestTempDir tmp;
    const string path = tmp.file("patch.bin");

    NindFile writer(path);
    REQUIRE(writer.open("wb"));
    writer.createBuffer(16);
    writer.putInt1(0xAB);
    const unsigned int placeholderOffset = writer.getOutBufferSize();
    writer.putInt3(0);
    writer.putInt1(0xCD);
    writer.putInt3(999999, placeholderOffset);
    const unsigned int written = writer.getOutBufferSize();
    writer.writeBuffer();
    writer.close();

    NindFile reader(path);
    REQUIRE(reader.open("rb"));
    reader.setPos(0, SEEK_SET);   // open() leaves the position at EOF (it seeks there to measure the file)
    reader.readBuffer(written);
    CHECK_EQ(0xABu, reader.getInt1());
    CHECK_EQ(999999u, reader.getInt3());
    CHECK_EQ(0xCDu, reader.getInt1());
    reader.close();
}
////////////////////////////////////////////////////////////
TEST_CASE("NindFileTest.UIntLatRoundTripsAcrossAllTierBoundaries") {
    const vector<unsigned int> values = {
        0, 1, 126, 127,               // 1 byte : 0-127
        128, 129, 16382, 16383,       // 2 bytes : 128-16383
        16384, 16385, 2097150, 2097151,     // 3 bytes : 16384-2097151
        2097152, 2097153, 268435454, 268435455,  // 4 bytes : 2097152-268435455
        268435456, 268435457, 4294967294u, 4294967295u  // 5 bytes : 268435456-4294967295
    };

    TestTempDir tmp;
    const string path = tmp.file("ulat.bin");

    NindFile writer(path);
    REQUIRE(writer.open("wb"));
    writer.createBuffer(values.size() * 5);
    for (unsigned int v : values) writer.putUIntLat(v);
    const unsigned int written = writer.getOutBufferSize();
    writer.writeBuffer();
    writer.close();

    NindFile reader(path);
    REQUIRE(reader.open("rb"));
    reader.setPos(0, SEEK_SET);   // open() leaves the position at EOF (it seeks there to measure the file)
    reader.readBuffer(written);
    for (unsigned int v : values) { INFO("value=" << v); CHECK_EQ(v, reader.getUIntLat()); }
    reader.close();
}
////////////////////////////////////////////////////////////
TEST_CASE("NindFileTest.SIntLatRoundTripsAcrossAllTierBoundaries") {
    const vector<signed int> values = {
        0, 63, -64, -63,                          // 1 byte
        64, 8191, -65, -8192,                     // 2 bytes
        8192, 1048575, -8193, -1048576,           // 3 bytes
        1048576, 134217727, -1048577, -134217728, // 4 bytes
        134217728, 2147483647, -134217729, static_cast<signed int>(-2147483647 - 1)  // 5 bytes
    };

    TestTempDir tmp;
    const string path = tmp.file("slat.bin");

    NindFile writer(path);
    REQUIRE(writer.open("wb"));
    writer.createBuffer(values.size() * 5);
    for (signed int v : values) writer.putSIntLat(v);
    const unsigned int written = writer.getOutBufferSize();
    writer.writeBuffer();
    writer.close();

    NindFile reader(path);
    REQUIRE(reader.open("rb"));
    reader.setPos(0, SEEK_SET);   // open() leaves the position at EOF (it seeks there to measure the file)
    reader.readBuffer(written);
    for (signed int v : values) { INFO("value=" << v); CHECK_EQ(v, reader.getSIntLat()); }
    reader.close();
}
////////////////////////////////////////////////////////////
TEST_CASE("NindFileTest.StringRoundTrip") {
    TestTempDir tmp;
    const string path = tmp.file("strings.bin");
    const string s1 = "hello nind";
    const string s2 = "";
    const string s3 = string(254, 'x');   // maximum length accepted by putString
    const string bytes = "raw-bytes-no-length-prefix";

    NindFile writer(path);
    REQUIRE(writer.open("wb"));
    writer.createBuffer(1024);
    writer.putString(s1);
    writer.putString(s2);
    writer.putString(s3);
    writer.putStringAsBytes(bytes);
    const unsigned int written = writer.getOutBufferSize();
    writer.writeBuffer();
    writer.close();

    NindFile reader(path);
    REQUIRE(reader.open("rb"));
    reader.setPos(0, SEEK_SET);   // open() leaves the position at EOF (it seeks there to measure the file)
    reader.readBuffer(written);
    CHECK_EQ(s1, reader.getString());
    CHECK_EQ(s2, reader.getString());
    CHECK_EQ(s3, reader.getString());
    CHECK_EQ(bytes, reader.getStringAsBytes(static_cast<unsigned char>(bytes.length())));
    reader.close();
}
////////////////////////////////////////////////////////////
TEST_CASE("NindFileTest.PutStringRejectsTooLong") {
    TestTempDir tmp;
    NindFile writer(tmp.file("toolong.bin"));
    REQUIRE(writer.open("wb"));
    writer.createBuffer(1024);
    const string tooLong(255, 'x');   // putString's limit is 254
    CHECK_THROWS_AS(writer.putString(tooLong), OutWriteBufferException);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindFileTest.DirectUnbufferedReadMatchesBufferedWrite") {
    // NindFile exposes two independent reading APIs: the buffered one
    // (readBuffer + getXxx) and a direct one (readXxx, unbuffered, reads
    // straight from the file at the current position). Both must agree on
    // what a writer produced.
    TestTempDir tmp;
    const string path = tmp.file("direct.bin");

    NindFile writer(path);
    REQUIRE(writer.open("wb"));
    writer.createBuffer(64);
    writer.putInt1(42);
    writer.putInt3(1234567);
    writer.putUIntLat(99999);
    writer.putSIntLat(-99999);
    writer.putString("direct-read");
    writer.writeBuffer();
    writer.close();

    NindFile reader(path);
    REQUIRE(reader.open("rb"));
    reader.setPos(0, SEEK_SET);   // open() leaves the position at EOF (it seeks there to measure the file)
    CHECK_EQ(42u, reader.readInt1());
    CHECK_EQ(1234567u, reader.readInt3());
    CHECK_EQ(99999u, reader.readUIntLat());
    CHECK_EQ(-99999, reader.readSIntLat());
    CHECK_EQ("direct-read", reader.readString());
    reader.close();
}
////////////////////////////////////////////////////////////
TEST_CASE("NindFileTest.SeekAndTell") {
    TestTempDir tmp;
    const string path = tmp.file("seek.bin");

    NindFile writer(path);
    REQUIRE(writer.open("wb"));
    writer.createBuffer(8);
    writer.putInt4(1);
    writer.putInt4(2);
    writer.writeBuffer();
    writer.close();

    NindFile reader(path);
    REQUIRE(reader.open("rb"));
    CHECK_EQ(8, reader.getFileSize());
    reader.setPos(4, SEEK_SET);
    CHECK_EQ(4, reader.getPos());
    reader.readBuffer(4);
    CHECK_EQ(2u, reader.getInt4());
    reader.close();
}
////////////////////////////////////////////////////////////
TEST_CASE("NindFileTest.ReadPastEndOfFileThrowsEof") {
    TestTempDir tmp;
    const string path = tmp.file("short.bin");

    NindFile writer(path);
    REQUIRE(writer.open("wb"));
    writer.createBuffer(4);
    writer.putInt4(1);
    writer.writeBuffer();
    writer.close();

    NindFile reader(path);
    REQUIRE(reader.open("rb"));
    reader.setPos(0, SEEK_SET);   // open() leaves the position at EOF (it seeks there to measure the file)
    CHECK_THROWS_AS(reader.readBuffer(8), EofException);
}
////////////////////////////////////////////////////////////
TEST_CASE("NindFileTest.OpenMissingFileForReadingFails") {
    TestTempDir tmp;
    NindFile reader(tmp.file("does_not_exist.bin"));
    CHECK_FALSE(reader.open("rb"));
}
