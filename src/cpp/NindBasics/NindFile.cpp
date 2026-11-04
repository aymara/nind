//
// C++ Implementation: NindFile
//
// Description: Les acces basiques a un fichier binaire sequentiel avec possibilites de bufferisation
// Le fichier est ecrit en little-endian
//
// Author: jys <jy.sage@orange.fr>, (C) LATEJCON 2017
//
// Copyright: 2014-2017 LATEJCON. See LICENCE.md file that comes with this distribution
// This file is part of NIND (as "nouvelle indexation").
// NIND is free software: you can redistribute it and/or modify it under the terms of the
// GNU Less General Public License (LGPL) as published by the Free Software Foundation,
// (see <http://www.gnu.org/licenses/>), either version 3 of the License, or any later version.
// NIND is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
// even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Less General Public License for more details.
////////////////////////////////////////////////////////////
#include "NindFile.h"
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/file.h>
#include <unistd.h>
#endif
//#include <iostream>
using namespace latecon::nindex;
using namespace std;
////////////////////////////////////////////////////////////
//taille de lecture au-delah de laquelle readBuffer() verifie d'abord la taille rejelle du fichier
#define READ_SIZE_CHECK_THRESHOLD 65536
////////////////////////////////////////////////////////////
#include "NindBytes.h"
using namespace latecon::nindex::bytes;
////////////////////////////////////////////////////////////
// <fichier>              ::= { <Entier1> | <Entier2> | <Entier3> | <Entier4> | <Entier5> |
//                              <EntierULat> | <EntierULat> |
//                              <MotUtf8> | <Utf8> | <Octet> }
// <MotUtf8>               ::= <longueur> <Utf8>
// <longueur>              ::= <Entier1>
// <Utf8>                  ::= { <Octet> }
// <Entier1>               ::= <Octet>
// <Entier2>               ::= <Octet> <Octet>
// <Entier3>               ::= <Octet> <Octet> <Octet>
// <Entier4>               ::= <Octet> <Octet> <Octet> <Octet>
// <Entier5>               ::= <Octet> <Octet> <Octet> <Octet> <Octet>
// <EntierULat>            ::= { <Octet> }
// <EntierSLat>            ::= { <Octet> }
////////////////////////////////////////////////////////////
//brief Creates NindFile with a specified name associated with.
//param fileName absolute path file name
//param bufferSize size of writing buffer (0 if just reader) */
NindFile::NindFile(const string &fileName):
    m_fileName(fileName),
    m_file(0),
    m_wbuffer(0),
    m_wbufferEnd(0),
    m_wPtr(0),
    m_in(&m_fileName),
    m_readHandle(-1),
    m_readHandleMutex(),
    m_fileSize(0),
    m_isLittleEndian(false),
    m_isBulk(false),
    m_nindSignalCatcher(NindSignalCatcher::Instance())
{
    //teste si la plateforme est little endian ou big endian
    const unsigned int testLittleEndian = 13;
    m_isLittleEndian = (testLittleEndian == (*(unsigned char*)&testLittleEndian)) ; // vrai si little endian (inutile)
    //cout<<"m_isLittleEndian="<<m_isLittleEndian<<endl;
}
////////////////////////////////////////////////////////////
NindFile::~NindFile()
{
    close();
    if (m_wbuffer != 0) {
        delete [] m_wbuffer;
        m_wbuffer = 0;
    }
}
////////////////////////////////////////////////////////////
// Begin a critical section where control-C will be temporaly catched. */
void NindFile::beginCriticalSection() {
    m_nindSignalCatcher->setCatcher();
}
////////////////////////////////////////////////////////////
// End a critical section where control-C have been temporaly catched. */
void NindFile::endCriticalSection(){
    m_nindSignalCatcher->resetCatcher();
}
////////////////////////////////////////////////////////////
//brief Open file associated with. No exceptions are raised.
//param accessMode authorized access modes are "rb", "wb", "ab", "r+b", "w+b", "a+b", "rb+", "wb+", "ab+"
//return true if success, false otherwise */
bool NindFile::open(const string &accessMode)
{
    //ouvre le fichier
    m_file =  fopen(m_fileName.c_str(), accessMode.c_str());
    if (!m_file) return false;

    //memorise sa taille
    fseek(m_file, 0, SEEK_END);
    m_fileSize = ftell(m_file);
    return true;
}
////////////////////////////////////////////////////////////
//brief Take an advisory exclusive lock on the open file, without waiting
//return true if the lock was taken, false if another open of the file holds it */
bool NindFile::lockExclusive()
{
#ifdef _WIN32
    return true;
#else
    //flock est par description de fichier ouvert : refuse aussi un 2e ejcrivain du mesme processus
    return flock(fileno(m_file), LOCK_EX | LOCK_NB) == 0;
#endif
}
////////////////////////////////////////////////////////////
//brief Close file associated with. Success in any case. */
void NindFile::close()
{
    if (m_file) {
        fclose(m_file);
        m_file = 0;
    }
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(m_readHandleMutex);
    const intptr_t handle = m_readHandle.exchange(-1);
    if (handle != -1) CloseHandle((HANDLE)handle);
#endif
}
////////////////////////////////////////////////////////////
//le descripteur des lectures positionnejes (readAt) : elles ne doivent pas dejplacer la position
//du FILE* de l'API sejquentielle. POSIX : pread ne la dejplace pas, c'est donc celui du FILE*
//(pas de 2e descripteur, et un fichier supprimej depuis son ouverture reste lisible). Windows :
//ReadFile avec un dejcalage dejplace le pointeur du handle, d'oug un handle ah lui, ouvert au
//premier besoin (un fichier ouvert n'y est pas supprimable). -1 si aucun.
intptr_t NindFile::readHandle() const
{
#ifndef _WIN32
    return (m_file != 0) ? (intptr_t)fileno(m_file) : -1;
#else
    intptr_t handle = m_readHandle.load(std::memory_order_acquire);
    if (handle != -1) return handle;
    std::lock_guard<std::mutex> lock(m_readHandleMutex);
    handle = m_readHandle.load(std::memory_order_acquire);
    if (handle != -1 || m_file == 0) return handle;
    HANDLE opened = CreateFileA(m_fileName.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    handle = (opened == INVALID_HANDLE_VALUE) ? -1 : (intptr_t)opened;
    m_readHandle.store(handle, std::memory_order_release);
    return handle;
#endif
}
////////////////////////////////////////////////////////////
//brief Size of the file now, from the read handle (thread-safe)
uint64_t NindFile::getReadFileSize() const
{
    const intptr_t handle = readHandle();
    if (handle == -1) throw ReadFileException(m_fileName);
#ifdef _WIN32
    LARGE_INTEGER size;
    if (!GetFileSizeEx((HANDLE)handle, &size)) throw ReadFileException(m_fileName);
    return (uint64_t)size.QuadPart;
#else
    struct stat fileStat;
    if (fstat((int)handle, &fileStat) != 0) throw ReadFileException(m_fileName);
    return (uint64_t)fileStat.st_size;
#endif
}
////////////////////////////////////////////////////////////
//brief Read bytesNb bytes at offset into buffer, without moving the file position (thread-safe)
void NindFile::readAt(const uint64_t offset,
                      const unsigned int bytesNb,
                      NindReadBuffer &buffer) const
{
    buffer.setFileName(&m_fileName);
    buffer.clear();
    const intptr_t handle = readHandle();
    if (handle == -1) throw ReadFileException(m_fileName);
    //une longueur lue dans un fichier corrompu ne doit pas provoquer une allocation dejmesureje
    if (bytesNb > READ_SIZE_CHECK_THRESHOLD && offset + bytesNb > getReadFileSize())
        throw EofException(m_fileName);
    unsigned char *bytes = buffer.prepare(bytesNb);
    unsigned int done = 0;
    while (done < bytesNb) {
        const uint64_t position = offset + done;
#ifdef _WIN32
        OVERLAPPED overlapped;
        memset(&overlapped, 0, sizeof(overlapped));
        overlapped.Offset = (DWORD)(position & 0xFFFFFFFFu);
        overlapped.OffsetHigh = (DWORD)(position >> 32);
        DWORD readSize = 0;
        if (!ReadFile((HANDLE)handle, bytes + done, (DWORD)(bytesNb - done), &readSize, &overlapped)) {
            buffer.clear();
            if (GetLastError() == ERROR_HANDLE_EOF) throw EofException(m_fileName);
            throw ReadFileException(m_fileName);
        }
#else
        const ssize_t readSize = pread((int)handle, bytes + done, bytesNb - done, (off_t)position);
        if (readSize < 0) {
            if (errno == EINTR) continue;
            buffer.clear();
            throw ReadFileException(m_fileName);
        }
#endif
        if (readSize == 0) {
            buffer.clear();
            throw EofException(m_fileName);
        }
        done += (unsigned int)readSize;
    }
}
////////////////////////////////////////////////////////////
//brief Get the actual size of the file on disk, as it is now
//return file size in bytes */
uint64_t NindFile::getCurrentFileSize()
{
    //pas de fflush : toutes les ejcritures de NindFile sont dejjah suivies d'un fflush
    struct stat fileStat;
    if (fstat(fileno(m_file), &fileStat) != 0) throw ReadFileException(m_fileName);
    return (uint64_t)fileStat.st_size;
}
////////////////////////////////////////////////////////////
//brief Read a single byte from the file
//return byte value */
unsigned char NindFile::readInt1()
{
    unsigned char byte;
    readBytes(&byte, 1);
    return byte;
}
////////////////////////////////////////////////////////////
//brief Read a 3-bytes integer from the file
//return 4-bytes integer */
unsigned int NindFile::readInt3()
{
    //petit boutiste
    unsigned char bytes[3];
    readBytes(bytes, 3);
    return le3(bytes);
}
////////////////////////////////////////////////////////////
//brief Get an unsigned latecon integer from the file
//return 4-bytes unsigned integer */
unsigned int NindFile::readUIntLat()
{
    unsigned char bytes[5];
    readBytes(bytes, 1);
    //0-2^7-1 : 0-127
    if (!(bytes[0]&0x80)) return latBits(bytes, 1);
    readBytes(bytes+1, 1);
    //2^7-2^14-1 : 128-16383
    if (!(bytes[0]&0x40)) return latBits(bytes, 2);
    readBytes(bytes+2, 1);
    //2^14-2^21-1 : 16384-2097151
    if (!(bytes[0]&0x20)) return latBits(bytes, 3);
    readBytes(bytes+3, 1);
    //2^21-2^28 : 2097152-268435455
    if (!(bytes[0]&0x10)) return latBits(bytes, 4);
    readBytes(bytes+4, 1);
    if (bytes[0]&0x08) throw FormatFileException(m_fileName);
    //2^28-2^32-1 : 268435456-4294967295
    return latBits(bytes, 5);
}
////////////////////////////////////////////////////////////
//brief Get a signed latecon integer from the file
//return 4-bytes signed integer */
signed int NindFile::readSIntLat()
{
    unsigned char bytes[5];
    readBytes(bytes, 1);
    //de -2^6 a 2^6-1 : de -64 a 63
    if (!(bytes[0]&0x80)) return signExtend(latBits(bytes, 1), 7);
    readBytes(bytes+1, 1);
    //de -2^13 a -2^6-1 et de 2^6 a 2^13-1 : de -8192 a -65 et de 64 a 8191
    if (!(bytes[0]&0x40)) return signExtend(latBits(bytes, 2), 14);
    readBytes(bytes+2, 1);
    //de -2^20 a -2^13-1 et de 2^13 a 2^20-1 : de -1048576 a -8193 et de 8192 a 1048575
    if (!(bytes[0]&0x20)) return signExtend(latBits(bytes, 3), 21);
    readBytes(bytes+3, 1);
    //de -2^27 a -2^20-1 et de 2^20 a 2^27-1 : de -134217728 a -1048577 et de 1048576 a 134217727
    if (!(bytes[0]&0x10)) return signExtend(latBits(bytes, 4), 28);
    readBytes(bytes+4, 1);
    if (bytes[0]&0x08) throw FormatFileException(m_fileName);
    //de -2^31 a -2^27-1 et de 2^27 a 2^31-1 : de -2147483648 a -134217729 et de 134217728 a 2147483647
    return signExtend(latBits(bytes, 5), 32);
}
////////////////////////////////////////////////////////////
//brief Read a string from the file
//return read string */
string NindFile::readString()
{
    //lit la longueur de la chaine
    unsigned char len;
    readBytes(&len, 1);
    //lit la chaine
    unsigned char bytes[256];
    readBytes(bytes, (unsigned int)len);
    return string((char*)bytes, len);
}
////////////////////////////////////////////////////////////
//brief Read bytes from the file into an internal buffer
//param bytesNb size of internal buffer to receive read datas */
void NindFile::readBuffer(const unsigned int bytesNb)
{
    //oublie l'ancien contenu d'abord : en cas d'ejchec ci-dessous, pas d'anciennes donnejes relues
    //comme si c'ejtaient les nouvelles
    m_in.clear();
    //une longueur lue dans un fichier corrompu ne doit pas provoquer une allocation dejmesureje :
    //au-delah d'un seuil, verifie que le fichier contient bien les octets demandejs
    if (bytesNb > READ_SIZE_CHECK_THRESHOLD) {
        const long int pos = ftell(m_file);
        struct stat fileStat;
        if (pos < 0 || fstat(fileno(m_file), &fileStat) != 0) throw ReadFileException(m_fileName);
        if ((uint64_t)pos + bytesNb > (uint64_t)fileStat.st_size) throw EofException(m_fileName);
    }
    //lit le fichier dans le buffer de lecture
    readBytes(m_in.prepare(bytesNb), bytesNb);
}
////////////////////////////////////////////////////////////
//the read buffer and its decoders are NindReadBuffer's (see NindReadBuffer.h)
void NindFile::setEndInBuffer(const unsigned int bytesNb) { m_in.setEndInBuffer(bytesNb); }
void NindFile::setAbsEndInBuffer(const unsigned int bytesNb) { m_in.setAbsEndInBuffer(bytesNb); }
unsigned int NindFile::getInt1() { return m_in.getInt1(); }
unsigned int NindFile::getInt2() { return m_in.getInt2(); }
unsigned int NindFile::getInt3() { return m_in.getInt3(); }
signed int NindFile::getSInt3() { return m_in.getSInt3(); }
unsigned int NindFile::getInt4() { return m_in.getInt4(); }
signed int NindFile::getSInt4() { return m_in.getSInt4(); }
uint64_t NindFile::getInt5() { return m_in.getInt5(); }
uint64_t NindFile::getInt6() { return m_in.getInt6(); }
uint64_t NindFile::getInt8() { return m_in.getInt8(); }
uint64_t NindFile::getUIntLat64() { return m_in.getUIntLat64(); }
unsigned int NindFile::getUIntLat() { return m_in.getUIntLat(); }
signed int NindFile::getSIntLat() { return m_in.getSIntLat(); }
string NindFile::getString() { return m_in.getString(); }
string NindFile::getStringAsBytes(const unsigned char length) { return m_in.getStringAsBytes(length); }
////////////////////////////////////////////////////////////
//brief Create an internal buffer for writing
//param bytesNb size of buffer */
void NindFile::createBuffer(const unsigned int bytesNb)
{
    if (m_wbuffer != 0) delete [] m_wbuffer;
    m_wbuffer = new (nothrow) unsigned char[bytesNb];
    if (m_wbuffer == 0) throw BadAllocException("in write buffer" + m_fileName);
    m_wbufferEnd = m_wbuffer + bytesNb;
    m_wPtr = m_wbuffer;
}
////////////////////////////////////////////////////////////
//brief Put padding into the internal buffer
//param bytesNb number of padding bytes to write */
void NindFile::putPad(const unsigned int bytesNb)
{
    if (m_wbufferEnd - m_wPtr < bytesNb) throw OutWriteBufferException("in write buffer (A) " + m_fileName);
    m_wPtr += bytesNb;
}
////////////////////////////////////////////////////////////
//brief Put one byte into the internal buffer
//param int1 byte value to write */
void NindFile::putInt1(const unsigned char int1)
{
    if (m_wbufferEnd - m_wPtr < 1) throw OutWriteBufferException("in write buffer (B) " + m_fileName);
    m_wPtr += 1;
    m_wPtr[-1] = int1;
}
////////////////////////////////////////////////////////////
//brief Put a 2-bytes integer into the internal buffer
//param int4 integer to write as 2-bytes*/
void NindFile::putInt2(const unsigned int int4)
{
    //gros-boutiste
    if (m_wbufferEnd - m_wPtr < 2) throw OutWriteBufferException("in write buffer (C) " + m_fileName);
    m_wPtr += 2;
    //en big-endian quelle que soit la plate-forme
    m_wPtr[-1] = (int4)&0xFF;
    m_wPtr[-2] = (int4>>8)&0xFF;
}
////////////////////////////////////////////////////////////
//brief Put a 3-bytes integer into the internal buffer
//param int4 integer to write as 3-bytes*/
void NindFile::putInt3(const unsigned int int4)
{
    //a value over 3 bytes would be written truncated: a corrupt file (e.g. a definition over 16 MiB)
    if (int4 > 0xFFFFFFu) throw OutOfBoundException("value over 3 bytes " + m_fileName);
    //petit boutiste
    if (m_wbufferEnd - m_wPtr < 3) throw OutWriteBufferException("in write buffer (D) " + m_fileName);
    m_wPtr += 3;
    //en little-endian quelle que soit la plate-forme
    m_wPtr[-3] = (int4)&0xFF;
    m_wPtr[-2] = (int4>>8)&0xFF;
    m_wPtr[-1] = (int4>>16)&0xFF;
}
////////////////////////////////////////////////////////////
//brief Put a 3-bytes integer into the internal buffer at the specified offset
//param int4 integer to write as 3-bytes
//param offset where to write the 3 bytes into the buffer*/
void NindFile::putInt3(const unsigned int int4,
                       const unsigned int offset)
{
    if (int4 > 0xFFFFFFu) throw OutOfBoundException("value over 3 bytes " + m_fileName);
    //petit boutiste
    if ((uint64_t)offset + 3 > (uint64_t)(m_wbufferEnd - m_wbuffer))
        throw OutWriteBufferException("in write buffer (E) " + m_fileName);
    unsigned char *wPtr = m_wbuffer + offset + 3;
    //en little-endian quelle que soit la plate-forme
    wPtr[-3] = (int4)&0xFF;
    wPtr[-2] = (int4>>8)&0xFF;
    wPtr[-1] = (int4>>16)&0xFF;
}
////////////////////////////////////////////////////////////
//brief Put a 4-bytes integer into the internal buffer
//param int4 integer to write as 4-bytes*/
void NindFile::putInt4(const uint64_t int4)
{
    //document ids are 64 bits: a v1 field of 4 bytes refuses what it can't hold
    if (int4 > 0xFFFFFFFFull) throw OutOfBoundException("value over 4 bytes " + m_fileName);
    //petit boutiste
    if (m_wbufferEnd - m_wPtr < 4) throw OutWriteBufferException("in write buffer (F) " + m_fileName);
    m_wPtr += 4;
    //en little-endian quelle que soit la plate-forme
    m_wPtr[-4] = (int4)&0xFF;
    m_wPtr[-3] = (int4>>8)&0xFF;
    m_wPtr[-2] = (int4>>16)&0xFF;
    m_wPtr[-1] = (int4>>24)&0xFF;
}
////////////////////////////////////////////////////////////
//brief Put a 5-bytes integer into the internal buffer
//param int8 long integer to write as 5-bytes*/
void NindFile::putInt5(const uint64_t int8)
{
    //a value over 5 bytes would be written truncated (e.g. an offset past 1 TiB)
    if (int8 > 0xFFFFFFFFFFull) throw OutOfBoundException("value over 5 bytes " + m_fileName);
    //gros boutiste
    if (m_wbufferEnd - m_wPtr < 5) throw OutWriteBufferException("in write buffer (G) " + m_fileName);
    m_wPtr += 5;
    //en big-endian quelle que soit la plate-forme
    m_wPtr[-1] = (int8)&0xFF;
    m_wPtr[-2] = (int8>>8)&0xFF;
    m_wPtr[-3] = (int8>>16)&0xFF;
    m_wPtr[-4] = (int8>>24)&0xFF;
    m_wPtr[-5] = (int8>>32)&0xFF;
}
////////////////////////////////////////////////////////////
void NindFile::putInt6(const uint64_t int8)
{
    if (int8 > 0xFFFFFFFFFFFFull) throw OutOfBoundException("value over 6 bytes " + m_fileName);
    if (m_wbufferEnd - m_wPtr < 6) throw OutWriteBufferException("in write buffer (G6) " + m_fileName);
    m_wPtr += 6;
    for (int i = 1; i <= 6; i++) m_wPtr[-i] = (unsigned char)((int8 >> (8 * (i - 1))) & 0xFF);
}
////////////////////////////////////////////////////////////
void NindFile::putInt8(const uint64_t int8)
{
    if (m_wbufferEnd - m_wPtr < 8) throw OutWriteBufferException("in write buffer (G8) " + m_fileName);
    m_wPtr += 8;
    for (int i = 1; i <= 8; i++) m_wPtr[-i] = (unsigned char)((int8 >> (8 * (i - 1))) & 0xFF);
}
////////////////////////////////////////////////////////////
void NindFile::putInt4(const uint64_t int4,
                       const unsigned int offset)
{
    if (int4 > 0xFFFFFFFFull) throw OutOfBoundException("value over 4 bytes " + m_fileName);
    if ((uint64_t)offset + 4 > (uint64_t)(m_wbufferEnd - m_wbuffer))
        throw OutWriteBufferException("in write buffer (E4) " + m_fileName);
    unsigned char *wPtr = m_wbuffer + offset;
    //en little-endian quelle que soit la plate-forme, comme putInt4
    for (int i = 0; i < 4; i++) wPtr[i] = (unsigned char)((int4 >> (8 * i)) & 0xFF);
}
////////////////////////////////////////////////////////////
void NindFile::putUIntLat64(const uint64_t int8)
{
    if (int8 <= 0xFFFFFFFFull) {
        putUIntLat(int8);
        return;
    }
    putInt1(0xF8);
    putInt8(int8);
}
////////////////////////////////////////////////////////////
//brief Put an unsigned latecon integer into the internal buffer
//param int4 unsigned integer to write as latecon integer*/
void NindFile::putUIntLat(const uint64_t int4)
{
    //a latecon integer holds 32 bits: refuses what it can't hold (EntierULat64 is v2's)
    if (int4 > 0xFFFFFFFFull) throw OutOfBoundException("value over 32 bits as latecon integer " + m_fileName);
    //il faut que ca aille vite pour les petits nombres
    if (m_wbufferEnd - m_wPtr < 1) throw OutWriteBufferException("in write buffer (H) " + m_fileName);
    m_wPtr += 1;
    //0-2^7-1 : 0-127
    if (int4 < 0x80) {
        m_wPtr[-1] = int4;
        return;
    }
    if (m_wbufferEnd - m_wPtr < 1) throw OutWriteBufferException("in write buffer (I) " + m_fileName);
    m_wPtr += 1;
    //2^7-2^14-1 : 128-16383
    if (int4 < 0x4000) {
        m_wPtr[-1] = (int4)&0xFF;
        m_wPtr[-2] = (int4>>8)|0x80;
        return;
    }
    if (m_wbufferEnd - m_wPtr < 1) throw OutWriteBufferException("in write buffer (J) " + m_fileName);
    m_wPtr += 1;
    //2^14-2^21-1 : 16384-2097151
    if (int4 < 0x200000) {
        m_wPtr[-1] = (int4)&0xFF;
        m_wPtr[-2] = (int4>>8)&0xFF;
        m_wPtr[-3] = (int4>>16)|0xC0;
        return;
    }
    if (m_wbufferEnd - m_wPtr < 1) throw OutWriteBufferException("in write buffer (K) " + m_fileName);
    m_wPtr += 1;
    //2^21-2^28 : 2097152-268435455
    if (int4 < 0x010000000) {
        m_wPtr[-1] = (int4)&0xFF;
        m_wPtr[-2] = (int4>>8)&0xFF;
        m_wPtr[-3] = (int4>>16)&0xFF;
        m_wPtr[-4] = (int4>>24)|0xE0;
        return;
    }
     if (m_wbufferEnd - m_wPtr < 1) throw OutWriteBufferException("in write buffer (L) " + m_fileName);
     m_wPtr += 1;
    //2^28-2^32-1 : 268435456-4294967295
    m_wPtr[-1] = (int4)&0xFF;
    m_wPtr[-2] = (int4>>8)&0xFF;
    m_wPtr[-3] = (int4>>16)&0xFF;
    m_wPtr[-4] = (int4>>24)&0xFF;
    m_wPtr[-5] = 0xF0;
}
////////////////////////////////////////////////////////////
//brief Put a signed latecon integer into the internal buffer
//param int4 signed integer to write as latecon integer*/
void NindFile::putSIntLat(const signed int int4)
{
    //il faut que ca aille vite pour les petits nombres
    if (m_wbufferEnd - m_wPtr < 1) throw OutWriteBufferException("in write buffer (M) " + m_fileName);
    m_wPtr += 1;
    //de -2^6 a 2^6-1 : de -64 a 63
    if (int4 >= -0x40 && int4 < 0x40) {
        m_wPtr[-1] = int4&0x7F;
        return;
    }
    if (m_wbufferEnd - m_wPtr < 1) throw OutWriteBufferException("in write buffer (N) " + m_fileName);
    m_wPtr += 1;
    //de -2^13 a -2^6-1 et de 2^6 a 2^13-1 : de -8192 a -65 et de 64 a 8191
    if (int4 >= -0x2000 && int4 < 0x2000) {
        m_wPtr[-1] = (int4)&0xFF;
        m_wPtr[-2] = ((int4>>8)&0x3F)|0x80;
        return;
    }
    if (m_wbufferEnd - m_wPtr < 1) throw OutWriteBufferException("in write buffer (O) " + m_fileName);
    m_wPtr += 1;
    //de -2^20 a -2^13-1 et de 2^13 a 2^20-1 : de -1048576 a -8193 et de 8192 a 1048575
    if (int4 >= -0x100000 && int4 < 0x100000) {
        m_wPtr[-1] = (int4)&0xFF;
        m_wPtr[-2] = (int4>>8)&0xFF;
        m_wPtr[-3] = ((int4>>16)&0x1F)|0xC0;
        return;
    }
    if (m_wbufferEnd - m_wPtr < 1) throw OutWriteBufferException("in write buffer (P) " + m_fileName);
    m_wPtr += 1;
    //de -2^27 a -2^20-1 et de 2^20 a 2^27-1 : de -134217728 a -1048577 et de 1048576 a 134217727
    if (int4 >= -0x8000000 && int4 < 0x8000000) {
        m_wPtr[-1] = (int4)&0xFF;
        m_wPtr[-2] = (int4>>8)&0xFF;
        m_wPtr[-3] = (int4>>16)&0xFF;
        m_wPtr[-4] = ((int4>>24)&0x0F)|0xE0;
        return;
    }
     if (m_wbufferEnd - m_wPtr < 1) throw OutWriteBufferException("in write buffer (Q) " + m_fileName);
     m_wPtr += 1;
    //de -2^31 a -2^27-1 et de 2^27 a 2^31-1 : de -2147483648 a -134217729 et de 134217728 a 2147483647
    m_wPtr[-1] = (int4)&0xFF;
    m_wPtr[-2] = (int4>>8)&0xFF;
    m_wPtr[-3] = (int4>>16)&0xFF;
    m_wPtr[-4] = (int4>>24)&0xFF;
    m_wPtr[-5] = ((int4>>31)&0x07)|0xF0;
}
////////////////////////////////////////////////////////////
//brief Put a string into the intermediate buffer
//param str string to write  */
void NindFile::putString(const string &str)
{
    if (str.length() > 254) throw OutWriteBufferException("String length " + m_fileName);
    const unsigned char stringLen = str.length();
    if (m_wbufferEnd - m_wPtr < stringLen + 1) throw OutWriteBufferException("in write buffer (R) " + m_fileName);
    //ecrit la taille de la chaine
    (*m_wPtr++) = stringLen;
    //ecrit la chaine
    memcpy(m_wPtr, (unsigned char*)str.c_str(), stringLen);
    m_wPtr += stringLen;
}
////////////////////////////////////////////////////////////
//brief Put a string as bytes into the intermediate buffer
//param str string to write  */
void NindFile::putStringAsBytes(const string &str)
{
    if (str.length() > 255) throw OutWriteBufferException("String length " + m_fileName);
    const unsigned char stringLen = str.length();
    if (m_wbufferEnd - m_wPtr < stringLen) throw OutWriteBufferException("in write buffer (S) " + m_fileName);
    //ecrit la chaine
    memcpy(m_wPtr, (unsigned char*)str.c_str(), stringLen);
    m_wPtr += stringLen;
}
////////////////////////////////////////////////////////////
//brief Write intermediate buffer to the file */
//param position offset into buffer of first byte to write
//param length length to write */
void NindFile::writeBuffer(const unsigned int position,
                           const unsigned int length)
{
    const unsigned int size = (length == 0) ? (m_wPtr - m_wbuffer) : length;
    if ((uint64_t)position + size > (uint64_t)(m_wbufferEnd - m_wbuffer))
        throw OutWriteBufferException("in write buffer (W) " + m_fileName);
    const unsigned int writeSize =  fwrite(m_wbuffer + position, 1, size, m_file);
    //remet le pointeur d'ejcriture en teste du buffer pour une hypothejtique rejutilisation
    m_wPtr = m_wbuffer;
    if (writeSize != size) throw WriteFileException(m_fileName);
    //en mode massif, ni vidange ni taille : l'appelant sait oug il ejcrit
    if (m_isBulk) return;
    fflush(m_file);
    //memorise la taille du fichier
    const long int current = ftell(m_file);
    if (current > m_fileSize) m_fileSize = current;
}
////////////////////////////////////////////////////////////
//brief Write byte value to the file
//param value value to write
//param count number of bytes to write*/
void NindFile::writeValue(const unsigned char value,
                          const unsigned int count)
{
    unsigned char* buffer = new (nothrow) unsigned char[count];
    if (buffer == 0) throw BadAllocException("in write value buffer " + m_fileName);
    for (unsigned int it = 0; it != count; it++) buffer[it] = value;
    const unsigned int writeSize =  fwrite(buffer, 1, count, m_file);
    delete [] buffer;
    if (writeSize != count) throw WriteFileException(m_fileName);
    if (m_isBulk) return;
    fflush(m_file);
    //memorise la taille du fichier
    const long int current = ftell(m_file);
    if (current > m_fileSize) m_fileSize = current;
}
////////////////////////////////////////////////////////////
//Read bytes from file into specified buffer
void NindFile::readBytes(unsigned char* bytes,
                         const unsigned int bytesNb)
{
    //fflush(m_file);
    const unsigned int readSize = fread(bytes, 1, bytesNb, m_file);
    if (readSize != bytesNb) {
        if (feof(m_file)) throw EofException(m_fileName);
        else throw ReadFileException(m_fileName);
    }
}
////////////////////////////////////////////////////////////
//brief Switch bulk mode on or off
//param bulk true to enter bulk mode, false to leave it */
void NindFile::setBulk(const bool bulk)
{
    if (bulk == m_isBulk) return;
    if (fflush(m_file) != 0) throw WriteFileException(m_fileName);
    m_isBulk = bulk;
    if (bulk) return;
    //en sortie, la taille du fichier est rejtablie
    const long int current = ftell(m_file);
    if (fseek(m_file, 0, SEEK_END) != 0) throw WriteFileException(m_fileName);
    m_fileSize = ftell(m_file);
    fseek(m_file, current, SEEK_SET);
}
////////////////////////////////////////////////////////////
//brief Replace the read buffer with bytes from memory
//param bytes bytes to read next
//param bytesNb number of bytes */
void NindFile::loadInBuffer(const unsigned char *bytes,
                            const unsigned int bytesNb)
{
    m_in.clear();
    unsigned char *buffer = m_in.prepare(bytesNb);
    if (bytesNb != 0) memcpy(buffer, bytes, bytesNb);
}
////////////////////////////////////////////////////////////
//brief Copy part of the write buffer
//param position offset into write buffer of the first byte to copy
//param length number of bytes to copy
//param bytes receives the copied bytes */
void NindFile::copyOutBuffer(const unsigned int position,
                             const unsigned int length,
                             std::vector<unsigned char> &bytes) const
{
    if ((uint64_t)position + length > (uint64_t)(m_wPtr - m_wbuffer))
        throw OutWriteBufferException("in write buffer (C) " + m_fileName);
    bytes.assign(m_wbuffer + position, m_wbuffer + position + length);
}
////////////////////////////////////////////////////////////
//brief Put bytes into the intermediate buffer
//param bytes bytes to put
//param bytesNb number of bytes */
void NindFile::putBytes(const unsigned char *bytes,
                        const unsigned int bytesNb)
{
    if ((size_t)(m_wbufferEnd - m_wPtr) < bytesNb) throw OutWriteBufferException("in write buffer (B) " + m_fileName);
    if (bytesNb != 0) memcpy(m_wPtr, bytes, bytesNb);
    m_wPtr += bytesNb;
}
////////////////////////////////////////////////////////////
