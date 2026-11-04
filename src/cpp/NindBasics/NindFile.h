//
// C++ Interface: NindFile
//
// Description: Les acces basiques a un fichier binaire sequentiel avec possibilites de bufferisation
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
#ifndef NindFile_H
#define NindFile_H
////////////////////////////////////////////////////////////
#include "NindCommonExport.h"
#include "NindExceptions.h"
#include <cstdint>
#include <atomic>
#include <mutex>
#include "NindSignalCatcher.h"
#include "NindReadBuffer.h"
#include <stdio.h>
#include <string>
#include <vector>
////////////////////////////////////////////////////////////
namespace latecon {
    namespace nindex {
////////////////////////////////////////////////////////////
class DLLExportLexicon NindFile {
public:
    /**\brief Creates NindFile with a specified name associated with.
    *\param fileName absolute path file name */
    NindFile(const std::string &fileName);

    virtual ~NindFile();
    
    /**\brief Begin a critical section where control-C will be temporaly catched. */
    void beginCriticalSection();
    
    /**\brief End a critical section where control-C have been temporaly catched. */
    void endCriticalSection();

    /**\brief Open file associated with. No exceptions are raised.
    *\param accessMode authorized access modes are "rb", "wb", "ab", "r+b", "w+b", "a+b", "rb+", "wb+", "ab+"
    *\return true if success, false otherwise */
    bool open(const std::string &accessMode);

    /**\brief Take an advisory exclusive lock on the open file, without waiting: a second
    * writer on the same file (in any process) is refused instead of silently corrupting it.
    * The lock is released when the file is closed. No-op (always true) on Windows.
    *\return true if the lock was taken, false if another open of the file holds it */
    bool lockExclusive();

    /**\brief Close file associated with. Success in any case. */
    void close();

    /**\brief get current position in file
    *\return current position in file */
    inline long int getPos() const;

    /**\brief get current size of file
    *\return current size of file */
    inline long int getFileSize() const;

    /**\brief Get the actual size of the file on disk, as it is now (another process may be extending it)
    * \return file size in bytes */
    uint64_t getCurrentFileSize();

    /**\brief get unique identifiant of file
    *\return identifiant of file */
    inline long int getFileIdent() const;

    /**\brief Return the effective size of read buffer 
    *\return the effective actual size */
    inline unsigned int getInBufferSize();

    /**\brief Set file on relative position
    *\param offset Number of bytes to offset from origin
    *\param origin Position from where offset is added :
    * SEEK_SET : Beginning of file, SEEK_CUR : Current position of the file pointer, SEEK_END : End of file
    *\return true if success, false otherwise */
    inline bool setPos(const long int offset, const int origin);

    /**brief clear input buffer in reading mode and write output buffer in writing mode
    *\return true if success, false otherwise */
    inline bool flush();

    /**\brief Read a single byte from the file
    *\return byte value */
    unsigned char readInt1();

    /**\brief Read a 3-bytes integer from the file
    *\return 4-bytes integer */
    unsigned int readInt3();

    /**\brief Get an unsigned latecon integer from the file
    *\return 4-bytes unsigned integer */
    unsigned int readUIntLat();
        
    /**\brief Get a signed latecon integer from the file
    *\return 4-bytes signed integer */
    signed int readSIntLat();
        
    /**\brief Read a string from the file
    *\return read string */
    std::string readString();
        
    /**\brief Read bytesNb bytes at offset into buffer, without moving the file position nor
    * touching the internal buffer: thread-safe (const), several threads can read one file at
    * once, each into its own buffer. Positioned reads (pread, ReadFile with an offset) on a
    * read-only handle of its own, opened at the first positioned read (so that a file only
    * read sequentially keeps one descriptor). Checks the bytes are in the file.
    * On Windows, concurrent reads of one handle are serialized by the system: correct, but
    * not parallel.
    *\param offset where to read in the file
    *\param bytesNb number of bytes to read
    *\param buffer receives the bytes (its previous content is forgotten, also on failure) */
    void readAt(const uint64_t offset,
                const unsigned int bytesNb,
                NindReadBuffer &buffer) const;

    /**\brief Size of the file now, from the read handle (thread-safe) */
    uint64_t getReadFileSize() const;

    /**\brief The internal read buffer, filled by readBuffer (for decoders shared with readAt) */
    inline NindReadBuffer &inBuffer() { return m_in; }

    /**\brief Read bytes from the file into an internal buffer
    *\param bytesNb size of internal buffer to receive read datas */
    void readBuffer(const unsigned int bytesNb);
        
    /**\brief Set logical end of read buffer specifying offset from current read pointer
    *\param bytesNb offset from current read pointer */
    void setEndInBuffer(const unsigned int bytesNb);

    /**\brief Set logical end of read buffer specifying offset from head of buffer
    *\param bytesNb offset from head of buffer */
    void setAbsEndInBuffer(const unsigned int bytesNb);

    /**\brief Test if effective buffer was entirely read
    *\return true if effective buffer was entirely read */
    inline bool endOfInBuffer();
    
