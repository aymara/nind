//
// NindBytes.h: byte assembly helpers shared by NindFile and NindReadBuffer (internal header)
//
// This file is part of NIND (as "nouvelle indexation").
// NIND is free software: you can redistribute it and/or modify it under the terms of the
// GNU Lesser General Public License (see <http://www.gnu.org/licenses/>), either version 3 of
// the License, or any later version.
////////////////////////////////////////////////////////////
#ifndef NindBytes_H
#define NindBytes_H
#include <cstdint>
namespace latecon {
    namespace nindex {
        namespace bytes {
//Les octets sont assemblejs en arithmejtique non signeje 32 bits : assembler en int
//(promotion des unsigned char) dejborde dehs que la valeur atteint 2^31 (comportement indejfini).
//Extension de signe d'une valeur non signeje de bitsNb bits, sans dejcalage de valeur nejgative.
static inline signed int signExtend(const uint32_t value, const unsigned int bitsNb)
{
    const uint32_t signBit = uint32_t(1) << (bitsNb - 1);
    return static_cast<int32_t>((value ^ signBit) - signBit);
}
static inline uint32_t be2(const unsigned char *b) { return (uint32_t(b[0]) << 8) | b[1]; }
static inline uint32_t be3(const unsigned char *b) { return (be2(b) << 8) | b[2]; }
static inline uint32_t be4(const unsigned char *b) { return (be3(b) << 8) | b[3]; }
static inline uint32_t le3(const unsigned char *b) { return (uint32_t(b[2]) << 16) | (uint32_t(b[1]) << 8) | b[0]; }
static inline uint32_t le4(const unsigned char *b) { return (uint32_t(b[3]) << 24) | le3(b); }
//Dejcode un entier latecon dont les nbBytes octets sont en bytes[0..nbBytes-1]
static inline uint32_t latBits(const unsigned char *b, const unsigned int bytesNb)
{
    switch (bytesNb) {
    case 1: return b[0] & 0x7F;
    case 2: return ((uint32_t(b[0]) & 0x3F) << 8) | b[1];
    case 3: return ((uint32_t(b[0]) & 0x1F) << 16) | (uint32_t(b[1]) << 8) | b[2];
    case 4: return ((uint32_t(b[0]) & 0x0F) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
    default: return be4(b + 1);
    }
}
        } // end namespace
    } // end namespace
} // end namespace
#endif
