//
// C++ Interface: NindReadBuffer
//
// Description: a read buffer and its decoders (fixed-width and latecon integers, strings),
// bounds-checked, independent of any file
//
// This file is part of NIND (as "nouvelle indexation").
// NIND is free software: you can redistribute it and/or modify it under the terms of the
// GNU Lesser General Public License (see <http://www.gnu.org/licenses/>), either version 3 of
// the License, or any later version.
////////////////////////////////////////////////////////////
#ifndef NindReadBuffer_H
#define NindReadBuffer_H
////////////////////////////////////////////////////////////
#include "NindCommonExport.h"
#include "NindExceptions.h"
#include <cstddef>
#include <cstdint>
#include <string>
////////////////////////////////////////////////////////////
namespace latecon {
    namespace nindex {
////////////////////////////////////////////////////////////
/**\brief Bytes read from a file, with the cursor and the decoders that consume them.
* Every decoder checks the bytes it reads are inside the buffer (OutReadBufferException).
* NindFile owns one for its sequential reads; a thread-safe read (NindFile::readAt) fills one
* owned by its caller, so concurrent reads of one file don't share a buffer.
* Capacity is reused from one read to the next up to MAX_KEPT_CAPACITY; a larger buffer is
* freed by the next prepare() or by shrink(). Not copyable. */
class DLLExportLexicon NindReadBuffer {
public:
    /**\brief largest capacity kept between reads (bytes) */
    static const size_t MAX_KEPT_CAPACITY = 1u << 20;

    /**\brief Creates an empty read buffer
    *\param fileName file the bytes come from (for exception messages); referenced, not
    * copied: it must outlive the reads (each read sets it, see NindFile::readAt) */
    explicit NindReadBuffer(const std::string *fileName = 0);

    ~NindReadBuffer();

    /**\brief set the file the bytes come from (for exception messages; referenced, not copied) */
    void setFileName(const std::string *fileName) { m_fileName = fileName; }

    /**\brief get the file the bytes come from */
    const std::string &getFileName() const { return m_fileName != 0 ? *m_fileName : noFileName(); }

    /**\brief Forget the content (the buffer is then empty) */
    void clear();

    /**\brief Make room for bytesNb bytes, forgetting the previous content
    *\param bytesNb number of bytes
    *\return where to put them; the buffer then holds them, cursor at their head */
    unsigned char *prepare(const unsigned int bytesNb);

    /**\brief Free the storage if larger than MAX_KEPT_CAPACITY (forgets the content then) */
    void shrink();

    /**\brief Set logical end of buffer specifying offset from current read pointer */
    void setEndInBuffer(const unsigned int bytesNb);

    /**\brief Set logical end of buffer specifying offset from head of buffer */
    void setAbsEndInBuffer(const unsigned int bytesNb);

    /**\brief Return the effective size of buffer */
    inline unsigned int getInBufferSize() const { return (unsigned int)(m_end - m_buffer); }

    /**\brief Test if effective buffer was entirely read */
    inline bool endOfInBuffer() const { return (m_ptr >= m_end); }

    /**\brief Set pointer in buffer, from buffer head */
    inline void setInBufferPtr(const unsigned int offset) {
        if (offset > (unsigned long)(m_absEnd - m_buffer))
            throw OutReadBufferException("in read buffer (V) " + getFileName());
        m_ptr = m_buffer + offset;
    }

    /**\brief Set pointer in buffer, from the current read pointer */
    inline void setRelInBufferPtr(const unsigned int offset) {
        if (offset > (unsigned long)(m_absEnd - m_ptr))
            throw OutReadBufferException("in read buffer (W) " + getFileName());
        m_ptr += offset;
    }

    unsigned int getInt1();
    unsigned int getInt2();
    unsigned int getInt3();
    signed int getSInt3();
    unsigned int getInt4();
    signed int getSInt4();
    uint64_t getInt5();
    unsigned int getUIntLat();
    signed int getSIntLat();
    std::string getString();
    std::string getStringAsBytes(const unsigned char length);

private:
    NindReadBuffer(const NindReadBuffer &);
    NindReadBuffer &operator=(const NindReadBuffer &);

    static const std::string &noFileName();

    const std::string *m_fileName;
    unsigned char *m_storage;       //allouej
    size_t m_capacity;              //taille allouej
    unsigned char *m_buffer;        //dejbut des donnejes (m_storage, ou 0 si vide)
    unsigned char *m_absEnd;        //fin absolue des donnejes
    unsigned char *m_end;           //fin ajustable
    unsigned char *m_ptr;           //pointeur de lecture
};
////////////////////////////////////////////////////////////
    } // end namespace
} // end namespace
#endif
////////////////////////////////////////////////////////////
