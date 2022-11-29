//
// C++ Interface: NindExceptions
//
// Description: les exceptions du projet NIND (nouvel index)
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
#ifndef NindExceptions_H
#define NindExceptions_H
////////////////////////////////////////////////////////////
#include <stdexcept>
#include <string>
////////////////////////////////////////////////////////////
namespace latecon {
    namespace nindex {
////////////////////////////////////////////////////////////
/**\brief when something on file fails  */
class FileException : public std::runtime_error {
public:
    std::string m_fileName;
    FileException(const char * name, const std::string& fileName) :
        std::runtime_error( (std::string(name)+": "+fileName).c_str() ),
        m_fileName(fileName) {}
    virtual ~FileException() throw() {}
};
/**\brief when EOF occurs (it can be a valid test of EOF)  */
class EofException : public FileException {
    public:
    EofException() :
        FileException("EofException", "") {}
    EofException(const std::string& fileName) :
        FileException("EofException", fileName) {}
};
/**\brief when decode file fails  */
class FormatFileException : public FileException {
public:
    FormatFileException() :
        FileException("FormatFileException", "") {}
    FormatFileException(const std::string& fileName) :
        FileException("FormatFileException", fileName) {}
};
/**\brief when an incompatible file is used  */
class IncompatibleFileException : public FileException {
    public:
    IncompatibleFileException() :
        FileException("IncompatibleFileException", "") {}
    IncompatibleFileException(const std::string& fileName) :
        FileException("IncompatibleFileException", fileName) {}
};
/**\brief when an invalid file is used  */
class InvalidFileException : public FileException {
    public:
    InvalidFileException() :
        FileException("InvalidFileException", "") {}
    InvalidFileException(const std::string& fileName) :
        FileException("InvalidFileException", fileName) {}
};
/**\brief when open file fails  */
class OpenFileException : public FileException {
public:
    OpenFileException() :
        FileException("OpenFileException", "") {}
    OpenFileException(const std::string& fileName) :
        FileException("OpenFileException", fileName) {}
};
/**\brief when read error occurs on a file  */
class ReadFileException : public FileException {
    public:
    ReadFileException() :
        FileException("ReadFileException", "") {}
    ReadFileException(const std::string& fileName) :
        FileException("ReadFileException", fileName) {}
};
/**\brief when seek error occurs on a file  */
class SeekFileException : public FileException {
    public:
    SeekFileException() :
        FileException("SeekFileException", "") {}
    SeekFileException(const std::string& fileName) :
        FileException("SeekFileException", fileName) {}
};
/**\brief when write error occurs on a file  */
class WriteFileException : public FileException {
    public:
    WriteFileException() :
        FileException("WriteFileException", "") {}
    WriteFileException(const std::string& fileName) :
        FileException("WriteFileException", fileName) {}
};
/**\brief when attempt to read over buffer  */
class OutReadBufferException : public FileException {
    public:
    OutReadBufferException() :
        FileException("Out read buffer error", "") {}
    OutReadBufferException(const std::string& fileName) :
        FileException("Out read buffer error", fileName) {}
};
/**\brief when attempt to write over buffer  */
class OutWriteBufferException : public FileException {
    public:
    OutWriteBufferException() :
        FileException("OutWriteBufferException", "") {}
    OutWriteBufferException(const std::string& fileName) :
        FileException("OutWriteBufferException", fileName) {}
};
/**\brief when a bad alloc occurs in allocating buffer  */
class BadAllocException : public FileException {
    public:
    BadAllocException() :
        FileException("BadAllocException", "") {}
    BadAllocException(const std::string& fileName) :
        FileException("BadAllocException", fileName) {}
};
/**\brief when an out of bound parameter is gotten   */
class OutOfBoundException : public FileException {
    public:
    OutOfBoundException() :
        FileException("OutOfBoundException", "") {}
    OutOfBoundException(const std::string& fileName) :
        FileException("OutOfBoundException", fileName) {}
};
/**\brief when bad use of lexicon is attempted  */
class BadUseException : public FileException {
public:
    BadUseException() :
        FileException("BadUseExceptione", "") {}
    BadUseException(const std::string& fileName) :
        FileException("BadUseException", fileName) {}
};
/**\brief when an error occurs on file index  */
class NindPadFileException : public FileException {
    public:
    NindPadFileException() :
        FileException("NindPadFileException", "") {}
    NindPadFileException(const std::string& fileName) :
        FileException("NindPadFileException", fileName) {}
};
/**\brief when an error occurs on file index  */
class NindIndexException : public FileException {
    public:
    NindIndexException() :
        FileException("Nind Index error", "") {}
    NindIndexException(const std::string& fileName) :
        FileException("Nind Index error", fileName) {}
};
/**\brief when an error occurs on term file index  */
class NindTermIndexException : public FileException {
    public:
    NindTermIndexException() :
        FileException("Nind Termindex error", "") {}
    NindTermIndexException(const std::string& fileName) :
        FileException("Nind Termindex error", fileName) {}
};
/**\brief when an error occurs on local file index  */
class NindLocalIndexException : public FileException {
    public:
    NindLocalIndexException() :
        FileException("Nind Localindex error", "") {}
    NindLocalIndexException(const std::string& fileName) :
        FileException("Nind Localindex error", fileName) {}
};
/**\brief when an error occurs on lexicon file index  */
class NindLexiconIndexException : public FileException {
    public:
    NindLexiconIndexException() :
        FileException("Nind Lexiconindex error", "") {}
    NindLexiconIndexException(const std::string& fileName) :
        FileException("Nind Lexiconindex error", fileName) {}
};
/**\brief when an error occurs on retro lexicon file index  */
class NindRetrolexiconIndexException : public FileException {
    public:
    NindRetrolexiconIndexException() :
        FileException("Nind RetroLexiconindex error", "") {}
    NindRetrolexiconIndexException(const std::string& fileName) :
        FileException("Nind RetroLexiconindex error", fileName) {}
};
/**\brief when an error occurs on retro lexicon file  */
class NindRetrolexiconException : public FileException {
    public:
    NindRetrolexiconException() :
        FileException("Nind RetroLexicon error", "") {}
    NindRetrolexiconException(const std::string& fileName) :
        FileException("Nind RetroLexicon error", fileName) {}
};
/**\brief when an error occurs on lexicon  */
class NindLexiconException : public FileException {
    public:
    NindLexiconException() :
        FileException("Nind Lexicon error", "") {}
    NindLexiconException(const std::string& fileName) :
        FileException("Nind Lexicon error", fileName) {}
};
/**\brief when a decode error occurs    */
class DecodeErrorException : public FileException {
    public:
    DecodeErrorException() :
        FileException("DecodeErrorException", "") {}
    DecodeErrorException(const std::string& error) :
        FileException("DecodeErrorException", error) {}
};
/**\brief when a encode error occurs    */
class EncodeErrorException : public FileException {
    public:
    EncodeErrorException() :
        FileException("DecodeErrorException", "") {}
    EncodeErrorException(const std::string& error) :
        FileException("DecodeErrorException", error) {}
};
/**\brief when lexicon is broken  */
class IntegrityException : public FileException {
public:
    IntegrityException() :
        FileException("Lexicon IntegrityException", "") {}
    IntegrityException(const std::string& fileName) :
        FileException("Lexicon IntegrityException", fileName) {}
};
////////////////////////////////////////////////////////////
    } // end namespace
} // end namespace
#endif
////////////////////////////////////////////////////////////