    /**\brief Set pointer in read buffer
    *\param offset from buffer head where to read next time */
    inline void setInBufferPtr(const unsigned int offset);
        
    /**\brief Set pointer in read buffer
    *\param offset from actual read pointer where to read next time */
    inline void setRelInBufferPtr(const unsigned int offset);

    /**\brief Get a 1-bytes integer from internal buffer
    *\return 4-bytes integer */
    unsigned int getInt1();

    /**\brief Get a 2-bytes integer from internal buffer
    *\return 4-bytes integer */
    unsigned int getInt2();

    /**\brief Get a 3-bytes integer from internal buffer
    *\return 4-bytes integer */
    unsigned int getInt3();

    /**\brief Get a 3-bytes signed integer from internal buffer
    *\return 4-bytes signed integer */
    signed int getSInt3();

    /**\brief Get a 4-bytes integer from internal buffer
    *\return 4-bytes integer */
    unsigned int getInt4();

    /**\brief Get a 4-bytes integer from internal buffer
    *\return 4-bytes integer */
    signed int getSInt4();

    /**\brief Get a 5-bytes integer from internal buffer
    *\return 8-bytes integer */
    uint64_t getInt5();
        
    /**\brief Get an unsigned latecon integer from internal buffer
    *\return 4-bytes unsigned integer */
    unsigned int getUIntLat();
        
    /**\brief Get a signed latecon integer from internal buffer
    *\return 4-bytes signed integer */
    signed int getSIntLat();

    /**\brief Get a string from internal buffer
    *\return read string */
    std::string getString();
    
    /**\brief Get a string from internal buffer
    *\param length number of bytes to read (bytes, not chars)
    *\return read string */
    std::string getStringAsBytes(const unsigned char length);

        
    /**\brief Create an internal buffer for writing
    *\param bytesNb size of buffer */
    void createBuffer(const unsigned int bytesNb);
        
    /**\brief Return the effective size of buffer for writing
     * \return the effective actual size */
    inline unsigned int getOutBufferSize();

    /**\brief Set pointer in write buffer
    *\param offset from buffer head where to write next time */
    inline void setOutBufferPtr(const unsigned int offset);
        
    /**\brief Put padding into the internal buffer
    *\param bytesNb number of padding bytes to write */
    void putPad(const unsigned int bytesNb);

    /**\brief Put one byte into the internal buffer
    *\param int1 byte value to write */
    void putInt1(const unsigned char int1);

    /**\brief Put a 2-bytes integer into the internal buffer
    *\param int4 integer to write as 2-bytes*/
    void putInt2(const unsigned int int4);

    /**\brief Put a 3-bytes integer into the internal buffer
    *\param int4 integer to write as 3-bytes*/
    void putInt3(const unsigned int int4);

    /**\brief Put a 3-bytes integer into the internal buffer at the specified offset
    *\param int4 integer to write as 3-bytes
    *\param offset where to write the 3 bytes into the buffer*/
    void putInt3(const unsigned int int4,
                 const unsigned int offset);

    /**\brief Put a 4-bytes integer into the internal buffer
    *\param int4 integer to write as 4-bytes*/
    void putInt4(const unsigned int int4);

    /**\brief Put a 5-bytes integer into the internal buffer
    *\param int8 long integer to write as 5-bytes*/
    void putInt5(const uint64_t int8);

    /**\brief Put an unsigned latecon integer into the internal buffer
    *\param int4 unsigned integer to write as latecon integer*/
    void putUIntLat(const unsigned int int4);

    /**\brief Put a signed latecon integer into the internal buffer
    *\param int4 signed integer to write as latecon integer*/
    void putSIntLat(const signed int int4);

    /**\brief Put a string into the intermediate buffer
    *\param str string to write  */
    void putString(const std::string &str);

    /**\brief Put a string as bytes into the intermediate buffer
    *\param str string to write  */
    void putStringAsBytes(const std::string &str);

    /**\brief Write intermediate buffer to the file 
    *\param position offset into buffer of first byte to write
    *\param length length to write */
    void writeBuffer(const unsigned int position = 0,
                     const unsigned int length = 0);
        
    /**\brief Write byte value to the file
    *\param value value to write
    *\param count number of bytes to write */
    void writeValue(const unsigned char value,
                    const unsigned int count);

    /**\brief Switch bulk mode on or off. In bulk mode, writes are not flushed one by one
    * (stdio buffers them) and critical sections don't install a signal handler: for a file
    * nobody reads while it is written. Leaving bulk mode flushes the file.
    *\param bulk true to enter bulk mode, false to leave it */
    void setBulk(const bool bulk);

    /**\brief Test if bulk mode is on
    *\return true if bulk mode is on */
    inline bool isBulk() const;

    /**\brief Replace the read buffer with bytes from memory (instead of the file)
    *\param bytes bytes to read next
    *\param bytesNb number of bytes */
    void loadInBuffer(const unsigned char *bytes,
                      const unsigned int bytesNb);

    /**\brief Copy part of the write buffer
    *\param position offset into write buffer of the first byte to copy
    *\param length number of bytes to copy
    *\param bytes receives the copied bytes */
    void copyOutBuffer(const unsigned int position,
                       const unsigned int length,
                       std::vector<unsigned char> &bytes) const;

