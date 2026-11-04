//
// C++ Implementation: NindReadBuffer
//
// This file is part of NIND (as "nouvelle indexation").
// NIND is free software: you can redistribute it and/or modify it under the terms of the
// GNU Lesser General Public License (see <http://www.gnu.org/licenses/>), either version 3 of
// the License, or any later version.
////////////////////////////////////////////////////////////
#include "NindReadBuffer.h"
#include "NindBytes.h"
#include <new>
using namespace latecon::nindex;
using namespace latecon::nindex::bytes;
using namespace std;
////////////////////////////////////////////////////////////
NindReadBuffer::NindReadBuffer(const string *fileName):
    m_fileName(fileName),
    m_storage(0),
    m_capacity(0),
    m_buffer(0),
    m_absEnd(0),
    m_end(0),
    m_ptr(0)
{
}
////////////////////////////////////////////////////////////
const string &NindReadBuffer::noFileName()
{
    static const string none;
    return none;
}
////////////////////////////////////////////////////////////
NindReadBuffer::~NindReadBuffer()
{
    delete [] m_storage;
}
////////////////////////////////////////////////////////////
void NindReadBuffer::clear()
{
    m_buffer = m_absEnd = m_end = m_ptr = 0;
}
////////////////////////////////////////////////////////////
unsigned char *NindReadBuffer::prepare(const unsigned int bytesNb)
{
    clear();
    //rejutilise la place si elle suffit (shrink() libehre une grande place)
    if (bytesNb > m_capacity) {
        delete [] m_storage;
        m_storage = 0;
        m_capacity = 0;
        const size_t capacity = (bytesNb == 0) ? 1 : bytesNb;
        m_storage = new (nothrow) unsigned char[capacity];
        if (m_storage == 0) throw BadAllocException("in read buffer " + getFileName());
        m_capacity = capacity;
    }
    m_buffer = m_storage;
    m_absEnd = m_buffer + bytesNb;
    m_end = m_absEnd;
    m_ptr = m_buffer;
    return m_buffer;
}
////////////////////////////////////////////////////////////
void NindReadBuffer::shrink()
{
    if (m_capacity <= MAX_KEPT_CAPACITY) return;
    clear();
    delete [] m_storage;
    m_storage = 0;
    m_capacity = 0;
}
////////////////////////////////////////////////////////////
//brief Set logical end of read buffer specifying offset from current read pointer
//param bytesNb offset from current read pointer */
void NindReadBuffer::setEndInBuffer(const unsigned int bytesNb)
{
    if (bytesNb > (size_t)(m_absEnd - m_ptr)) throw OutReadBufferException("in read buffer (A) " + getFileName());
    m_end = m_ptr + bytesNb;
}
////////////////////////////////////////////////////////////
//brief Set logical end of read buffer specifying offset from head of buffer
//param bytesNb offset from head of buffer */
void NindReadBuffer::setAbsEndInBuffer(const unsigned int bytesNb)
{
    if (bytesNb > (size_t)(m_absEnd - m_buffer)) throw OutReadBufferException("in read buffer (AA) " + getFileName());
    m_end = m_buffer + bytesNb;
}
////////////////////////////////////////////////////////////
//brief Get a 1-bytes integer from internal buffer
//return 4-bytes integer */
unsigned int NindReadBuffer::getInt1()
{
    if (m_end - m_ptr < 1) throw OutReadBufferException("in read buffer (B) " + getFileName());
    m_ptr += 1;
    return m_ptr[-1];
}
////////////////////////////////////////////////////////////
//brief Get a 2-bytes integer from internal buffer
//return 4-bytes integer */
unsigned int NindReadBuffer::getInt2()
{
    //gros-boutiste
    if (m_end - m_ptr < 2) throw OutReadBufferException("in read buffer (C) " + getFileName());
    m_ptr += 2;
    return be2(m_ptr - 2);
}
////////////////////////////////////////////////////////////
//brief Get a 3-bytes integer from internal buffer
//return 4-bytes integer */
unsigned int NindReadBuffer::getInt3()
{
    //petit boutiste
    if (m_end - m_ptr < 3) throw OutReadBufferException("in read buffer (D) " + getFileName());
    m_ptr += 3;
    return le3(m_ptr - 3);
}
////////////////////////////////////////////////////////////
//brief Get a 3-bytes signed integer from internal buffer
//return 4-bytes signed integer */
signed int NindReadBuffer::getSInt3()
{
    //petit boutiste
    if (m_end - m_ptr < 3) throw OutReadBufferException("in read buffer (D2) " + getFileName());
    m_ptr += 3;
    return signExtend(le3(m_ptr - 3), 24);
}
////////////////////////////////////////////////////////////
//brief Get a 4-bytes integer from internal buffer
//return 4-bytes integer */
unsigned int NindReadBuffer::getInt4()
{
    //petit boutiste
    if (m_end - m_ptr < 4) throw OutReadBufferException("in read buffer (E) " + getFileName());
    m_ptr += 4;
    return le4(m_ptr - 4);
}
////////////////////////////////////////////////////////////
//brief Get a 4-bytes integer from internal buffer
//return 4-bytes integer */
signed int NindReadBuffer::getSInt4()
{
    //petit boutiste
    if (m_end - m_ptr < 4) throw OutReadBufferException("in read buffer (E) " + getFileName());
    m_ptr += 4;
    return signExtend(le4(m_ptr - 4), 32);
}
////////////////////////////////////////////////////////////
//brief Get a 5-bytes integer from internal buffer
//return 8-bytes integer */
uint64_t NindReadBuffer::getInt5()
{
    //gros boutiste
    if (m_end - m_ptr < 5) throw OutReadBufferException("in read buffer (F) " + getFileName());
    m_ptr += 5;
    //la valeur peut depasser 32 bits : force le calcul en 64 bits des le premier terme
    return (static_cast<uint64_t>(m_ptr[-5]) << 32) | be4(m_ptr - 4);
}
////////////////////////////////////////////////////////////
uint64_t NindReadBuffer::getInt6()
{
    //gros boutiste
    if (m_end - m_ptr < 6) throw OutReadBufferException("in read buffer (F6) " + getFileName());
    m_ptr += 6;
    return (static_cast<uint64_t>(be2(m_ptr - 6)) << 32) | be4(m_ptr - 4);
}
////////////////////////////////////////////////////////////
uint64_t NindReadBuffer::getInt8()
{
    //gros boutiste
    if (m_end - m_ptr < 8) throw OutReadBufferException("in read buffer (F8) " + getFileName());
    m_ptr += 8;
    return (static_cast<uint64_t>(be4(m_ptr - 8)) << 32) | be4(m_ptr - 4);
}
////////////////////////////////////////////////////////////
uint64_t NindReadBuffer::getUIntLat64()
{
    //EntierULat64 : 0xF8 (11111000, refusej par getUIntLat) suivi de 8 octets, sinon un EntierULat
    if (m_end - m_ptr < 1) throw OutReadBufferException("in read buffer (G64) " + getFileName());
    if (m_ptr[0] != 0xF8) return getUIntLat();
    m_ptr += 1;
    const uint64_t value = getInt8();
    //forme canonique : une valeur sous 2^32 s'ejcrit en EntierULat
    if (value <= 0xFFFFFFFFull) throw OutReadBufferException("in read buffer (H64) " + getFileName());
    return value;
}
////////////////////////////////////////////////////////////
//brief Get an unsigned latecon integer from internal buffer
//return 4-bytes unsigned integer */
unsigned int NindReadBuffer::getUIntLat()
{
    //il faut que ca aille vite pour les petits nombres
    const unsigned char *bytes = m_ptr;
    const ptrdiff_t available = m_end - m_ptr;
    if (available < 1) throw OutReadBufferException("in read buffer (G) " + getFileName());
    //0-2^7-1 : 0-127
    if (!(bytes[0]&0x80)) { m_ptr += 1; return latBits(bytes, 1); }
    if (available < 2) throw OutReadBufferException("in read buffer (H) " + getFileName());
    //2^7-2^14-1 : 128-16383
    if (!(bytes[0]&0x40)) { m_ptr += 2; return latBits(bytes, 2); }
    if (available < 3) throw OutReadBufferException("in read buffer (I) " + getFileName());
    //2^14-2^21-1 : 16384-2097151
    if (!(bytes[0]&0x20)) { m_ptr += 3; return latBits(bytes, 3); }
    if (available < 4) throw OutReadBufferException("in read buffer (J) " + getFileName());
    //2^21-2^28 : 2097152-268435455
    if (!(bytes[0]&0x10)) { m_ptr += 4; return latBits(bytes, 4); }
    if (available < 5) throw OutReadBufferException("in read buffer (K) " + getFileName());
    if (bytes[0]&0x08) throw OutReadBufferException("in read buffer (L) " + getFileName());
    //2^28-2^32-1 : 268435456-4294967295
    m_ptr += 5;
    return latBits(bytes, 5);
}
////////////////////////////////////////////////////////////
//brief Get a signed latecon integer from internal buffer
//return 4-bytes signed integer */
signed int NindReadBuffer::getSIntLat()
{
    //il faut que ca aille vite pour les petits nombres
    const unsigned char *bytes = m_ptr;
    const ptrdiff_t available = m_end - m_ptr;
    if (available < 1) throw OutReadBufferException("in read buffer (M) " + getFileName());
    //de -2^6 a 2^6-1 : de -64 a 63
    if (!(bytes[0]&0x80)) { m_ptr += 1; return signExtend(latBits(bytes, 1), 7); }
    if (available < 2) throw OutReadBufferException("in read buffer (N) " + getFileName());
    //de -2^13 a -2^6-1 et de 2^6 a 2^13-1 : de -8192 a -65 et de 64 a 8191
    if (!(bytes[0]&0x40)) { m_ptr += 2; return signExtend(latBits(bytes, 2), 14); }
    if (available < 3) throw OutReadBufferException("in read buffer (O) " + getFileName());
    //de -2^20 a -2^13-1 et de 2^13 a 2^20-1 : de -1048576 a -8193 et de 8192 a 1048575
    if (!(bytes[0]&0x20)) { m_ptr += 3; return signExtend(latBits(bytes, 3), 21); }
    if (available < 4) throw OutReadBufferException("in read buffer (P) " + getFileName());
    //de -2^27 a -2^20-1 et de 2^20 a 2^27-1 : de -134217728 a -1048577 et de 1048576 a 134217727
    if (!(bytes[0]&0x10)) { m_ptr += 4; return signExtend(latBits(bytes, 4), 28); }
    if (available < 5) throw OutReadBufferException("in read buffer (Q) " + getFileName());
    if (bytes[0]&0x08) throw OutReadBufferException("in read buffer (R) " + getFileName());
    //de -2^31 a -2^27-1 et de 2^27 a 2^31-1 : de -2147483648 a -134217729 et de 134217728 a 2147483647
    m_ptr += 5;
    return signExtend(latBits(bytes, 5), 32);
}
////////////////////////////////////////////////////////////
//brief Get a string from internal buffer
//return read string */
string NindReadBuffer::getString()
{
    //lit la longueur de la chaine
    if (m_end - m_ptr < 1) throw OutReadBufferException("in read buffer (S) " + getFileName());
    const unsigned char length = *m_ptr++;
    //lit la chaine
    if (m_end - m_ptr < length) throw OutReadBufferException("in read buffer (T) " + getFileName());
    m_ptr += length;
    return string((char*)(m_ptr-length), length);
}
////////////////////////////////////////////////////////////
//brief Get a string from internal buffer
//param length number of bytes to read (bytes, not chars)
//return read string */
string NindReadBuffer::getStringAsBytes(const unsigned char length)
{
    //lit la chaine
    if (m_end - m_ptr < length) throw OutReadBufferException("in read buffer (U) " + getFileName());
    m_ptr += length;
    return string((char*)(m_ptr-length), length);
}
