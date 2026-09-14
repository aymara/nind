"""Round-trip and boundary tests for the low-level NindFile binary codec.

NindFile is the foundation every other nind format is built on: a single
binary encoding bug here (wrong endianness, wrong varint tier boundary...)
would silently corrupt every higher-level format, so its encode/decode pairs
are tested directly and exhaustively at every tier boundary.
"""
import struct

import pytest

from nind.NindFile import NindFile, clefA, clefB


def test_fixed_width_integers_round_trip(tmp_path):
    path = str(tmp_path / "ints.bin")
    nf = NindFile(path, enEjcriture=True)
    nf.ejcritNombre1(0xAB)
    nf.ejcritNombre3(0x123456)
    nf.ejcritNombre4(0x12345678)
    nf.ejcritNombre5(0x123456789A)
    nf.close()

    nf = NindFile(path)
    assert nf.litNombre1() == 0xAB
    assert nf.litNombre3() == 0x123456
    assert nf.litNombre4() == 0x12345678
    assert nf.litNombre5() == 0x123456789A
    nf.close()


def test_litNombre2_reads_big_endian(tmp_path):
    # No writer exists for Nombre2 (litNombre2 has no ejcrit counterpart), so
    # the raw bytes are written directly to pin down the documented
    # "gros-boutiste" (big-endian) byte order.
    path = tmp_path / "nombre2.bin"
    path.write_bytes(bytes([0x12, 0x34]))
    nf = NindFile(str(path))
    assert nf.litNombre2() == 0x1234
    nf.close()


def test_litNombreS3_and_litNombreS4_decode_twos_complement(tmp_path):
    path = tmp_path / "signed.bin"
    # NindFile's Nombre3/4 are little-endian ("petit-boutiste"); build the
    # raw bytes for two negative values by hand to check sign decoding.
    path.write_bytes(struct.pack("<i", -12345)[:3] + struct.pack("<i", -987654321))
    nf = NindFile(str(path))
    assert nf.litNombreS3() == -12345
    assert nf.litNombreS4() == -987654321
    nf.close()


def test_put_int3_offset_patch_pattern(tmp_path):
    # Exercises the "write a placeholder, seek back once the real value is
    # known" pattern used throughout nind_engine.py's writers.
    path = str(tmp_path / "patch.bin")
    nf = NindFile(path, enEjcriture=True)
    nf.ejcritNombre1(0xAB)
    placeholder_pos = nf.tell()
    nf.ejcritNombre3(0)
    nf.ejcritNombre1(0xCD)
    nf.seek(placeholder_pos, 0)
    nf.ejcritNombre3(999999)
    nf.seek(0, 2)
    nf.close()

    nf = NindFile(path)
    assert nf.litNombre1() == 0xAB
    assert nf.litNombre3() == 999999
    assert nf.litNombre1() == 0xCD
    nf.close()


ULAT_BOUNDARY_VALUES = [
    0, 1, 126, 127,                                    # 1 byte  : 0-127
    128, 129, 16382, 16383,                            # 2 bytes : 128-16383
    16384, 16385, 2097150, 2097151,                    # 3 bytes : 16384-2097151
    2097152, 2097153, 268435454, 268435455,            # 4 bytes : 2097152-268435455
    268435456, 268435457, 4294967294, 4294967295,      # 5 bytes : 268435456-4294967295
]


@pytest.mark.parametrize("value", ULAT_BOUNDARY_VALUES)
def test_ulat_round_trips_at_every_tier_boundary(tmp_path, value):
    path = str(tmp_path / "ulat.bin")
    nf = NindFile(path, enEjcriture=True)
    nf.ejcritNombreULat(value)
    nf.close()
    nf = NindFile(path)
    assert nf.litNombreULat() == value
    nf.close()


def test_ulat_rejects_values_above_32_bits(tmp_path):
    nf = NindFile(str(tmp_path / "toobig.bin"), enEjcriture=True)
    with pytest.raises(ValueError):
        nf.ejcritNombreULat(0x100000000)


SLAT_BOUNDARY_VALUES = [
    0, 63, -64, -63,                                      # 1 byte
    64, 8191, -65, -8192,                                 # 2 bytes
    8192, 1048575, -8193, -1048576,                       # 3 bytes
    1048576, 134217727, -1048577, -134217728,             # 4 bytes
    134217728, 2147483647, -134217729, -2147483648,       # 5 bytes
]


@pytest.mark.parametrize("value", SLAT_BOUNDARY_VALUES)
def test_slat_round_trips_at_every_tier_boundary(tmp_path, value):
    path = str(tmp_path / "slat.bin")
    nf = NindFile(path, enEjcriture=True)
    nf.ejcritNombreSLat(value)
    nf.close()
    nf = NindFile(path)
    assert nf.litNombreSLat() == value
    nf.close()


def test_slat_rejects_values_outside_32_bit_signed_range(tmp_path):
    nf = NindFile(str(tmp_path / "toobig.bin"), enEjcriture=True)
    with pytest.raises(ValueError):
        nf.ejcritNombreSLat(2147483648)
    with pytest.raises(ValueError):
        nf.ejcritNombreSLat(-2147483649)


def test_multiple_ulat_and_slat_values_in_sequence(tmp_path):
    # A realistic usage pattern: many varints written back to back, as the
    # higher-level formats do for postings lists and term-position deltas.
    path = str(tmp_path / "sequence.bin")
    ulat_values = [0, 5, 200, 90000, 5000000]
    slat_values = [0, -5, 200, -90000, 5000000]
    nf = NindFile(path, enEjcriture=True)
    for v in ulat_values:
        nf.ejcritNombreULat(v)
    for v in slat_values:
        nf.ejcritNombreSLat(v)
    nf.close()

    nf = NindFile(path)
    assert [nf.litNombreULat() for _ in ulat_values] == ulat_values
    assert [nf.litNombreSLat() for _ in slat_values] == slat_values
    nf.close()


def test_string_round_trip_with_length_prefix(tmp_path):
    # litString/ejcritChaine together implement the length-prefixed "MotUtf8"
    # pattern (1-byte length + utf-8 bytes) used for lexicon words.
    path = str(tmp_path / "string.bin")
    word = "épistémologie"
    encoded = word.encode("utf-8")
    nf = NindFile(path, enEjcriture=True)
    nf.ejcritNombre1(len(encoded))
    nf.ejcritChaine(word)
    nf.close()

    nf = NindFile(path)
    length = nf.litNombre1()
    assert length == len(encoded)
    assert nf.litChaine(length) == word
    nf.close()


def test_ejcritZejros_writes_the_requested_number_of_zero_bytes(tmp_path):
    path = tmp_path / "zeros.bin"
    nf = NindFile(str(path), enEjcriture=True)
    nf.ejcritNombre1(0xFF)
    nf.ejcritZejros(5)
    nf.ejcritNombre1(0xEE)
    nf.close()
    assert path.read_bytes() == bytes([0xFF, 0, 0, 0, 0, 0, 0xEE])


def test_clef_functions_are_deterministic_and_word_sensitive():
    assert clefA("alpha") == clefA("alpha")
    assert clefB("alpha") == clefB("alpha")
    assert clefA("alpha") != clefA("beta")
    assert clefB("alpha") != clefB("beta")
    # clefA and clefB are independent hash functions (used respectively for
    # debugging and for the actual bucket lookup) and need not agree.
    assert isinstance(clefA("alpha"), int)
    assert isinstance(clefB("alpha"), int)