    /**\brief Put bytes into the intermediate buffer
    *\param bytes bytes to put
    *\param bytesNb number of bytes */
    void putBytes(const unsigned char *bytes,
                  const unsigned int bytesNb);
       
private:
    //non copiable : possehde un FILE* et deux buffers allouejs (une copie provoquerait double fclose/delete[])
    NindFile(const NindFile &);
    NindFile &operator=(const NindFile &);

    //Read bytes from file into specified buffer
    void readBytes(unsigned char* bytes,
                   const unsigned int bytesNb);

    std::string m_fileName;
    FILE *m_file;
    unsigned char *m_wbuffer;        //buffer d'ecriture
    unsigned char *m_wbufferEnd;     //fin buffer d'ecriture
    unsigned char *m_wPtr;           //pointeur buffer d'ecriture
    NindReadBuffer m_in;             //buffer de lecture (et ses dejcodeurs)
    //descripteur (POSIX) ou HANDLE (Windows) en lecture seule, pour readAt : ouvert au premier
    //besoin (readHandle), -1 sinon
    mutable std::atomic<intptr_t> m_readHandle;
    mutable std::mutex m_readHandleMutex;
    intptr_t readHandle() const;
    long int m_fileSize;             //taille du fichier
    bool m_isLittleEndian;
    bool m_isBulk;                   //vrai en mode massif (ejcritures non vidanges une ah une)
    NindSignalCatcher *m_nindSignalCatcher;
};
////////////////////////////////////////////////////////////
/**\brief Scoped critical section: control-C is deferred for the lifetime of this object.
 * The section is closed on every exit path, including early returns and exceptions. */
class NindCriticalSection {
public:
    //en mode massif, le fichier n'est cohejrent qu'ah la fin : rien ah protejger
    explicit NindCriticalSection(NindFile &file): m_file(file), m_isActive(!file.isBulk()) {
        if (m_isActive) m_file.beginCriticalSection(); }
    ~NindCriticalSection() { if (m_isActive) m_file.endCriticalSection(); }
private:
    NindCriticalSection(const NindCriticalSection &);
    NindCriticalSection &operator=(const NindCriticalSection &);
    NindFile &m_file;
    const bool m_isActive;
};
////////////////////////////////////////////////////////////
//brief Test if bulk mode is on
//return true if bulk mode is on */
inline bool NindFile::isBulk() const
{
    return m_isBulk;
}
////////////////////////////////////////////////////////////
//brief get current position in file
//return current position in file */
inline long int NindFile::getPos() const
{
    return ftell(m_file);
}
////////////////////////////////////////////////////////////
//brief get current size of file
//return current size of file */
inline long int NindFile::getFileSize() const
{
    return m_fileSize;
}
////////////////////////////////////////////////////////////
//brief get unique identifiant of file
//return identifiant of file */
inline long int NindFile::getFileIdent() const
{
    return (long int)m_file;
}
////////////////////////////////////////////////////////////
//brief Return the effective size of read buffer 
//return the effective actual size */
inline unsigned int NindFile::getInBufferSize()
{
    return m_in.getInBufferSize();
}
////////////////////////////////////////////////////////////
//brief Set file on relative position
//param offset Number of bytes to offset from origin
//param origin Position from where offset is added :
//SEEK_SET : Beginning of file, SEEK_CUR : Current position of the file pointer, SEEK_END : End of file
//return true if success, false otherwise */
inline bool NindFile::setPos(const long int offset,
                      const int origin)
{
    return fseek(m_file, offset, origin);
}
////////////////////////////////////////////////////////////
//brief clear input buffer in reading mode and write output buffer in writing mode
//return true if success, false otherwise */
inline bool NindFile::flush()
{
    return fflush(m_file);
}
////////////////////////////////////////////////////////////
//brief Test if effective buffer was entirely read
//return true if effective buffer was entirely read */
inline bool NindFile::endOfInBuffer()
{
    return m_in.endOfInBuffer();
}
////////////////////////////////////////////////////////////
//brief Set pointer in read buffer
//param offset from buffer head where to read next time */
inline void NindFile::setInBufferPtr(const unsigned int offset)
{
    m_in.setInBufferPtr(offset);
}
////////////////////////////////////////////////////////////
//brief Set pointer in read buffer
//param offset from actual read pointer where to read next time */
inline void NindFile::setRelInBufferPtr(const unsigned int offset)
{
    m_in.setRelInBufferPtr(offset);
}
////////////////////////////////////////////////////////////
//brief Return the effective size of buffer for writing
//return the effective actual size */
inline unsigned int NindFile::getOutBufferSize()
{
    return (m_wPtr - m_wbuffer);
}
////////////////////////////////////////////////////////////
//\brief Set pointer in write buffer
//param offset from buffer head where to write next time */
inline void NindFile::setOutBufferPtr(const unsigned int offset)
{
    m_wPtr = m_wbuffer + offset;
}
////////////////////////////////////////////////////////////
    } // end namespace
} // end namespace
#endif
////////////////////////////////////////////////////////////
